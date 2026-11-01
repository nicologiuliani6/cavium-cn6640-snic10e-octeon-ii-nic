/* SPDX-License-Identifier: GPL-2.0
 * octnic.h — Windows driver for Cavium CN6640 SNIC10E shared-memory NIC.
 * Translates the Linux octnic driver to NDIS 6.x miniport + kernel-mode DPC polling.
 *
 * Architecture: same BAR2 shared-memory layout as Linux.
 *   CTRL page (offset 0x000000): magic, version, card/host ready, ring indices, DMA config
 *   TX descriptor ring (offset 0x001000): 128 entries, 8 bytes each {len, phase}
 *   RX descriptor ring (offset 0x002000): 128 entries, 8 bytes each {len, flags}
 *   TX buffer (offset 0x100000): 4MB per port, where host PIO writes frames
 *   RX buffer (offset 0x280000): 4MB per port, where card DPI DMA writes frames
 *   Per-port stride: 4MB (0x400000)
 */

#pragma once

#include <ntddk.h>
#include <ndis.h>

/* ---- PCI identifiers ---- */
#define PCI_VENDOR_ID_CAVIUM      0x177d
#define PCI_DEVICE_ID_CN6640      0x0092
#define PCI_BAR_IDX_SHARED_MEM    2   /* BAR2 = card DRAM window */

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

/* ---- Module parameters (registry-configured) ---- */
#define DEFAULT_PORTS             2
#define DEFAULT_POLL_US           20          /* busy-poll interval in microseconds */
#define DEFAULT_HRX               1           /* host-RAM RX descriptors */
#define DEFAULT_RXTHREADS         8           /* parallel RX drain threads per port */
#define DEFAULT_NTXQ              8           /* multi-queue TX queues */

/* ---- NDIS version ---- */
#define NDIS_MINIPORT_MAJOR_VERSION   6
#define NDIS_MINIPORT_MINOR_VERSION   13

/* ---- Debug ---- */
#define DBG_TRACE       0x00000001u
#define DBG_INIT        0x00000002u
#define DBG_TX          0x00000004u
#define DBG_RX          0x00000008u
#define DBG_ERROR       0x00000010u

#define DEBUG_LEVEL     (DBG_TRACE | DBG_INIT | DBG_ERROR)

