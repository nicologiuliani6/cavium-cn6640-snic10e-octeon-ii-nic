/* SPDX-License-Identifier: GPL-2.0
 * octnic_main.c â€” Windows NDIS 6.20 miniport driver for Cavium CN6640 SNIC10E.
 * DriverEntry, NDIS handler glue, BAR2 register helpers, debug log.
 */

#include "octnic.h"

OCT_CARD g_Card;

/* ---- debug log: DbgPrint + (at PASSIVE) append to %SystemRoot%\octnic.log ---- */
VOID OctLog(_In_ PCSTR fmt, ...)
{
    CHAR buf[256];
    va_list ap;
    size_t len = 0;

    va_start(ap, fmt);
    RtlStringCbVPrintfA(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL, "%s", buf);

    if (KeGetCurrentIrql() == PASSIVE_LEVEL &&
        NT_SUCCESS(RtlStringCbLengthA(buf, sizeof(buf), &len))) {
        UNICODE_STRING name;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK iosb;
        LARGE_INTEGER off;
        HANDLE h;

        RtlInitUnicodeString(&name, L"\\SystemRoot\\octnic.log");
        InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateFile(&h, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                                    FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
            off.LowPart = FILE_WRITE_TO_END_OF_FILE;
            off.HighPart = -1;
            ZwWriteFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)len, &off, NULL);
            ZwClose(h);
        }
    }
}

/* ---- BAR2 access helpers ---- */
ULONG OctnicReadCtrl(PPORT_CONTEXT p, ULONG offset)
{
    return *(volatile ULONG*)(p->portBase + offset);
}

VOID OctnicWriteCtrl(PPORT_CONTEXT p, ULONG offset, ULONG value)
{
    *(volatile ULONG*)(p->portBase + offset) = value;
    _mm_sfence();               /* flush write-combining buffers */
    _mm_mfence();
}

/* ---- registry parameters ---- */
static ULONG OctFnv(PCWSTR s, ULONG chars)
{
    ULONG h = 2166136261u, i;
    for (i = 0; i < chars; i++) { h ^= (UCHAR)s[i]; h *= 16777619u; h ^= (UCHAR)(s[i] >> 8); h *= 16777619u; }
    return h;
}

