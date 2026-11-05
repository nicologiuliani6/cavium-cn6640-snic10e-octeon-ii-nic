/* SPDX-License-Identifier: BSD-2-Clause
 * if_octnic.h — FreeBSD driver for Cavium CN6640 SNIC10E shared-memory NIC.
 * Mirrors linuxmod/octnic.c register layout and protocol exactly.
 */

#ifndef _IF_OCTNIC_H
#define _IF_OCTNIC_H

/* ---- PCI identifiers ---- */
#define OCT_PCI_VENDOR_CAVIUM   0x177d
#define OCT_PCI_DEVICE_CN6640   0x0092

/* ---- Shared-memory layout (identical to Linux) ---- */
#define OCTSHM_MAGIC            0x4f435348u   /* "OCSH" */
#define OCT_RING_SZ             128u
#define OCT_RING_MASK           (OCT_RING_SZ - 1)
#define OCT_BUF_SZ              9216u
#define OCT_HRX_HDR             8u            /* {len,phase} header before each hrx frame */
#define OCT_POOL_SZ             (OCT_RING_SZ * OCT_BUF_SZ)

/* BAR2 per-port offsets */
#define OCT_CTRL_OFF            0x000000u
#define OCT_TXDESC_OFF          0x001000u
#define OCT_RXDESC_OFF          0x002000u
#define OCT_TXBUF_OFF           0x100000u
#define OCT_RXBUF_OFF           0x280000u
#define OCT_PORT_STRIDE         0x400000u     /* 4 MB per port */
#define OCT_MAX_PORTS           2u

/* Control page field byte offsets */
#define C_MAGIC                 0x00u
#define C_VERSION               0x04u
#define C_CARD_READY            0x08u
#define C_HOST_READY            0x0cu
#define C_TX_PROD               0x10u
#define C_TX_CONS               0x14u
#define C_RX_PROD               0x18u
#define C_RX_CONS               0x1cu
#define C_HEARTBEAT             0x20u
#define C_DMA_ENABLE            0x24u
#define C_TX_DMA_BASE           0x28u         /* 64-bit */
#define C_RX_DMA_BASE           0x30u         /* 64-bit */
#define C_RESV0                 0x38u         /* card board temp (millidegC) */
#define C_RESV1                 0x3cu         /* card Octeon die temp (millidegC) */
#define C_LINK                  0x40u         /* bit31=published, bit0=SFP+ link up */

/* Driver tunables (mapped to SYSCTL_INT) */
#define OCT_DEFAULT_PORTS       1
#define OCT_DEFAULT_RXTHREADS   2
#define OCT_POLL_US             200
#define OCT_NTXQ                1
#define OCT_TEMP_WARN_MC        85000
#define OCT_TEMP_CRIT_MC        100000

/* Phase helpers (log2(RING_SZ)=7) */
#define OCT_TX_PHASE(tp)  (((tp) >> 7) & 1u)
#define OCT_RX_PHASE(rc)  (((rc) >> 7) & 1u)

/* ---- Per-port state ---- */
struct oct_port {
    int                 idx;
    bus_space_tag_t     bt;        /* BAR2 bus space tag */
    bus_space_handle_t  bsh;       /* BAR2 mapping for this port */
    struct ifnet        *ifp;

    /* DMA coherent pools (ztx mode) */
    void                *tx_dma_va;
    void                *rx_dma_va;
    bus_dma_tag_t       tx_dtag, rx_dtag;
    bus_dmamap_t        tx_dmap, rx_dmap;

    /* Poll threads: consumer shadows per thread */
    u_int               rc_pub[8];
    int                 n_threads;
    struct task         *poll_thr[8];

    /* TX producer state */
    volatile int        tx_claim;
    u_int               tc_cache;
    u_int               tp_shadow;

    /* RX consumer shadow */
    u_int               rc_shadow;
};

/* ---- Driver globals ---- */
extern struct oct_port  oct_ports[OCT_MAX_PORTS];
extern int              oct_nports;
extern int              oct_rxthreads;
extern int              oct_poll_us;
extern int              oct_ntxq;
extern int              oct_dma;
extern int              oct_hrx;
extern int              oct_lockfree;
extern int              oct_ztx;

/* ---- Prototypes (if_octnic.c) ---- */
int     octnic_attach(device_t dev);
int     octnic_detach(device_t dev);
static void    octnic_init(struct ifnet *ifp);
int     octnic_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data);
void    octnic_start(struct ifnet *ifp);
void    octnic_qflush(struct ifnet *ifp);
void    octnic_sysctl_init(void);

/* ---- Bus helpers ---- */
static __inline u_int32_t oct_read4(struct oct_port *p, u_int off)
{
    return bus_space_read_4(p->bt, p->bsh, off);
}
static __inline void oct_write4(struct oct_port *p, u_int off, u_int32_t v)
{
    bus_space_write_4(p->bt, p->bsh, off, v);
}
static __inline u_int64_t oct_read8(struct oct_port *p, u_int off)
{
    return bus_space_read_8(p->bt, p->bsh, off);
}
static __inline void oct_write8(struct oct_port *p, u_int off, u_int64_t v)
{
    bus_space_write_8(p->bt, p->bsh, off, v);
}

/* ---- mbuf helpers (BSD equivalent of Linux sk_buff ops) ---- */
static __inline void oct_mbuf_free(struct mbuf *m) { if (m) m_freem(m); }

#endif /* _IF_OCTNIC_H */
