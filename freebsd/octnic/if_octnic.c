/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * if_octnic.c — FreeBSD kernel module for Cavium CN6640-SNIC10E 10GbE NIC.
 * Two ports (oct0, oct1) over PCIe BAR2 shared-memory datapath.
 * Byte-identical register layout and ring protocol to linuxmod/octnic.c.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <machine/bus.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/taskqueue.h>
#include <sys/errno.h>
#include <sys/callout.h>

#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <sys/ck.h>
#include <net/if_var.h>
#include <net/altq/if_altq.h>
#include <net/if_private.h>
#include <net/ifq.h>
#include <net/ethernet.h>
#include <net/if_types.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "octnic_regs.h"

/* ----------------------------------------------------------------
 * Module parameters — match Linux defaults (linuxmod lines 58-90)
 * ---------------------------------------------------------------- */
static int oct_ports = 1;
SYSCTL_INT(_kern, OID_AUTO, octnic_ports, CTLFLAG_RW, &oct_ports, 0, "Number of card ports");

static int oct_poll_us = 200;
SYSCTL_INT(_kern, OID_AUTO, octnic_poll_us, CTLFLAG_RW, &oct_poll_us, 0, "Poll interval us");

static int ntxq = 1;
SYSCTL_INT(_kern, OID_AUTO, octnic_ntxq, CTLFLAG_RW, &ntxq, 0, "Number of TX queues");

static int oct_rxthreads = 1;
SYSCTL_INT(_kern, OID_AUTO, octnic_rxthreads, CTLFLAG_RW, &oct_rxthreads, 0,
    "Number of parallel RX drain threads (power of 2)");

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

/* Temp thresholds — millidegC, match Linux module params (linuxmod lines 478-481) */
static int oct_temp_warn_mc = 85000;
SYSCTL_INT(_kern, OID_AUTO, octnic_temp_warn, CTLFLAG_RW, &oct_temp_warn_mc, 0,
    "Temperature warning threshold (millidegC)");

static int oct_temp_crit_mc = 100000;
SYSCTL_INT(_kern, OID_AUTO, octnic_temp_crit, CTLFLAG_RW, &oct_temp_crit_mc, 0,
    "Temperature critical threshold (millidegC)");

/* Heartbeat timeout in seconds (linuxmod line 482) */
static int oct_hb_timeout = 10;
SYSCTL_INT(_kern, OID_AUTO, octnic_hb_timeout, CTLFLAG_RW, &oct_hb_timeout, 0,
    "Heartbeat freeze timeout (seconds)");

/* Power model params (milliwatts) — linuxmod lines 430-433 */
static int p_base_mw = 18000;
SYSCTL_INT(_kern, OID_AUTO, octnic_p_base_mw, CTLFLAG_RW, &p_base_mw, 0,
    "Card baseline power estimate (mW)");

static int p_gbps_mw = 700;
SYSCTL_INT(_kern, OID_AUTO, octnic_p_gbps_mw, CTLFLAG_RW, &p_gbps_mw, 0,
    "Extra power per Gbps of aggregate TX+RX traffic (mW)");

/* ----------------------------------------------------------------
 * Constants — derived from linuxmod lines 28-36
 * ---------------------------------------------------------------- */
#define POOL_SZ (OCTNIC_RING_SIZE * OCTNIC_BUF_SZ) /* 128*9216 = 1179648 */

/* ----------------------------------------------------------------
 * Per-port software state — mirrors linuxmod octnic.host_port (lines 97-109)
 * ---------------------------------------------------------------- */
struct octnic_port {
	struct octnic_softc	*op_sc;
	int			 op_port_id;
	struct ifnet		*op_ifp;

	/* Shadow counters (host owns, kept in RAM) */
	uint32_t		 op_tx_prod;	/* linuxmod line 105: tp_shadow */
	uint32_t		 op_rx_cons;	/* linuxmod line 107: rc_shadow */

	/* Parallel TX CAS counter — FreeBSD atomic_fetchadd_int */
	volatile u_int		 op_tx_claim;

	/* Last observed card TX consumer (cache) */
	uint32_t		 op_tc_cache;

	/* Multi-thread RX consumer per thread */
	uint32_t		 op_rc_pub[8];

	/* MAC address */
	uint8_t			 op_macaddr[6];

	/* DMA pools (hrx mode) */
	bus_dma_tag_t		 op_dmat;
	bus_dmamap_t		 op_dmamap;
	struct task		 op_rx_task;
	uint8_t			*op_rx_dma_va;
	bus_addr_t		 op_rx_dma_bus;
};

