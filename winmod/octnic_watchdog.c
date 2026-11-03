/* SPDX-License-Identifier: GPL-2.0
 * octnic_watchdog.c — card health watchdog (NDIS timer, DISPATCH_LEVEL, BAR reads only).
 */

#include "octnic.h"

#define WD_PERIOD_MS      2000
#define WD_DEAD_100NS     (10LL * 10000000LL)    /* 10 s without heartbeat */
#define TEMP_WARN_MC      85000
#define TEMP_CRIT_MC      100000

static VOID OctnicCheckCardHealth(PDRIVER_CONTEXT d)
{
    PPORT_CONTEXT p = &d->ports[0];
    BOOLEAN lost = FALSE;
    ULONG hb;
    LARGE_INTEGER now;
    LONG i, tmax = 0;

    if (!d->bar2Mapping || !p->initialized) return;

    KeQuerySystemTimePrecise(&now);
    hb = OctnicReadCtrl(p, C_HEARTBEAT);
    if (hb != d->lastHeartbeat) {
        d->lastHeartbeat = hb;
        d->heartbeatTime = now;
    } else if (now.QuadPart - d->heartbeatTime.QuadPart > WD_DEAD_100NS) {
        lost = TRUE;
    }
    if (!d->bar2Mapping || OctnicReadCtrl(p, C_MAGIC) != OCTSHM_MAGIC) lost = TRUE;   /* BARs gone / reset */

    if (!lost) {
        for (i = 0; i < 2; i++) {
            LONG t = (LONG)OctnicReadCtrl(p, i == 0 ? C_RESV0 : C_RESV1);
            if (t > tmax) tmax = t;
        }
        if (tmax >= TEMP_CRIT_MC)
            DPRINT(DBG_ERROR, "CRITICAL card temperature %ld.%ldC", tmax / 1000, (tmax % 1000) / 100);
        else if (tmax >= TEMP_WARN_MC)
            DPRINT(DBG_ERROR, "high card temperature %ld.%ldC", tmax / 1000, (tmax % 1000) / 100);
    }

    if (lost && !d->cardLost) {
        d->cardLost = TRUE;
        DPRINT(DBG_ERROR, "CARD LOST (heartbeat frozen)");
    } else if (!lost && d->cardLost) {
        d->cardLost = FALSE;
        DPRINT(DBG_INIT, "card recovered");
    }
}

static VOID OctnicWatchdogTimer(PVOID s1, PVOID ctx, PVOID s2, PVOID s3)
{
    UNREFERENCED_PARAMETER(s1);
    UNREFERENCED_PARAMETER(s2);
    UNREFERENCED_PARAMETER(s3);
    OctnicCheckCardHealth((PDRIVER_CONTEXT)ctx);
}

NDIS_STATUS OctnicWatchdogInit(PDRIVER_CONTEXT d)
{
    NDIS_TIMER_CHARACTERISTICS tc;
    LARGE_INTEGER due;

    RtlZeroMemory(&tc, sizeof(tc));
    tc.Header.Type     = NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS;
    tc.Header.Revision = NDIS_TIMER_CHARACTERISTICS_REVISION_1;
    tc.Header.Size     = NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1;
    tc.AllocationTag   = OCTNIC_TAG;
    tc.TimerFunction   = OctnicWatchdogTimer;
    tc.FunctionContext = d;

    if (NdisAllocateTimerObject(d->adapterHandle, &tc, &d->watchdogTimer) != NDIS_STATUS_SUCCESS)
        return NDIS_STATUS_RESOURCES;

    d->lastHeartbeat = OctnicReadCtrl(&d->ports[0], C_HEARTBEAT);
    KeQuerySystemTimePrecise(&d->heartbeatTime);

    due.QuadPart = -(LONGLONG)WD_PERIOD_MS * 10000;
    NdisSetTimerObject(d->watchdogTimer, due, WD_PERIOD_MS, NULL);
    return NDIS_STATUS_SUCCESS;
}

VOID OctnicWatchdogCleanup(PDRIVER_CONTEXT d)
{
    if (d->watchdogTimer) {
        NdisCancelTimerObject(d->watchdogTimer);
        NdisFreeTimerObject(d->watchdogTimer);
        d->watchdogTimer = NULL;
    }
}
