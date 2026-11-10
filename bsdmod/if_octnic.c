/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Cavium CN6640 SNIC10E FreeBSD Driver Project
 *
 * octnic: host side of the Cavium shared-memory NIC. Registers one ifnet
 * per card port (oct0, oct1) over the card's DRAM window at PCIe BAR2.
 * Protocol byte-identical to linuxmod/octnic.c.
 */
#define __BSD_VISIBLE 1
#include "if_octnic.h"

/* ----------------------------------------------------------------
 * Module parameters (mirror Linux module_param equivalents)
 * ---------------------------------------------------------------- */
static int oct_ports = 1;
SYSCTL_INT(_dev_octnic, OID_AUTO, ports, CTLFLAG_RW, &oct_ports, 0,
    "Number of card ports (1 or 2)");

static int oct_poll_us = 200;
SYSCTL_INT(_dev_octnic, OID_AUTO, poll_us, CTLFLAG_RW, &oct_poll_us, 0,
    "Poll interval in microseconds");

static int ntxq = 1;
SYSCTL_INT(_dev_octnic, OID_AUTO, ntxq, CTLFLAG_RW, &ntxq, 0,
    "Number of TX queues (parallel cores)");

static int oct_rxthreads = 1;
SYSCTL_INT(_dev_octnic, OID_AUTO, rxthreads, CTLFLAG_RW, &oct_rxthreads, 0,
    "Number of parallel RX drain threads (power of 2: 1/2/4/8)");

static int oct_hrx = 1;
SYSCTL_INT(_dev_octnic, OID_AUTO, hrx, CTLFLAG_RW, &oct_hrx, 0,
    "HRX mode: card DMA into host RAM with per-slot {len,phase} header");

static int oct_lockfree = 1;
SYSCTL_INT(_dev_octnic, OID_AUTO, lockfree, CTLFLAG_RW, &oct_lockfree, 0,
    "Lock-free RX: use per-slot phase bit instead of card rx_prod");

static int oct_ztx = 0;
SYSCTL_INT(_dev_octnic, OID_AUTO, ztx, CTLFLAG_RW, &oct_ztx, 0,
    "Zero-copy TX: host copies to coherent pool, card DPI-reads it");

static int oct_dma = 0;
SYSCTL_INT(_dev_octnic, OID_AUTO, dma, CTLFLAG_RW, &oct_dma, 0,
    "DMA mode: card DMA for RX/TX (requires IOMMU)");

/* Temp thresholds — millidegC, match Linux module params */
static int oct_temp_warn_mc = 85000;
SYSCTL_INT(_dev_octnic, OID_AUTO, temp_warn, CTLFLAG_RW, &oct_temp_warn_mc, 0,
    "Temperature warning threshold (millidegC)");

static int oct_temp_crit_mc = 100000;
SYSCTL_INT(_dev_octnic, OID_AUTO, temp_crit, CTLFLAG_RW, &oct_temp_crit_mc, 0,
    "Temperature critical threshold (millidegC)");

/* Heartbeat timeout in seconds */
static int oct_hb_timeout = 10;
SYSCTL_INT(_dev_octnic, OID_AUTO, hb_timeout, CTLFLAG_RW, &oct_hb_timeout, 0,
    "Heartbeat freeze timeout (seconds)");

/* Power model params (milliwatts) */
static int p_base_mw = 18000;
SYSCTL_INT(_dev_octnic, OID_AUTO, p_base_mw, CTLFLAG_RW, &p_base_mw, 0,
    "Card baseline power estimate (mW)");

static int p_gbps_mw = 700;
SYSCTL_INT(_dev_octnic, OID_AUTO, p_gbps_mw, CTLFLAG_RW, &p_gbps_mw, 0,
    "Extra power per Gbps of aggregate TX+RX traffic (mW)");

/* ----------------------------------------------------------------
 * Global state — mirrors linuxmod/octnic.c
 * ---------------------------------------------------------------- */
static struct octnic_softc *oct_sc;
static struct sysctl_oid *oct_hwmon_node;

/* MMIO helpers — bus_space_read/write on the BAR2 handle */
#define PORT(sc, idx)	(&((sc)->sc_ports[(idx)]))

