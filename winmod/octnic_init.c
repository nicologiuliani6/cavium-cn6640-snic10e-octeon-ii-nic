/* SPDX-License-Identifier: GPL-2.0
 * octnic_init.c — Windows NDIS miniport driver for Cavium CN6640 SNIC10E.
 * Hardware discovery, BAR mapping, port init, DPC-based polling management.
 *
 * Threading model (kernel-mode only):
 *   - KDPC timer for RX ring drain (DISPATCH_LEVEL)
 *   - IoAllocateWorkItem for watchdog (PASSIVE_LEVEL)
 *   - KeSetEvent for shutdown signaling
 */

#include "octnic.h"

/* ============================================================
 * PCI device discovery — scan config space for CN6640
 * ============================================================ */
static BOOLEAN OctnicFindCard(OUT PULONG busNumber, OUT PUSHORT deviceNumber, OUT PUSHORT functionNumber)
{
    USHORT bus, dev, func;
    for (bus = 0; bus <= 255; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (func = 0; func < 8; func++) {
                ULONG configValue = 0;
                USHORT vendorId, deviceId;
                configValue = HalGetBusData(PCIConfiguration, bus,
                                            dev << 3 | func, &configValue, sizeof(ULONG));
                if (configValue != sizeof(ULONG)) continue;
                vendorId = (USHORT)(configValue >> 16);
                deviceId = (USHORT)(configValue & 0xFFFF);
                if (vendorId == PCI_VENDOR_ID_CAVIUM && deviceId == PCI_DEVICE_ID_CN6640) {
                    *busNumber = bus;
                    *deviceNumber = (USHORT)dev;
                    *functionNumber = (USHORT)func;
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

/* ============================================================
 * BAR2 mapping — map card DRAM window with non-cached access
 * ============================================================ */
static NTSTATUS OctnicMapBar2(PDRIVER_CONTEXT drvCtx, ULONG bus, USHORT dev, USHORT func)
{
    PHYSICAL_ADDRESS bar2Phys;
    ULONG configValue;

    DPRINT(DBG_INIT, "OctnicMapBar2: mapping BAR2 for bus=%lu dev=%u func=%u",
           bus, dev, func);

    /* Read BAR2 base from PCI config space (offset 0x10 + 4*bar_idx = 0x18) */
    configValue = HalGetBusData(PCIConfiguration, bus,
                                dev << 3 | func, 0x18, &configValue, sizeof(ULONG));
    if (configValue != sizeof(ULONG)) {
        DPRINT(DBG_ERROR, "OctnicMapBar2: failed to read BAR2 config");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    bar2Phys.QuadPart = configValue & 0xFFFFFFF0ULL; /* Mask type bits */
    if (bar2Phys.QuadPart == 0) {
        DPRINT(DBG_ERROR, "OctnicMapBar2: BAR2 base is zero");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    drvCtx->bar2Phys = bar2Phys;
    drvCtx->bar2Size = PORT_STRIDE * drvCtx->numPorts; /* 4MB or 8MB */

    /* Map BAR2 into kernel virtual address space — non-cached for correctness */
    drvCtx->bar2Mapping = MmMapIoSpace(bar2Phys, drvCtx->bar2Size, MmNonCached);
    if (drvCtx->bar2Mapping == NULL) {
        DPRINT(DBG_ERROR, "OctnicMapBar2: MmMapIoSpace failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DPRINT(DBG_INIT, "OctnicMapBar2: BAR2 mapped at %p (phys 0x%llX, size 0x%llx)",
           drvCtx->bar2Mapping, bar2Phys.QuadPart, drvCtx->bar2Size);
    return STATUS_SUCCESS;
}

/* ============================================================
 * Port initialization — verify magic, set up ring state
 * ============================================================ */
static NTSTATUS OctnicInitPort(PDRIVER_CONTEXT drvCtx, PPORT_CONTEXT portCtx, ULONG idx)
{
    PVOID portBase = (PBYTE)drvCtx->bar2Mapping + idx * PORT_STRIDE;
    ULONG magic, version, cardReady;

    DPRINT(DBG_INIT, "OctnicInitPort: initializing port %lu", idx);

    portCtx->portBase = portBase;
    portCtx->idx = idx;
    portCtx->initialized = FALSE;
    portCtx->dmaEnabled = FALSE;
    portCtx->txClaim = 0;
    portCtx->txProdShadow = 0;
    portCtx->txConsCache = 0;
    portCtx->rxConsShadow = 0;
    portCtx->dpcCount = 0;
    RtlZeroMemory(portCtx->rcPub, sizeof(portCtx->rcPub));

    /* Verify magic number */
    magic = OctnicReadCtrl(portCtx, C_MAGIC);
    if (magic != OCTSHM_MAGIC) {
        DPRINT(DBG_ERROR, "OctnicInitPort: port%lu bad magic 0x%08X", idx, magic);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    version = OctnicReadCtrl(portCtx, C_VERSION);
    cardReady = OctnicReadCtrl(portCtx, C_CARD_READY);

    DPRINT(DBG_INIT, "OctnicInitPort: port%lu magic OK ver=%u card_ready=%u",
           idx, version, cardReady);

    if (!cardReady) {
        DPRINT(DBG_ERROR, "OctnicInitPort: port%lu card not ready", idx);
        return STATUS_TIMEOUT;
    }

    /* Initialize shadows to current ring state */
    portCtx->txProdShadow = OctnicReadCtrl(portCtx, C_TX_PROD);
    portCtx->txConsCache = OctnicReadCtrl(portCtx, C_TX_CONS);
    portCtx->rxConsShadow = OctnicReadCtrl(portCtx, C_RX_PROD);

    /* Set host ready flag */
    OctnicWriteCtrl(portCtx, C_HOST_READY, 1);

    /* Initialize RX consumer */
    if (drvCtx->hrx) {
        portCtx->rxConsShadow = 0;
    }
    OctnicWriteCtrl(portCtx, C_RX_CONS, portCtx->rxConsShadow);

    /* Initialize per-DPC RX consumer offsets */
    ULONG i;
    for (i = 0; i < drvCtx->rxThreads && i < 8; i++) {
        portCtx->rcPub[i] = portCtx->rxConsShadow + i;
    }

    portCtx->initialized = TRUE;
    DPRINT(DBG_INIT, "OctnicInitPort: port%lu initialized successfully", idx);
    return STATUS_SUCCESS;
}

/* ============================================================
 * DPC initialization — one KDPC per RX consumer thread
 * ============================================================ */
VOID OctnicDpcInitialize(PPORT_CONTEXT portCtx)
{
    PDRIVER_CONTEXT drvCtx = (PDRIVER_CONTEXT)((CHAR*)portCtx - FIELD_OFFSET(DRIVER_CONTEXT, ports[0]));
    ULONG count = drvCtx->rxThreads;
    if (count > 8) count = 8;

    for (ULONG i = 0; i < count; i++) {
        portCtx->dpcCtx[i].tid = i;
        portCtx->dpcCtx[i].portCtx = portCtx;
        KeInitializeDpc(&portCtx->dpcCtx[i].dpc, OctnicDpcTimer, &portCtx->dpcCtx[i]);
    }
    portCtx->dpcCount = count;
}

/* Start all DPCs for all ports */
VOID OctnicDpcStartAll(PDRIVER_CONTEXT drvCtx)
{
    for (ULONG i = 0; i < drvCtx->numPorts && i < MAX_PORTS; i++) {
        PPORT_CONTEXT portCtx = &drvCtx->ports[i];
        if (!portCtx->initialized) continue;

        OctnicDpcInitialize(portCtx);
        for (ULONG t = 0; t < portCtx->dpcCount; t++) {
            KeSetTimerEx(&portCtx->dpcCtx[t].dpc,
                         LONGLONG_MIN + (drvCtx->pollUs * 10), /* interval in 100ns units */
                         drvCtx->pollUs, /* period in ms */
                         NULL);
        }
    }
}

/* Stop all DPCs for all ports */
VOID OctnicDpcStopAll(PDRIVER_CONTEXT drvCtx)
{
    for (ULONG i = 0; i < drvCtx->numPorts && i < MAX_PORTS; i++) {
        PPORT_CONTEXT portCtx = &drvCtx->ports[i];
        if (!portCtx->initialized) continue;

        for (ULONG t = 0; t < portCtx->dpcCount; t++) {
            KeCancelTimer(&portCtx->dpcCtx[t].dpc);
        }
    }
}

/* ============================================================
 * Miniport Initialize handler — called by NDIS
 * Returns NDIS_STATUS (not NTSTATUS) per NDIS 6.x spec.
 * ============================================================ */
NDIS_STATUS OctnicMiniportInitialize(NDIS_HANDLE miniportContext, NDIS_HANDLE wrapperContext,
                                     NDIS_MINIPORT_INIT_ATTRIBUTES* attributes)
{
    PDRIVER_CONTEXT drvCtx = &g_DriverContext;
    ULONG bus, dev, func;
    ULONG i;

    DPRINT(DBG_INIT, "OctnicMiniportInitialize: starting");
    UNREFERENCED_PARAMETER(miniportContext);
    UNREFERENCED_PARAMETER(wrapperContext);
    UNREFERENCED_PARAMETER(attributes);

    /* Find the Cavium card */
    if (!OctnicFindCard(&bus, &dev, &func)) {
        DPRINT(DBG_ERROR, "OctnicMiniportInitialize: CN6640 not found on PCI");
        return NDIS_STATUS_FAILURE;
    }

    DPRINT(DBG_INIT, "OctnicMiniportInitialize: found card bus=%lu dev=%u func=%u",
           bus, dev, func);

    /* Map BAR2 */
    if (OctnicMapBar2(drvCtx, bus, dev, func) != STATUS_SUCCESS) {
        DPRINT(DBG_ERROR, "OctnicMiniportInitialize: BAR2 mapping failed");
        return NDIS_STATUS_FAILURE;
    }

    /* Initialize each port */
    for (i = 0; i < drvCtx->numPorts && i < MAX_PORTS; i++) {
        if (OctnicInitPort(drvCtx, &drvCtx->ports[i], i) != STATUS_SUCCESS) {
            DPRINT(DBG_ERROR, "OctnicMiniportInitialize: port%lu init failed", i);
            for (ULONG j = 0; j < i; j++) drvCtx->ports[j].initialized = FALSE;
            MmUnmapIoSpace(drvCtx->bar2Mapping, drvCtx->bar2Size);
            drvCtx->bar2Mapping = NULL;
            return NDIS_STATUS_FAILURE;
        }
    }

    /* Allocate NetBufferList pool for RX */
    if (OctnicAllocateNetBufferListPool(drvCtx) != STATUS_SUCCESS) {
        DPRINT(DBG_ERROR, "OctnicMiniportInitialize: NBL pool alloc failed");
        goto cleanup;
    }

    /* Create watchdog (runs at PASSIVE_LEVEL via IoAllocateWorkItem) */
    if (OctnicWatchdogInit(drvCtx) != STATUS_SUCCESS) {
        DPRINT(DBG_ERROR, "OctnicMiniportInitialize: watchdog init failed");
        goto cleanup;
    }

    /* Start DPC-based RX polling */
    OctnicDpcStartAll(drvCtx);

    DPRINT(DBG_INIT, "OctnicMiniportInitialize: completed successfully");
    return NDIS_STATUS_SUCCESS;

cleanup:
    OctnicFreeNetBufferListPool(drvCtx);
    OctnicDpcStopAll(drvCtx);
    OctnicWatchdogCleanup(drvCtx);
    if (drvCtx->bar2Mapping) {
        MmUnmapIoSpace(drvCtx->bar2Mapping, drvCtx->bar2Size);
        drvCtx->bar2Mapping = NULL;
    }
    return NDIS_STATUS_FAILURE;
}

/* ============================================================
 * Miniport Halt handler — cleanup
 * ============================================================ */
VOID OctnicMiniportHalt(NDIS_HANDLE miniportContext, NDIS_HALT_ACTION haltAction)
{
    PDRIVER_CONTEXT drvCtx = &g_DriverContext;
    UNREFERENCED_PARAMETER(miniportContext);
    UNREFERENCED_PARAMETER(haltAction);

    DPRINT(DBG_INIT, "OctnicMiniportHalt: halting driver");

    /* Stop all DPC timers */
    OctnicDpcStopAll(drvCtx);

    /* Clear host ready flag on each port */
    for (ULONG i = 0; i < drvCtx->numPorts && i < MAX_PORTS; i++) {
        if (drvCtx->ports[i].initialized) {
            OctnicWriteCtrl(&drvCtx->ports[i], C_HOST_READY, 0);
        }
    }

    /* Free resources */
    OctnicFreeNetBufferListPool(drvCtx);

    /* Unmap BAR2 */
    if (drvCtx->bar2Mapping) {
        MmUnmapIoSpace(drvCtx->bar2Mapping, drvCtx->bar2Size);
        drvCtx->bar2Mapping = NULL;
    }

    /* Cleanup watchdog (frees work item and context) */
    OctnicWatchdogCleanup(drvCtx);

    DPRINT(DBG_INIT, "OctnicMiniportHalt: completed");
}

/* ============================================================
 * Miniport Reset handler — reset port state
 * NDIS 6.x: VOID ResetHandler(MiniportAdapterContext, MediumChanged)
 * ============================================================ */
VOID OctnicMiniportReset(NDIS_HANDLE miniportContext, BOOLEAN* mediumChanged)
{
    PDRIVER_CONTEXT drvCtx = &g_DriverContext;
    UNREFERENCED_PARAMETER(miniportContext);

    DPRINT(DBG_INIT, "OctnicMiniportReset: resetting");
    *mediumChanged = FALSE;

    /* Stop polling during reset */
    OctnicDpcStopAll(drvCtx);

    /* Re-initialize each port */
    for (ULONG i = 0; i < drvCtx->numPorts && i < MAX_PORTS; i++) {
        if (drvCtx->ports[i].initialized) {
            OctnicWriteCtrl(&drvCtx->ports[i], C_HOST_READY, 0);
            if (OctnicInitPort(drvCtx, &drvCtx->ports[i], i) != STATUS_SUCCESS) {
                DPRINT(DBG_ERROR, "OctnicMiniportReset: port%lu re-init failed", i);
                /* Continue anyway — ports may recover */
            }
        }
    }

    /* Restart polling */
    OctnicDpcStartAll(drvCtx);

    DPRINT(DBG_INIT, "OctnicMiniportReset: completed");
}
