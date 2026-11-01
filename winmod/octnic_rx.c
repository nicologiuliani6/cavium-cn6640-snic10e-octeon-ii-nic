/* SPDX-License-Identifier: GPL-2.0
 * octnic_rx.c — Windows NDIS miniport driver for Cavium CN6640 SNIC10E.
 * RX path: kernel-mode DPC timer drains card->host RX ring into NDIS indication frames.
 *
 * All threading is kernel-mode:
 *   - KDPC timer fires at DISPATCH_LEVEL every pollUs ms to drain RX rings
 *   - No user-mode APIs (no _beginthreadex, SleepEx, WaitForSingleObject)
 */

#include "octnic.h"

/* ============================================================
 * DPC timer callback — fires periodically to drain RX ring
 * ============================================================ */
VOID NTAPI OctnicDpcTimer(PKDPC dpc, PVOID deferredContext, PVOID systemArgument1, PVOID systemArgument2)
{
    PPORT_DPC_CTX dpcCtx = (PPORT_DPC_CTX)deferredContext;
    PPORT_CONTEXT portCtx = dpcCtx->portCtx;
    ULONG tid = dpcCtx->tid;

    UNREFERENCED_PARAMETER(systemArgument1);
    UNREFERENCED_PARAMETER(systemArgument2);

    /* Drain the ring */
    ULONG drained = OctnicDrainRing(portCtx, tid, 64);
    (void)drained;
}

/* ============================================================
 * RX drain — called from DPC context to drain RX ring
 * ============================================================ */
ULONG OctnicDrainRing(PPORT_CONTEXT portCtx, ULONG tid, ULONG budget)
{
    PDRIVER_CONTEXT drvCtx = (PDRIVER_CONTEXT)((CHAR*)portCtx - FIELD_OFFSET(DRIVER_CONTEXT, ports[0]));
    ULONG stride = drvCtx->rxThreads;
    ULONG rc = portCtx->rcPub[tid];
    ULONG n = 0;

    DPRINT(DBG_RX, "OctnicDrainRing: port%lu tid%u budget%lu", portCtx->idx, tid, budget);

    while (n < budget) {
        ULONG slot = rc & RING_MASK;
        ULONG len;
        BOOLEAN ready;

        if (drvCtx->hrx) {
            /* hrx mode: descriptor in local host RAM (card DPI'd it) */
            UCHAR* h = (UCHAR*)portCtx->portBase + slot * BUF_SZ;
            ULONG phase = *(volatile ULONG*)(h + 4);

            if (phase != RX_PHASE(rc)) {
                ready = FALSE;
            } else {
                _mm_lfence();
                len = *(volatile ULONG*)h;
                ready = TRUE;
            }
        } else if (drvCtx->lockfree) {
            /* Lockfree mode: check phase bit in descriptor */
            struct octshm_desc* desc = (struct octshm_desc*)((PBYTE)portCtx->portBase + RXDESC_OFF + slot * 8);
            ULONG descFlags = desc->flags;

            if ((descFlags & 1u) != RX_PHASE(rc)) {
                ready = FALSE;
            } else {
                len = desc->len;
                ready = TRUE;
            }
        } else {
            /* Traditional mode: compare rx_cons with rx_prod */
            ULONG rp = OctnicReadCtrl(portCtx, C_RX_PROD);
            if (rc == rp) {
                ready = FALSE;
            } else {
                struct octshm_desc* desc = (struct octshm_desc*)((PBYTE)portCtx->portBase + RXDESC_OFF + slot * 8);
                len = desc->len;
                ready = TRUE;
            }
        }

        if (!ready) break;

        if (len > 0 && len <= BUF_SZ) {
            /* Allocate NetBufferList from pool — NDIS 6.x returns PNET_BUFFER_LIST directly */
            PNET_BUFFER_LIST nbl = NdisMAllocateNetBufferList(
                portCtx->netBufferListPool,
                (NDIS_POOL_INDEX)0,   /* PoolIndex */
                0,                    /* NetBufferContextOffset */
                0,                    /* NetBufferContextSize */
                0                     /* NetBufferListContextSize */
            );

            if (nbl != NULL) {
                PNET_BUFFER netBuffer = NdisGetNetBufferListNetBuffer(nbl);
                if (netBuffer != NULL) {
                    /* Copy frame data from BAR2 RX buffer */
                    PVOID rxData = (PBYTE)portCtx->portBase + RXBUF_OFF + slot * BUF_SZ;
                    ULONG dataLen = 0;
                    UCHAR* dest = NdisGetNetBufferContents(netBuffer, &dataLen);
                    if (dest != NULL && dataLen >= len) {
                        RtlCopyMemory(dest, rxData, len);
                    }

                    /* Indicate received packet to protocol drivers */
                    NdisMIndicateReceiveNetBufferLists(
                        portCtx->miniportHandle,
                        0, nbl, 1, 0, NDIS_INDICATION_PRIORITY_HIGH
                    );
                } else {
                    /* No NetBuffer in the NBL — free it */
                    NdisMDereferenceNetBufferList(nbl);
                }
            } else {
                DPRINT(DBG_ERROR, "OctnicDrainRing: NetBufferList alloc failed");
            }
        }

        /* Advance to next slot owned by this DPC */
        rc += stride;
        n++;
    }

    /* Update per-DPC consumer */
    portCtx->rcPub[tid] = rc;

    /* Publish minimum consumer across all DPCs */
    ULONG minConsumer = portCtx->rcPub[0];
    for (ULONG i = 1; i < stride; i++) {
        if ((LONG)(portCtx->rcPub[i] - minConsumer) < 0) {
            minConsumer = portCtx->rcPub[i];
        }
    }

    if (minConsumer != portCtx->rxConsShadow) {
        portCtx->rxConsShadow = minConsumer;
        OctnicWriteCtrl(portCtx, C_RX_CONS, minConsumer);
    }

    DPRINT(DBG_RX, "OctnicDrainRing: port%lu tid%u drained %u frames",
           portCtx->idx, tid, n);
    return n;
}

/* ============================================================
 * RX cleanup — called during halt or reset
 * ============================================================ */
VOID OctnicRxCleanup(PPORT_CONTEXT portCtx)
{
    PDRIVER_CONTEXT drvCtx = (PDRIVER_CONTEXT)((CHAR*)portCtx - FIELD_OFFSET(DRIVER_CONTEXT, ports[0]));

    DPRINT(DBG_RX, "OctnicRxCleanup: clearing RX state for port%lu", portCtx->idx);

    /* Reset RX consumer */
    portCtx->rxConsShadow = OctnicReadCtrl(portCtx, C_RX_PROD);
    if (drvCtx->hrx) {
        portCtx->rxConsShadow = 0;
    }
    OctnicWriteCtrl(portCtx, C_RX_CONS, portCtx->rxConsShadow);

    /* Reset per-DPC consumers */
    for (ULONG i = 0; i < drvCtx->rxThreads && i < 8; i++) {
        portCtx->rcPub[i] = portCtx->rxConsShadow + i;
    }
}