/* ----------------------------------------------------------------
 * Softc — one per PCI device, two ports
 * ---------------------------------------------------------------- */
struct octnic_softc {
	device_t		 sc_dev;
	struct resource		*sc_bar2_res;
	int			 sc_bar2_rid;
	bus_space_tag_t		 sc_bst;
	bus_space_handle_t	 sc_bsh;
	bus_dma_tag_t		 sc_parent_dmat;

	struct octnic_port	 sc_ports[OCTNIC_MAX_PORTS];

	/* Sysctl */
	struct sysctl_ctx_list	 sc_sysctl_ctx;
	struct sysctl_oid	*sc_sysctl_tree;

	/* Temperature thresholds (millidegC) — match Linux module params */
	int			 sc_temp_warn_mc;
	int			 sc_temp_crit_mc;

	/* Heartbeat / watchdog */
	uint32_t		 sc_heartbeat_last;
	time_t			 sc_hb_time;

	/* Card lost flag */
	int			 sc_card_lost;

	/* Lock for port state */
	struct mtx		 sc_lock;
	struct callout		sc_timer;
	struct task		 sc_watchdog_task;
};

#define PORT(sc, idx)	(&((sc)->sc_ports[(idx)]))

/* ----------------------------------------------------------------
 * MMIO helpers — bus_space_read/write on the BAR2 handle
 * linuxmod lines 117-120
 * ---------------------------------------------------------------- */
static inline uint32_t
prd(struct octnic_port *p, uint32_t off)
{
	struct octnic_softc *sc = p->op_sc;
	return (bus_space_read_4(sc->sc_bst, sc->sc_bsh,
	    p->op_port_id * OCTNIC_PORT_STRIDE + off));
}

static inline void
pwr(struct octnic_port *p, uint32_t off, uint32_t v)
{
	struct octnic_softc *sc = p->op_sc;
	bus_space_write_4(sc->sc_bst, sc->sc_bsh,
	    p->op_port_id * OCTNIC_PORT_STRIDE + off, v);
}

static inline uint64_t
prd64(struct octnic_port *p, uint32_t off)
{
	struct octnic_softc *sc = p->op_sc;
	return (bus_space_read_8(sc->sc_bst, sc->sc_bsh,
	    p->op_port_id * OCTNIC_PORT_STRIDE + off));
}

static inline void
pwr64(struct octnic_port *p, uint32_t off, uint64_t v)
{
	struct octnic_softc *sc = p->op_sc;
	bus_space_write_8(sc->sc_bst, sc->sc_bsh,
	    p->op_port_id * OCTNIC_PORT_STRIDE + off, v);
}

/* ----------------------------------------------------------------
 * DMA helpers — busdma for hrx pool
 * linuxmod lines 122-161: octdma_alloc / octdma_free
 * ---------------------------------------------------------------- */

/* Callback: save first segment bus address from loaded map */
static void
octnic_dma_callback(void *arg, struct bus_dma_segment *segs, int nsegs, int error)
{
	struct {
		bus_addr_t *addr;
		int *err;
	} *cbarg = arg;

	if (error == 0 && nsegs > 0)
		*cbarg->addr = segs[0].ds_addr;
	if (cbarg->err)
		*cbarg->err = error;
}