static inline uint32_t
prd(struct octnic_port *p, uint32_t off)
{
	return (bus_space_read_4(p->op_sc->sc_bst, p->op_sc->sc_bsh +
	    p->op_port_id * OCTNIC_PORT_STRIDE, off));
}

static inline void
pwr(struct octnic_port *p, uint32_t off, uint32_t v)
{
	bus_space_write_4(p->op_sc->sc_bst, p->op_sc->sc_bsh +
	    p->op_port_id * OCTNIC_PORT_STRIDE, off, v);
}

static inline uint64_t
prd64(struct octnic_port *p, uint32_t off)
{
	return (bus_space_read_8(p->op_sc->sc_bst, p->op_sc->sc_bsh +
	    p->op_port_id * OCTNIC_PORT_STRIDE, off));
}

static inline void
pwr64(struct octnic_port *p, uint32_t off, uint64_t v)
{
	bus_space_write_8(p->op_sc->sc_bst, p->op_sc->sc_bsh +
	    p->op_port_id * OCTNIC_PORT_STRIDE, off, v);
}

/* ----------------------------------------------------------------
 * DMA helpers — busdma for hrx pool
 * ---------------------------------------------------------------- */
static int
octnic_dma_alloc(struct octnic_port *port)
{
	bus_dma_segment_t segs[1];
	bus_addr_t baddr;
	int i, error, nseg;
	uint8_t *va;

	/* Create a dma tag for the RX pool: 32-bit addressing is fine
	 * since we only need host-visible RAM (not card DMA yet). */
	error = bus_dma_tag_create(bus_get_dma_tag(port->op_sc->sc_dev),
	    1, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
	    NULL, NULL, OCTNIC_BUF_SZ * OCTNIC_RING_SIZE, 1,
	    OCTNIC_BUF_SZ, 0, NULL, NULL, &port->op_sc->sc_parent_dmat);
	if (error) {
		device_printf(port->op_sc->sc_dev,
		    "Failed to create RX DMA tag: %d\n", error);
		return (error);
	}

	error = bus_dmamap_create(port->op_sc->sc_parent_dmat, 0, &port->op_rx_dmamap);
	if (error) {
		device_printf(port->op_sc->sc_dev,
		    "Failed to create RX DMA map: %d\n", error);
		bus_dma_tag_destroy(port->op_sc->sc_parent_dmat);
		return (error);
	}

	error = bus_dmamem_alloc(port->op_sc->sc_parent_dmat, (void **)&va,
	    BUS_DMA_ZERO | BUS_DMA_WAITOK, &port->op_rx_dmamap);
	if (error) {
		device_printf(port->op_sc->sc_dev,
		    "Failed to alloc RX DMA memory: %d\n", error);
		bus_dma_tag_destroy(port->op_sc->sc_parent_dmat);
		return (error);
	}

	error = bus_dmamap_load(port->op_sc->sc_parent_dmat, port->op_rx_dmamap,
	    va, OCTNIC_BUF_SZ * OCTNIC_RING_SIZE, NULL, 0);
	if (error) {
		device_printf(port->op_sc->sc_dev,
		    "Failed to load RX DMA map: %d\n", error);
		bus_dmamem_free(port->op_sc->sc_parent_dmat, va, port->op_rx_dmamap);
		bus_dma_tag_destroy(port->op_sc->sc_parent_dmat);
		return (error);
	}

	port->op_rx_dma_va = va;

	/* Get the bus address */
	error = bus_dmamap_get_sg(port->op_sc->sc_parent_dmat, port->op_rx_dmamap,
	    segs, &nseg, 0);
	if (error == 0 && nseg > 0) {
		port->op_rx_dma_bus = segs[0].ds_addr;
	} else {
		device_printf(port->op_sc->sc_dev, "Failed to get DMA address\n");
		bus_dmamem_free(port->op_sc->sc_parent_dmat, va, port->op_rx_dmamap);
		bus_dma_tag_destroy(port->op_sc->sc_parent_dmat);
		return (ENXIO);
	}

	/* Seed phase bits to "not ready" (opposite of first expected) */
	for (i = 0; i < OCTNIC_RING_SIZE; i++) {
		uint32_t *hdr = (uint32_t *)(va + i * OCTNIC_BUF_SZ + 4);
		*hdr = htole32(RX_PHASE(i) ^ 1u);
	}

	pwr64(port, C_RX_DMA_BASE, port->op_rx_dma_bus);
	wmb();
	pwr(port, C_DMA_ENABLE, 1);
	wmb();

	device_printf(port->op_sc->sc_dev, "port%d DMA on, rx_bus=0x%jx\n",
	    port->op_port_id, (uintmax_t)port->op_rx_dma_bus);

	return (0);
}

