/* SPDX-License-Identifier: GPL-2.0
 * octnic_rx.c — RX path (hrx mode, identical protocol to linuxmod oct_drain()).
 *
 * The card DMA-writes each frame into host RAM slot s at rxDmaVa + s*BUF_SZ as
 *   [ len:4 ][ phase:4 ][ frame ... ]      (phase written LAST)
 * A slot is ready when its phase word equals RX_PHASE(rc). N threads per port drain
 * disjoint slots (slot % N == tid); the published consumer is the minimum over threads.
 */

#include "octnic.h"

static BOOLEAN OctnicRxAccept(POCT_ADAPTER a, const UCHAR* f, ULONG len, ULONG filter)
{
    if (!filter) return FALSE;                 /* no protocol bound yet */
    if (filter & NDIS_PACKET_TYPE_PROMISCUOUS) return TRUE;
    if (len < 14) return FALSE;
    if (f[0] & 1) {                            /* multicast / broadcast */
        if (f[0] == 0xff && f[1] == 0xff && f[2] == 0xff && f[3] == 0xff && f[4] == 0xff && f[5] == 0xff)
            return (filter & NDIS_PACKET_TYPE_BROADCAST) != 0;
        return (filter & (NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_ALL_MULTICAST)) != 0;
    }
    return (filter & NDIS_PACKET_TYPE_DIRECTED) && RtlEqualMemory(f, a->mac, ETH_ALEN);
}

/* DISPATCH_LEVEL. Returns number of slots consumed. */
static ULONG OctnicDrain(POCT_ADAPTER a, PPORT_CONTEXT p, ULONG tid, ULONG budget)
{
    ULONG stride = p->nThreads;
    ULONG rc = (ULONG)p->rcPub[tid];
    ULONG n = 0, i, minC;
    ULONG filter = a->packetFilter;
    PNET_BUFFER_LIST head = NULL, tail = NULL;
    ULONG cnt = 0;

    while (n < budget) {
        ULONG slot = rc & RING_MASK;
        PUCHAR h = (PUCHAR)p->rxDmaVa + (SIZE_T)slot * BUF_SZ;
        ULONG len;

        if (*(volatile ULONG*)(h + 4) != RX_PHASE(rc)) break;       /* not filled yet */
        _ReadWriteBarrier();                                          /* phase seen => data landed */
        len = *(volatile ULONG*)h;

        if (len > 0 && len <= BUF_SZ - HRX_HDR) {
            const UCHAR* f = h + HRX_HDR;
            if (!OctnicRxAccept(a, f, len, filter)) {
                InterlockedIncrement64(&a->rxFiltered);
            } else {
                PRX_BUF rb = OctnicRxBufGet(a);
                if (rb != NULL) {
                    PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(rb->nbl);
                    RtlCopyMemory(rb->data, f, len);
                    NET_BUFFER_CURRENT_MDL(nb) = rb->mdl;
                    NET_BUFFER_CURRENT_MDL_OFFSET(nb) = 0;
                    NET_BUFFER_DATA_OFFSET(nb) = 0;
                    NET_BUFFER_DATA_LENGTH(nb) = len;
                    NET_BUFFER_LIST_NEXT_NBL(rb->nbl) = NULL;
                    NET_BUFFER_LIST_STATUS(rb->nbl) = NDIS_STATUS_SUCCESS;
                    rb->nbl->SourceHandle = a->handle;

                    if (tail) NET_BUFFER_LIST_NEXT_NBL(tail) = rb->nbl; else head = rb->nbl;
                    tail = rb->nbl;
                    cnt++;
                    InterlockedIncrement64(&a->rxPkts);
                    InterlockedAdd64(&a->rxBytes, len);
                } else {
                    InterlockedIncrement64(&a->rxNoBuf);
                }
            }
        }
        rc += stride;                          /* next slot THIS thread owns */
        n++;
    }
    p->rcPub[tid] = (LONG)rc;

    /* publish consumer = min across threads (signed diff: values stay within a batch) */
    minC = (ULONG)p->rcPub[0];
    for (i = 1; i < stride; i++)
        if ((LONG)((ULONG)p->rcPub[i] - minC) < 0) minC = (ULONG)p->rcPub[i];
    if (n && minC != (ULONG)p->rxConsShadow) {
        p->rxConsShadow = (LONG)minC;
        OctnicWriteCtrl(p, C_RX_CONS, minC);
    }

    if (head != NULL) {
        InterlockedAdd(&a->rxOutstanding, (LONG)cnt);
        NdisMIndicateReceiveNetBufferLists(a->handle, head, NDIS_DEFAULT_PORT_NUMBER, cnt,
                                           NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);
    }
    return n;
}

