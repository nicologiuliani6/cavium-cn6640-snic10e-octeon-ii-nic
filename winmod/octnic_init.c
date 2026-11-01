/* SPDX-License-Identifier: GPL-2.0
 * octnic_init.c — adapter init: NDIS attributes, BAR2 mapping, port init, DPC polling.
 */

#include "octnic.h"

/* ---- BAR2 mapping: pick the memory resource that carries the octshm magic ---- */
static NDIS_STATUS OctnicMapBar2(PDRIVER_CONTEXT d, PNDIS_MINIPORT_INIT_PARAMETERS ip)
{
    PCM_PARTIAL_RESOURCE_LIST list;
    SIZE_T need = (SIZE_T)PORT_STRIDE * d->numPorts;
    ULONG i;

    if (ip->AllocatedResources == NULL)
        return NDIS_STATUS_RESOURCES;

    list = ip->AllocatedResources;   /* PCM_PARTIAL_RESOURCE_LIST */
    for (i = 0; i < list->Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &list->PartialDescriptors[i];
        PUCHAR va;

        if (r->Type != CmResourceTypeMemory || r->u.Memory.Length < need) continue;

        va = MmMapIoSpaceEx(r->u.Memory.Start, need, PAGE_READWRITE | PAGE_NOCACHE);
        if (va == NULL) continue;

        if (*(volatile ULONG*)(va + C_MAGIC) == OCTSHM_MAGIC) {
            d->bar2Mapping = va;
            d->bar2Phys    = r->u.Memory.Start;
            d->bar2Size    = need;
            DPRINT(DBG_INIT, "BAR2 at phys 0x%llX mapped %p", r->u.Memory.Start.QuadPart, va);
            return NDIS_STATUS_SUCCESS;
        }
        MmUnmapIoSpace(va, need);
    }
    DPRINT(DBG_ERROR, "no memory resource with octshm magic");
    return NDIS_STATUS_ADAPTER_NOT_FOUND;
}

static NTSTATUS OctnicInitPort(PDRIVER_CONTEXT d, PPORT_CONTEXT p, ULONG idx)
{
    ULONG i;

    p->portBase = d->bar2Mapping + (SIZE_T)idx * PORT_STRIDE;
    p->idx = idx;
    p->initialized = FALSE;
    p->txClaim = 0;
    p->txProdShadow = 0;
    p->txConsCache = 0;
    p->rxConsShadow = 0;
    p->dpcCount = 0;

    if (OctnicReadCtrl(p, C_MAGIC) != OCTSHM_MAGIC) {
        DPRINT(DBG_ERROR, "port%lu bad magic", idx);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!OctnicReadCtrl(p, C_CARD_READY)) {
        DPRINT(DBG_ERROR, "port%lu card not ready", idx);
        return STATUS_TIMEOUT;
    }

    p->txProdShadow = OctnicReadCtrl(p, C_TX_PROD);
    p->txConsCache  = OctnicReadCtrl(p, C_TX_CONS);
    p->txClaim      = p->txProdShadow;
    p->rxConsShadow = d->hrx ? 0 : OctnicReadCtrl(p, C_RX_PROD);

    OctnicWriteCtrl(p, C_HOST_READY, 1);
    OctnicWriteCtrl(p, C_RX_CONS, p->rxConsShadow);

    for (i = 0; i < MAX_RX_THREADS; i++) p->rcPub[i] = p->rxConsShadow + i;

    p->initialized = TRUE;
    return STATUS_SUCCESS;
}

/* ---- DPC polling: per-thread KTIMER -> KDPC ---- */
VOID OctnicDpcStartAll(PDRIVER_CONTEXT d)
{
    ULONG i, t, ncpu = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    LARGE_INTEGER due;

    if (InterlockedExchange(&d->polling, 1)) return;
    due.QuadPart = -(LONGLONG)d->pollMs * 10000;

    for (i = 0; i < d->numPorts && i < MAX_PORTS; i++) {
        PPORT_CONTEXT p = &d->ports[i];
        if (!p->initialized) continue;
        p->dpcCount = d->rxThreads;
        for (t = 0; t < p->dpcCount; t++) {
            PPORT_DPC_CTX c = &p->dpcCtx[t];
            c->tid = t;
            c->portCtx = p;
            KeInitializeDpc(&c->dpc, OctnicDpcTimer, c);
            KeSetTargetProcessorDpc(&c->dpc, (CCHAR)(t % ncpu));
            KeInitializeTimerEx(&c->timer, SynchronizationTimer);
            KeSetTimerEx(&c->timer, due, (LONG)d->pollMs, &c->dpc);
        }
    }
}

VOID OctnicDpcStopAll(PDRIVER_CONTEXT d)
{
    ULONG i, t;

    if (!InterlockedExchange(&d->polling, 0)) return;
    for (i = 0; i < d->numPorts && i < MAX_PORTS; i++) {
        PPORT_CONTEXT p = &d->ports[i];
        if (!p->initialized) continue;
        for (t = 0; t < p->dpcCount; t++) KeCancelTimer(&p->dpcCtx[t].timer);
    }
    KeFlushQueuedDpcs();   /* PASSIVE_LEVEL only: wait for running DPCs */
}