static int
octnic_dma_alloc(struct octnic_port *port)
{
	struct octnic_softc *sc = port->op_sc;
	uint8_t *va;
	bus_addr_t bus_addr;
	int error, i;

	/* Create DMA tag for RX pool — linuxmod line 130 */
	error = bus_dma_tag_create(bus_get_dma_tag(sc->sc_dev),
	    1, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
	    NULL, NULL, POOL_SZ, 1, POOL_SZ, 0, NULL, NULL,
	    &port->op_dmat);
	if (error) {
		device_printf(sc->sc_dev, "Failed to create RX DMA tag: %d\n", error);
		return (error);
	}

	/* Allocate coherent memory — linuxmod line 130: dma_alloc_coherent */
	error = bus_dmamem_alloc(port->op_dmat, (void **)&va,
	    BUS_DMA_ZERO | BUS_DMA_WAITOK, NULL);
	if (error) {
		device_printf(sc->sc_dev, "Failed to alloc RX DMA memory: %d\n", error);
		bus_dma_tag_destroy(port->op_dmat);
		return (error);
	}

	/* Create DMA map */
	error = bus_dmamap_create(port->op_dmat, 0, &port->op_dmamap);
	if (error) {
		device_printf(sc->sc_dev, "Failed to create RX DMA map: %d\n", error);
		bus_dmamem_free(port->op_dmat, va, NULL);
		bus_dma_tag_destroy(port->op_dmat);
		return (error);
	}

	/* Load DMA map to get bus address via callback */
	bus_addr = 0;
	error = bus_dmamap_load(port->op_dmat, port->op_dmamap, va, POOL_SZ,
	    octnic_dma_callback, &(struct { bus_addr_t *addr; int *err; }){&bus_addr, &error},
	    BUS_DMA_NOWAIT);
	if (error) {
		device_printf(sc->sc_dev, "Failed to load RX DMA map: %d\n", error);
		bus_dmamem_free(port->op_dmat, va, port->op_dmamap);
		bus_dma_tag_destroy(port->op_dmat);
		return (error);
	}

	port->op_rx_dma_va = va;
	port->op_rx_dma_bus = bus_addr;

	/* Seed phase bits to "not ready" (linuxmod lines 134-141) */
	for (i = 0; i < OCTNIC_RING_SIZE; i++) {
		uint32_t *hdr = (uint32_t *)(va + i * OCTNIC_BUF_SZ + 4);
		*hdr = htole32(OCTNIC_RX_PHASE(i) ^ 1u);
	}

	/* Publish DMA bases to card — linuxmod lines 142-146 */
	pwr64(port, C_RX_DMA_BASE, port->op_rx_dma_bus);
	bus_space_barrier(sc->sc_bst, sc->sc_bsh,
	    port->op_port_id * OCTNIC_PORT_STRIDE + C_RX_DMA_BASE, 8,
	    BUS_SPACE_BARRIER_WRITE);
	pwr(port, C_DMA_ENABLE, 1);
	bus_space_barrier(sc->sc_bst, sc->sc_bsh,
	    port->op_port_id * OCTNIC_PORT_STRIDE + C_DMA_ENABLE, 4,
	    BUS_SPACE_BARRIER_WRITE);

	device_printf(sc->sc_dev, "port%d DMA on, rx_bus=0x%jx\n",
	    port->op_port_id, (uintmax_t)port->op_rx_dma_bus);

	return (0);
}

static void
octnic_dma_free(struct octnic_port *port)
{
	struct octnic_softc *sc;

	if (!port || !port->op_sc)
		return;

	sc = port->op_sc;

	if (!port->op_rx_dma_va || !port->op_dmamap)
		return;

	pwr(port, C_DMA_ENABLE, 0);
	bus_space_barrier(sc->sc_bst, sc->sc_bsh,
	    port->op_port_id * OCTNIC_PORT_STRIDE + C_DMA_ENABLE, 4,
	    BUS_SPACE_BARRIER_WRITE);
	DELAY(100000); /* 100ms — let card disarm + in-flight DMA drain */

	bus_dmamap_unload(port->op_dmat, port->op_dmamap);
	bus_dmamem_free(port->op_dmat, port->op_rx_dma_va, port->op_dmamap);
	port->op_rx_dma_va = NULL;
	port->op_dmamap = NULL;

	if (port->op_dmat) {
		bus_dma_tag_destroy(port->op_dmat);
		port->op_dmat = NULL;
	}
}

/* ----------------------------------------------------------------
 * TX — oct_xmit equivalent (linuxmod lines 163-202)
 * ---------------------------------------------------------------- */
