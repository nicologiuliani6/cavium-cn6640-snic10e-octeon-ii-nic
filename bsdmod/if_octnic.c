/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2026 Cavium CN6640 SNIC10E FreeBSD Driver Project
 *
 * octnic: host side of the Cavium shared-memory NIC. Registers one ifnet
 * per card port (oct0, oct1) over the card's DRAM window at PCIe BAR2.
 * Protocol byte-identical to linuxmod/octnic.c.
 */

#include "if_octnic.h"

/* ----------------------------------------------------------------
 * Module parameters — match Linux defaults
 * ---------------------------------------------------------------- */
static int oct_ports = 1;
SYSCTL_INT(_kern, OID_AUTO, octnic_ports, CTLFLAG_RW, &oct_ports, 0,
    "Number of card ports (1 or 2)");

static int oct_poll_us = 200;
SYSCTL_INT(_kern, OID_AUTO, octnic_poll_us, CTLFLAG_RW, &oct_poll_us, 0,
    "Poll interval in microseconds");

static int ntxq = 1;
SYSCTL_INT(_kern, OID_AUTO, octnic_ntxq, CTLFLAG_RW, &ntxq, 0,
    "Number of TX queues (parallel cores)");

static int oct_rxthreads = 1;
SYSCTL_INT(_kern, OID_AUTO, octnic_rxthreads, CTLFLAG_RW, &oct_rxthreads, 0,
    "Number of parallel RX drain threads (power of 2: 1/2/4/8)");

static int oct_hrx = 1;
SYSCTL_INT(_kern, OID_AUTO, octnic_hrx, CTLFLAG_RW, &oct_hrx, 0,
    "HRX mode: card DMA into host RAM with per-slot {len,phase} header");

static int oct_lockfree = 1;
SYSCTL_INT(_kern, OID_AUTO, octnic_lockfree, CTLFLAG_RW, &oct_lockfree, 0,
    "Lock-free RX: use per-slot phase bit instead of card rx_prod");

static int oct_ztx = 0;
SYSCTL_INT(_kern, OID_AUTO, octnic_ztx, CTLFLAG_RW, &oct_ztx, 0,
    "Zero-copy TX: host copies to coherent pool, card DPI-reads it");

static int oct_dma = 0;
SYSCTL_INT(_kern, OID_AUTO, octnic_dma, CTLFLAG_RW, &oct_dma, 0,
    "DMA mode: card DMA for RX/TX (requires IOMMU)");

/* Temp thresholds — millidegC, match Linux module params */
static int oct_temp_warn_mc = 85000;
SYSCTL_INT(_kern, OID_AUTO, octnic_temp_warn, CTLFLAG_RW, &oct_temp_warn_mc, 0,
    "Temperature warning threshold (millidegC)");

static int oct_temp_crit_mc = 100000;
SYSCTL_INT(_kern, OID_AUTO, octnic_temp_crit, CTLFLAG_RW, &oct_temp_crit_mc, 0,
    "Temperature critical threshold (millidegC)");

/* Heartbeat timeout in seconds */
static int oct_hb_timeout = 10;
SYSCTL_INT(_kern, OID_AUTO, octnic_hb_timeout, CTLFLAG_RW, &oct_hb_timeout, 0,
    "Heartbeat freeze timeout (seconds)");

/* Power model params (milliwatts) */
static int p_base_mw = 18000;
SYSCTL_INT(_kern, OID_AUTO, octnic_p_base_mw, CTLFLAG_RW, &p_base_mw, 0,
    "Card baseline power estimate (mW)");

static int p_gbps_mw = 700;
SYSCTL_INT(_kern, OID_AUTO, octnic_p_gbps_mw, CTLFLAG_RW, &p_gbps_mw, 0,
    "Extra power per Gbps of aggregate TX+RX traffic (mW)");

/* ----------------------------------------------------------------
 * Global state — mirrors linuxmod/octnic.c
 * ---------------------------------------------------------------- */
#define PORT(sc, idx)	(&((sc)->sc_ports[(idx)]))

/* MMIO helpers — bus_space_read/write on the BAR2 handle */
static inline uint32_t
prd(struct octnic_port *p, uint32_t off)
{
	return (bus_space_read_4(p->op_sc->sc_bst,
	    p->op_sc->sc_bsh, p->op_port_id * OCTNIC_PORT_STRIDE + off));
}

