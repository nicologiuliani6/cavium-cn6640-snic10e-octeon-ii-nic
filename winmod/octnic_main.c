/* SPDX-License-Identifier: GPL-2.0
 * octnic_main.c — Windows NDIS 6.20 miniport driver for Cavium CN6640 SNIC10E.
 * DriverEntry, NDIS handler glue, BAR2 register helpers.
 *
 * Datapath (same as Linux octnic): BAR2 shared-memory rings.
 *   TX: host PIO-writes frames into the BAR2 TX buffer.
 *   RX: KTIMER+DPC polling drains the RX ring into preallocated NBLs.
 */

#include "octnic.h"

DRIVER_CONTEXT g_DriverContext;

/* ---- BAR2 access helpers ---- */
ULONG OctnicReadCtrl(PPORT_CONTEXT portCtx, ULONG offset)
{
    return *(volatile ULONG*)(portCtx->portBase + offset);
}

VOID OctnicWriteCtrl(PPORT_CONTEXT portCtx, ULONG offset, ULONG value)
{
    *(volatile ULONG*)(portCtx->portBase + offset) = value;
    _mm_mfence();
}

/* ---- NDIS handlers ---- */
MINIPORT_INITIALIZE             OctnicInitializeEx;
MINIPORT_HALT                   OctnicHaltEx;
MINIPORT_UNLOAD                 OctnicUnload;
MINIPORT_PAUSE                  OctnicPause;
MINIPORT_RESTART                OctnicRestart;
MINIPORT_CANCEL_SEND            OctnicCancelSend;
MINIPORT_CHECK_FOR_HANG         OctnicCheckForHang;
MINIPORT_RESET                  OctnicResetEx;
MINIPORT_DEVICE_PNP_EVENT_NOTIFY OctnicPnpEvent;
MINIPORT_SHUTDOWN               OctnicShutdown;
MINIPORT_CANCEL_OID_REQUEST     OctnicCancelOidRequest;

NDIS_STATUS OctnicInitializeEx(NDIS_HANDLE adapterHandle, NDIS_HANDLE driverContext,
                               PNDIS_MINIPORT_INIT_PARAMETERS ip)
{
    UNREFERENCED_PARAMETER(driverContext);
    return OctnicMiniportInitialize(adapterHandle, ip);
}

VOID OctnicHaltEx(NDIS_HANDLE ctx, NDIS_HALT_ACTION action)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(action);
    OctnicMiniportHalt();
}

NDIS_STATUS OctnicPause(NDIS_HANDLE ctx, PNDIS_MINIPORT_PAUSE_PARAMETERS pp)
{
    PDRIVER_CONTEXT d = &g_DriverContext;
    LARGE_INTEGER wait;
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(pp);

    OctnicDpcStopAll(d);
    /* NDIS has stopped sends; wait until protocols return every indicated NBL. */
    wait.QuadPart = -10000LL;   /* 1 ms */
    while (d->rxOutstanding > 0) {
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
    }
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS OctnicRestart(NDIS_HANDLE ctx, PNDIS_MINIPORT_RESTART_PARAMETERS rp)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(rp);
    OctnicDpcStartAll(&g_DriverContext);
    return NDIS_STATUS_SUCCESS;
}

VOID OctnicCancelSend(NDIS_HANDLE ctx, PVOID cancelId)
{
    /* ponytail: sends complete synchronously, nothing to cancel */
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(cancelId);
}

BOOLEAN OctnicCheckForHang(NDIS_HANDLE ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    return g_DriverContext.cardLost;
}

NDIS_STATUS OctnicResetEx(NDIS_HANDLE ctx, PBOOLEAN addressingReset)
{
    UNREFERENCED_PARAMETER(ctx);
    *addressingReset = FALSE;
    OctnicMiniportReset();
    return NDIS_STATUS_SUCCESS;
}

VOID OctnicPnpEvent(NDIS_HANDLE ctx, PNET_DEVICE_PNP_EVENT ev)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(ev);
}

VOID OctnicShutdown(NDIS_HANDLE ctx, NDIS_SHUTDOWN_ACTION action)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(action);
}

VOID OctnicCancelOidRequest(NDIS_HANDLE ctx, PVOID requestId)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(requestId);
}

VOID OctnicUnload(PDRIVER_OBJECT driverObject)
{
    UNREFERENCED_PARAMETER(driverObject);
    DPRINT(DBG_INIT, "unload");
    if (g_DriverContext.driverHandle) {
        NdisMDeregisterMiniportDriver(g_DriverContext.driverHandle);
        g_DriverContext.driverHandle = NULL;
    }
}

/* ---- DriverEntry ---- */
DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS chars;
    NDIS_STATUS status;

    DPRINT(DBG_INIT, "DriverEntry");

    RtlZeroMemory(&g_DriverContext, sizeof(g_DriverContext));
    g_DriverContext.numPorts   = DEFAULT_PORTS;
    g_DriverContext.hrx        = DEFAULT_HRX;
    g_DriverContext.rxThreads  = DEFAULT_RXTHREADS;
    g_DriverContext.pollMs     = DEFAULT_POLL_MS;
    g_DriverContext.lockfree   = TRUE;
    KeInitializeSpinLock(&g_DriverContext.rxLock);
    InitializeListHead(&g_DriverContext.rxFree);

    RtlZeroMemory(&chars, sizeof(chars));
    chars.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    chars.Header.Size     = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    chars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    chars.MajorNdisVersion = NDIS_MINIPORT_MAJOR_VERSION;
    chars.MinorNdisVersion = NDIS_MINIPORT_MINOR_VERSION;
    chars.MajorDriverVersion = 1;
    chars.MinorDriverVersion = 0;

    chars.InitializeHandlerEx          = OctnicInitializeEx;
    chars.HaltHandlerEx                = OctnicHaltEx;
    chars.UnloadHandler                = OctnicUnload;
    chars.PauseHandler                 = OctnicPause;
    chars.RestartHandler               = OctnicRestart;
    chars.OidRequestHandler            = OctnicOidRequest;
    chars.SendNetBufferListsHandler    = OctnicSendNetBufferLists;
    chars.ReturnNetBufferListsHandler  = OctnicReturnNetBufferLists;
    chars.CancelSendHandler            = OctnicCancelSend;
    chars.CheckForHangHandlerEx        = OctnicCheckForHang;
    chars.ResetHandlerEx               = OctnicResetEx;
    chars.DevicePnPEventNotifyHandler  = OctnicPnpEvent;
    chars.ShutdownHandlerEx            = OctnicShutdown;
    chars.CancelOidRequestHandler      = OctnicCancelOidRequest;

    status = NdisMRegisterMiniportDriver(driverObject, registryPath,
                                         (NDIS_HANDLE)&g_DriverContext, &chars,
                                         &g_DriverContext.driverHandle);
    if (status != NDIS_STATUS_SUCCESS) {
        DPRINT(DBG_ERROR, "NdisMRegisterMiniportDriver failed %08X", status);
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}