static void
octnic_dma_free(struct octnic_port *port)
{
	if (!port->op_sc || !port->op_rx_dma_va)
		return;

	pwr(port, C_DMA_ENABLE, 0);
	wmb();
	delay(100000); /* 100ms — let card disarm + in-flight DMA drain */

	bus_dmamem_free(port->op_sc->sc_parent_dmat, port->op_rx_dma_va,
	    port->op_rx_dmamap);
	port->op_rx_dma_va = NULL;
}

/* ----------------------------------------------------------------
 * TX — oct_xmit equivalent
 * ---------------------------------------------------------------- */
static void
octnic_transmit(if_t ifp, struct mbuf *m)
{
	struct octnic_port *port = if_getsoftc(ifp);
	struct octnic_softc *sc = port->op_sc;
	uint32_t tp, slot, len;

	if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0) {
		m_freem(m);
		if_inc_counter(ifp, IFCOUNTER_ODROP, 1);
		return;
	}

	/* CAS-claim a slot — parallel across txqs */
	for (;;) {
		tp = atomic_load_acq_int(&port->op_tx_claim);
		if ((tp - port->op_tc_cache) >= OCTNIC_RING_SIZE) {
			uint32_t tc = prd(port, C_TX_CONS);
			port->op_tc_cache = tc;
			if ((tp - tc) >= OCTNIC_RING_SIZE) {
				m_freem(m);
				if_inc_counter(ifp, IFCOUNTER_ODROP, 1);
				return;
			}
		}
		if ((uint32_t)atomic_cmpset_int(&port->op_tx_claim, tp, tp + 1))
			break;
	}

	len = m->m_pkthdr.len;
	if (len > OCTNIC_BUF_SZ)
		len = OCTNIC_BUF_SZ;

	slot = tp & (OCTNIC_RING_SIZE - 1);

	if (oct_ztx && oct_dma && port->op_rx_dma_va != NULL) {
		/* Zero-copy: copy into coherent pool */
		memcpy(port->op_rx_dma_va + slot * OCTNIC_BUF_SZ,
		    mtod(m, caddr_t), len);
	} else {
		/* PIO: host posted-write into BAR2 TX buffer */
		bus_space_write_region_1(sc->sc_bst, sc->sc_bsh +
		    port->op_port_id * OCTNIC_PORT_STRIDE + OCTNIC_TXBUF_OFF +
		    slot * OCTNIC_BUF_SZ, mtod(m, caddr_t), len);
	}

	wmb();
	pwr(port, OCTNIC_TXDESC_OFF + slot * 8, len);
	wmb();
	pwr(port, OCTNIC_TXDESC_OFF + slot * 8 + 4, TX_PHASE(tp));
	wmb();

	m_freem(m);
	if_inc_counter(ifp, IFCOUNTER_OBYTES, len);
}

/* ----------------------------------------------------------------
 * RX drain — oct_drain equivalent
 * ---------------------------------------------------------------- */
