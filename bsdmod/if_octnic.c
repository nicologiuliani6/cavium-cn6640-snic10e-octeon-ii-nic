/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Cavium CN6640 SNIC10E FreeBSD Driver Project
 */

#include "if_octnic.h"

static int octnic_probe(device_t dev);
static int octnic_attach(device_t dev);
static int octnic_detach(device_t dev);

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
	if (pci_get_vendor(dev) == OCTNIC_VENDOR_ID &&
	    pci_get_device(dev) == OCTNIC_DEVICE_ID) {
		device_set_desc(dev, "Cavium CN6640 SNIC10E 10GbE NIC");
		return (BUS_PROBE_DEFAULT);
	}
	return (ENXIO);
}

static int
octnic_attach(device_t dev)
{
	struct octnic_softc *sc;
	int error = 0;

	sc = device_get_softc(dev);
	sc->sc_dev = dev;

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

	device_printf(dev, "Successfully attached Cavium SNIC10E BAR2\n");
	return (0);
}

static int
octnic_detach(device_t dev)
{
	struct octnic_softc *sc;

	sc = device_get_softc(dev);

	if (sc->sc_bar2_res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_bar2_rid,
		    sc->sc_bar2_res);
	}

	device_printf(dev, "Detached Cavium SNIC10E driver\n");
	return (0);
}
