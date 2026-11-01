/* SPDX-License-Identifier: GPL-2.0
 * octnic_utils.c — preallocated RX buffer pool (NBL + MDL + data).
 */

#include "octnic.h"

NDIS_STATUS OctnicRxPoolCreate(PDRIVER_CONTEXT d)
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

    d->nblPool = NdisAllocateNetBufferListPool(d->adapterHandle, &pp);
    if (d->nblPool == NULL) return NDIS_STATUS_RESOURCES;

    d->rxBufs = NdisAllocateMemoryWithTagPriority(d->adapterHandle,
                    RX_POOL_TOTAL * sizeof(RX_BUF), OCTNIC_TAG, NormalPoolPriority);
    if (d->rxBufs == NULL) {
        OctnicRxPoolDestroy(d);
        return NDIS_STATUS_RESOURCES;
    }
    RtlZeroMemory(d->rxBufs, RX_POOL_TOTAL * sizeof(RX_BUF));

    for (i = 0; i < RX_POOL_TOTAL; i++) {
        PRX_BUF rb = &d->rxBufs[i];
        rb->data = NdisAllocateMemoryWithTagPriority(d->adapterHandle, BUF_SZ,
                        OCTNIC_TAG, NormalPoolPriority);
        if (rb->data == NULL) goto fail;
        rb->mdl = NdisAllocateMdl(d->adapterHandle, rb->data, BUF_SZ);
        if (rb->mdl == NULL) goto fail;
        rb->nbl = NdisAllocateNetBufferAndNetBufferList(d->nblPool, 0, 0, rb->mdl, 0, 0);
        if (rb->nbl == NULL) goto fail;
        rb->nbl->MiniportReserved[0] = rb;
        InsertTailList(&d->rxFree, &rb->link);
    }
    return NDIS_STATUS_SUCCESS;

fail:
    OctnicRxPoolDestroy(d);
    return NDIS_STATUS_RESOURCES;
}

VOID OctnicRxPoolDestroy(PDRIVER_CONTEXT d)
{
    ULONG i;
    if (d->rxBufs) {
        for (i = 0; i < RX_POOL_TOTAL; i++) {
            PRX_BUF rb = &d->rxBufs[i];
            if (rb->nbl)  NdisFreeNetBufferList(rb->nbl);
            if (rb->mdl)  NdisFreeMdl(rb->mdl);
            if (rb->data) NdisFreeMemory(rb->data, 0, 0);
        }
        NdisFreeMemory(d->rxBufs, 0, 0);
        d->rxBufs = NULL;
    }
    InitializeListHead(&d->rxFree);
    if (d->nblPool) {
        NdisFreeNetBufferListPool(d->nblPool);
        d->nblPool = NULL;
    }
}

PRX_BUF OctnicRxBufGet(PDRIVER_CONTEXT d)
{
    KIRQL irql;
    PRX_BUF rb = NULL;
    KeAcquireSpinLock(&d->rxLock, &irql);
    if (!IsListEmpty(&d->rxFree)) {
        rb = CONTAINING_RECORD(RemoveHeadList(&d->rxFree), RX_BUF, link);
    }
    KeReleaseSpinLock(&d->rxLock, irql);
    return rb;
}

VOID OctnicRxBufPut(PDRIVER_CONTEXT d, PRX_BUF rb)
{
    KIRQL irql;
    KeAcquireSpinLock(&d->rxLock, &irql);
    InsertTailList(&d->rxFree, &rb->link);
    KeReleaseSpinLock(&d->rxLock, irql);
}