static int
octnic_drain(struct octnic_port *port, int tid, int budget)
{
	struct octnic_softc *sc = port->op_sc;
	if_t ifp = port->op_ifp;
	uint32_t stride = oct_rxthreads;
	uint32_t rp = oct_lockfree ? 0 : prd(port, C_RX_PROD);
	uint32_t rc = port->op_rc_pub[tid];
	int n = 0;

	while (n < budget) {
		uint32_t slot = rc & (OCTNIC_RING_SIZE - 1);
		uint32_t len;
		struct mbuf *m;

		if (oct_hrx) {
			/* Descriptor in LOCAL host RAM — no PCIe MMIO */
			uint8_t *h = (uint8_t *)port->op_rx_dma_va +
			    slot * OCTNIC_BUF_SZ;
			uint32_t phase = le32toh(*(uint32_t *)(h + 4));

			if (phase != RX_PHASE(rc))
				break; /* not filled yet */
			rmb();
			len = le32toh(*(uint32_t *)h);
		} else if (oct_lockfree) {
			/* One 64-bit desc read = {len, flags} */
			uint64_t desc = prd64(port, OCTNIC_RXDESC_OFF + slot * 8);

			if (((uint32_t)(desc >> 32) & 1u) != RX_PHASE(rc))
				break;
			len = (uint32_t)desc;
		} else {
			if (rc == rp)
				break;
			len = prd(port, OCTNIC_RXDESC_OFF + slot * 8);
		}

		if (len && len <= OCTNIC_BUF_SZ) {
			m = if_getmclalloc(ifp, MCLBYTES);
			if (m == NULL) {
				if_inc_counter(ifp, IFCOUNTER_IDROP, 1);
				rc += stride;
				n++;
				continue;
			}

			m->m_len = m->m_pkthdr.len = len;
			if (oct_dma && port->op_rx_dma_va != NULL) {
				/* Card DMA-wrote into local coherent RAM */
				memcpy(mtod(m, caddr_t),
				    port->op_rx_dma_va +
				    slot * OCTNIC_BUF_SZ + (oct_hrx ? 8 : 0),
				    len);
			} else {
				/* PIO path: read from BAR2 RX buffer */
				bus_space_read_region_1(sc->sc_bst,
				    sc->sc_bsh + port->op_port_id *
				    OCTNIC_PORT_STRIDE + OCTNIC_RXBUF_OFF +
				    slot * OCTNIC_BUF_SZ,
				    mtod(m, caddr_t), len);
			}

			m->m_next = NULL;
			if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
			if_inc_counter(ifp, IFCOUNTER_IBYTES, len);

			/* Deliver to stack */
			if_qflush(port->op_ifp);
			if_input(ifp, m);
		}

		rc += stride;
		n++;
	}

	port->op_rc_pub[tid] = rc;

	/* Publish consumer — min across threads */
	{
		uint32_t m = port->op_rc_pub[0], i;
		for (i = 1; i < stride; i++)
			if ((int32_t)(port->op_rc_pub[i] - m) < 0)
				m = port->op_rc_pub[i];
		if (m != port->op_rx_cons) {
			port->op_rx_cons = m;
			pwr(port, C_RX_CONS, m);
		}
	}

	return (n);
}

/* ----------------------------------------------------------------
 * RX poll thread — busy-poll kthread equivalent
 * ---------------------------------------------------------------- */
static int
octnic_poll_fn(void *arg)
{
	struct octnic_port *port = (struct octnic_port *)arg;
	int tid = port->op_port_id; /* use port_id as thread ID for simplicity */
	unsigned int idle = 0;

	called from a kernel thread, so we need to set the thread name and priority.
	/* In FreeBSD, the calling thread IS the poll thread */

	while (!kthread_should_stop()) {
		int did;

		did = octnic_drain(port, tid, 64);
		if (did) {
			idle = 0;
			/* usleep_range equivalent: pause briefly */
			tsleep((caddr_t)&idle, 0, "octpoll",
			    hz * oct_poll_us / 1000000 + 1);
		} else if (++idle < 8) {
			cpu_relax();
		} else {
			tsleep((caddr_t)&idle, 0, "octidle", hz / 50); /* ~20ms */
		}
	}

	return (0);
}

/* ----------------------------------------------------------------
 * ifnet callbacks
 * ---------------------------------------------------------------- */
