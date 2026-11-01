/* SPDX-License-Identifier: GPL-2.0
 * octnic.h — Windows driver for Cavium CN6640 SNIC10E shared-memory NIC.
 * NDIS 6.20 miniport (one adapter) + kernel-mode DPC polling.
 *
 * Architecture: same BAR2 shared-memory layout as Linux.
 *   CTRL page (offset 0x000000): magic, version, card/host ready, ring indices, DMA config
 *   TX descriptor ring (offset 0x001000): 128 entries, 8 bytes each {len, phase}
 *   RX descriptor ring (offset 0x002000): 128 entries, 8 bytes each {len, flags}
 *   TX buffer (offset 0x100000): where host PIO writes frames
 *   RX buffer (offset 0x280000): where card DPI DMA writes frames
 *   Per-port stride: 4MB (0x400000)
 *
 * NDIS_MINIPORT_DRIVER / NDIS620_MINIPORT are defined by the vcxproj.
 */

#pragma once

#include <ndis.h>
#include <intrin.h>

#define ETH_ALEN                  6

/* ---- PCI identifiers ---- */
#define PCI_VENDOR_ID_CAVIUM      0x177d
#define PCI_DEVICE_ID_CN6640      0x0092

/* ---- Shared-memory layout (same as Linux) ---- */
#define OCTSHM_MAGIC              0x4f435348u   /* "OCSH" */
#define OCTSHM_VERSION            2u
#define RING_SZ                   128u
#define RING_MASK                 (RING_SZ - 1)
#define BUF_SZ                    9216u
#define CTRL_OFF                  0x000000u
#define TXDESC_OFF                0x001000u
#define RXDESC_OFF                0x002000u
#define TXBUF_OFF                 0x100000u
#define RXBUF_OFF                 0x280000u
#define PORT_STRIDE               0x400000u     /* 4MB per port */
#define MAX_PORTS                 2u

/* ---- Control page field offsets ---- */
#define C_MAGIC                   0x00u
#define C_VERSION                 0x04u
#define C_CARD_READY              0x08u
#define C_HOST_READY              0x0cu
#define C_TX_PROD                 0x10u
#define C_TX_CONS                 0x14u
#define C_RX_PROD                 0x18u
#define C_RX_CONS                 0x1cu
#define C_HEARTBEAT               0x20u
#define C_DMA_ENABLE              0x24u
#define C_TX_DMA_BASE             0x28u
#define C_RX_DMA_BASE             0x30u
#define C_RESV0                   0x38u       /* card board temp (millidegC) */
#define C_RESV1                   0x3cu       /* card Octeon die temp (millidegC) */
#define C_LINK                    0x40u       /* bit31=published, bit0=SFP+ link up */

/* ---- Driver parameters ---- */
/* ponytail: one NDIS adapter drives port 0 only; a 2nd adapter needs a bus-style
 * enumerator (one PCI function = one NDIS adapter). Set 2 to also RX from port 1. */
#define DEFAULT_PORTS             1
#define DEFAULT_POLL_MS           1           /* KTIMER period (ms); DPC re-queues itself while busy */
#define DEFAULT_HRX               1           /* host-RAM RX descriptors */
#define MAX_RX_THREADS            8
#define DEFAULT_RXTHREADS         4           /* parallel RX drain DPCs per port */
#define RX_POOL_TOTAL             512u        /* preallocated RX buffers (NBL+MDL+data) */
#define OCTNIC_TAG                'cinO'

#define OCTNIC_MTU                1500u
#define OCTNIC_MAX_FRAME          (OCTNIC_MTU + 14u)
#define OCTNIC_LINK_BPS           10000000000ULL

/* ---- Debug ---- */
#define DBG_TRACE       0x00000001u
#define DBG_INIT        0x00000002u
#define DBG_TX          0x00000004u
#define DBG_RX          0x00000008u
#define DBG_ERROR       0x00000010u

#define DEBUG_LEVEL     (DBG_INIT | DBG_ERROR)

#define DPRINT(level, fmt, ...) do { if ((DEBUG_LEVEL & (level))) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "octnic: " fmt "\n", ##__VA_ARGS__); } while (0)

/* ---- Hardware descriptor structures ---- */
#pragma pack(push, 1)
struct octshm_desc {
    ULONG len;
    ULONG flags;
};
#pragma pack(pop)

/* ---- Phase bit helpers (log2(RING_SZ)=7) ---- */
static __inline ULONG TX_PHASE(ULONG tp) { return (tp >> 7) & 1u; }
static __inline ULONG RX_PHASE(ULONG rc) { return (rc >> 7) & 1u; }

/* ---- Forward declarations ---- */
typedef struct _PORT_CONTEXT PORT_CONTEXT, *PPORT_CONTEXT;

/* ---- Per-port DPC polling context ---- */
typedef struct _PORT_DPC_CTX {
    KDPC            dpc;
    KTIMER          timer;
    ULONG           tid;           /* 0..rxThreads-1 */
    PPORT_CONTEXT   portCtx;
} PORT_DPC_CTX, *PPORT_DPC_CTX;