static inline void
pwr(struct octnic_port *p, uint32_t off, uint32_t v)
{
	bus_space_write_4(p->op_sc->sc_bst,
	    p->op_sc->sc_bsh, p->op_port_id * OCTNIC_PORT_STRIDE + off, v);
}

static inline uint64_t
prd64(struct octnic_port *p, uint32_t off)
{
	return (bus_space_read_8(p->op_sc->sc_bst,
	    p->op_sc->sc_bsh, p->op_port_id * OCTNIC_PORT_STRIDE + off));
}

static inline void
pwr64(struct octnic_port *p, uint32_t off, uint64_t v)
{
	bus_space_write_8(p->op_sc->sc_bst,
	    p->op_sc->sc_bsh, p->op_port_id * OCTNIC_PORT_STRIDE + off, v);
}

/* ----------------------------------------------------------------
 * DMA helpers — busdma for hrx pool
 * ---------------------------------------------------------------- */
struct octnic_dmamap_arg {
	bus_addr_t	da_addr;
	int		da_error;
};

static void
octnic_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	struct octnic_dmamap_arg *da = arg;

	if (error) {
		da->da_error = error;
		return;
	}
	da->da_addr = segs[0].ds_addr;
}

static int
octnic_dma_alloc(struct octnic_port *port)
{
	struct octnic_dmamap_arg da;
	int error;
	uint8_t *va;
	int i;

	error = bus_dma_tag_create(bus_get_dma_tag(port->op_sc->sc_dev),
	    1, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
	    NULL, NULL, OCTNIC_BUF_SZ * OCTNIC_RING_SIZE, 1,
	    OCTNIC_BUF_SZ * OCTNIC_RING_SIZE, 0, NULL, NULL, &port->op_sc->sc_parent_dmat);
	if (error) {
		device_printf(port->op_sc->sc_dev,
		    "Failed to create RX DMA tag: %d\n", error);
		return (error);
	}

	error = bus_dmamem_alloc(port->op_sc->sc_parent_dmat, (void **)&va,
	    BUS_DMA_ZERO | BUS_DMA_WAITOK, &port->op_rx_dmamap);
	if (error) {
		device_printf(port->op_sc->sc_dev,
		    "Failed to alloc RX DMA memory: %d\n", error);
		return (error);
	}

	da.da_error = 0;
	error = bus_dmamap_load(port->op_sc->sc_parent_dmat, port->op_rx_dmamap,
	    va, OCTNIC_BUF_SZ * OCTNIC_RING_SIZE, octnic_dmamap_cb, &da, 0);
	if (error || da.da_error) {
		device_printf(port->op_sc->sc_dev,
		    "Failed to load RX DMA map: %d\n", error ? error : da.da_error);
		bus_dmamem_free(port->op_sc->sc_parent_dmat, va,
		    port->op_rx_dmamap);
		port->op_rx_dmamap = NULL;
		return (error ? error : da.da_error);
	}

	port->op_rx_dma_va = va;
	port->op_rx_dma_bus = da.da_addr;

	/* Seed phase bits to "not ready" (opposite of first expected) */
	for (i = 0; i < OCTNIC_RING_SIZE; i++) {
		uint32_t *hdr = (uint32_t *)(va + i * OCTNIC_BUF_SZ + 4);
		*hdr = htole32(RX_PHASE(i) ^ 1u);
	}

	pwr64(port, C_RX_DMA_BASE, port->op_rx_dma_bus);
	bus_space_barrier(port->op_sc->sc_bst, port->op_sc->sc_bsh, 0, 0, BUS_SPACE_BARRIER_WRITE);
	pwr(port, C_DMA_ENABLE, 1);
	bus_space_barrier(port->op_sc->sc_bst, port->op_sc->sc_bsh, 0, 0, BUS_SPACE_BARRIER_WRITE);

	device_printf(port->op_sc->sc_dev, "port%d DMA on, rx_bus=0x%jx\n",
	    port->op_port_id, (uintmax_t)port->op_rx_dma_bus);

	return (0);
}

