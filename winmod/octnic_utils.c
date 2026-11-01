/* SPDX-License-Identifier: GPL-2.0
 * octnic_utils.c — Windows NDIS miniport driver for Cavium CN6640 SNIC10E.
 * Utility functions: NetBufferList pool allocation, cloning, cleanup.
 */

#include "octnic.h"

/* ============================================================
 * NetBufferList pool allocation
 * ============================================================ */
NTSTATUS OctnicAllocateNetBufferListPool(PDRIVER_CONTEXT drvCtx)
{
    NDIS_STATUS status;
    NDIS_NET_BUFFER_LIST_POOL_CONFIGURATION poolConfig;

    DPRINT(DBG_INIT, "OctnicAllocateNetBufferListPool: allocating");

    /* Initialize pool configuration for NDIS 6.x */
    NdisZeroMemory(&poolConfig, sizeof(poolConfig));
    poolConfig.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    poolConfig.Header.Revision = NDIS_NET_BUFFER_LIST_POOL_CONFIGURATION_REVISION_1;
    poolConfig.Header.Size = sizeof(poolConfig);
    poolConfig.fAllocateNetBuffer = TRUE;
    poolConfig.ContextSize = 0;
    poolConfig.NumNetBufferLists = RING_SZ * drvCtx->numPorts * 4;
    poolConfig.PoolTag = 'CINo';

    /* Create the pool — first param is the miniport adapter handle */
    status = NdisMAllocateNetBufferListPool(
        drvCtx->ndisMiniportHandle,
        &poolConfig,
        &drvCtx->ports[0].netBufferListPool
    );

    if (status != NDIS_STATUS_SUCCESS) {
        DPRINT(DBG_ERROR, "OctnicAllocateNetBufferListPool: failed %08X", status);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DPRINT(DBG_INIT, "OctnicAllocateNetBufferListPool: pool created successfully");
    return STATUS_SUCCESS;
}

/* ============================================================
 * NetBufferList pool cleanup
 * ============================================================ */
VOID OctnicFreeNetBufferListPool(PDRIVER_CONTEXT drvCtx)
{
    ULONG i;

    DPRINT(DBG_INIT, "OctnicFreeNetBufferListPool: freeing");

    for (i = 0; i < drvCtx->numPorts && i < MAX_PORTS; i++) {
        if (drvCtx->ports[i].netBufferListPool) {
            NdisMFreeNetBufferListPool(drvCtx->ports[i].netBufferListPool);
            drvCtx->ports[i].netBufferListPool = NULL;
        }
    }

    DPRINT(DBG_INIT, "OctnicFreeNetBufferListPool: freed");
}

/* ============================================================
 * MAC address validation
 * ============================================================ */
BOOLEAN OctnicValidateMacAddress(PVOID macAddress, ULONG length)
{
    if (macAddress == NULL || length != ETH_ALEN) return FALSE;

    PUCHAR mac = (PUCHAR)macAddress;
    BOOLEAN allZero = TRUE;
    for (ULONG i = 0; i < ETH_ALEN; i++) {
        if (mac[i] != 0) { allZero = FALSE; break; }
    }
    return !allZero;
}
