/* SPDX-License-Identifier: GPL-2.0
 * octnic_tx.c — TX path: NBL -> CAS slot claim -> PIO write into BAR2 TX buffer.
 */

#include "octnic.h"

/* Copy a NET_BUFFER's payload (MDL chain) to dst. */
static BOOLEAN OctnicCopyNb(PNET_BUFFER nb, PUCHAR dst, ULONG len)
{
    PMDL mdl = NET_BUFFER_CURRENT_MDL(nb);
    ULONG off = NET_BUFFER_CURRENT_MDL_OFFSET(nb);

    while (len > 0 && mdl != NULL) {
        PVOID va;
        ULONG mlen, n;
        NdisQueryMdl(mdl, &va, &mlen, NormalPagePriority | MdlMappingNoExecute);
        if (va == NULL) return FALSE;
        if (mlen > off) {
            n = mlen - off;
            if (n > len) n = len;
            RtlCopyMemory(dst, (PUCHAR)va + off, n);
            dst += n;
            len -= n;
        }
        off = (off > mlen) ? off - mlen : 0;
        mdl = mdl->Next;
    }
    return len == 0;
}

static NDIS_STATUS OctnicTransmitFrame(PPORT_CONTEXT p, PNET_BUFFER nb)
{
    ULONG len = NET_BUFFER_DATA_LENGTH(nb);
    ULONG claim, slot;
    volatile struct octshm_desc* desc;

    if (len == 0 || len > BUF_SZ) return NDIS_STATUS_INVALID_LENGTH;

    /* CAS-claim a slot (parallel TX producers) */
    for (;;) {
        claim = p->txClaim;
        if ((claim - p->txConsCache) >= RING_SZ) {
            ULONG tc = OctnicReadCtrl(p, C_TX_CONS);
            p->txConsCache = tc;
            if ((claim - tc) >= RING_SZ) return NDIS_STATUS_RESOURCES;   /* ring full */
        }
        if ((ULONG)InterlockedCompareExchange((volatile LONG*)&p->txClaim,
                                              (LONG)(claim + 1), (LONG)claim) == claim)
            break;
    }

    slot = claim & RING_MASK;
    if (!OctnicCopyNb(nb, p->portBase + TXBUF_OFF + (SIZE_T)slot * BUF_SZ, len)) {
        /* slot already claimed: publish an empty frame so the card does not stall */
        len = 0;
    }
    _mm_mfence();

    desc = (volatile struct octshm_desc*)(p->portBase + TXDESC_OFF + slot * sizeof(struct octshm_desc));
    desc->len = len;
    _mm_mfence();
    desc->flags = TX_PHASE(claim);       /* last write marks the slot ready */
    _mm_mfence();

    p->txProdShadow = claim + 1;
    return len ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE;
}

VOID OctnicSendNetBufferLists(NDIS_HANDLE ctx, PNET_BUFFER_LIST nblList,
                              NDIS_PORT_NUMBER port, ULONG sendFlags)
{
    PDRIVER_CONTEXT d = &g_DriverContext;
    PNET_BUFFER_LIST nbl;
    ULONG completeFlags = NDIS_TEST_SEND_AT_DISPATCH_LEVEL(sendFlags) ?
                          NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0;
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(port);

    for (nbl = nblList; nbl != NULL; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
        PNET_BUFFER nb;
        NDIS_STATUS st = NDIS_STATUS_SUCCESS;

        for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb != NULL; nb = NET_BUFFER_NEXT_NB(nb)) {
            ULONG len = NET_BUFFER_DATA_LENGTH(nb);
            NDIS_STATUS s = (d->cardLost || !d->ports[0].initialized) ?
                            NDIS_STATUS_MEDIA_DISCONNECTED :
                            OctnicTransmitFrame(&d->ports[0], nb);
            if (s == NDIS_STATUS_SUCCESS) {
                InterlockedIncrement64(&d->txPkts);
                InterlockedAdd64(&d->txBytes, len);
            } else {
                InterlockedIncrement64(&d->txErrors);
                st = s;
            }
        }
        NET_BUFFER_LIST_STATUS(nbl) = st;
    }

    NdisMSendNetBufferListsComplete(d->adapterHandle, nblList, completeFlags);
}

VOID OctnicTxCleanup(PPORT_CONTEXT p)
{
    p->txClaim = 0;
    p->txProdShadow = 0;
    p->txConsCache = 0;
    OctnicWriteCtrl(p, C_HOST_READY, 0);
}