static VOID OctnicDeriveMac(VOID)
{
    WCHAR guid[96];
    UNICODE_STRING us;
    RTL_QUERY_REGISTRY_TABLE q[2];
    ULONG h;
    size_t n = 0;

    RtlZeroMemory(guid, sizeof(guid));
    us.Buffer = guid; us.Length = 0; us.MaximumLength = sizeof(guid) - sizeof(WCHAR);
    RtlZeroMemory(q, sizeof(q));
    q[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED;
    q[0].Name = L"MachineGuid";
    q[0].EntryContext = &us;
    h = 0x0C4C4C4u;
    if (NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
            L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Cryptography", q, NULL, NULL))) {
        n = us.Length / sizeof(WCHAR);
        h = OctFnv(guid, (ULONG)n);
    }
    g_Card.macBase[0] = (UCHAR)(h >> 16);
    g_Card.macBase[1] = (UCHAR)(h >> 8);
    g_Card.macBase[2] = (UCHAR)h;
    g_Card.macValid = TRUE;
}

VOID OctnicReloadParams(PUNICODE_STRING registryPath)
{
    ULONG rxThreads = DEFAULT_RXTHREADS;
    WCHAR path[260];
    RTL_QUERY_REGISTRY_TABLE q[2];
    ULONG def = DEFAULT_RXTHREADS;

    if (registryPath && registryPath->Length / sizeof(WCHAR) < 200 &&
        NT_SUCCESS(RtlStringCbPrintfW(path, sizeof(path), L"%wZ\\Parameters", registryPath))) {
        RtlZeroMemory(q, sizeof(q));
        q[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
        q[0].Name = L"RxThreads";
        q[0].EntryContext = &rxThreads;
        q[0].DefaultType = REG_DWORD;
        q[0].DefaultData = &def;
        q[0].DefaultLength = sizeof(ULONG);
        RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, q, NULL, NULL);
    }
    if (rxThreads < 1) rxThreads = 1;
    if (rxThreads > MAX_RX_THREADS) rxThreads = MAX_RX_THREADS;
    while (rxThreads & (rxThreads - 1)) rxThreads &= rxThreads - 1;   /* floor to power of 2 */
    g_Card.rxThreads = rxThreads;
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

NDIS_HANDLE g_driverHandle;

NDIS_STATUS OctnicInitializeEx(NDIS_HANDLE adapterHandle, NDIS_HANDLE driverContext,
                               PNDIS_MINIPORT_INIT_PARAMETERS ip)
{
    UNREFERENCED_PARAMETER(driverContext);
    return OctnicMiniportInitialize(adapterHandle, ip);
}

VOID OctnicHaltEx(NDIS_HANDLE ctx, NDIS_HALT_ACTION action)
{
    UNREFERENCED_PARAMETER(action);
    OctnicMiniportHalt((POCT_ADAPTER)ctx);
}

NDIS_STATUS OctnicPause(NDIS_HANDLE ctx, PNDIS_MINIPORT_PAUSE_PARAMETERS pp)
{
    POCT_ADAPTER a = (POCT_ADAPTER)ctx;
    LARGE_INTEGER wait;
    UNREFERENCED_PARAMETER(pp);

    InterlockedExchange(&a->paused, 1);
    /* NDIS has stopped sends; wait until protocols return every indicated NBL. */
    wait.QuadPart = -10000LL;   /* 1 ms */
    while (a->rxOutstanding > 0) {
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
    }
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS OctnicRestart(NDIS_HANDLE ctx, PNDIS_MINIPORT_RESTART_PARAMETERS rp)
{
    POCT_ADAPTER a = (POCT_ADAPTER)ctx;
    UNREFERENCED_PARAMETER(rp);
    InterlockedExchange(&a->paused, 0);
    return NDIS_STATUS_SUCCESS;
}

VOID OctnicCancelSend(NDIS_HANDLE ctx, PVOID cancelId)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(cancelId);   /* sends complete synchronously */
}

BOOLEAN OctnicCheckForHang(NDIS_HANDLE ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    return FALSE;       /* our own watchdog handles card loss (link down + re-arm) */
}

NDIS_STATUS OctnicResetEx(NDIS_HANDLE ctx, PBOOLEAN addressingReset)
{
    UNREFERENCED_PARAMETER(ctx);
    *addressingReset = FALSE;
    return NDIS_STATUS_SUCCESS;
}

VOID OctnicPnpEvent(NDIS_HANDLE ctx, PNET_DEVICE_PNP_EVENT ev)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(ev);
}

VOID OctnicShutdown(NDIS_HANDLE ctx, NDIS_SHUTDOWN_ACTION action)
{
    POCT_ADAPTER a = (POCT_ADAPTER)ctx;
    UNREFERENCED_PARAMETER(action);
    /* leave the card quiescent for the next OS (Linux must find it idle) */
    if (a && a->port && a->port->armed) OctnicPortDisarm(a->port, FALSE);
}

VOID OctnicCancelOidRequest(NDIS_HANDLE ctx, PVOID requestId)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(requestId);
}

VOID OctnicUnload(PDRIVER_OBJECT driverObject)
{
    UNREFERENCED_PARAMETER(driverObject);
    OctnicCtlUnregister();
    ExSetTimerResolution(0, FALSE);
    if (g_driverHandle) {
        NdisMDeregisterMiniportDriver(g_driverHandle);
        g_driverHandle = NULL;
    }
}

/* ---- DriverEntry ---- */
DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS chars;
    NDIS_STATUS status;
    ULONG i;

    RtlZeroMemory(&g_Card, sizeof(g_Card));
    KeInitializeSpinLock(&g_Card.lock);
    for (i = 0; i < MAX_PORTS; i++) {
        g_Card.ports[i].idx = i;
        KeInitializeSpinLock(&g_Card.ports[i].lock);
    }
    OctnicReloadParams(registryPath);
    OctnicDeriveMac();
    ExSetTimerResolution(5000, TRUE);     /* 0.5 ms: idle RX threads wake quickly */
    OctLog("octnic: DriverEntry rxThreads=%lu\n", g_Card.rxThreads);

    RtlZeroMemory(&chars, sizeof(chars));
    chars.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    chars.Header.Size     = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    chars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    chars.MajorNdisVersion = NDIS_MINIPORT_MAJOR_VERSION;
    chars.MinorNdisVersion = NDIS_MINIPORT_MINOR_VERSION;
    chars.MajorDriverVersion = 1;
    chars.MinorDriverVersion = 1;

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

    status = NdisMRegisterMiniportDriver(driverObject, registryPath, NULL, &chars, &g_driverHandle);
    if (status != NDIS_STATUS_SUCCESS) {
        OctLog("octnic: NdisMRegisterMiniportDriver failed %08X\n", status);
        ExSetTimerResolution(0, FALSE);
        return STATUS_UNSUCCESSFUL;
    }
return STATUS_SUCCESS;
}
