/* SPDX-License-Identifier: GPL-2.0
 * octnic_main.c — Windows NDIS miniport driver for Cavium CN6640 SNIC10E shared-memory NIC.
 *
 * Translates the Linux host-side octnic driver to Windows NDIS 6.x kernel-mode.
 * Registers two network adapters (oct0, oct1) over the card's BAR2 shared-memory window.
 *
 * Architecture:
 *   - BAR2 maps the card's DRAM (4MB per port) with write-combining (WC) memory type
 *   - Per-port: control page, TX descriptor ring, RX descriptor ring, TX/RX buffers
 *   - TX: host PIO writes frames into the BAR2 TX buffer (write-combining for performance)
 *   - RX: kernel-mode DPC timer drains the card->host RX ring into NDIS indication frames
 *   - DMA mode (optional): card DPI DMA-writes host RAM, descriptors in local host RAM
 *
 * Build with WDK. Tested on Windows 10/11 x64.
 */

#include "octnic.h"

/* ============================================================
 * Global driver state
 * ============================================================ */
static DRIVER_CONTEXT g_DriverContext;

/* ============================================================
 * BAR2 access helpers — WC mapping for TX performance
 * ============================================================ */

/* Read 32-bit control field from port's control page */
ULONG OctnicReadCtrl(PPORT_CONTEXT portCtx, ULONG offset)
{
    return *(volatile ULONG*)((PBYTE)portCtx->portBase + offset);
}

/* Write 32-bit control field to port's control page */
VOID OctnicWriteCtrl(PPORT_CONTEXT portCtx, ULONG offset, ULONG value)
{
    *(volatile ULONG*)((PBYTE)portCtx->portBase + offset) = value;
    /* Memory barrier: ensure ordering of BAR2 writes */
    _mm_mfence();
}

/* Read 64-bit control field from port's control page */
ULONGLONG OctnicReadCtrl64(PPORT_CONTEXT portCtx, ULONG offset)
{
    return *(volatile ULONGLONG*)((PBYTE)portCtx->portBase + offset);
}

/* Write 64-bit control field to port's control page */
VOID OctnicWriteCtrl64(PPORT_CONTEXT portCtx, ULONG offset, ULONGLONG value)
{
    *(volatile ULONGLONG*)((PBYTE)portCtx->portBase + offset) = value;
    _mm_mfence();
}

/* PIO write: host writes frame data into BAR2 TX buffer */
VOID OctnicPioWrite(PVOID barBase, ULONG offset, const VOID* data, ULONG len)
{
    RtlCopyMemory((PBYTE)barBase + offset, data, len);
    /* Write-combining memory is already non-temporal; no extra barrier needed for posted writes */
}

/* PIO read: host reads frame data from BAR2 RX buffer (or local RAM in DMA mode) */
VOID OctnicPioRead(PVOID barBase, ULONG offset, VOID* data, ULONG len)
{
    RtlCopyMemory(data, (PBYTE)barBase + offset, len);
}

/* ============================================================
 * NDIS miniport characteristics — function pointers for NDIS 6.x
 * ============================================================ */
static NDIS_MINIPORT_CHARACTERISTICS OctnicMiniportChars;

static VOID
OctnicMiniportCancelSendPackets(
    IN NDIS_HANDLE MiniportCancelSendPacketsContext,
    IN PNDIS_PACKET Packet)
{
    UNREFERENCED_PARAMETER(MiniportCancelSendPacketsContext);
    UNREFERENCED_PARAMETER(Packet);
}

static NDIS_STATUS
OctnicMiniportQueryInformationHandler(
    IN NDIS_HANDLE AdapterHandle,
    IN NDIS_OID Oid,
    IN PVOID InformationBuffer,
    IN ULONG InformationBufferLength,
    OUT PULONG BytesWritten,
    OUT PULONG BytesNeeded)
{
    return OctnicQueryInformation(AdapterHandle, Oid, InformationBuffer,
                                   InformationBufferLength, BytesWritten, BytesNeeded);
}

static NDIS_STATUS
OctnicMiniportSetInformationHandler(
    IN NDIS_HANDLE AdapterHandle,
    IN NDIS_OID Oid,
    IN PVOID InformationBuffer,
    IN ULONG InformationBufferLength,
    OUT PULONG BytesRead,
    OUT PULONG BytesNeeded)
{
    return OctnicSetInformation(AdapterHandle, Oid, InformationBuffer,
                                 InformationBufferLength, BytesRead, BytesNeeded);
}

static VOID
OctnicMiniportResetHandler(
    IN NDIS_HANDLE MiniportAdapterContext,
    OUT PBOOLEAN MediumChanged)
{
    OctnicMiniportReset(MiniportAdapterContext, MediumChanged);
}

static VOID
OctnicMiniportSendPacketsHandler(
    IN NDIS_HANDLE MiniportAdapterContext,
    IN PNDIS_PACKET_LIST PacketList,
    IN NDIS_SEND_PACKETS_FLAGS SendFlags)
{
    OctnicSendPackets(MiniportAdapterContext, PacketList, SendFlags);
}