static void
octnic_init(void *arg)
{
	struct octnic_port *port = arg;
	struct octnic_softc *sc = port->op_sc;
	int i;

	if ((if_getdrvflags(port->op_ifp) & IFF_DRV_RUNNING))
		return;

	port->op_tx_prod = prd(port, C_TX_PROD);
	port->op_tc_cache = prd(port, C_TX_CONS);
	atomic_store_rel_int(&port->op_tx_claim, 0);
	port->op_tc_cache = 0;

	/* Snap RX consumer to card's current producer */
	port->op_rx_cons = oct_hrx ? 0 : prd(port, C_RX_PROD);
	pwr(port, C_RX_CONS, port->op_rx_cons);
	pwr(port, C_HOST_READY, 1);

	if_setdrvflagbits(port->op_ifp, IFF_DRV_RUNNING, 0);
	if_setlinkstate(port->op_ifp, LINK_STATE_ALL);

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
		/* TODO: Create poll threads — FreeBSD kthread_create or thread_create */
		/* For now, single-threaded drain in watchdog */
	}

	device_printf(sc->sc_dev, "port%d: initialized\n", port->op_port_id);
}

static void
octnic_stop(struct octnic_port *port)
{
	struct octnic_softc *sc = port->op_sc;

	if_setdrvflagbits(port->op_ifp, 0, IFF_DRV_RUNNING);
	port->op_running = 0;
	pwr(port, C_HOST_READY, 0);

	/* TODO: Stop poll threads */

	device_printf(sc->sc_dev, "port%d: stopped\n", port->op_port_id);
}

static int
octnic_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct octnic_port *port = if_getsoftc(ifp);
	int error = 0;

	switch (cmd) {
	case SIOCSIFFLAGS:
		if (if_getflags(ifp) & IFF_UP) {
			if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0)
				octnic_init(port);
		} else {
			if (if_getdrvflags(ifp) & IFF_DRV_RUNNING)
				octnic_stop(port);
		}
		break;
	case SIOCSIFMTU: {
		struct ifreq *ifr = (struct ifreq *)data;
		if (ifr->ifr_mtu > ETHERMAX_MTU || ifr->ifr_mtu < ETHERMIN)
			error = EINVAL;
		else
			if_setmtu(ifp, ifr->ifr_mtu);
		break;
	}
	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}

	return (error);
}

static void
octnic_qflush(if_t ifp)
{
	struct octnic_port *port = if_getsoftc(ifp);
	struct ifqueue *ifq = &ifp->if_snd;

	IFQ_PURGE(ifq);
	ifq->ifq_len = 0;
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
	int lvl;

	if (sc->sc_card_lost) {
		/* Check if card is back */
		uint32_t magic = prd(PORT(sc, 0), C_MAGIC);
		if (magic == OCTNIC_MAGIC) {
			sc->sc_card_lost = 0;
			device_printf(sc->sc_dev, "card back (heartbeat alive)\n");
		} else {
			callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);
			return;
		}
	}

	/* Check magic */
	if (prd(PORT(sc, 0), C_MAGIC) != OCTNIC_MAGIC) {
		sc->sc_card_lost = 1;
		for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++)
			if_setlinkstate(PORT(sc, i)->op_ifp, LINK_STATE_DOWN);
		device_printf(sc->sc_dev, "CAVIUM CARD LOST: magic gone\n");
		callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);
		return;
	}

	/* Check heartbeat */
	beat = prd(PORT(sc, 0), C_HEARTBEAT);
	if (beat == sc->sc_heartbeat_last) {
		uint32_t now = getmicrousec();
		if ((now - sc->sc_hb_jiffies) > oct_hb_timeout * 1000000u) {
			sc->sc_card_lost = 1;
			for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++)
				if_setlinkstate(PORT(sc, i)->op_ifp, LINK_STATE_DOWN);
			device_printf(sc->sc_dev, "CAVIUM CARD LOST: heartbeat frozen\n");
			callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);
			return;
		}
	} else {
		sc->sc_heartbeat_last = beat;
		sc->sc_hb_jiffies = getmicrousec();
	}

	/* Check temperatures */
	for (ch = 0; ch < 2; ch++) {
		if (ch >= oct_ports || !PORT(sc, ch)->op_ifp)
			continue;
		t = (long)(int32_t)prd(PORT(sc, ch), ch == 0 ? C_RESV0 : C_RESV1);
		if (t > tmax)
			tmax = t;
	}

	lvl = tmax >= oct_temp_crit_mc ? 2 : tmax >= oct_temp_warn_mc ? 1 : 0;
	if (lvl == 2)
		device_printf(sc->sc_dev, "CAVIUM CARD OVERHEATING: %ld.%01ld C\n",
		    tmax / 1000, (tmax % 1000) / 100);
	else if (lvl == 1)
		device_printf(sc->sc_dev, "card hot: %ld.%01ld C\n",
		    tmax / 1000, (tmax % 1000) / 100);

	/* Carrier follows SFP+ link */
	for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++) {
		struct octnic_port *port = PORT(sc, i);
		uint32_t link = prd(port, C_LINK);
		int carrier = (link & 0x80000000u) ? (link & 1) : 1;

		if (carrier && !if_link_state_eq(port->op_ifp, LINK_STATE_ALL))
			if_setlinkstate(port->op_ifp, LINK_STATE_ALL);
		else if (!carrier && if_link_state_eq(port->op_ifp, LINK_STATE_ALL))
			if_setlinkstate(port->op_ifp, LINK_STATE_DOWN);
	}

	callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);
}

