/* SPDX-License-Identifier: GPL-2.0
 * octnic_tx.c — TX path: NBL -> CAS slot claim -> PIO write into BAR2 TX buffer.
 * Mirrors linuxmod oct_xmit(): drop on full ring without claiming a slot,
 * data first, then len, then the phase word LAST (card drains by phase).
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
            off = 0;
        } else {
            off -= mlen;
        }
        mdl = mdl->Next;
    }
    return len == 0;
}

static NDIS_STATUS OctnicTransmitFrame(POCT_ADAPTER a, PNET_BUFFER nb)
{
    PPORT_CONTEXT p = a->port;
    ULONG len = NET_BUFFER_DATA_LENGTH(nb);
    ULONG claim, slot;
    volatile struct octshm_desc* desc;
    NDIS_STATUS st = NDIS_STATUS_SUCCESS;

    if (len == 0 || len > BUF_SZ) return NDIS_STATUS_INVALID_LENGTH;

    InterlockedIncrement(&p->users);
    if (!p->armed) {
        InterlockedDecrement(&p->users);
        return NDIS_STATUS_MEDIA_DISCONNECTED;
    }

    /* CAS-claim a slot (parallel TX producers); on full ring DROP without claiming. */
    for (;;) {
        claim = (ULONG)p->txClaim;
        if ((claim - (ULONG)p->txConsCache) >= RING_SZ) {
            ULONG tc = OctnicReadCtrl(p, C_TX_CONS);
            p->txConsCache = (LONG)tc;
            if ((claim - tc) >= RING_SZ) {
                InterlockedIncrement64(&a->txDrops);
                InterlockedDecrement(&p->users);
                return NDIS_STATUS_SUCCESS;          /* tail drop, like Linux */
            }
        }
        if ((ULONG)InterlockedCompareExchange(&p->txClaim, (LONG)(claim + 1), (LONG)claim) == claim)
            break;
    }

    slot = claim & RING_MASK;
    if (!OctnicCopyNb(nb, p->portBase + TXBUF_OFF + (SIZE_T)slot * BUF_SZ, len)) {
        len = 0;                                     /* slot claimed: publish empty frame */
        st = NDIS_STATUS_FAILURE;
    }
    _mm_sfence(); _mm_mfence();                      /* data before len */

    desc = (volatile struct octshm_desc*)(p->portBase + TXDESC_OFF + slot * sizeof(struct octshm_desc));
    desc->len = len;
    _mm_sfence(); _mm_mfence();                      /* len before phase */
    desc->flags = TX_PHASE(claim);                   /* last write marks the slot ready */
    _mm_sfence(); _mm_mfence();

    InterlockedDecrement(&p->users);
    return st;
}

VOID OctnicSendNetBufferLists(NDIS_HANDLE ctx, PNET_BUFFER_LIST nblList,
                              NDIS_PORT_NUMBER port, ULONG sendFlags)
{
    POCT_ADAPTER a = (POCT_ADAPTER)ctx;
    PNET_BUFFER_LIST nbl;
    ULONG completeFlags = NDIS_TEST_SEND_AT_DISPATCH_LEVEL(sendFlags) ?
                          NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0;
    UNREFERENCED_PARAMETER(port);

    for (nbl = nblList; nbl != NULL; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
        PNET_BUFFER nb;
        NDIS_STATUS st = NDIS_STATUS_SUCCESS;

        for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb != NULL; nb = NET_BUFFER_NEXT_NB(nb)) {
            ULONG len = NET_BUFFER_DATA_LENGTH(nb);
            NDIS_STATUS s = a->halting ? NDIS_STATUS_MEDIA_DISCONNECTED : OctnicTransmitFrame(a, nb);
            if (s == NDIS_STATUS_SUCCESS) {
                InterlockedIncrement64(&a->txPkts);
                InterlockedAdd64(&a->txBytes, len);
            } else {
                InterlockedIncrement64(&a->txErrors);
                st = s;
            }
        }
        NET_BUFFER_LIST_STATUS(nbl) = st;
    }

    NdisMSendNetBufferListsComplete(a->handle, nblList, completeFlags);
}
