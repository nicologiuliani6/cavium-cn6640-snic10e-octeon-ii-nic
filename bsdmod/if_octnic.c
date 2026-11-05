/* SPDX-License-Identifier: BSD-2-Clause
 * if_octnic.c — FreeBSD driver for Cavium CN6640 SNIC10E (mirrors linuxmod/octnic.c)
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <machine/bus.h>
#include <sys/rman.h>
#include <machine/atomic.h>
#include <sys/taskqueue.h>

#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_types.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include "if_octnic.h"
#define OCT_RID_BAR2 0x12
static struct sysctl_ctx_list octnic_sysctl_ctx;
static struct sysctl_oid *octnic_sysctl_tree;

/* Module params (mirror Linux module_param) */
int oct_nports=OCT_DEFAULT_PORTS, oct_rxthreads=OCT_DEFAULT_RXTHREADS;
int oct_poll_us=OCT_POLL_US, oct_ntxq=OCT_NTXQ, oct_dma=0;
int oct_hrx=1, oct_lockfree=1, oct_ztx=0;
int oct_temp_warn_mc=OCT_TEMP_WARN_MC, oct_temp_crit_mc=OCT_TEMP_CRIT_MC;

/* Per-port state (declared in header) */
struct oct_port oct_ports[OCT_MAX_PORTS];

static inline u_int32_t oct_r4(struct oct_port *p, u_int off)
{ return bus_space_read_4(p->bt, p->bsh, off); }
static inline void oct_w4(struct oct_port *p, u_int off, u_int32_t v)
{ bus_space_write_4(p->bt, p->bsh, off, v); }

/* ---- attach: BAR2 alloc/map, ifnet creation ---- */
int octnic_attach(device_t dev)
{
    struct resource *res; bus_space_tag_t bt; bus_space_handle_t bsh; int i;
    device_set_desc(dev, "Cavium CN6640 SNIC10E");
    res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, OCT_RID_BAR2, RF_ACTIVE);
    if (!res) { device_printf(dev, "BAR2 alloc fail\n"); return ENXIO; }
    bt = res->r_bustag;
    if (bus_space_map(bt, rman_get_start(res), rman_get_size(res), 0, &bsh)) {
        bus_release_resource(dev, SYS_RES_MEMORY, OCT_RID_BAR2, res); return ENOMEM; }

    /* TODO: verify OCTSHM_MAGIC at C_MAGIC before continuing */
    device_printf(dev, "BAR2 mapped 0x%lx/%lu\n", rman_get_start(res), rman_get_size(res));

    for (i = 0; i < oct_nports && i < OCT_MAX_PORTS; i++) {
        struct oct_port *p = &oct_ports[i]; struct ifnet *ifp;
        p->idx = i; p->bt = bt; p->bsh = bsh + (i * OCT_PORT_STRIDE);
        p->tx_claim = 0;
        ifp = if_alloc(IFT_ETHER); if (!ifp) continue;
        p->ifp = ifp; ifp->if_softc = p; ifp->if_mtu = 1500;
        if_initname(ifp, "oct", i);
        ifp->if_start = octnic_start; ifp->if_ioctl = octnic_ioctl;
        ifp->if_qflush = octnic_qflush; ifp->if_flags = IFF_SIMPLEX|IFF_MULTICAST;
        /* TODO: set MAC (Linux: crc32(devfn) -> {0x02,0xc4,h>>24,...}) */
        ether_ifattach(ifp, NULL);
        device_printf(dev, "registered %s\n", ifp->if_xname);
    }
    octnic_sysctl_init(); return 0;
}

/* ---- detach: tear down ---- */
int octnic_detach(device_t dev)
{
    struct resource *res; int i; struct oct_port *p0 = &oct_ports[0];
    for (i = 0; i < oct_nports && i < OCT_MAX_PORTS; i++) {
        struct oct_port *p = &oct_ports[i];
        if (p->ifp) { ether_ifdetach(p->ifp); if_free(p->ifp); p->ifp = NULL; }
    }
    res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, OCT_RID_BAR2, RF_ACTIVE);
    if (res) {
        bus_space_unmap(p0->bt, p0->bsh, rman_get_size(res));
        bus_release_resource(dev, SYS_RES_MEMORY, OCT_RID_BAR2, res);
    }
    return 0;
}

/* ---- init: called when ifp goes up ---- */
static void octnic_init(struct ifnet *ifp)
{
    struct oct_port *p = ifp->if_softc;
    /* TODO: sync ring counters from card, start poll threads */
    p->tp_shadow = oct_r4(p, C_TX_PROD); p->tc_cache = oct_r4(p, C_TX_CONS);
    p->rc_shadow = oct_r4(p, C_RX_PROD);
    oct_w4(p, C_RX_CONS, p->rc_shadow); oct_w4(p, C_HOST_READY, 1);
    ifp->if_flags |= IFF_DRV_RUNNING; ifp->if_flags &= ~IFF_DRV_OACTIVE;
}