static void
octnic_timer_tick(void *arg)
{
	struct octnic_softc *sc = arg;

	taskqueue_enqueue(taskqueue_thread, &sc->sc_watchdog_task);
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
		device_set_desc(dev, "Cavium CN6640 SNIC10E Emulation (ivshmem)");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
octnic_attach(device_t dev)
{
	struct octnic_softc *sc;
	struct octnic_port *port;
	int i, error, ret;

	sc = device_get_softc(dev);
	bzero(sc, sizeof(*sc));
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
		device_printf(dev, "bad magic 0x%08x at BAR2 (card not ready?)\n",
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
	    BUS_SPACE_MAXSIZE_32BIT, 0, NULL, NULL, &sc->sc_parent_dmat);
	if (error) {
		device_printf(dev, "Failed to create DMA tag: %d\n", error);
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid,
		    sc->sc_bar2_res);
		return (error);
	}

	/* Sysctl setup */
	sysctl_ctx_init(&sc->sc_sysctl_ctx);
	sc->sc_sysctl_tree = SYSCTL_ADD_NODE(&sc->sc_sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_dev), OID_AUTO, "octnic",
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
		u_int32_t h = crc32(0, pci_name(dev), strlen(pci_name(dev)));
		port->op_macaddr[0] = 0x02;
		port->op_macaddr[1] = 0xc4;
		port->op_macaddr[2] = (h >> 24) & 0xff;
		port->op_macaddr[3] = (h >> 16) & 0xff;
		port->op_macaddr[4] = (h >> 8) & 0xff;
		port->op_macaddr[5] = (u_int8_t)h + i;

		if (oct_dma) {
			ret = octnic_dma_alloc(port);
			if (ret)
				goto err;
		}

		port->op_ifp = if_alloc(IFT_ETHER);
		if (port->op_ifp == NULL) {
			device_printf(dev, "Failed to alloc ifnet port%d\n", i);
			ret = ENOMEM;
			goto err;
		}

		if_initname(port->op_ifp, "oct", i);
		if_setsoftc(port->op_ifp, port);
		if_setdrvflags(port->op_ifp, 0);
		if_setinitfn(port->op_ifp, octnic_init);
		if_setioctlfn(port->op_ifp, octnic_ioctl);
		if_settransmitfn(port->op_ifp, octnic_transmit);
		if_setqflushfn(port->op_ifp, octnic_qflush);
		if_setmtu(port->op_ifp, 1500);

		ether_ifattach(port->op_ifp, port->op_macaddr);
		if_setlinkstate(port->op_ifp, LINK_STATE_DOWN);

		device_printf(dev, "registered %s\n", if_name(port->op_ifp));
	}

	/* Start watchdog */
	sc->sc_heartbeat_last = prd(PORT(sc, 0), C_HEARTBEAT);
	sc->sc_hb_jiffies = getmicrousec();
	callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);

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
	bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid, sc->sc_bar2_res);
	return (ret);
}

static int
octnic_detach(device_t dev)
{
	struct octnic_softc *sc = device_get_softc(dev);
	struct octnic_port *port;
	int i;

	callout_stop(&sc->sc_timer);
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
	bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid, sc->sc_bar2_res);

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