static void
octnic_dma_free(struct octnic_port *port)
{
	if (!port->op_sc || !port->op_rx_dma_va || !port->op_rx_dmamap)
		return;

	pwr(port, C_DMA_ENABLE, 0);
	bus_space_barrier(port->op_sc->sc_bst, port->op_sc->sc_bsh, 0, 0, BUS_SPACE_BARRIER_WRITE);
	DELAY(100000); /* 100ms — let card disarm + in-flight DMA drain */

	bus_dmamap_unload(port->op_sc->sc_parent_dmat, port->op_rx_dmamap);
	bus_dmamem_free(port->op_sc->sc_parent_dmat, port->op_rx_dma_va,
	    port->op_rx_dmamap);
	port->op_rx_dma_va = NULL;
	port->op_rx_dmamap = NULL;
}

/* ----------------------------------------------------------------
 * TX — oct_xmit equivalent
 * ---------------------------------------------------------------- */
static int
octnic_start(struct ifnet *ifp, struct mbuf *m0)
{
	struct octnic_port *port = ifp->if_softc;
	struct octnic_softc *sc = port->op_sc;
	uint32_t tp, slot, len;

	if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0) {
		m_freem(m0);
		if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
		return (0);
	}

	/* CAS-claim a slot — parallel across txqs */
	for (;;) {
		tp = (uint32_t)atomic_fetchadd_int(&port->op_tx_claim, 1);
		if ((tp - port->op_tc_cache) >= OCTNIC_RING_SIZE) {
			uint32_t tc = prd(port, C_TX_CONS);
			port->op_tc_cache = tc;
			if ((tp - tc) >= OCTNIC_RING_SIZE) {
				m_freem(m0);
				if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
				return (0);
			}
		}

		slot = tp & (OCTNIC_RING_SIZE - 1);

		/* Check phase bit — slot available if phase matches */
		uint32_t desc_phase = prd(port, OCTNIC_TXDESC_OFF +
		    slot * 8 + 4) & 1u;
		if ((TX_PHASE(tp) == desc_phase))
			break;

		/* Slot not available, try again */
		continue;
	}

	len = m0->m_pkthdr.len;
	if (len > OCTNIC_BUF_SZ)
		len = OCTNIC_BUF_SZ;

	if (oct_ztx && oct_dma && port->op_rx_dma_va != NULL) {
		/* Zero-copy: copy into coherent pool */
		memcpy(port->op_rx_dma_va + slot * OCTNIC_BUF_SZ,
		    mtod(m0, caddr_t), len);
	} else {
		/* PIO: host posted-write into BAR2 TX buffer */
		bus_space_write_region_1(sc->sc_bst, sc->sc_bsh,
		    port->op_port_id * OCTNIC_PORT_STRIDE +
		    OCTNIC_TXBUF_OFF + slot * OCTNIC_BUF_SZ,
		    mtod(m0, caddr_t), len);
	}

	bus_space_barrier(sc->sc_bst, sc->sc_bsh, 0, 0, BUS_SPACE_BARRIER_WRITE);
	pwr(port, OCTNIC_TXDESC_OFF + slot * 8, len);
	bus_space_barrier(sc->sc_bst, sc->sc_bsh, 0, 0, BUS_SPACE_BARRIER_WRITE);
	pwr(port, OCTNIC_TXDESC_OFF + slot * 8 + 4, TX_PHASE(tp));
	bus_space_barrier(sc->sc_bst, sc->sc_bsh, 0, 0, BUS_SPACE_BARRIER_WRITE);

	m_freem(m0);
	if_inc_counter(ifp, IFCOUNTER_OBYTES, len);
	return (0);
}

/* ----------------------------------------------------------------
 * RX drain — oct_drain equivalent
 * ---------------------------------------------------------------- */
