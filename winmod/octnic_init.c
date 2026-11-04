/* SPDX-License-Identifier: GPL-2.0
 * octnic_init.c â€” adapter init/halt, BAR2 mapping, host DMA memory, port arm/disarm.
 *
 * Two NDIS adapters share one card (see octnic.h). Port 0 (the PCI instance) owns
 * the BAR2 mapping and the host DMA pools of BOTH ports; port 1 (root-enumerated)
 * only borrows them. Neither adapter requires the card to be booted at init time:
 * the adapter always comes up (Device Manager = OK), media stays "disconnected" and
 * the watchdog arms the port as soon as the card's shared-memory block is alive.
 */

#include "octnic.h"

#define BAR_NEED     ((SIZE_T)PORT_STRIDE * MAX_PORTS)
#define MAX_BAR_CAND 4

static PUCHAR g_barCandVa[MAX_BAR_CAND];
static SIZE_T g_barCandMapLen[MAX_BAR_CAND];

/* control-device access to candidate 0 (BAR2): pinned against halt/unmap via ports[0].users */
PUCHAR OctnicCtlAcquire(SIZE_T* len)
{
    InterlockedIncrement(&g_Card.ports[0].users);
    if (g_Card.owner == NULL || g_Card.owner->halting || g_barCandVa[0] == NULL) {
        InterlockedDecrement(&g_Card.ports[0].users);
        return NULL;
    }
    *len = g_barCandMapLen[0];
    return g_barCandVa[0];
}

VOID OctnicCtlRelease(VOID)
{
    InterlockedDecrement(&g_Card.ports[0].users);
}

/* ---- which port is this NDIS instance? (INF writes OctPort per device section) ---- */
static ULONG OctnicReadPortIdx(NDIS_HANDLE h, BOOLEAN hasMem)
{
    NDIS_CONFIGURATION_OBJECT co;
    NDIS_HANDLE cfg = NULL;
    NDIS_STATUS st;
    PNDIS_CONFIGURATION_PARAMETER par = NULL;
    NDIS_STRING kw = NDIS_STRING_CONST("OctPort");
    ULONG idx = hasMem ? 0 : 1;

    RtlZeroMemory(&co, sizeof(co));
    co.Header.Type     = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
    co.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
    co.Header.Size     = NDIS_SIZEOF_CONFIGURATION_OBJECT_REVISION_1;
    co.NdisHandle      = h;
    if (NdisOpenConfigurationEx(&co, &cfg) == NDIS_STATUS_SUCCESS) {
        NdisReadConfiguration(&st, &par, cfg, &kw, NdisParameterInteger);
        if (st == NDIS_STATUS_SUCCESS && par != NULL &&
            par->ParameterType == NdisParameterInteger &&
            par->ParameterData.IntegerData < MAX_PORTS)
            idx = par->ParameterData.IntegerData;
        NdisCloseConfiguration(cfg);
    }
    return idx;
}