static int
octnic_transmit(if_t ifp, struct mbuf *m0)
{
	struct octnic_port *port = if_getsoftc(ifp);
	struct octnic_softc *sc = port->op_sc;
	uint32_t tp, slot, len;

	if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0) {
		m_freem(m0);
		return (ENETDOWN);
	}

	/* CAS-claim a slot — parallel across txqs (linuxmod lines 170-184) */
	for (;;) {
		tp = (uint32_t)atomic_fetchadd_int(&port->op_tx_claim, 1);
		if ((tp - port->op_tc_cache) >= OCTNIC_RING_SIZE) {
			uint32_t tc = prd(port, C_TX_CONS);
			port->op_tc_cache = tc;
			if ((tp - tc) >= OCTNIC_RING_SIZE) {
				m_freem(m0);
				return (ENOBUFS);
			}
		}

		slot = tp & (OCTNIC_RING_SIZE - 1);

		/* Phase check: if descriptor phase matches our phase, slot is free */
		uint32_t desc_phase = prd(port, OCTNIC_TXDESC_OFF + slot * 8 + 4) & 1u;
		if ((OCTNIC_TX_PHASE(tp) == desc_phase))
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
		    mtod(m0, uint8_t *), len);
	} else {
		/* PIO: host posted-write into BAR2 TX buffer (linuxmod line 192) */
		bus_space_write_region_1(sc->sc_bst, sc->sc_bsh,
		    port->op_port_id * OCTNIC_PORT_STRIDE +
		    OCTNIC_TXBUF_OFF + slot * OCTNIC_BUF_SZ,
		    mtod(m0, uint8_t *), len);
	}

	bus_space_barrier(sc->sc_bst, sc->sc_bsh,
	    port->op_port_id * OCTNIC_PORT_STRIDE + OCTNIC_TXBUF_OFF +
	    slot * OCTNIC_BUF_SZ, len, BUS_SPACE_BARRIER_WRITE);
	pwr(port, OCTNIC_TXDESC_OFF + slot * 8, len); /* desc len */
	bus_space_barrier(sc->sc_bst, sc->sc_bsh,
	    port->op_port_id * OCTNIC_PORT_STRIDE + OCTNIC_TXDESC_OFF + slot * 8,
	    4, BUS_SPACE_BARRIER_WRITE);
	pwr(port, OCTNIC_TXDESC_OFF + slot * 8 + 4,
	    OCTNIC_TX_PHASE(tp)); /* phase LAST = slot ready to xmit */
	bus_space_barrier(sc->sc_bst, sc->sc_bsh,
	    port->op_port_id * OCTNIC_PORT_STRIDE + OCTNIC_TXDESC_OFF + slot * 8 + 4,
	    4, BUS_SPACE_BARRIER_WRITE);

	m_freem(m0);
	if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
	if_inc_counter(ifp, IFCOUNTER_OBYTES, len);
	return (0);
}

/* ----------------------------------------------------------------
 * RX drain — oct_drain equivalent (linuxmod lines 206-284)
 * ---------------------------------------------------------------- */
static void
octnic_drain(void *arg, int pending)
{
	struct octnic_port *port = arg;
	struct octnic_softc *sc = port->op_sc;
	struct ifnet *ifp = port->op_ifp;
	uint32_t stride = (oct_rxthreads > 1) ? oct_rxthreads : 1;
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

			if (phase != OCTNIC_RX_PHASE(rc))
				break; /* not filled yet */
			atomic_thread_fence_acq();
			len = le32toh(*(uint32_t *)h);
		} else if (oct_lockfree) {
			/* One 64-bit desc read = {len, flags} */
			uint64_t desc = prd64(port, OCTNIC_RXDESC_OFF + slot * 8);

			if (((uint32_t)(desc >> 32) & 1u) != OCTNIC_RX_PHASE(rc))
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
				rc += stride;
				n++;
				continue;
			}

			if (oct_dma && port->op_rx_dma_va != NULL) {
				/* Card DMA-wrote into local coherent RAM */
				memcpy(mtod(m, caddr_t),
				    (uint8_t *)port->op_rx_dma_va +
				    slot * OCTNIC_BUF_SZ +
				    (oct_hrx ? OCTNIC_HRX_HDR : 0), len);
			} else {
				/* PIO path: read from BAR2 RX buffer */
				bus_space_read_region_1(sc->sc_bst, sc->sc_bsh,
				    port->op_port_id * OCTNIC_PORT_STRIDE +
				    OCTNIC_RXBUF_OFF + slot * OCTNIC_BUF_SZ,
				    mtod(m, uint8_t *), len);
			}

			m->m_len = m->m_pkthdr.len = len;
			m->m_pkthdr.rcvif = ifp;
			if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
			if_inc_counter(ifp, IFCOUNTER_IBYTES, len);

			if_input(ifp, m);
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
	struct octnic_softc *sc = port->op_sc;
	int i;

	if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING))
		return;

	port->op_tx_prod = prd(port, C_TX_PROD); /* linuxmod line 320 */
	port->op_tc_cache = prd(port, C_TX_CONS);
	atomic_store_rel_int(&port->op_tx_claim, 0);
	port->op_tc_cache = 0;

	/* Snap RX consumer to card's current producer (linuxmod lines 326-329) */
	port->op_rx_cons = oct_hrx ? 0 : prd(port, C_RX_PROD);
	pwr(port, C_RX_CONS, port->op_rx_cons);
	pwr(port, C_HOST_READY, 1); /* linuxmod line 330 */

	if_setdrvflagbits(ifp, IFF_DRV_RUNNING, 0);

	/* Validate rxthreads (linuxmod lines 334-341) */
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

	device_printf(sc->sc_dev, "port%d: initialized\n", port->op_port_id);
}