static void
octnic_drain(void *arg, int pending)
{
	struct octnic_port *port = (struct octnic_port *)arg;
	struct octnic_softc *sc = port->op_sc;
	struct ifnet *ifp = port->op_ifp;
	uint32_t stride = oct_rxthreads;
	uint32_t rp = oct_lockfree ? 0 : prd(port, C_RX_PROD);
	uint32_t rc = port->op_rc_pub[0]; /* simplified for single thread */
	int n = 0;

	while (n < 64) {
		uint32_t slot = rc & (OCTNIC_RING_SIZE - 1);
		uint32_t len;
		struct mbuf *m;

		if (oct_hrx) {
			/* Descriptor in LOCAL host RAM — no PCIe MMIO */
			uint8_t *h = port->op_rx_dma_va + slot * OCTNIC_BUF_SZ;
			uint32_t phase = le32toh(*(uint32_t *)(h + 4));

			if (phase != RX_PHASE(rc))
				break; /* not filled yet */
			atomic_thread_fence_acq();
			len = le32toh(*(uint32_t *)h);
		} else if (oct_lockfree) {
			/* One 64-bit desc read = {len, flags} */
			uint64_t desc = prd64(port, OCTNIC_RXDESC_OFF +
			    slot * 8);

			if (((uint32_t)(desc >> 32) & 1u) != RX_PHASE(rc))
				break;
			len = (uint32_t)desc;
		} else {
			if (rc == rp)
				break;
			len = prd(port, OCTNIC_RXDESC_OFF + slot * 8);
		}

		if (len && len <= OCTNIC_BUF_SZ) {
			m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
			if (m == NULL) {
				if_inc_counter(ifp, IFCOUNTER_IQDROPS, 1);
				rc += stride;
				n++;
				continue;
			}

			m->m_len = m->m_pkthdr.len = len;
			if (oct_dma && port->op_rx_dma_va != NULL) {
				/* Card DMA-wrote into local coherent RAM */
				memcpy(mtod(m, caddr_t),
				    (uint8_t *)port->op_rx_dma_va +
				    slot * OCTNIC_BUF_SZ +
				    (oct_hrx ? 8 : 0), len);
			} else {
				/* PIO path: read from BAR2 RX buffer */
				bus_space_read_region_1(sc->sc_bst, sc->sc_bsh,
				    port->op_port_id * OCTNIC_PORT_STRIDE +
				    OCTNIC_RXBUF_OFF + slot * OCTNIC_BUF_SZ,
				    mtod(m, caddr_t), len);
			}

			m->m_pkthdr.rcvif = ifp;
			if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
			if_inc_counter(ifp, IFCOUNTER_IBYTES, len);

			/* Deliver to stack */
			(*ifp->if_input)(ifp, m);
		}

		rc += stride;
		n++;
	}

	port->op_rc_pub[0] = rc;

	if (rc != port->op_rx_cons) {
		port->op_rx_cons = rc;
		pwr(port, C_RX_CONS, rc);
	}
}

/* ----------------------------------------------------------------
 * ifnet callbacks
 * ---------------------------------------------------------------- */
static void
octnic_init(void *arg)
{
	struct octnic_port *port = arg;
	struct ifnet *ifp = port->op_ifp;
	int i;

	if ((ifp->if_drv_flags & IFF_DRV_RUNNING))
		return;

	port->op_tx_prod = prd(port, C_TX_PROD);
	port->op_tc_cache = prd(port, C_TX_CONS);
	atomic_store_rel_int(&port->op_tx_claim, 0);
	port->op_tc_cache = 0;

	/* Snap RX consumer to card's current producer */
	port->op_rx_cons = oct_hrx ? 0 : prd(port, C_RX_PROD);
	pwr(port, C_RX_CONS, port->op_rx_cons);
	pwr(port, C_HOST_READY, 1);

	ifp->if_drv_flags |= IFF_DRV_RUNNING;

	/* Validate rxthreads */
	if (oct_rxthreads < 1) oct_rxthreads = 1;
	if (oct_rxthreads > 8) oct_rxthreads = 8;
	while (oct_rxthreads & (oct_rxthreads - 1))
		oct_rxthreads &= oct_rxthreads - 1;

	/* Multi-thread drain only where slots carry per-slot phase */
	if (!oct_hrx && !oct_lockfree)
		oct_rxthreads = 1;

	for (i = 0; i < oct_rxthreads; i++) {
		port->op_rc_pub[i] = port->op_rx_cons + i;
	}

	device_printf(port->op_sc->sc_dev, "port%d: initialized\n", port->op_port_id);
}

static void
octnic_stop(struct octnic_port *port)
{
	struct ifnet *ifp = port->op_ifp;

	ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
	port->op_running = 0;
	pwr(port, C_HOST_READY, 0);

	device_printf(port->op_sc->sc_dev, "port%d: stopped\n", port->op_port_id);
}

