/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
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
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/taskqueue.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/gsb_crc32.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_types.h>


#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "octnic_regs.h"

/* ----------------------------------------------------------------
 * Per-port software state — mirrors linuxmod/octnic.c host_port
 * ---------------------------------------------------------------- */
struct octnic_port {
	struct octnic_softc	*op_sc;
	int			 op_port_id;
	struct ifnet		*op_ifp;

	/* Shadow counters (host owns, kept in RAM) */
	uint32_t		 op_tx_prod;	/* TX producer shadow */
	uint32_t		 op_rx_cons;	/* RX consumer shadow */

	/* Parallel TX CAS counter — FreeBSD uses atomic_fetchadd_int */
	int			 op_tx_claim;

	/* Last observed card TX consumer (cache) */
	uint32_t		 op_tc_cache;

	/* Multi-thread RX consumer per thread */
	uint32_t		 op_rc_pub[8];

	/* MAC address */
	uint8_t			 op_macaddr[6];

	/* Running flag */
	int			 op_running;

	/* DMA pools (hrx mode) */
	uint8_t			*op_rx_dma_va;
	bus_addr_t		 op_rx_dma_bus;
	bus_dmamap_t		 op_rx_dmamap;
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

	/* Test mode (ivshmem) */
	int			 sc_test_mode;

	/* Lock for port state */
	struct mtx		 sc_lock;
	struct callout		sc_timer;
	struct task		 sc_watchdog_task;
};

/* ----------------------------------------------------------------
 * Function prototypes
 * ---------------------------------------------------------------- */
static int octnic_probe(device_t dev);
static int octnic_attach(device_t dev);
static int octnic_detach(device_t dev);

static void octnic_init(void *arg);
static void octnic_stop(struct octnic_port *port);
static int  octnic_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data);
static void octnic_start(struct ifnet *ifp, struct mbuf *m0);
static void octnic_qflush(struct ifnet *ifp);

/* Per-port RX drain */
static void octnic_drain(void *arg, int pending);

/* Watchdog */
static void octnic_watchdog(void *arg, int pending);
static void octnic_timer_tick(void *arg);

/* DMA helpers */
static int  octnic_dma_alloc(struct octnic_port *port);
static void octnic_dma_free(struct octnic_port *port);

#endif /* _IF_OCTNIC_H_ */
