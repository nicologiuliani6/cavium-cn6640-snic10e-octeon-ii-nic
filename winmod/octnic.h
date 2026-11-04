/* SPDX-License-Identifier: GPL-2.0
 * octnic.h â€” Windows driver for Cavium CN6640 SNIC10E shared-memory NIC.
 * NDIS 6.20 miniport, TWO adapters (oct0 / oct1) sharing one physical card.
 *
 * One PCI function can only bind one NDIS adapter, so:
 *   port 0 = the PCI device PCI\VEN_177D&DEV_0092   (owns BAR2 mapping + host DMA memory)
 *   port 1 = a root-enumerated device ROOT\OCTNIC_PORT1 (created by the installer);
 *            it has no hardware resources and uses port 0's mapping / DMA memory.
 * Both instances are served by the same octnic.sys. The registry value OctPort
 * (written by the INF) tells an instance which port it is.
 *
 * Shared-memory protocol = identical to linuxmod/octnic.c (card runs dma=2 hrx=1
 * lockfree=1): per-port 4 MB BAR2 region, TX = PIO into BAR2 + phase bit,
 * RX = card DMA-writes {len,phase}+frame into host RAM slots (hrx).
 *
 * NDIS_MINIPORT_DRIVER / NDIS620_MINIPORT are defined by the vcxproj.
 */

#pragma once

#include <ndis.h>
#include <ntstrsafe.h>
#include <intrin.h>

#define ETH_ALEN                  6

/* ---- PCI identifiers ---- */
#define PCI_VENDOR_ID_CAVIUM      0x177d
#define PCI_DEVICE_ID_CN6640      0x0092

/* ---- Shared-memory layout (same as Linux) ---- */
#define OCTSHM_MAGIC              0x4f435348u   /* "OCSH" */
#define RING_SZ                   128u
#define RING_MASK                 (RING_SZ - 1)
#define BUF_SZ                    9216u
#define HRX_HDR                   8u            /* {len,phase} header before each hrx frame */
#define POOL_SZ                   (RING_SZ * BUF_SZ)
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
#define C_TX_DMA_BASE             0x28u       /* 64-bit */
#define C_RX_DMA_BASE             0x30u       /* 64-bit */
#define C_RESV0                   0x38u       /* card board temp (millidegC) */
#define C_RESV1                   0x3cu       /* card Octeon die temp (millidegC) */
#define C_LINK                    0x40u       /* bit31=published, bit0=SFP+ link up */

/* ---- Driver parameters ---- */
#define MAX_RX_THREADS            8
#define DEFAULT_RXTHREADS         2           /* power of two dividing RING_SZ; per port */
#define RX_BUDGET                 64u
#define RX_POOL_TOTAL             512u        /* preallocated RX buffers per adapter */
#define OCTNIC_TAG                'cinO'

#define OCTNIC_MTU                1500u
#define OCTNIC_MAX_FRAME          (OCTNIC_MTU + 14u)
#define OCTNIC_LINK_BPS           10000000000ULL

#define WD_PERIOD_MS              1000
#define WD_DEAD_100NS             (10LL * 10000000LL)    /* 10 s without heartbeat */
#define TEMP_WARN_MC              85000
#define TEMP_CRIT_MC              100000

/* ---- Debug / log ---- */
VOID OctLog(_In_ PCSTR fmt, ...);
#define DPRINT(level, fmt, ...)   OctLog("octnic: " fmt "\n", ##__VA_ARGS__)
#define DBG_INIT  0
#define DBG_ERROR 0
#define DBG_TX    0
#define DBG_RX    0

/* ---- Hardware descriptor ---- */
#pragma pack(push, 1)
struct octshm_desc {
    ULONG len;
    ULONG flags;
};
#pragma pack(pop)

/* ---- Phase bit helpers (log2(RING_SZ)=7) ---- */
static __inline ULONG TX_PHASE(ULONG tp) { return (tp >> 7) & 1u; }
static __inline ULONG RX_PHASE(ULONG rc) { return (rc >> 7) & 1u; }

typedef struct _OCT_ADAPTER OCT_ADAPTER, *POCT_ADAPTER;

/* ---- RX drain thread context ---- */
typedef struct _RX_THREAD {
    PVOID               port;          /* PPORT_CONTEXT */
    ULONG               tid;
    PKTHREAD            thread;
} RX_THREAD;

/* ---- Per-port (card side) state: lives in g_Card, outlives adapters ---- */
typedef struct _PORT_CONTEXT {
    ULONG               idx;
    PUCHAR              portBase;        /* BAR2 + idx * PORT_STRIDE (valid while card.ready) */

    /* host DMA pools (allocated by port 0's adapter, NdisMAllocateSharedMemory) */
    PVOID               txDmaVa;
    PVOID               rxDmaVa;
    PHYSICAL_ADDRESS    txDmaPa;
    PHYSICAL_ADDRESS    rxDmaPa;

    KSPIN_LOCK          lock;            /* serialises arm / disarm */
    volatile LONG       armed;           /* datapath live */
    volatile LONG       users;           /* threads / senders currently touching BAR or DMA */
    volatile LONG       stop;            /* rx threads must exit */

    volatile LONG       txClaim;
    volatile LONG       txConsCache;
    volatile LONG       rxConsShadow;
    volatile LONG       rcPub[MAX_RX_THREADS];

    ULONG               nThreads;
    RX_THREAD           thr[MAX_RX_THREADS];

    POCT_ADAPTER        adapter;         /* NDIS adapter bound to this port (or NULL) */
} PORT_CONTEXT, *PPORT_CONTEXT;