static int
octnic_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct octnic_port *port = ifp->if_softc;
	int error = 0;

	switch (cmd) {
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0)
				octnic_init(port);
		} else {
			if (ifp->if_drv_flags & IFF_DRV_RUNNING)
				octnic_stop(port);
		}
		break;
	case SIOCSIFMTU: {
		struct ifreq *ifr = (struct ifreq *)data;
		if (ifr->ifr_mtu > 65535 || ifr->ifr_mtu < ETHERMIN)
			error = EINVAL;
		else
			ifp->if_mtu = ifr->ifr_mtu;
		break;
	}
	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}

	return (error);
}

static void
octnic_qflush(struct ifnet *ifp)
{

	IFQ_PURGE(&ifp->if_snd);
}

/* ----------------------------------------------------------------
 * Watchdog / heartbeat — card health monitoring
 * ---------------------------------------------------------------- */
static void
octnic_watchdog(void *arg, int pending)
{
	struct octnic_softc *sc = arg;
	uint32_t beat;
	int i, ch;
	long t, tmax = 0;

	if (sc->sc_card_lost) {
		/* Check if card is back */
		uint32_t magic = prd(PORT(sc, 0), C_MAGIC);
		if (magic == OCTNIC_MAGIC) {
			sc->sc_card_lost = 0;
			device_printf(sc->sc_dev, "card back\n");
		} else {
			return;
		}
	}

	/* Check magic */
	if (prd(PORT(sc, 0), C_MAGIC) != OCTNIC_MAGIC) {
		sc->sc_card_lost = 1;
		for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++)
			if_link_state_change(PORT(sc, i)->op_ifp, LINK_STATE_DOWN);
		device_printf(sc->sc_dev, "CAVIUM CARD LOST: magic gone\n");
		return;
	}

	/* Check heartbeat */
	beat = prd(PORT(sc, 0), C_HEARTBEAT);
	if (beat == sc->sc_heartbeat_last) {
		if ((time_uptime - sc->sc_hb_time) > oct_hb_timeout) {
			sc->sc_card_lost = 1;
			for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++)
				if_link_state_change(PORT(sc, i)->op_ifp,
				    LINK_STATE_DOWN);
			device_printf(sc->sc_dev,
			    "CAVIUM CARD LOST: heartbeat frozen\n");
			return;
		}
	} else {
		sc->sc_heartbeat_last = beat;
		sc->sc_hb_time = time_uptime;
	}

	/* Drain RX in poll mode */
	for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++) {
		if (PORT(sc, i)->op_ifp->if_drv_flags & IFF_DRV_RUNNING)
			octnic_drain(PORT(sc, i), 0);
	}

	/* Check temperatures */
	for (ch = 0; ch < 2; ch++) {
		if (ch >= oct_ports || !PORT(sc, ch)->op_ifp)
			continue;
		t = (long)(int32_t)prd(PORT(sc, ch),
		    ch == 0 ? C_RESV0 : C_RESV1);
		if (t > tmax)
			tmax = t;
	}

	if (tmax >= oct_temp_crit_mc)
		device_printf(sc->sc_dev, "CAVIUM CARD OVERHEATING: %ld.%01ld C\n",
		    tmax / 1000, (tmax % 1000) / 100);
	else if (tmax >= oct_temp_warn_mc)
		device_printf(sc->sc_dev, "card hot: %ld.%01ld C\n",
		    tmax / 1000, (tmax % 1000) / 100);

	/* Carrier follows SFP+ link */
	for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++) {
		struct octnic_port *port = PORT(sc, i);
		uint32_t link = prd(port, C_LINK);
		int carrier = (link & 0x80000000u) ? (link & 1) : 1;

		if (carrier && port->op_ifp->if_link_state != LINK_STATE_UP)
			if_link_state_change(port->op_ifp, LINK_STATE_UP);
		else if (!carrier && port->op_ifp->if_link_state != LINK_STATE_DOWN)
			if_link_state_change(port->op_ifp, LINK_STATE_DOWN);
	}
}

static void
octnic_timer_tick(void *arg)
{
	struct octnic_softc *sc = arg;

	taskqueue_enqueue(taskqueue_thread, &sc->sc_watchdog_task);
	callout_reset(&sc->sc_timer, (hz * oct_poll_us) / 1000000, octnic_timer_tick, sc);
}

