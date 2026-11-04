/* SPDX-License-Identifier: GPL-2.0
 * octnic_utils.c — per-adapter preallocated RX buffer pool (NBL + MDL + data).
 */

#include "octnic.h"

NDIS_STATUS OctnicRxPoolCreate(POCT_ADAPTER a)
{
    NET_BUFFER_LIST_POOL_PARAMETERS pp;
    ULONG i;

    RtlZeroMemory(&pp, sizeof(pp));
    pp.Header.Type     = NDIS_OBJECT_TYPE_DEFAULT;
    pp.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.Header.Size     = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.ProtocolId      = NDIS_PROTOCOL_ID_DEFAULT;
    pp.fAllocateNetBuffer = TRUE;
    pp.PoolTag         = OCTNIC_TAG;

    a->nblPool = NdisAllocateNetBufferListPool(a->handle, &pp);
    if (a->nblPool == NULL) return NDIS_STATUS_RESOURCES;

    a->rxBufs = NdisAllocateMemoryWithTagPriority(a->handle,
                    RX_POOL_TOTAL * sizeof(RX_BUF), OCTNIC_TAG, NormalPoolPriority);
    if (a->rxBufs == NULL) {
        OctnicRxPoolDestroy(a);
        return NDIS_STATUS_RESOURCES;
    }
    RtlZeroMemory(a->rxBufs, RX_POOL_TOTAL * sizeof(RX_BUF));

    for (i = 0; i < RX_POOL_TOTAL; i++) {
        PRX_BUF rb = &a->rxBufs[i];
        rb->data = NdisAllocateMemoryWithTagPriority(a->handle, BUF_SZ,
                        OCTNIC_TAG, NormalPoolPriority);
        if (rb->data == NULL) goto fail;
        rb->mdl = NdisAllocateMdl(a->handle, rb->data, BUF_SZ);
        if (rb->mdl == NULL) goto fail;
        rb->nbl = NdisAllocateNetBufferAndNetBufferList(a->nblPool, 0, 0, rb->mdl, 0, 0);
        if (rb->nbl == NULL) goto fail;
        rb->nbl->MiniportReserved[0] = rb;
        rb->nbl->SourceHandle = a->handle;
        InsertTailList(&a->rxFree, &rb->link);
    }
    return NDIS_STATUS_SUCCESS;

fail:
    OctnicRxPoolDestroy(a);
    return NDIS_STATUS_RESOURCES;
}

VOID OctnicRxPoolDestroy(POCT_ADAPTER a)
{
    ULONG i;
    if (a->rxBufs) {
        for (i = 0; i < RX_POOL_TOTAL; i++) {
            PRX_BUF rb = &a->rxBufs[i];
            if (rb->nbl)  NdisFreeNetBufferList(rb->nbl);
            if (rb->mdl)  NdisFreeMdl(rb->mdl);
            if (rb->data) NdisFreeMemory(rb->data, 0, 0);
        }
        NdisFreeMemory(a->rxBufs, 0, 0);
        a->rxBufs = NULL;
    }
    InitializeListHead(&a->rxFree);
    if (a->nblPool) {
        NdisFreeNetBufferListPool(a->nblPool);
        a->nblPool = NULL;
    }
}

PRX_BUF OctnicRxBufGet(POCT_ADAPTER a)
{
    KIRQL irql;
    PRX_BUF rb = NULL;
    KeAcquireSpinLock(&a->rxLock, &irql);
    if (!IsListEmpty(&a->rxFree))
        rb = CONTAINING_RECORD(RemoveHeadList(&a->rxFree), RX_BUF, link);
    KeReleaseSpinLock(&a->rxLock, irql);
    return rb;
}

VOID OctnicRxBufPut(POCT_ADAPTER a, PRX_BUF rb)
{
    KIRQL irql;
    KeAcquireSpinLock(&a->rxLock, &irql);
    InsertTailList(&a->rxFree, &rb->link);
    KeReleaseSpinLock(&a->rxLock, irql);
}