/* ---- The card (shared by both adapters) ---- */
typedef struct _OCT_CARD {
    KSPIN_LOCK          lock;
    volatile LONG       ready;           /* BAR mapped + DMA memory valid */
    PUCHAR              bar;
    SIZE_T              barSize;
    PHYSICAL_ADDRESS    barPhys;
    POCT_ADAPTER        owner;           /* port-0 adapter, owns BAR + DMA memory */
    PORT_CONTEXT        ports[MAX_PORTS];
    ULONG               rxThreads;       /* from registry */
    UCHAR               macBase[3];      /* machine-derived MAC bytes */
    BOOLEAN             macValid;
} OCT_CARD;

/* ---- Preallocated RX buffer: NBL + MDL + data ---- */
typedef struct _RX_BUF {
    LIST_ENTRY          link;
    PNET_BUFFER_LIST    nbl;
    PMDL                mdl;
    PUCHAR              data;
} RX_BUF, *PRX_BUF;

/* ---- Per NDIS adapter ---- */
struct _OCT_ADAPTER {
    NDIS_HANDLE         handle;
    ULONG               portIdx;
    PPORT_CONTEXT       port;
    BOOLEAN             isOwner;         /* port-0 PCI instance */

    /* RX buffer pool */
    NDIS_HANDLE         nblPool;
    PRX_BUF             rxBufs;
    LIST_ENTRY          rxFree;
    KSPIN_LOCK          rxLock;
    volatile LONG       rxOutstanding;   /* NBLs held by NDIS */

    UCHAR               mac[ETH_ALEN];
    volatile ULONG      packetFilter;
    ULONG               lookahead;

    volatile LONG       paused;
    volatile LONG       halting;
    volatile LONG       linkUp;          /* last link state we indicated */
    volatile LONG       cardLost;

    NDIS_HANDLE         wdTimer;
    volatile LONG       wdBusy;          /* watchdog callback running */
    ULONG               lastHb;
    LARGE_INTEGER       hbTime;
    BOOLEAN             hbValid;         /* lastHb is a real baseline */
    ULONG               hbAlive;         /* heartbeat movements seen */
    BOOLEAN             tempWarned;

    /* port-0 only: candidate BAR resources and DMA registration */
    ULONG               nBarCand;
    PHYSICAL_ADDRESS    barCandPhys[4];
    ULONG               barCandLen[4];
    NDIS_HANDLE         dmaHandle;

    /* statistics */
    volatile LONG64     txPkts, txBytes, txErrors, txDrops;
    volatile LONG64     rxPkts, rxBytes, rxNoBuf, rxFiltered;
};

extern OCT_CARD g_Card;
extern NDIS_HANDLE g_driverHandle;

/* ---- main.c ---- */
ULONG OctnicReadCtrl(PPORT_CONTEXT p, ULONG offset);
VOID  OctnicWriteCtrl(PPORT_CONTEXT p, ULONG offset, ULONG value);

/* ---- init.c ---- */
NDIS_STATUS OctnicMiniportInitialize(NDIS_HANDLE adapterHandle, PNDIS_MINIPORT_INIT_PARAMETERS ip);
VOID        OctnicMiniportHalt(POCT_ADAPTER a);
VOID        OctnicReloadParams(PUNICODE_STRING registryPath);
VOID        OctnicIndicateLink(POCT_ADAPTER a, BOOLEAN up);
BOOLEAN     OctnicChooseBar(POCT_ADAPTER a);
BOOLEAN     OctnicPortArm(PPORT_CONTEXT p);
VOID        OctnicPortDisarm(PPORT_CONTEXT p, BOOLEAN wait);
VOID        OctnicPortWaitIdle(PPORT_CONTEXT p);

/* ---- ctl.c (control device) + init.c helpers ---- */
NTSTATUS OctnicCtlRegister(NDIS_HANDLE driverHandle);
VOID     OctnicCtlUnregister(VOID);
PUCHAR   OctnicCtlAcquire(SIZE_T* len);
VOID     OctnicCtlRelease(VOID);

/* ---- tx.c ---- */
MINIPORT_SEND_NET_BUFFER_LISTS OctnicSendNetBufferLists;

/* ---- rx.c ---- */
MINIPORT_RETURN_NET_BUFFER_LISTS OctnicReturnNetBufferLists;
VOID  OctnicRxThreadStart(PPORT_CONTEXT p);
VOID  OctnicRxThreadStop(PPORT_CONTEXT p);

/* ---- utils.c: RX pool ---- */
NDIS_STATUS OctnicRxPoolCreate(POCT_ADAPTER a);
VOID        OctnicRxPoolDestroy(POCT_ADAPTER a);
PRX_BUF     OctnicRxBufGet(POCT_ADAPTER a);
VOID        OctnicRxBufPut(POCT_ADAPTER a, PRX_BUF rb);

/* ---- oid.c ---- */
extern const NDIS_OID g_SupportedOids[];
extern const ULONG    g_SupportedOidCount;
MINIPORT_OID_REQUEST OctnicOidRequest;

/* ---- watchdog.c ---- */
NDIS_STATUS OctnicWatchdogInit(POCT_ADAPTER a);
VOID        OctnicWatchdogCleanup(POCT_ADAPTER a);
