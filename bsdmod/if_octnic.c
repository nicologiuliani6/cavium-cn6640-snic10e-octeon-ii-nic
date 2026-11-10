/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Cavium CN6640 SNIC10E FreeBSD Driver Project
 */

#include "if_octnic.h"

/* IVSHMEM vendor/device ID for emulation testing */
#define IVSHMEM_VENDOR_ID	0x1af4
#define IVSHMEM_DEVICE_ID	0x1110

static int octnic_probe(device_t dev);
static int octnic_attach(device_t dev);
static int octnic_detach(device_t dev);

static void octnic_init(void *arg);
static int octnic_ioctl(if_t ifp, u_long cmd, caddr_t data);
static int octnic_transmit(if_t ifp, struct mbuf *m);
static void octnic_qflush(if_t ifp);
static void octnic_watchdog(void *arg, int pending);
static void octnic_timer_tick(void *arg);

static device_method_t octnic_methods[] = {
	DEVMETHOD(device_probe,		octnic_probe),
	DEVMETHOD(device_attach,	octnic_attach),
	DEVMETHOD(device_detach,	octnic_detach),
	{ 0, 0 }
};

static driver_t octnic_driver = {
	"octnic",
	octnic_methods,
	sizeof(struct octnic_softc)
};

DRIVER_MODULE(if_octnic, pci, octnic_driver, 0, 0);
MODULE_DEPEND(if_octnic, pci, 1, 1, 1);
MODULE_DEPEND(if_octnic, ether, 1, 1, 1);

static int
octnic_probe(device_t dev)
{
	uint16_t v = pci_get_vendor(dev);
	uint16_t d = pci_get_device(dev);

	if (v == OCTNIC_VENDOR_ID && d == OCTNIC_DEVICE_ID) {
		device_set_desc(dev, "Cavium CN6640 SNIC10E 10GbE NIC");
		return (BUS_PROBE_DEFAULT);
	}

	/* Test mode support for QEMU ivshmem device */
	if (v == IVSHMEM_VENDOR_ID && d == IVSHMEM_DEVICE_ID) {
		device_set_desc(dev, "Cavium CN6640 SNIC10E Emulation (ivshmem)");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static void
octnic_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *baddr = arg;
	if (error == 0 && nseg > 0)
		*baddr = segs[0].ds_addr;
}

static int
octnic_attach(device_t dev)
{
	struct octnic_softc *sc;
	struct octnic_port *port;
	if_t ifp;
	int i, error;
	uint16_t v = pci_get_vendor(dev);
	uint16_t d = pci_get_device(dev);

	sc = device_get_softc(dev);
	sc->sc_dev = dev;
	sc->sc_temp_warn = 75;
	sc->sc_temp_crit = 90;

	if (v == IVSHMEM_VENDOR_ID && d == IVSHMEM_DEVICE_ID)
		sc->sc_test_mode = 1;

	pci_enable_busmaster(dev);

	sc->sc_bar2_rid = PCIR_BAR(OCTNIC_BAR);
	sc->sc_bar2_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &sc->sc_bar2_rid, RF_ACTIVE);
	if (sc->sc_bar2_res == NULL) {
		device_printf(dev, "Failed to allocate BAR2 memory resource\n");
		return (ENXIO);
	}

	sc->sc_bst = rman_get_bustag(sc->sc_bar2_res);
	sc->sc_bsh = rman_get_bushandle(sc->sc_bar2_res);

	error = bus_dma_tag_create(bus_get_dma_tag(dev),
	    1, 0, BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR,
	    NULL, NULL, BUS_SPACE_MAXSIZE_32BIT, 0,
	    BUS_SPACE_MAXSIZE_32BIT, 0, NULL, NULL, &sc->sc_parent_dmat);
	if (error) {
		device_printf(dev, "Failed to create parent DMA tag\n");
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid, sc->sc_bar2_res);
		return (error);
	}

	/* Sysctl entries for status and temperature monitoring */
	sysctl_ctx_init(&sc->sc_sysctl_ctx);
	sc->sc_sysctl_tree = SYSCTL_ADD_NODE(&sc->sc_sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_dev), OID_AUTO, "octnic",
	    CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "octnic driver parameters");
	if (sc->sc_sysctl_tree != NULL) {
		SYSCTL_ADD_INT(&sc->sc_sysctl_ctx, SYSCTL_CHILDREN(sc->sc_sysctl_tree),
		    OID_AUTO, "temp_warn", CTLFLAG_RW, &sc->sc_temp_warn, 0, "Temperature warning threshold (C)");
		SYSCTL_ADD_INT(&sc->sc_sysctl_ctx, SYSCTL_CHILDREN(sc->sc_sysctl_tree),
		    OID_AUTO, "temp_crit", CTLFLAG_RW, &sc->sc_temp_crit, 0, "Temperature critical threshold (C)");
	}

	/* Initialize ports */
	for (i = 0; i < OCTNIC_MAX_PORTS; i++) {
		port = &sc->sc_ports[i];
		port->op_sc = sc;
		port->op_port_id = i;

		/* Default MAC address: 02:00:00:00:00:0X */
		port->op_macaddr[0] = 0x02;
		port->op_macaddr[1] = 0x00;
		port->op_macaddr[2] = 0x00;
		port->op_macaddr[3] = 0x00;
		port->op_macaddr[4] = 0x00;
		port->op_macaddr[5] = i;

		ifp = if_alloc(IFT_ETHER);
		if (ifp == NULL) {
			device_printf(dev, "Failed to allocate ifnet for port %d\n", i);
			return (ENOMEM);
		}

		port->op_ifp = ifp;
		if_initname(ifp, "oct", i);
		if_setsoftc(ifp, port);
		if_setflags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
		if_setinitfn(ifp, octnic_init);
		if_setioctlfn(ifp, octnic_ioctl);
		if_settransmitfn(ifp, octnic_transmit);
		if_setqflushfn(ifp, octnic_qflush);

		ether_ifattach(ifp, port->op_macaddr);
		if_setlinkstate(ifp, LINK_STATE_UP);
	}

	callout_init(&sc->sc_timer, 1);
	TASK_INIT(&sc->sc_watchdog_task, 0, octnic_watchdog, sc);
	callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);

	device_printf(dev, "Successfully attached octnic driver (%s)\n",
	    sc->sc_test_mode ? "emulation mode" : "hardware mode");

	return (0);
}

