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
#include <sys/taskqueue.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_types.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "octnic_regs.h"

struct octnic_softc;

/* Virtio-style Ring Descriptor (16 bytes) matching octnic.c */
struct octnic_desc {
	uint64_t	addr;	/* Host physical address or BAR2 offset */
	uint32_t	len;	/* Buffer length / flags */
	uint32_t	flags;	/* Phase bit, wrap, status */
};

struct octnic_tx_ring {
	struct octnic_desc	*desc;
	uint32_t		prod;
	uint32_t		cons;
	uint8_t			phase;
	bus_dma_tag_t		dmat;
	bus_dmamap_t		maps[OCTNIC_RING_SIZE];
	struct mbuf		*bufs[OCTNIC_RING_SIZE];
};

struct octnic_rx_ring {
	struct octnic_desc	*desc;
	uint32_t		prod;
	uint32_t		cons;
	uint8_t			phase;
	bus_dma_tag_t		dmat;
	bus_dmamap_t		maps[OCTNIC_RING_SIZE];
	struct mbuf		*bufs[OCTNIC_RING_SIZE];
};

struct octnic_port {
	struct octnic_softc	*op_sc;
	int			op_port_id;
	if_t			op_ifp;
	struct octnic_tx_ring	op_tx;
	struct octnic_rx_ring	op_rx;
	uint8_t			op_macaddr[6];
	int			op_running;
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
	int			sc_heartbeat_last;
	struct callout		sc_timer;
	struct task		sc_watchdog_task;
	int			sc_test_mode;	/* 1 for ivshmem emulation mode */
};

#endif /* _IF_OCTNIC_H_ */