/* ---- NDIS attributes ---- */
static NDIS_STATUS OctnicSetAttributes(PDRIVER_CONTEXT d)
{
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES reg;
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES gen;
    NDIS_STATUS status;

    RtlZeroMemory(&reg, sizeof(reg));
    reg.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    reg.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    reg.Header.Size     = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    reg.MiniportAdapterContext = d;
    reg.AttributeFlags  = NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE | NDIS_MINIPORT_ATTRIBUTES_BUS_MASTER;
    reg.CheckForHangTimeInSeconds = 4;
    reg.InterfaceType   = NdisInterfacePci;
    status = NdisMSetMiniportAttributes(d->adapterHandle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&reg);
    if (status != NDIS_STATUS_SUCCESS) return status;

    /* locally-administered MAC derived from BAR2 phys + port */
    d->mac[0] = 0x02; d->mac[1] = 0xC4;
    d->mac[2] = (UCHAR)(d->bar2Phys.LowPart >> 24);
    d->mac[3] = (UCHAR)(d->bar2Phys.LowPart >> 16);
    d->mac[4] = (UCHAR)(d->bar2Phys.LowPart >> 8);
    d->mac[5] = 0;

    RtlZeroMemory(&gen, sizeof(gen));
    gen.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    gen.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    gen.Header.Size     = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    gen.MediaType       = NdisMedium802_3;
    gen.PhysicalMediumType = NdisPhysicalMedium802_3;
    gen.MtuSize         = OCTNIC_MTU;
    gen.MaxXmitLinkSpeed = gen.XmitLinkSpeed = OCTNIC_LINK_BPS;
    gen.MaxRcvLinkSpeed  = gen.RcvLinkSpeed  = OCTNIC_LINK_BPS;
    gen.MediaConnectState = MediaConnectStateConnected;
    gen.MediaDuplexState  = MediaDuplexStateFull;
    gen.LookaheadSize   = OCTNIC_MAX_FRAME;
    gen.PowerManagementCapabilities = NULL;
    gen.MacOptions      = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                          NDIS_MAC_OPTION_NO_LOOPBACK;
    gen.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST |
                          NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST |
                          NDIS_PACKET_TYPE_PROMISCUOUS;
    gen.MaxMulticastListSize = 0;
    gen.MacAddressLength = ETH_ALEN;
    RtlCopyMemory(gen.PermanentMacAddress, d->mac, ETH_ALEN);
    RtlCopyMemory(gen.CurrentMacAddress,   d->mac, ETH_ALEN);
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

    return NdisMSetMiniportAttributes(d->adapterHandle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&gen);
}

static VOID OctnicTeardown(PDRIVER_CONTEXT d)
{
    OctnicWatchdogCleanup(d);
    OctnicDpcStopAll(d);
    OctnicRxPoolDestroy(d);
    if (d->bar2Mapping) {
        MmUnmapIoSpace(d->bar2Mapping, d->bar2Size);
        d->bar2Mapping = NULL;
    }
    d->adapterHandle = NULL;
}

NDIS_STATUS OctnicMiniportInitialize(NDIS_HANDLE adapterHandle, PNDIS_MINIPORT_INIT_PARAMETERS ip)
{
    PDRIVER_CONTEXT d = &g_DriverContext;
    NDIS_STATUS status;
    ULONG i;

    if (d->adapterHandle != NULL) return NDIS_STATUS_FAILURE;   /* single adapter */
    d->adapterHandle = adapterHandle;
    d->packetFilter = 0;
    d->lookahead = OCTNIC_MAX_FRAME;
    d->cardLost = FALSE;

    /* registration attributes must precede other NDIS calls; MAC needs BAR2 phys,
     * so map first (plain WDM calls), then set attributes. */
    status = OctnicMapBar2(d, ip);
    if (status != NDIS_STATUS_SUCCESS) goto fail;

    status = OctnicSetAttributes(d);
    if (status != NDIS_STATUS_SUCCESS) goto fail;

    for (i = 0; i < d->numPorts && i < MAX_PORTS; i++) {
        if (OctnicInitPort(d, &d->ports[i], i) != STATUS_SUCCESS) {
            status = NDIS_STATUS_FAILURE;
            goto fail;
        }
    }

    status = OctnicRxPoolCreate(d);
    if (status != NDIS_STATUS_SUCCESS) goto fail;

    status = OctnicWatchdogInit(d);
    if (status != NDIS_STATUS_SUCCESS) goto fail;

    /* polling starts in MiniportRestart */
    DPRINT(DBG_INIT, "initialized");
    return NDIS_STATUS_SUCCESS;

fail:
    DPRINT(DBG_ERROR, "init failed %08X", status);
    OctnicTeardown(d);
    return status;
}

VOID OctnicMiniportHalt(VOID)
{
    PDRIVER_CONTEXT d = &g_DriverContext;
    ULONG i;

    OctnicDpcStopAll(d);
    for (i = 0; i < d->numPorts && i < MAX_PORTS; i++) {
        if (d->ports[i].initialized) {
            OctnicWriteCtrl(&d->ports[i], C_HOST_READY, 0);
            d->ports[i].initialized = FALSE;
        }
    }
    OctnicTeardown(d);
    DPRINT(DBG_INIT, "halted");
}

VOID OctnicMiniportReset(VOID)
{
    PDRIVER_CONTEXT d = &g_DriverContext;
    ULONG i;
    LONG wasPolling = d->polling;

    OctnicDpcStopAll(d);
    for (i = 0; i < d->numPorts && i < MAX_PORTS; i++) {
        if (d->ports[i].initialized) {
            OctnicWriteCtrl(&d->ports[i], C_HOST_READY, 0);
            if (OctnicInitPort(d, &d->ports[i], i) != STATUS_SUCCESS)
                DPRINT(DBG_ERROR, "port%lu re-init failed", i);
        }
    }
    d->cardLost = FALSE;
    if (wasPolling) OctnicDpcStartAll(d);
}