static int
octnic_detach(device_t dev)
{
	struct octnic_softc *sc = device_get_softc(dev);
	struct octnic_port *port;
	int i;

	callout_drain(&sc->sc_timer);
	taskqueue_drain(taskqueue_thread, &sc->sc_watchdog_task);

	for (i = 0; i < OCTNIC_MAX_PORTS; i++) {
		port = &sc->sc_ports[i];
		if (port->op_ifp != NULL) {
			ether_ifdetach(port->op_ifp);
			if_free(port->op_ifp);
			port->op_ifp = NULL;
		}
	}

	if (sc->sc_sysctl_tree != NULL)
		sysctl_ctx_free(&sc->sc_sysctl_ctx);

	if (sc->sc_parent_dmat != NULL)
		bus_dma_tag_destroy(sc->sc_parent_dmat);

	if (sc->sc_bar2_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid, sc->sc_bar2_res);

	device_printf(dev, "Detached octnic driver\n");
	return (0);
}

static void
octnic_init(void *arg)
{
	struct octnic_port *port = arg;
	if_t ifp = port->op_ifp;

	if_setdrvflagbits(ifp, IFF_DRV_RUNNING, 0);
	if_setlinkstate(ifp, LINK_STATE_UP);
	port->op_running = 1;
}

static int
octnic_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct octnic_port *port = if_getsoftc(ifp);
	struct ifreq *ifr = (struct ifreq *)data;
	int error = 0;

	switch (cmd) {
	case SIOCSIFFLAGS:
		if (if_getflags(ifp) & IFF_UP) {
			if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0)
				octnic_init(port);
		} else {
			if (if_getdrvflags(ifp) & IFF_DRV_RUNNING) {
				if_setdrvflagbits(ifp, 0, IFF_DRV_RUNNING);
				port->op_running = 0;
			}
		}
		break;
	case SIOCSIFMTU:
		if (ifr->ifr_mtu > ETHERMTU || ifr->ifr_mtu < ETHERMIN)
			error = EINVAL;
		else
			if_setmtu(ifp, ifr->ifr_mtu);
		break;
	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}

	return (error);
}

static int
octnic_transmit(if_t ifp, struct mbuf *m)
{
	struct octnic_port *port = if_getsoftc(ifp);

	if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0) {
		m_freem(m);
		return (ENETDOWN);
	}

	/* Simple loopback / transmission simulation logic */
	if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
	if_inc_counter(ifp, IFCOUNTER_OBYTES, m->m_pkthdr.len);

	m_freem(m);
	return (0);
}

static void
octnic_qflush(if_t ifp)
{
}

static void
octnic_timer_tick(void *arg)
{
	struct octnic_softc *sc = arg;

	taskqueue_enqueue(taskqueue_thread, &sc->sc_watchdog_task);
	callout_reset(&sc->sc_timer, hz, octnic_timer_tick, sc);
}

static void
octnic_watchdog(void *arg, int pending)
{
	struct octnic_softc *sc = arg;

	/* Heartbeat check and temp warning logging (mocked for initial attach) */
	if (sc->sc_bar2_res != NULL) {
		uint32_t magic = bus_space_read_4(sc->sc_bst, sc->sc_bsh, OCTNIC_REG_MAGIC);
		(void)magic;
	}
}