/* ---- ioctl ---- */
int octnic_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
    struct ifreq *ifr = (struct ifreq *)data; int error = 0;
    switch (cmd) {
    case SIOCSIFFLAGS:
        if (ifr->ifr_flags & IFF_UP) {
            if (!(ifp->if_flags & IFF_DRV_RUNNING)) octnic_init(ifp);
        } else {
            if (ifp->if_flags & IFF_DRV_RUNNING) {
                oct_w4(ifp->if_softc, C_HOST_READY, 0);
                ifp->if_flags &= ~IFF_DRV_RUNNING;
            }
        } break;
    case SIOCSIFMTU:
        if (ifr->ifr_mtu < 68 || ifr->ifr_mtu > 9216) error = EINVAL;
        else ifp->if_mtu = ifr->ifr_mtu; break;
    default: error = ether_ioctl(ifp, cmd, data); break;
    }
    return error;
}

/* ---- start: transmit (mirrors Linux oct_xmit CAS+PIO) ---- */
void octnic_start(struct ifnet *ifp)
{
    struct oct_port *p = ifp->if_softc; struct mbuf *m; u_int slot, len, tp;
    IFQ_DEQUEUE(&ifp->if_snd, m);
    while (m) {
        tp = atomic_fetchadd_int(&p->tx_claim, 1);
        if ((tp - p->tc_cache) >= OCT_RING_SZ) {
            IFQ_PREPEND(&ifp->if_snd, m); ifp->if_flags |= IFF_DRV_OACTIVE; return; }
        len = m->m_pkthdr.len; if (len > OCT_BUF_SZ) len = OCT_BUF_SZ;
        slot = tp & OCT_RING_MASK;
        /* TODO: memcpy data to BAR2 TXBUF_OFF+slot*BUF_SZ (PIO) or DMA pool */
        oct_w4(p, OCT_TXDESC_OFF + slot*8, len);
        oct_w4(p, OCT_TXDESC_OFF + slot*8+4, OCT_TX_PHASE(tp));
        if_inc_counter(ifp, IFCOUNTER_OERRORS, 1); m_freem(m);
        IFQ_DEQUEUE(&ifp->if_snd, m);
    }
    ifp->if_flags &= ~IFF_DRV_OACTIVE;
}

/* ---- qflush: drain tx queue ---- */
void octnic_qflush(struct ifnet *ifp)
{ struct mbuf *m; while ((m = IFQ_DEQUEUE(&ifp->if_snd)) != NULL) m_freem(m); }

/* ---- sysctl: params + TODO temp reading ---- */
void octnic_sysctl_init(void)
{
    struct sysctl_oid *child;
    sysctl_ctx_init(&octnic_sysctl_ctx);
    octnic_sysctl_tree = SYSCTL_ADD_NODE(&octnic_sysctl_ctx,
        SYSCTL_CHILDREN(_kernel), OID_AUTO, "octnic", CTLFLAG_RW, 0, "octnic driver");
    child = SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "temp_warn_mc", CTLFLAG_RW, &oct_temp_warn_mc, 0, "warn threshold (mC)");
    SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "temp_crit_mc", CTLFLAG_RW, &oct_temp_crit_mc, 0, "critical threshold (mC)");
    SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "ports", CTLFLAG_RW, &oct_nports, 0, "port count");
    SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "rxthreads", CTLFLAG_RW, &oct_rxthreads, 0, "RX threads");
    SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "poll_us", CTLFLAG_RW, &oct_poll_us, 0, "poll us");
    SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "ntxq", CTLFLAG_RW, &oct_ntxq, 0, "TX queues");
    SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "dma", CTLFLAG_RW, &oct_dma, 0, "DMA mode");
    SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "hrx", CTLFLAG_RW, &oct_hrx, 0, "local desc");
    SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "lockfree", CTLFLAG_RW, &oct_lockfree, 0, "lockfree RX");
    SYSCTL_ADD_INT(&octnic_sysctl_ctx, SYSCTL_CHILDREN(octnic_sysctl_tree),
        OID_AUTO, "ztx", CTLFLAG_RW, &oct_ztx, 0, "zero-copy TX");
    /* TODO: add temp1/temp2 sysctls reading C_RESV0/C_RESV1 */
}

/* ---- module glue ---- */
static device_method_t octnic_methods[] = {
    DEVMETHOD(device_attach, octnic_attach),
    DEVMETHOD(device_detach, octnic_detach), DEVMETHOD_END
};
static driver_t octnic_driver = { "octnic", octnic_methods, sizeof(struct oct_port) };
static devclass_t octnic_devclass;
DRIVER_MODULE(octnic, pci, octnic_driver, 0, 0);
MODULE_VERSION(octnic, 1);
