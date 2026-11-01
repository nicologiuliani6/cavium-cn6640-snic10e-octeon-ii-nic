/* SPDX-License-Identifier: GPL-2.0
 * octnic_rx.c — RX path: KTIMER->KDPC drains the card->host ring into preallocated NBLs.
 * All kernel-mode, DISPATCH_LEVEL; no threads.
 */

#include "octnic.h"

#define RX_BUDGET 256

VOID OctnicDpcTimer(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    PPORT_DPC_CTX c = (PPORT_DPC_CTX)ctx;
    UNREFERENCED_PARAMETER(a1);
    UNREFERENCED_PARAMETER(a2);

    if (!g_DriverContext.polling || g_DriverContext.cardLost) return;
    /* still busy: run again immediately instead of waiting for the next timer tick */
    if (OctnicDrainRing(c->portCtx, c->tid, RX_BUDGET) >= RX_BUDGET)
        KeInsertQueueDpc(dpc, NULL, NULL);
}

ULONG OctnicDrainRing(PPORT_CONTEXT p, ULONG tid, ULONG budget)
{
    PDRIVER_CONTEXT d = &g_DriverContext;
    ULONG stride = d->rxThreads;
    ULONG rc = p->rcPub[tid];
    ULONG n = 0, i, minC;

    while (n < budget) {
        ULONG slot = rc & RING_MASK;
        ULONG len = 0;
        BOOLEAN ready = FALSE;
        PUCHAR src = p->portBase + RXBUF_OFF + (SIZE_T)slot * BUF_SZ;

        if (d->hrx) {
            PUCHAR h = p->portBase + (SIZE_T)slot * BUF_SZ;
            if (*(volatile ULONG*)(h + 4) == RX_PHASE(rc)) {
                _mm_lfence();
                len = *(volatile ULONG*)h;
                ready = TRUE;
            }
        } else if (d->lockfree) {
            volatile struct octshm_desc* desc =
                (volatile struct octshm_desc*)(p->portBase + RXDESC_OFF + slot * sizeof(struct octshm_desc));
            if ((desc->flags & 1u) == RX_PHASE(rc)) {
                len = desc->len;
                ready = TRUE;
            }
        } else if (rc != OctnicReadCtrl(p, C_RX_PROD)) {
            volatile struct octshm_desc* desc =
                (volatile struct octshm_desc*)(p->portBase + RXDESC_OFF + slot * sizeof(struct octshm_desc));
            len = desc->len;
            ready = TRUE;
        }

        if (!ready) break;

        if (len > 0 && len <= BUF_SZ) {
            PRX_BUF rb = OctnicRxBufGet(d);
            if (rb != NULL) {
                PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(rb->nbl);
                RtlCopyMemory(rb->data, src, len);
                NET_BUFFER_CURRENT_MDL(nb) = rb->mdl;
                NET_BUFFER_CURRENT_MDL_OFFSET(nb) = 0;
                NET_BUFFER_DATA_OFFSET(nb) = 0;
                NET_BUFFER_DATA_LENGTH(nb) = len;
                NET_BUFFER_LIST_NEXT_NBL(rb->nbl) = NULL;
                NET_BUFFER_LIST_STATUS(rb->nbl) = NDIS_STATUS_SUCCESS;

                InterlockedIncrement(&d->rxOutstanding);
                InterlockedIncrement64(&d->rxPkts);
                InterlockedAdd64(&d->rxBytes, len);
                NdisMIndicateReceiveNetBufferLists(d->adapterHandle, rb->nbl,
                        NDIS_DEFAULT_PORT_NUMBER, 1, NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);
            } else {
                InterlockedIncrement64(&d->rxNoBuf);
            }
        }

        rc += stride;
        n++;
    }

    p->rcPub[tid] = rc;

    /* publish the minimum consumer across all DPCs */
    minC = p->rcPub[0];
    for (i = 1; i < stride; i++) {
        if ((LONG)(p->rcPub[i] - minC) < 0) minC = p->rcPub[i];
    }
    if (minC != p->rxConsShadow) {
        p->rxConsShadow = minC;
        OctnicWriteCtrl(p, C_RX_CONS, minC);
    }
    return n;
}

/* NDIS hands NBLs back when the stack is done with them */
VOID OctnicReturnNetBufferLists(NDIS_HANDLE ctx, PNET_BUFFER_LIST nblList, ULONG returnFlags)
{
    PDRIVER_CONTEXT d = &g_DriverContext;
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(returnFlags);

    while (nblList != NULL) {
        PNET_BUFFER_LIST next = NET_BUFFER_LIST_NEXT_NBL(nblList);
        OctnicRxBufPut(d, (PRX_BUF)nblList->MiniportReserved[0]);
        InterlockedDecrement(&d->rxOutstanding);
        nblList = next;
    }
}

VOID OctnicRxCleanup(PPORT_CONTEXT p)
{
    PDRIVER_CONTEXT d = &g_DriverContext;
    ULONG i;

    p->rxConsShadow = d->hrx ? 0 : OctnicReadCtrl(p, C_RX_PROD);
    OctnicWriteCtrl(p, C_RX_CONS, p->rxConsShadow);
    for (i = 0; i < MAX_RX_THREADS; i++) p->rcPub[i] = p->rxConsShadow + i;
}