#ifdef NDEBUG
#define DPRINT(level, fmt, ...) do { if ((DEBUG_LEVEL & (level))) \
    DbgPrint("octnic: " fmt "\n", ##__VA_ARGS__); } while(0)
#else
#define DPRINT(level, fmt, ...) do { if ((DEBUG_LEVEL & (level))) \
    DbgPrintEx(DPFLTR_IHVDRIVER, DPFLTR_INFO_LEVEL, "octnic[%s]: " fmt "\n", \
               #level, ##__VA_ARGS__); } while(0)
#endif

/* ---- Hardware descriptor structures ---- */
#pragma pack(push, 1)
struct octshm_ctrl {
    ULONG magic;
    ULONG version;
    ULONG card_ready;
    ULONG host_ready;
    ULONG tx_prod;
    ULONG tx_cons;
    ULONG rx_prod;
    ULONG rx_cons;
    ULONG heartbeat;
    ULONG dma_enable;
    ULONGLONG tx_dma_base;
    ULONGLONG rx_dma_base;
    ULONG resv[3];       /* temps, link state */
};

struct octshm_desc {
    ULONG len;
    ULONG flags;
};
#pragma pack(pop)

/* ---- Phase bit helpers (log2(RING_SZ)=7) ---- */
static inline ULONG TX_PHASE(ULONG tp) { return (tp >> 7) & 1u; }
static inline ULONG RX_PHASE(ULONG rc) { return (rc >> 7) & 1u; }

/* ---- Per-port DPC polling context ---- */
typedef struct _PORT_DPC_CTX {
    KDPC                        dpc;           /* Windows DPC object */
    ULONG                       tid;           /* thread ID (0..rxThreads-1) */
    PPORT_CONTEXT               portCtx;       /* back-pointer to port */
} PORT_DPC_CTX, *PPORT_DPC_CTX;

/* ---- Per-port state ---- */
typedef struct _PORT_CONTEXT {
    PVOID               portBase;        /* BAR2 + idx * PORT_STRIDE */
    NDIS_HANDLE         miniportHandle;
    NDIS_HANDLE         netBufferListPool;
    ULONG               txClaim;         /* host TX producer (CAS) */
    ULONG               txProdShadow;    /* cached card tx_prod */
    ULONG               txConsCache;     /* last card tx_cons */
    ULONG               rxConsShadow;    /* host RX consumer */
    ULONG               rcPub[8];        /* per-DPC RX consumer */
    ULONG               idx;             /* port index (0 or 1) */
    BOOLEAN             initialized;
    BOOLEAN             dmaEnabled;
    ULONG               hwQueueId;       /* hardware queue for NDIS multi-queue */

    /* DPC polling: one DPC per consumer thread */
    PORT_DPC_CTX        dpcCtx[8];       /* up to 8 DPCs per port */
    ULONG               dpcCount;        /* actual DPC count (= rxThreads) */
} PORT_CONTEXT, *PPORT_CONTEXT;

/* ---- Driver global state ---- */
typedef struct _DRIVER_CONTEXT {
    NDIS_HANDLE                     ndisMiniportHandle;
    PVOID                           bar2Mapping;     /* MmMapIoSpace result */
    PHYSICAL_ADDRESS                bar2Phys;
    SIZE_T                          bar2Size;
    PORT_CONTEXT                    ports[MAX_PORTS];
    ULONG                           numPorts;
    BOOLEAN                         hrx;             /* host-RAM RX descriptors */
    ULONG                           rxThreads;       /* parallel RX DPCs per port */
    ULONG                           nTxQ;            /* TX queues */
    ULONG                           pollUs;          /* poll interval (us) */
    BOOLEAN                         lockfree;
    BOOLEAN                         ztx;               /* zero-copy TX */
    BOOLEAN                         dmaMode;           /* DMA vs PIO */

    /* Stop event for DPC threads (used at DISPATCH_LEVEL) */
    KEVENT                          stopEvent;

    /* Watchdog: uses IoAllocateWorkItem (runs at PASSIVE_LEVEL) */
    PWORK_ITEM                      watchdogWorkItem;
    ULONG                           lastHeartbeat;
    LARGE_INTEGER                   heartbeatJiffies;  /* KeQuerySystemTimePrecise ticks */
    BOOLEAN                         cardLost;
} DRIVER_CONTEXT, *PDRIVER_CONTEXT;

/* ---- Function prototypes ---- */

/* Initialization / cleanup */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
VOID OctnicDriverUnload(PDRIVER_OBJECT DriverObject);
NDIS_STATUS OctnicMiniportInitialize(NDIS_HANDLE miniportContext, NDIS_HANDLE wrapperContext,
                                     NDIS_MINIPORT_INIT_ATTRIBUTES* attributes);
VOID OctnicMiniportReset(NDIS_HANDLE miniportContext, BOOLEAN* mediumChanged);
VOID OctnicMiniportHalt(NDIS_HANDLE miniportContext, NDIS_HALT_ACTION haltAction);

/* TX */
VOID OctnicSendPackets(NDIS_HANDLE miniportContext, PNDIS_PACKET_LIST packetList,
                        NDIS_SEND_PACKETS_FLAGS sendFlags);
NTSTATUS OctnicTransmitFrame(PPORT_CONTEXT portCtx, PNET_BUFFER netBuffer);

/* RX — DPC-based polling */
VOID NTAPI OctnicDpcTimer(PKDPC dpc, PVOID deferredContext, PVOID systemArgument1, PVOID systemArgument2);
VOID OctnicDpcInitialize(PPORT_CONTEXT portCtx);
VOID OctnicDpcStartAll(PDRIVER_CONTEXT drvCtx);
VOID OctnicDpcStopAll(PDRIVER_CONTEXT drvCtx);

/* RX — ring drain (called from DPC context) */
ULONG OctnicDrainRing(PPORT_CONTEXT portCtx, ULONG tid, ULONG budget);

/* RX — cleanup */
VOID OctnicRxCleanup(PPORT_CONTEXT portCtx);

/* Watchdog */
VOID OctnicWatchdogWorkItemCallback(PWORK_ITEM workItem);
VOID OctnicCheckCardHealth(PDRIVER_CONTEXT drvCtx);

/* BAR2 access helpers */
ULONG OctnicReadCtrl(PPORT_CONTEXT portCtx, ULONG offset);
VOID OctnicWriteCtrl(PPORT_CONTEXT portCtx, ULONG offset, ULONG value);
ULONGLONG OctnicReadCtrl64(PPORT_CONTEXT portCtx, ULONG offset);
VOID OctnicWriteCtrl64(PPORT_CONTEXT portCtx, ULONG offset, ULONGLONG value);

/* Memory operations */
VOID OctnicPioWrite(PVOID barBase, ULONG offset, const VOID* data, ULONG len);
VOID OctnicPioRead(PVOID barBase, ULONG offset, VOID* data, ULONG len);

/* NDIS helpers */
NTSTATUS OctnicAllocateNetBufferListPool(PDRIVER_CONTEXT drvCtx);
VOID OctnicFreeNetBufferListPool(PDRIVER_CONTEXT drvCtx);

/* MAC address / OID handling */
NDIS_STATUS OctnicQueryInformation(NDIS_HANDLE miniportContext, NDIS_OID oid,
                                   PVOID buffer, ULONG bufferLength,
                                   PULONG bytesWritten, PULONG bytesNeeded);
NDIS_STATUS OctnicSetInformation(NDIS_HANDLE miniportContext, NDIS_OID oid,
                                 PVOID buffer, ULONG bufferLength,
                                 PULONG bytesRead, PULONG bytesNeeded);

/* Statistics */
VOID OctnicQueryStatistics(PDRIVER_CONTEXT drvCtx, PNDIS_MINIPORT_STATISTICS stats);
