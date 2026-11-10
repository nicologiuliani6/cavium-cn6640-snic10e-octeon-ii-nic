/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Cavium CN6640 SNIC10E FreeBSD Driver Project
 */

#ifndef _IF_OCTNIC_H_
#define _IF_OCTNIC_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <machine/bus.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_types.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "octnic_regs.h"

struct octnic_softc;

struct octnic_port {
	struct octnic_softc	*op_sc;
	int			op_port_id;
	struct ifnet		*op_ifp;
	int			op_detached;
};

struct octnic_softc {
	device_t		sc_dev;
	struct resource		*sc_bar2_res;
	int			sc_bar2_rid;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	bus_dma_tag_t		sc_parent_dmat;
	struct octnic_port	sc_ports[OCTNIC_MAX_PORTS];
	struct sysctl_ctx_list	sc_sysctl_ctx;
	struct sysctl_oid	*sc_sysctl_tree;
	int			sc_temp_warn;
	int			sc_temp_crit;
};

#endif /* _IF_OCTNIC_H_ */