/* ----------------------------------------------------------------
 * PCI probe/attach/detach
 * ---------------------------------------------------------------- */
static int
octnic_probe(device_t dev)
{
	uint16_t v = pci_get_vendor(dev);
	uint16_t d = pci_get_device(dev);

	if (v == OCTNIC_VENDOR_ID && d == OCTNIC_DEVICE_ID) {
		device_set_desc(dev, "Cavium CN6640 SNIC10E 10GbE NIC");
		return (BUS_PROBE_DEFAULT);
	}

	if (v == IVSHMEM_VENDOR_ID && d == IVSHMEM_DEVICE_ID) {
		device_set_desc(dev,
		    "Cavium CN6640 SNIC10E Emulation (ivshmem)");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
octnic_attach(device_t dev)
{
	struct octnic_softc *sc;
	struct octnic_port *port;
	int i, error;

	sc = device_get_softc(dev);
	sc->sc_dev = dev;
	sc->sc_temp_warn_mc = oct_temp_warn_mc;
	sc->sc_temp_crit_mc = oct_temp_crit_mc;

	if (pci_get_vendor(dev) == IVSHMEM_VENDOR_ID &&
	    pci_get_device(dev) == IVSHMEM_DEVICE_ID)
		sc->sc_test_mode = 1;

	/* Enable bus mastering */
	pci_enable_busmaster(dev);

	/* Allocate BAR2 resource */
	sc->sc_bar2_rid = PCIR_BAR(OCTNIC_BAR);
	sc->sc_bar2_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &sc->sc_bar2_rid, RF_ACTIVE);
	if (sc->sc_bar2_res == NULL) {
		device_printf(dev, "Failed to allocate BAR2\n");
		return (ENXIO);
	}

	sc->sc_bst = rman_get_bustag(sc->sc_bar2_res);
	sc->sc_bsh = rman_get_bushandle(sc->sc_bar2_res);

	/* Verify magic on port 0 */
	if (prd(PORT(sc, 0), C_MAGIC) != OCTNIC_MAGIC) {
		device_printf(dev, "bad magic 0x%08x at BAR2\n",
		    prd(PORT(sc, 0), C_MAGIC));
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid,
		    sc->sc_bar2_res);
		return (ENXIO);
	}

	device_printf(dev, "magic OK ver=%u card_ready=%u ports=%d\n",
	    prd(PORT(sc, 0), C_VERSION),
	    prd(PORT(sc, 0), C_CARD_READY), oct_ports);

	if (oct_ports < 1) oct_ports = 1;
	if (oct_ports > OCTNIC_MAX_PORTS) oct_ports = OCTNIC_MAX_PORTS;

	if (ntxq < 1) ntxq = 1;
	if (ntxq > 8) ntxq = 8;

	/* Create DMA tag */
	error = bus_dma_tag_create(bus_get_dma_tag(dev),
	    1, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
	    NULL, NULL, BUS_SPACE_MAXSIZE_32BIT, 0,
	    BUS_SPACE_MAXSIZE_32BIT, 0, NULL, NULL,
	    &sc->sc_parent_dmat);
	if (error) {
		device_printf(dev, "Failed to create DMA tag: %d\n", error);
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid,
		    sc->sc_bar2_res);
		return (error);
	}

	/* Sysctl setup */
	sysctl_ctx_init(&sc->sc_sysctl_ctx);
	sc->sc_sysctl_tree = SYSCTL_ADD_NODE(&sc->sc_sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_kern), OID_AUTO, "octnic",
	    CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "octnic driver");

	mtx_init(&sc->sc_lock, "octnic", NULL, MTX_DEF);
	callout_init_mtx(&sc->sc_timer, &sc->sc_lock, 0);
	TASK_INIT(&sc->sc_watchdog_task, 0, octnic_watchdog, sc);

	/* Initialize ports */
	for (i = 0; i < oct_ports; i++) {
		port = PORT(sc, i);
		port->op_sc = sc;
		port->op_port_id = i;

		/* MAC: 02:c4:xx:xx:xx:xx based on device name hash */
		/* Generate a stable MAC hash from domain:bus:slot:function */
		uint32_t h = (uint32_t)(pci_get_domain(dev) << 24) |
		    (uint32_t)(pci_get_bus(dev) << 16) |
		    (uint32_t)(pci_get_slot(dev) << 8) |
		    (uint32_t)(pci_get_function(dev));
		port->op_macaddr[0] = 0x02;
		port->op_macaddr[1] = 0xc4;
		port->op_macaddr[2] = (h >> 24) & 0xff;
		port->op_macaddr[3] = (h >> 16) & 0xff;
		port->op_macaddr[4] = (h >> 8) & 0xff;
		port->op_macaddr[5] = (uint8_t)h + i;

		if (oct_dma) {
			error = octnic_dma_alloc(port);
			if (error)
				goto err;
		}

		port->op_ifp = if_alloc(IFT_ETHER);
		if (port->op_ifp == NULL) {
			device_printf(dev, "Failed to alloc ifnet port%d\n", i);
			error = ENOMEM;
			goto err;
		}

		if_initname(port->op_ifp, "oct", i);
		if_setsoftc(port->op_ifp, port);
		if_setdrvflags(port->op_ifp, 0);
		if_setinitfn(port->op_ifp, octnic_init);
		if_setioctlfn(port->op_ifp, octnic_ioctl);
		if_settransmitfn(port->op_ifp, octnic_start);
		if_setqflushfn(port->op_ifp, octnic_qflush);
		if_setmtu(port->op_ifp, 1500);

		ether_ifattach(port->op_ifp, port->op_macaddr);
		if_link_state_change(port->op_ifp, LINK_STATE_DOWN);

		device_printf(dev, "registered %s\n", if_name(port->op_ifp));
	}

	/* Start watchdog */
	sc->sc_heartbeat_last = prd(PORT(sc, 0), C_HEARTBEAT);
	sc->sc_hb_time = time_uptime;
	mtx_lock(&sc->sc_lock);
	callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);
	mtx_unlock(&sc->sc_lock);

	device_printf(dev, "octnic driver attached (%s)\n",
	    sc->sc_test_mode ? "emulation" : "hardware");

	return (0);

