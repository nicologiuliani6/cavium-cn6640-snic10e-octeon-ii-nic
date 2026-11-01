/* SPDX-License-Identifier: GPL-2.0
 * octnic_watchdog.c — Windows NDIS miniport driver for Cavium CN6640 SNIC10E.
 * Card health watchdog via IoAllocateWorkItem (runs at PASSIVE_LEVEL).
 */

#include "octnic.h"

/* ============================================================
 * Card health check — monitors heartbeat, temps, PCIe config
 * ============================================================ */
VOID OctnicCheckCardHealth(PDRIVER_CONTEXT drvCtx)
{
    if (!drvCtx->bar2Mapping) return;

    BOOLEAN cardLost = FALSE;
    ULONG currentHeartbeat;

    /* Check heartbeat from port 0 */
    if (drvCtx->numPorts > 0 && drvCtx->ports[0].initialized) {
        currentHeartbeat = OctnicReadCtrl(&drvCtx->ports[0], C_HEARTBEAT);

        /* Check if heartbeat is advancing */
        if (currentHeartbeat == drvCtx->lastHeartbeat) {
            LARGE_INTEGER currentTime;
            KeQuerySystemTimePrecise(&currentTime);

            /* Check if 10+ seconds have elapsed since last heartbeat change */
            LONGLONG elapsed = currentTime.QuadPart - drvCtx->heartbeatJiffies.QuadPart;
            if (elapsed > 100000000LL * 10) { /* 10 seconds in 100ns ticks */
                cardLost = TRUE;
                DPRINT(DBG_ERROR, "OctnicCheckCardHealth: heartbeat frozen — card hung!");
            }
        } else {
            drvCtx->lastHeartbeat = currentHeartbeat;
            KeQuerySystemTimePrecise(&drvCtx->heartbeatJiffies);
        }
    } else {
        cardLost = TRUE;
    }

    /* Check temperatures from port 0 */
    if (!cardLost && drvCtx->numPorts > 0 && drvCtx->ports[0].initialized) {
        LONG tempMax = 0;
        for (ULONG i = 0; i < 2; i++) {
            ULONG offset = (i == 0) ? C_RESV0 : C_RESV1;
            LONG temp = (LONG)OctnicReadCtrl(&drvCtx->ports[0], offset);
            if (temp > tempMax) tempMax = temp;
        }

        /* Temperature thresholds (millidegC) */
        const LONG TEMP_WARN_MC = 85000;   /* 85°C */
        const LONG TEMP_CRIT_MC = 100000;  /* 100°C */

        if (tempMax >= TEMP_CRIT_MC) {
            DPRINT(DBG_ERROR, "OctnicCheckCardHealth: CRITICAL TEMPERATURE %ld.%01dC",
                   tempMax / 1000, (tempMax % 1000) / 100);
        } else if (tempMax >= TEMP_WARN_MC) {
            DPRINT(DBG_ERROR, "OctnicCheckCardHealth: HIGH TEMPERATURE %ld.%01dC",
                   tempMax / 1000, (tempMax % 1000) / 100);
        }
    }

    /* Update card lost state */
    if (cardLost && !drvCtx->cardLost) {
        drvCtx->cardLost = TRUE;
        DPRINT(DBG_ERROR, "OctnicCheckCardHealth: CARD LOST — disabling all ports");
        for (ULONG i = 0; i < drvCtx->numPorts && i < MAX_PORTS; i++) {
            OctnicWriteCtrl(&drvCtx->ports[i], C_HOST_READY, 0);
        }
    } else if (!cardLost && drvCtx->cardLost) {
        drvCtx->cardLost = FALSE;
        DPRINT(DBG_INIT, "OctnicCheckCardHealth: Card recovered");
        for (ULONG i = 0; i < drvCtx->numPorts && i < MAX_PORTS; i++) {
            if (drvCtx->ports[i].initialized) {
                OctnicWriteCtrl(&drvCtx->ports[i], C_HOST_READY, 1);
            }
        }
    }
}

/* ============================================================
 * Watchdog work item callback — runs periodically to check card health
 *
 * Pattern: IoAllocateWorkItem + IoQueueWorkItem.
 * Since IoQueueWorkItem's context pointer is not accessible from within
 * the callback, we use the global g_DriverContext (already initialized
 * at this point) and the globally stored watchdogWorkItem handle.
 * Runs at PASSIVE_LEVEL.
 * ============================================================ */
static VOID OctnicWatchdogCallback(PWORK_ITEM workItem)
{
    UNREFERENCED_PARAMETER(workItem);

    PDRIVER_CONTEXT drvCtx = &g_DriverContext;

    /* Check card health */
    OctnicCheckCardHealth(drvCtx);

    /* Re-schedule: delay 2 seconds then queue again */
    LARGE_INTEGER delay;
    delay.QuadPart = -20000000LL; /* -2 seconds in 100ns units */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    IoQueueWorkItem(drvCtx->watchdogWorkItem, OctnicWatchdogCallback,
                    DelayedWorkQueue, NULL);
}

/* ============================================================
 * Initialize watchdog — called from OctnicMiniportInitialize
 * ============================================================ */
NTSTATUS OctnicWatchdogInit(PDRIVER_CONTEXT drvCtx)
{
    drvCtx->watchdogWorkItem = IoAllocateWorkItem(drvCtx, OctnicWatchdogCallback);
    if (!drvCtx->watchdogWorkItem) {
        DPRINT(DBG_ERROR, "OctnicWatchdogInit: WorkItem alloc failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Initial queue */
    IoQueueWorkItem(drvCtx->watchdogWorkItem, OctnicWatchdogCallback,
                    DelayedWorkQueue, NULL);

    DPRINT(DBG_INIT, "OctnicWatchdogInit: watchdog scheduled");
    return STATUS_SUCCESS;
}

/* ============================================================
 * Cleanup watchdog — called from OctnicMiniportHalt
 * ============================================================ */
VOID OctnicWatchdogCleanup(PDRIVER_CONTEXT drvCtx)
{
    if (drvCtx->watchdogWorkItem) {
        IoFreeWorkItem(drvCtx->watchdogWorkItem);
        drvCtx->watchdogWorkItem = NULL;
    }

    DPRINT(DBG_INIT, "OctnicWatchdogCleanup: watchdog freed");
}
