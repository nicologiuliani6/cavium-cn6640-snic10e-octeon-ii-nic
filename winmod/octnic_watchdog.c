/* SPDX-License-Identifier: GPL-2.0
 * octnic_watchdog.c — 1 Hz per-adapter timer (DISPATCH_LEVEL, BAR reads only).
 *
 * Job: find BAR2 (port 0), arm the port when the card's shared-memory block is
 * alive, disarm + drop link when the card resets/hangs (heartbeat frozen / magic
 * gone), follow the SFP+ link bit, watch temperatures. Mirrors Linux wd_fn().
 */

#include "octnic.h"

static VOID OctnicWdTick(POCT_ADAPTER a)
{
    PPORT_CONTEXT p = a->port;
    BOOLEAN want = FALSE;

    if (a->halting) return;

    InterlockedIncrement(&p->users);            /* pin BAR against unmap in halt */
    if (g_Card.ready && p->portBase != NULL) {
        LARGE_INTEGER now;
        ULONG magic = OctnicReadCtrl(p, C_MAGIC);
        ULONG hb = OctnicReadCtrl(p, C_HEARTBEAT);
        BOOLEAN dead = FALSE;

        KeQuerySystemTimePrecise(&now);
        if (!a->hbValid) {
            a->lastHb = hb; a->hbTime = now; a->hbValid = TRUE; a->hbAlive = 0;
        } else if (hb != a->lastHb) {
            a->lastHb = hb; a->hbTime = now; if (a->hbAlive < 1000) a->hbAlive++;
        } else if (now.QuadPart - a->hbTime.QuadPart > WD_DEAD_100NS) {
            dead = TRUE;
        }
        if (magic != OCTSHM_MAGIC) dead = TRUE;     /* BARs gone / card reset */

        if (p->armed) {
            if (dead) {
                OctnicPortDisarm(p, FALSE);
                InterlockedExchange(&a->cardLost, 1);
                a->hbValid = FALSE; a->hbAlive = 0;
                DPRINT(0, "oct%lu CARD LOST (heartbeat frozen / magic gone)", a->portIdx);
            }
        } else if (!dead && a->hbAlive >= 1) {
            if (OctnicPortArm(p)) {
                InterlockedExchange(&a->cardLost, 0);
                DPRINT(0, "oct%lu armed", a->portIdx);
            }
        }

        if (p->armed) {
            ULONG lk = OctnicReadCtrl(p, C_LINK);
            want = (lk & 0x80000000u) ? ((lk & 1u) != 0) : TRUE;
            if (a->isOwner) {
                LONG t0 = (LONG)OctnicReadCtrl(p, C_RESV0), t1 = (LONG)OctnicReadCtrl(p, C_RESV1);
                LONG tmax = t0 > t1 ? t0 : t1;
                if (tmax >= TEMP_WARN_MC && !a->tempWarned) {
                    a->tempWarned = TRUE;
                    DPRINT(0, "card temperature high: %ld.%ld C", tmax / 1000, (tmax % 1000) / 100);
                } else if (tmax < TEMP_WARN_MC - 5000) {
                    a->tempWarned = FALSE;
                }
            }
        }
    } else {
        a->hbValid = FALSE; a->hbAlive = 0;
    }
    InterlockedDecrement(&p->users);

    OctnicIndicateLink(a, want);
}

static VOID OctnicWatchdogTimer(PVOID s1, PVOID ctx, PVOID s2, PVOID s3)
{
    POCT_ADAPTER a = (POCT_ADAPTER)ctx;
    UNREFERENCED_PARAMETER(s1);
    UNREFERENCED_PARAMETER(s2);
    UNREFERENCED_PARAMETER(s3);

    if (InterlockedExchange(&a->wdBusy, 1)) return;
    OctnicWdTick(a);
    InterlockedExchange(&a->wdBusy, 0);
}

NDIS_STATUS OctnicWatchdogInit(POCT_ADAPTER a)
{
    NDIS_TIMER_CHARACTERISTICS tc;
    LARGE_INTEGER due;

    RtlZeroMemory(&tc, sizeof(tc));
    tc.Header.Type     = NDIS_OBJECT_TYPE_TIMER_CHARACTERISTICS;
    tc.Header.Revision = NDIS_TIMER_CHARACTERISTICS_REVISION_1;
    tc.Header.Size     = NDIS_SIZEOF_TIMER_CHARACTERISTICS_REVISION_1;
    tc.AllocationTag   = OCTNIC_TAG;
    tc.TimerFunction   = OctnicWatchdogTimer;
    tc.FunctionContext = a;

    if (NdisAllocateTimerObject(a->handle, &tc, &a->wdTimer) != NDIS_STATUS_SUCCESS)
        return NDIS_STATUS_RESOURCES;

    due.QuadPart = -(LONGLONG)WD_PERIOD_MS * 10000;
    NdisSetTimerObject(a->wdTimer, due, WD_PERIOD_MS, NULL);
    return NDIS_STATUS_SUCCESS;
}

VOID OctnicWatchdogCleanup(POCT_ADAPTER a)
{
    LARGE_INTEGER d;

    if (a->wdTimer) {
        NdisCancelTimerObject(a->wdTimer);
        d.QuadPart = -100000LL;                                  /* 10 ms */
        while (a->wdBusy) KeDelayExecutionThread(KernelMode, FALSE, &d);
        NdisFreeTimerObject(a->wdTimer);
        a->wdTimer = NULL;
    }
}