err:
	for (i = 0; i < oct_ports; i++) {
		port = PORT(sc, i);
		if (port->op_ifp != NULL) {
			ether_ifdetach(port->op_ifp);
			if_free(port->op_ifp);
			port->op_ifp = NULL;
		}
		if (oct_dma)
			octnic_dma_free(port);
	}
	bus_dma_tag_destroy(sc->sc_parent_dmat);
	bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid,
	    sc->sc_bar2_res);
	return (error);
}

static int
octnic_detach(device_t dev)
{
	struct octnic_softc *sc = device_get_softc(dev);
	struct octnic_port *port;
	int i;

	mtx_lock(&sc->sc_lock);
	callout_stop(&sc->sc_timer);
	mtx_unlock(&sc->sc_lock);
	taskqueue_drain(taskqueue_thread, &sc->sc_watchdog_task);

	for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++) {
		port = PORT(sc, i);
		if (port->op_ifp != NULL) {
			octnic_stop(port);
			ether_ifdetach(port->op_ifp);
			if_free(port->op_ifp);
			port->op_ifp = NULL;
		}
		if (oct_dma)
			octnic_dma_free(port);
	}

	if (sc->sc_sysctl_tree != NULL)
		sysctl_ctx_free(&sc->sc_sysctl_ctx);

	mtx_destroy(&sc->sc_lock);
	bus_dma_tag_destroy(sc->sc_parent_dmat);
	bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid,
	    sc->sc_bar2_res);

	device_printf(dev, "octnic detached\n");
	return (0);
}

/* ----------------------------------------------------------------
 * Driver registration
 * ---------------------------------------------------------------- */
static device_method_t octnic_methods[] = {
	DEVMETHOD(device_probe,		octnic_probe),
	DEVMETHOD(device_attach,	octnic_attach),
	DEVMETHOD(device_detach,	octnic_detach),
	DEVMETHOD_END
};

static driver_t octnic_driver = {
	"octnic",
	octnic_methods,
	sizeof(struct octnic_softc)
};

DRIVER_MODULE(if_octnic, pci, octnic_driver, 0, 0);
MODULE_DEPEND(if_octnic, pci, 1, 1, 1);
MODULE_DEPEND(if_octnic, ether, 1, 1, 1);