/* ---- port-0: map every large memory BAR as a candidate; BAR2 is picked by its magic ---- */
static NDIS_STATUS OctnicMapCandidates(POCT_ADAPTER a, PNDIS_MINIPORT_INIT_PARAMETERS ip)
{
    PCM_PARTIAL_RESOURCE_LIST list = ip->AllocatedResources;
    ULONG i;

    if (list == NULL) return NDIS_STATUS_RESOURCES;
    for (i = 0; i < list->Count && a->nBarCand < MAX_BAR_CAND; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &list->PartialDescriptors[i];
        PUCHAR va;
        SIZE_T mapLen;

        OctLog("octnic: res[%lu] type=%u start=0x%llX len=0x%lX\n", i, r->Type,
               (r->Type == CmResourceTypeMemory) ? (ULONGLONG)r->u.Memory.Start.QuadPart : 0ULL,
               (r->Type == CmResourceTypeMemory) ? r->u.Memory.Length : 0UL);
        if (r->Type != CmResourceTypeMemory || r->u.Memory.Length < BAR_NEED) continue;

        /* map the whole window (64 MB): octboot pushes the OpenWrt image at +0x10000 */
        mapLen = r->u.Memory.Length > 0x4000000u ? 0x4000000u : r->u.Memory.Length;
        va = MmMapIoSpaceEx(r->u.Memory.Start, mapLen, PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (va == NULL) continue;
        g_barCandMapLen[a->nBarCand] = mapLen;
        g_barCandVa[a->nBarCand] = va;
        a->barCandPhys[a->nBarCand] = r->u.Memory.Start;
        a->barCandLen[a->nBarCand] = r->u.Memory.Length;
        a->nBarCand++;
    }
    return a->nBarCand ? NDIS_STATUS_SUCCESS : NDIS_STATUS_RESOURCES;
}

/* Called from the port-0 watchdog until a candidate carries the octshm magic. */
BOOLEAN OctnicChooseBar(POCT_ADAPTER a)
{
    ULONG c, i;
    KIRQL irql;

    static ULONG dbgTick;
    for (c = 0; c < a->nBarCand; c++) {
        if (*(volatile ULONG*)(g_barCandVa[c] + C_MAGIC) != OCTSHM_MAGIC) {
            if ((dbgTick++ % 5) == 0)
                DPRINT(0, "cand%lu phys=0x%llX [0]=%08X [8]=%08X [0x10000]=%08X", c,
                       (ULONGLONG)a->barCandPhys[c].QuadPart,
                       *(volatile ULONG*)(g_barCandVa[c] + 0), *(volatile ULONG*)(g_barCandVa[c] + 8),
                       *(volatile ULONG*)(g_barCandVa[c] + 0x10000));
            continue;
        }

        KeAcquireSpinLock(&g_Card.lock, &irql);
        if (!g_Card.ready) {
            g_Card.bar = g_barCandVa[c];
            g_Card.barSize = BAR_NEED;
            g_Card.barPhys = a->barCandPhys[c];
            for (i = 0; i < MAX_PORTS; i++)
                g_Card.ports[i].portBase = g_Card.bar + (SIZE_T)i * PORT_STRIDE;
            InterlockedExchange(&g_Card.ready, 1);
            DPRINT(0, "BAR2 found: phys 0x%llX", (ULONGLONG)g_Card.barPhys.QuadPart);
        }
        KeReleaseSpinLock(&g_Card.lock, irql);
        return TRUE;
    }
    return FALSE;
}

/* ---- host DMA pools (port-0 adapter's DMA handle) ---- */
static VOID OctnicSgStub(PVOID a, PVOID b, PVOID c, PVOID d)
{
    UNREFERENCED_PARAMETER(a); UNREFERENCED_PARAMETER(b);
    UNREFERENCED_PARAMETER(c); UNREFERENCED_PARAMETER(d);
}

static NDIS_STATUS OctnicDmaSetup(POCT_ADAPTER a)
{
    NDIS_SG_DMA_DESCRIPTION dd;
    NDIS_STATUS st;
    ULONG i;

    RtlZeroMemory(&dd, sizeof(dd));
    dd.Header.Type     = NDIS_OBJECT_TYPE_SG_DMA_DESCRIPTION;
    dd.Header.Revision = NDIS_SG_DMA_DESCRIPTION_REVISION_1;
    dd.Header.Size     = sizeof(dd);
    dd.Flags           = NDIS_SG_DMA_64_BIT_ADDRESS;
    dd.MaximumPhysicalMapping = BUF_SZ;
    dd.ProcessSGListHandler   = (PVOID)OctnicSgStub;
    dd.SharedMemAllocateCompleteHandler = (PVOID)OctnicSgStub;
    st = NdisMRegisterScatterGatherDma(a->handle, &dd, &a->dmaHandle);
    if (st != NDIS_STATUS_SUCCESS) {
        DPRINT(0, "NdisMRegisterScatterGatherDma failed %08X", st);
        return st;
    }

    for (i = 0; i < MAX_PORTS; i++) {
        PPORT_CONTEXT p = &g_Card.ports[i];
        NdisMAllocateSharedMemory(a->handle, POOL_SZ, TRUE, &p->txDmaVa, &p->txDmaPa);
        NdisMAllocateSharedMemory(a->handle, POOL_SZ, TRUE, &p->rxDmaVa, &p->rxDmaPa);
        if (!p->txDmaVa || !p->rxDmaVa) {
            DPRINT(0, "shared memory allocation failed (port %lu)", i);
            return NDIS_STATUS_RESOURCES;
        }
        RtlZeroMemory(p->txDmaVa, POOL_SZ);
        RtlZeroMemory(p->rxDmaVa, POOL_SZ);
        DPRINT(0, "port%lu DMA tx_pa=0x%llX rx_pa=0x%llX", i,
               (ULONGLONG)p->txDmaPa.QuadPart, (ULONGLONG)p->rxDmaPa.QuadPart);
    }
    return NDIS_STATUS_SUCCESS;
}

static VOID OctnicDmaFree(POCT_ADAPTER a)
{
    ULONG i;
    for (i = 0; i < MAX_PORTS; i++) {
        PPORT_CONTEXT p = &g_Card.ports[i];
        if (p->txDmaVa) NdisMFreeSharedMemory(a->handle, POOL_SZ, TRUE, p->txDmaVa, p->txDmaPa);
        if (p->rxDmaVa) NdisMFreeSharedMemory(a->handle, POOL_SZ, TRUE, p->rxDmaVa, p->rxDmaPa);
        p->txDmaVa = p->rxDmaVa = NULL;
    }
}

/* ---- arm / disarm (mirror of Linux octdma_alloc + oct_open / oct_stop) ---- */
BOOLEAN OctnicPortArm(PPORT_CONTEXT p)
{
    KIRQL irql;
    BOOLEAN ok = FALSE;
    ULONG s, i;

    KeAcquireSpinLock(&p->lock, &irql);
    if (!g_Card.ready || p->armed || p->users > 1 ||   /* 1 = the calling watchdog tick */ p->rxDmaVa == NULL || p->portBase == NULL)
        goto out;
    if (OctnicReadCtrl(p, C_MAGIC) != OCTSHM_MAGIC) goto out;

    /* seed each slot's header phase to "not ready" (opposite of first expected) */
    for (s = 0; s < RING_SZ; s++) {
        PULONG h = (PULONG)((PUCHAR)p->rxDmaVa + (SIZE_T)s * BUF_SZ);
        h[0] = 0;
        h[1] = RX_PHASE(s) ^ 1u;
    }
    KeMemoryBarrier();

    *(volatile ULONG64*)(p->portBase + C_TX_DMA_BASE) = (ULONG64)p->txDmaPa.QuadPart;
    *(volatile ULONG64*)(p->portBase + C_RX_DMA_BASE) = (ULONG64)p->rxDmaPa.QuadPart;
    _mm_sfence(); _mm_mfence();
    OctnicWriteCtrl(p, C_DMA_ENABLE, 1);

    p->txClaim = 0;
    p->txConsCache = 0;
    p->rxConsShadow = 0;               /* hrx ring is fresh at 0 */
    for (i = 0; i < MAX_RX_THREADS; i++) p->rcPub[i] = (LONG)i;
    OctnicWriteCtrl(p, C_RX_CONS, 0);
    OctnicWriteCtrl(p, C_HOST_READY, 1);

    InterlockedExchange(&p->armed, 1);
    ok = TRUE;
out:
    KeReleaseSpinLock(&p->lock, irql);
    return ok;
}

VOID OctnicPortWaitIdle(PPORT_CONTEXT p)
{
    LARGE_INTEGER d;
    d.QuadPart = -10000LL;
    while (p->users != 0) KeDelayExecutionThread(KernelMode, FALSE, &d);
}

/* wait=TRUE only at PASSIVE_LEVEL (halt): quiesce users and let the card drain DMA */
VOID OctnicPortDisarm(PPORT_CONTEXT p, BOOLEAN wait)
{
    KIRQL irql;
    BOOLEAN was;
    LARGE_INTEGER d;

    KeAcquireSpinLock(&p->lock, &irql);
    was = InterlockedExchange(&p->armed, 0) != 0;
    KeReleaseSpinLock(&p->lock, irql);

    if (was && g_Card.bar && p->portBase && OctnicReadCtrl(p, C_MAGIC) == OCTSHM_MAGIC) {
        OctnicWriteCtrl(p, C_HOST_READY, 0);
        if (wait) OctnicWriteCtrl(p, C_DMA_ENABLE, 0);
    }
    if (wait) {
        OctnicPortWaitIdle(p);
        if (was) { d.QuadPart = -1000000LL; KeDelayExecutionThread(KernelMode, FALSE, &d); } /* 100 ms: in-flight DMA lands */
    }
}

/* ---- link indication ---- */
VOID OctnicIndicateLink(POCT_ADAPTER a, BOOLEAN up)
{
    NDIS_STATUS_INDICATION si;
    NDIS_LINK_STATE ls;

    if (InterlockedExchange(&a->linkUp, up ? 1 : 0) == (up ? 1 : 0)) return;

    RtlZeroMemory(&ls, sizeof(ls));
    ls.Header.Type     = NDIS_OBJECT_TYPE_DEFAULT;
    ls.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    ls.Header.Size     = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    ls.MediaConnectState = up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    ls.MediaDuplexState  = up ? MediaDuplexStateFull : MediaDuplexStateUnknown;
    ls.XmitLinkSpeed = ls.RcvLinkSpeed = up ? OCTNIC_LINK_BPS : NDIS_LINK_SPEED_UNKNOWN;
    ls.PauseFunctions = NdisPauseFunctionsUnsupported;

    RtlZeroMemory(&si, sizeof(si));
    si.Header.Type     = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    si.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    si.Header.Size     = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    si.SourceHandle    = a->handle;
    si.StatusCode      = NDIS_STATUS_LINK_STATE;
    si.StatusBuffer    = &ls;
    si.StatusBufferSize = sizeof(ls);
    NdisMIndicateStatusEx(a->handle, &si);
    DPRINT(0, "oct%lu link %s", a->portIdx, up ? "UP" : "DOWN");
}

/* ---- NDIS attributes ---- */
static NDIS_STATUS OctnicSetRegAttributes(POCT_ADAPTER a)
{
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES reg;

    RtlZeroMemory(&reg, sizeof(reg));
    reg.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    reg.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    reg.Header.Size     = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    reg.MiniportAdapterContext = a;
    reg.AttributeFlags  = a->isOwner ?
        (NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE | NDIS_MINIPORT_ATTRIBUTES_BUS_MASTER) : 0;
    reg.CheckForHangTimeInSeconds = 0;
    reg.InterfaceType   = a->isOwner ? NdisInterfacePci : NdisInterfaceInternal;
    return NdisMSetMiniportAttributes(a->handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&reg);
}

static NDIS_STATUS OctnicSetGenAttributes(POCT_ADAPTER a)
{
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES gen;

    RtlZeroMemory(&gen, sizeof(gen));
    gen.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    gen.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    gen.Header.Size     = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    gen.MediaType       = NdisMedium802_3;
    gen.PhysicalMediumType = NdisPhysicalMedium802_3;
    gen.MtuSize         = OCTNIC_MTU;
    gen.MaxXmitLinkSpeed = gen.XmitLinkSpeed = OCTNIC_LINK_BPS;
    gen.MaxRcvLinkSpeed  = gen.RcvLinkSpeed  = OCTNIC_LINK_BPS;
    gen.MediaConnectState = MediaConnectStateDisconnected;   /* watchdog raises it */
    gen.MediaDuplexState  = MediaDuplexStateFull;
    gen.LookaheadSize   = OCTNIC_MAX_FRAME;
    gen.PowerManagementCapabilities = NULL;
    gen.MacOptions      = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                          NDIS_MAC_OPTION_NO_LOOPBACK;
    gen.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST |
                          NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST |
                          NDIS_PACKET_TYPE_PROMISCUOUS;
    gen.MaxMulticastListSize = 32;
    gen.MacAddressLength = ETH_ALEN;
    RtlCopyMemory(gen.PermanentMacAddress, a->mac, ETH_ALEN);
    RtlCopyMemory(gen.CurrentMacAddress,   a->mac, ETH_ALEN);
    gen.RecvScaleCapabilities = NULL;
    gen.AccessType      = NET_IF_ACCESS_BROADCAST;
    gen.DirectionType   = NET_IF_DIRECTION_SENDRECEIVE;
    gen.ConnectionType  = NET_IF_CONNECTION_DEDICATED;
    gen.IfType          = IF_TYPE_ETHERNET_CSMACD;
    gen.IfConnectorPresent = TRUE;
    gen.SupportedStatistics = NDIS_STATISTICS_XMIT_OK_SUPPORTED | NDIS_STATISTICS_RCV_OK_SUPPORTED |
                          NDIS_STATISTICS_XMIT_ERROR_SUPPORTED | NDIS_STATISTICS_RCV_ERROR_SUPPORTED |
                          NDIS_STATISTICS_RCV_NO_BUFFER_SUPPORTED;
    gen.SupportedPauseFunctions = NdisPauseFunctionsUnsupported;
    gen.SupportedOidList       = (PNDIS_OID)g_SupportedOids;
    gen.SupportedOidListLength = g_SupportedOidCount * sizeof(NDIS_OID);

    return NdisMSetMiniportAttributes(a->handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&gen);
}

/* ---- adapter lifecycle ---- */
static VOID OctnicFreeAdapter(POCT_ADAPTER a)
{
    if (a->port && a->port->adapter == a) {
        OctnicRxThreadStop(a->port);
        a->port->adapter = NULL;
    }
    OctnicRxPoolDestroy(a);
    NdisFreeMemory(a, 0, 0);
}

NDIS_STATUS OctnicMiniportInitialize(NDIS_HANDLE adapterHandle, PNDIS_MINIPORT_INIT_PARAMETERS ip)
{
    POCT_ADAPTER a;
    NDIS_STATUS status;
    BOOLEAN hasMem = FALSE;
    ULONG i;

    if (ip->AllocatedResources != NULL) {
        for (i = 0; i < ip->AllocatedResources->Count; i++)
            if (ip->AllocatedResources->PartialDescriptors[i].Type == CmResourceTypeMemory) hasMem = TRUE;
    }

    a = (POCT_ADAPTER)NdisAllocateMemoryWithTagPriority(adapterHandle, sizeof(OCT_ADAPTER),
                                                        OCTNIC_TAG, NormalPoolPriority);
    if (a == NULL) return NDIS_STATUS_RESOURCES;
    RtlZeroMemory(a, sizeof(*a));
    a->handle    = adapterHandle;
    a->portIdx   = OctnicReadPortIdx(adapterHandle, hasMem);
    a->port      = &g_Card.ports[a->portIdx];
    a->isOwner   = (a->portIdx == 0);
    a->lookahead = OCTNIC_MAX_FRAME;
    KeInitializeSpinLock(&a->rxLock);
    InitializeListHead(&a->rxFree);
    DPRINT(0, "init oct%lu hasMem=%d", a->portIdx, hasMem);

    if (a->port->adapter != NULL || (a->isOwner && g_Card.owner != NULL)) {
        DPRINT(0, "oct%lu already initialised", a->portIdx);
        NdisFreeMemory(a, 0, 0);
        return NDIS_STATUS_FAILURE;
    }

    /* stable, machine-derived, locally administered MAC: 02:C4:xx:xx:xx:port */
    a->mac[0] = 0x02; a->mac[1] = 0xC4;
    a->mac[2] = g_Card.macBase[0]; a->mac[3] = g_Card.macBase[1];
    a->mac[4] = g_Card.macBase[2]; a->mac[5] = (UCHAR)a->portIdx;

    if (a->isOwner) {
        status = OctnicMapCandidates(a, ip);
        if (status != NDIS_STATUS_SUCCESS) { DPRINT(0, "no usable BAR"); goto fail; }
    }

    status = OctnicSetRegAttributes(a);
    if (status != NDIS_STATUS_SUCCESS) { DPRINT(0, "reg attrs %08X", status); goto fail; }

    if (a->isOwner) {
        status = OctnicDmaSetup(a);
        if (status != NDIS_STATUS_SUCCESS) goto fail;
    }

    status = OctnicSetGenAttributes(a);
    if (status != NDIS_STATUS_SUCCESS) { DPRINT(0, "gen attrs %08X", status); goto fail; }

    status = OctnicRxPoolCreate(a);
    if (status != NDIS_STATUS_SUCCESS) goto fail;

    a->port->nThreads = g_Card.rxThreads;
    a->port->adapter = a;
    OctnicRxThreadStart(a->port);

    status = OctnicWatchdogInit(a);
    if (status != NDIS_STATUS_SUCCESS) goto fail;

    if (a->isOwner) g_Card.owner = a;
    OctnicCtlRegister(g_driverHandle);          /* \\.\octnic exists only while an adapter is alive */
    DPRINT(0, "oct%lu initialised (card %s)", a->portIdx, g_Card.ready ? "alive" : "not up yet");
    return NDIS_STATUS_SUCCESS;

fail:
    DPRINT(0, "oct%lu init failed %08X", a->portIdx, status);
    if (a->isOwner) {
        ULONG c;
        OctnicDmaFree(a);
        for (c = 0; c < a->nBarCand; c++) { MmUnmapIoSpace(g_barCandVa[c], g_barCandMapLen[c]); g_barCandVa[c] = NULL; }
    }
    OctnicFreeAdapter(a);
    return status;
}

VOID OctnicMiniportHalt(POCT_ADAPTER a)
{
    LARGE_INTEGER d;
    ULONG i;

    InterlockedExchange(&a->halting, 1);
    OctnicWatchdogCleanup(a);

    if (a->isOwner) {
        /* no new arms on either port, then quiesce both before BAR/DMA go away */
        InterlockedExchange(&g_Card.ready, 0);
        for (i = 0; i < MAX_PORTS; i++) OctnicPortDisarm(&g_Card.ports[i], TRUE);
        for (i = 0; i < MAX_PORTS; i++) OctnicPortWaitIdle(&g_Card.ports[i]);
        d.QuadPart = -1000000LL;                    /* 100 ms for the card to disarm */
        KeDelayExecutionThread(KernelMode, FALSE, &d);
        OctnicDmaFree(a);
        for (i = 0; i < a->nBarCand; i++) { MmUnmapIoSpace(g_barCandVa[i], g_barCandMapLen[i]); g_barCandVa[i] = NULL; }
        g_Card.bar = NULL;
        for (i = 0; i < MAX_PORTS; i++) g_Card.ports[i].portBase = NULL;
        g_Card.owner = NULL;
    } else {
        OctnicPortDisarm(a->port, TRUE);
    }
    DPRINT(0, "oct%lu halted", a->portIdx);
    OctnicFreeAdapter(a);
    /* last adapter gone: drop the control device so NDIS can unload the driver (updates without reboot) */
    if (g_Card.ports[0].adapter == NULL && g_Card.ports[1].adapter == NULL) OctnicCtlUnregister();
}