/* NDIS hands NBLs back when the stack is done with them */
VOID OctnicReturnNetBufferLists(NDIS_HANDLE ctx, PNET_BUFFER_LIST nblList, ULONG returnFlags)
{
    POCT_ADAPTER a = (POCT_ADAPTER)ctx;
    UNREFERENCED_PARAMETER(returnFlags);

    while (nblList != NULL) {
        PNET_BUFFER_LIST next = NET_BUFFER_LIST_NEXT_NBL(nblList);
        NET_BUFFER_LIST_NEXT_NBL(nblList) = NULL;
        OctnicRxBufPut(a, (PRX_BUF)nblList->MiniportReserved[0]);
        InterlockedDecrement(&a->rxOutstanding);
        nblList = next;
    }
}

/* ---- drain threads (one set per port) ---- */
static KSTART_ROUTINE OctnicRxThread;

static VOID OctnicRxThread(PVOID ctx)
{
    RX_THREAD* t = (RX_THREAD*)ctx;
    PPORT_CONTEXT p = (PPORT_CONTEXT)t->port;
    LARGE_INTEGER nap;
    ULONG idle = 0;

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    while (!p->stop) {
        ULONG n = 0;

        if (!p->armed) {
            if (t->tid == 0 && p->idx == 0 && !g_Card.ready) {
                /* PASSIVE_LEVEL: look for the card's shared-memory block (BAR2) */
                InterlockedIncrement(&p->users);
                if (p->adapter != NULL && !p->adapter->halting) OctnicChooseBar(p->adapter);
                InterlockedDecrement(&p->users);
                nap.QuadPart = -10000000LL;                 /* 1 s */
                KeDelayExecutionThread(KernelMode, FALSE, &nap);
                continue;
            }
            nap.QuadPart = -200000LL;                       /* 20 ms */
            KeDelayExecutionThread(KernelMode, FALSE, &nap);
            idle = 0;
            continue;
        }

        InterlockedIncrement(&p->users);
        if (p->armed) {
            POCT_ADAPTER a = p->adapter;
            if (a != NULL && !a->paused && !a->halting) {
                KIRQL old;
                KeRaiseIrql(DISPATCH_LEVEL, &old);
                n = OctnicDrain(a, p, t->tid, RX_BUDGET);
                KeLowerIrql(old);
            }
        }
        InterlockedDecrement(&p->users);

        if (n) {
            idle = 0;
        } else if (++idle < 25000) {
            KeStallExecutionProcessor(2);                   /* hot spin ~50 ms after last frame */
        } else {
            nap.QuadPart = -5000LL;                         /* 0.5 ms */
            KeDelayExecutionThread(KernelMode, FALSE, &nap);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID OctnicRxThreadStart(PPORT_CONTEXT p)
{
    ULONG tid;

    p->stop = 0;
    for (tid = 0; tid < p->nThreads; tid++) {
        HANDLE h = NULL;
        p->thr[tid].port = p;
        p->thr[tid].tid = tid;
        p->thr[tid].thread = NULL;
        if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                            OctnicRxThread, &p->thr[tid]))) {
            ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode,
                                      (PVOID*)&p->thr[tid].thread, NULL);
            ZwClose(h);
        } else {
            DPRINT(0, "port%lu rx thread %lu create failed", p->idx, tid);
        }
    }
}

VOID OctnicRxThreadStop(PPORT_CONTEXT p)
{
    ULONG tid;

    InterlockedExchange(&p->stop, 1);
    for (tid = 0; tid < p->nThreads; tid++) {
        if (p->thr[tid].thread) {
            KeWaitForSingleObject(p->thr[tid].thread, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(p->thr[tid].thread);
            p->thr[tid].thread = NULL;
        }
    }
}