static void
octnic_stop(struct octnic_port *port)
{
	struct ifnet *ifp = port->op_ifp;
	struct octnic_softc *sc = port->op_sc;

	if_setdrvflagbits(ifp, 0, IFF_DRV_RUNNING);
	pwr(port, C_HOST_READY, 0); /* linuxmod line 373 */

	device_printf(sc->sc_dev, "port%d: stopped\n", port->op_port_id);
}

static int
octnic_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
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
		if (ifr->ifr_mtu > 65535 || ifr->ifr_mtu < ETHERMIN)
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

/* ----------------------------------------------------------------
 * Watchdog / heartbeat — card health monitoring
 * linuxmod lines 469-583: wd_fn
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

	/* Check magic (linuxmod line 541) */
	if (prd(PORT(sc, 0), C_MAGIC) != OCTNIC_MAGIC) {
		sc->sc_card_lost = 1;
		for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++)
			if_link_state_change(PORT(sc, i)->op_ifp, LINK_STATE_DOWN);
		device_printf(sc->sc_dev, "CAVIUM CARD LOST: magic gone\n");
		return;
	}

	/* Check heartbeat (linuxmod lines 543-551) */
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

	/* Check temperatures (linuxmod lines 564-579) */
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

	/* Carrier follows SFP+ link (linuxmod lines 496-527) */
	for (i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++) {
		struct octnic_port *port = PORT(sc, i);
		uint32_t link = prd(port, C_LINK);
		int carrier = (link & 0x80000000u) ? (link & 1) : 1;

		/* ponytail: set state every poll, if_link_state_change on same state is a no-op; cache it if spam matters */
		if_link_state_change(port->op_ifp, carrier ? LINK_STATE_UP : LINK_STATE_DOWN);
	}
}

static void
octnic_timer_tick(void *arg)
{
	struct octnic_softc *sc = arg;

	taskqueue_enqueue(taskqueue_thread, &sc->sc_watchdog_task);
	for (int i = 0; i < oct_ports && i < OCTNIC_MAX_PORTS; i++)
		taskqueue_enqueue(taskqueue_thread, &PORT(sc, i)->op_rx_task);
	callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);
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

	/* Enable bus mastering (linuxmod lines 662-664) */
	pci_enable_busmaster(dev);

	/* Allocate BAR2 resource (linuxmod line 661) */
	sc->sc_bar2_rid = PCIR_BAR(OCTNIC_BAR);
	sc->sc_bar2_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &sc->sc_bar2_rid, RF_ACTIVE);
	if (sc->sc_bar2_res == NULL) {
		device_printf(dev, "Failed to allocate BAR2\n");
		return (ENXIO);
	}

	sc->sc_bst = rman_get_bustag(sc->sc_bar2_res);
	sc->sc_bsh = rman_get_bushandle(sc->sc_bar2_res);

	/* Verify magic on port 0 (linuxmod lines 679-686) */
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

	/* Create parent DMA tag */
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
	    CTLFLAG_RW, 0, "octnic driver");

	mtx_init(&sc->sc_lock, "octnic", NULL, MTX_DEF);
	callout_init_mtx(&sc->sc_timer, &sc->sc_lock, 0);
	TASK_INIT(&sc->sc_watchdog_task, 0, octnic_watchdog, sc);

	/* Initialize ports (linuxmod lines 693-735) */
	for (i = 0; i < oct_ports; i++) {
		port = PORT(sc, i);
		port->op_sc = sc;
		port->op_port_id = i;
		TASK_INIT(&port->op_rx_task, 0, octnic_drain, port);

		/* MAC: 02:c4:xx:xx:xx:xx based on device BDF hash (linuxmod lines 715-721) */
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
		if_setinitfn(port->op_ifp, octnic_init);
		if_setioctlfn(port->op_ifp, octnic_ioctl);
		if_settransmitfn(port->op_ifp, octnic_transmit);
		if_setmtu(port->op_ifp, 1500);

		ether_ifattach(port->op_ifp, port->op_macaddr);
		if_link_state_change(port->op_ifp, LINK_STATE_DOWN);

		device_printf(dev, "registered %s\n", if_name(port->op_ifp));
	}

	/* Start watchdog (linuxmod lines 747-750) */
	sc->sc_heartbeat_last = prd(PORT(sc, 0), C_HEARTBEAT);
	sc->sc_hb_time = time_uptime;
	mtx_lock(&sc->sc_lock);
	callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);
	mtx_unlock(&sc->sc_lock);

	device_printf(dev, "octnic driver attached (%s)\n",
	    "hardware");

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