/* ---- Per-port state ---- */
struct _PORT_CONTEXT {
    PUCHAR              portBase;        /* BAR2 + idx * PORT_STRIDE */
    volatile ULONG      txClaim;         /* host TX producer (CAS) */
    volatile ULONG      txProdShadow;
    volatile ULONG      txConsCache;
    volatile ULONG      rxConsShadow;
    volatile ULONG      rcPub[MAX_RX_THREADS];
    ULONG               idx;
    BOOLEAN             initialized;
    PORT_DPC_CTX        dpcCtx[MAX_RX_THREADS];
    ULONG               dpcCount;
};

/* ---- Preallocated RX buffer: NBL + MDL + data ---- */
typedef struct _RX_BUF {
    LIST_ENTRY          link;
    PNET_BUFFER_LIST    nbl;
    PMDL                mdl;
    PUCHAR              data;
} RX_BUF, *PRX_BUF;

/* ---- Driver global state ---- */
typedef struct _DRIVER_CONTEXT {
    NDIS_HANDLE         driverHandle;    /* NdisMRegisterMiniportDriver */
    NDIS_HANDLE         adapterHandle;   /* MiniportInitializeEx handle (single adapter) */
    NDIS_HANDLE         nblPool;
    NDIS_HANDLE         watchdogTimer;

    PUCHAR              bar2Mapping;
    PHYSICAL_ADDRESS    bar2Phys;
    SIZE_T              bar2Size;
    PORT_CONTEXT        ports[MAX_PORTS];
    ULONG               numPorts;
    BOOLEAN             hrx;
    ULONG               rxThreads;
    ULONG               pollMs;
    BOOLEAN             lockfree;

    volatile LONG       polling;         /* DPCs armed */
    volatile LONG       rxOutstanding;   /* NBLs held by NDIS */

    /* RX buffer pool */
    PRX_BUF             rxBufs;
    LIST_ENTRY          rxFree;
    KSPIN_LOCK          rxLock;

    /* adapter settings */
    UCHAR               mac[ETH_ALEN];
    ULONG               packetFilter;
    ULONG               lookahead;

    /* statistics */
    volatile LONG64     txPkts, txBytes, txErrors;
    volatile LONG64     rxPkts, rxBytes, rxNoBuf;

    /* watchdog */
    ULONG               lastHeartbeat;
    LARGE_INTEGER       heartbeatTime;
    BOOLEAN             cardLost;
} DRIVER_CONTEXT, *PDRIVER_CONTEXT;

extern DRIVER_CONTEXT g_DriverContext;

/* ---- init.c ---- */
NDIS_STATUS OctnicMiniportInitialize(NDIS_HANDLE adapterHandle, PNDIS_MINIPORT_INIT_PARAMETERS ip);
VOID        OctnicMiniportHalt(VOID);
VOID        OctnicMiniportReset(VOID);
VOID        OctnicDpcStartAll(PDRIVER_CONTEXT drvCtx);
VOID        OctnicDpcStopAll(PDRIVER_CONTEXT drvCtx);

/* ---- main.c: BAR2 access helpers ---- */
ULONG OctnicReadCtrl(PPORT_CONTEXT portCtx, ULONG offset);
VOID  OctnicWriteCtrl(PPORT_CONTEXT portCtx, ULONG offset, ULONG value);

/* ---- tx.c ---- */
VOID OctnicSendNetBufferLists(NDIS_HANDLE ctx, PNET_BUFFER_LIST nblList, NDIS_PORT_NUMBER port, ULONG sendFlags);
VOID OctnicTxCleanup(PPORT_CONTEXT portCtx);

/* ---- rx.c ---- */
KDEFERRED_ROUTINE OctnicDpcTimer;
ULONG OctnicDrainRing(PPORT_CONTEXT portCtx, ULONG tid, ULONG budget);
VOID  OctnicRxCleanup(PPORT_CONTEXT portCtx);
VOID  OctnicReturnNetBufferLists(NDIS_HANDLE ctx, PNET_BUFFER_LIST nblList, ULONG returnFlags);

/* ---- utils.c: RX pool ---- */
NDIS_STATUS OctnicRxPoolCreate(PDRIVER_CONTEXT drvCtx);
VOID        OctnicRxPoolDestroy(PDRIVER_CONTEXT drvCtx);
PRX_BUF     OctnicRxBufGet(PDRIVER_CONTEXT drvCtx);
VOID        OctnicRxBufPut(PDRIVER_CONTEXT drvCtx, PRX_BUF rb);

/* ---- oid.c ---- */
extern const NDIS_OID g_SupportedOids[];
extern const ULONG    g_SupportedOidCount;
NDIS_STATUS OctnicOidRequest(NDIS_HANDLE ctx, PNDIS_OID_REQUEST req);

/* ---- watchdog.c ---- */
NDIS_STATUS OctnicWatchdogInit(PDRIVER_CONTEXT drvCtx);
VOID        OctnicWatchdogCleanup(PDRIVER_CONTEXT drvCtx);