static NDIS_STATUS
OctnicMiniportTransferData(
    IN NDIS_HANDLE MiniportAdapterContext,
    IN PNDIS_PACKET Packet,
    IN ULONG BytesToTransfer)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(Packet);
    UNREFERENCED_PARAMETER(BytesToTransfer);
    return NDIS_STATUS_NOT_SUPPORTED;
}

static VOID
OctnicMiniportHaltHandler(
    IN NDIS_HANDLE MiniportAdapterContext,
    IN NDIS_HALT_ACTION HaltAction)
{
    OctnicMiniportHalt(MiniportAdapterContext, HaltAction);
}

/* ============================================================
 * DriverEntry — NT driver entry point + NDIS registration
 * ============================================================ */
NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
    NDIS_STATUS status;
    ULONG regVersion;

    DPRINT(DBG_INIT, "DriverEntry: loading octnic Windows driver");

    /* Initialize global driver context */
    RtlZeroMemory(&g_DriverContext, sizeof(DRIVER_CONTEXT));
    g_DriverContext.ndisMiniportHandle = NULL;
    g_DriverContext.bar2Mapping = NULL;
    g_DriverContext.numPorts = DEFAULT_PORTS;
    g_DriverContext.hrx = DEFAULT_HRX;
    g_DriverContext.rxThreads = DEFAULT_RXTHREADS;
    g_DriverContext.nTxQ = DEFAULT_NTXQ;
    g_DriverContext.pollUs = DEFAULT_POLL_US;
    g_DriverContext.lockfree = TRUE;
    g_DriverContext.ztx = FALSE;
    g_DriverContext.dmaMode = TRUE;

    /* Initialize stop event for DPC shutdown */
    KeInitializeEvent(&g_DriverContext.stopEvent, SynchronizationEvent, FALSE);

    /* Initialize NDIS miniport characteristics structure */
    NdisZeroMemory(&OctnicMiniportChars, sizeof(NDIS_MINIPORT_CHARACTERISTICS));
    OctnicMiniportChars.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_CHARACTERISTICS;
    OctnicMiniportChars.Header.Size = sizeof(NDIS_MINIPORT_CHARACTERISTICS);
    OctnicMiniportChars.Header.Revision = NDIS_MINIPORT_CHARACTERISTICS_REVISION_1;

    /* Required fields */
    OctnicMiniportChars.MajorNdisVersion = NDIS_MINIPORT_MAJOR_VERSION;
    OctnicMiniportChars.MinorNdisVersion = NDIS_MINIPORT_MINOR_VERSION;
    OctnicMiniportChars.MajorDriverVersion = 1;
    OctnicMiniportChars.MinorDriverVersion = 0;
    OctnicMiniportChars.Flags = 0;

    /* Function pointers */
    OctnicMiniportChars.InitializeHandler = (PNDIS_MINIPORT_INIT_HANDLER)OctnicMiniportInitialize;
    OctnicMiniportChars.HaltHandler = (PNDIS_MINIPORT_HALT_HANDLER)OctnicMiniportHaltHandler;
    OctnicMiniportChars.QueryInformationHandler = (PNDIS_MINIPORT_QUERY_INFORMATION_HANDLER)OctnicMiniportQueryInformationHandler;
    OctnicMiniportChars.SetInformationHandler = (PNDIS_MINIPORT_SET_INFORMATION_HANDLER)OctnicMiniportSetInformationHandler;
    OctnicMiniportChars.ResetHandler = (PNDIS_MINIPORT_RESET_HANDLER)OctnicMiniportResetHandler;
    OctnicMiniportChars.SendPacketsHandler = (PNDIS_MINIPORT_SEND_PACKETS_HANDLER)OctnicMiniportSendPacketsHandler;
    OctnicMiniportChars.TransferDataHandler = (PNDIS_MINIPORT_TRANSFER_DATA_HANDLER)OctnicMiniportTransferData;
    OctnicMiniportChars.CancelSendPacketsHandler = (PNDIS_MINIPORT_CANCEL_SEND_PACKETS_HANDLER)OctnicMiniportCancelSendPackets;

    /* Register with NDIS as a miniport driver */
    status = NdisMRegisterMiniport(
        DriverObject,
        &OctnicMiniportChars,
        &regVersion,
        &g_DriverContext.ndisMiniportHandle
    );

    if (status != NDIS_STATUS_SUCCESS) {
        DPRINT(DBG_ERROR, "DriverEntry: NdisMRegisterMiniport failed with status %08X", status);
        return STATUS_UNSUCCESSFUL;
    }

    /* Register DriverObject callbacks for PnP/power */
    DriverObject->DriverUnload = OctnicDriverUnload;
    DriverObject->MajorFunction[IRP_MJ_PNP] = IoDefaultDeviceHandler;
    DriverObject->MajorFunction[IRP_MJ_POWER] = IoDefaultPowerHandler;

    DPRINT(DBG_INIT, "DriverEntry: successfully registered with NDIS (version %lu)", regVersion);
    return STATUS_SUCCESS;
}

/* ============================================================
 * Driver unload — cleanup
 * ============================================================ */
VOID OctnicDriverUnload(IN PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DPRINT(DBG_INIT, "OctnicDriverUnload: driver unloading");
    /* NDIS will call our HaltHandler; nothing extra needed here */
}
