/* SPDX-License-Identifier: GPL-2.0
 * octnic_tx.c — Windows NDIS miniport driver for Cavium CN6640 SNIC10E.
 * TX path: NDIS SendPackets -> CAS slot claim -> PIO write into BAR2 TX buffer.
 */

#include "octnic.h"

/* ============================================================
 * TX transmit — called by NDIS when packets arrive
 *
 * NDIS 6.x signature: SendPackets(MiniportAdapterContext, PacketList, SendFlags)
 * PacketList is a PNDIS_PACKET_LIST (linked list of NBLs).
 * ============================================================ */
VOID OctnicSendPackets(NDIS_HANDLE miniportContext, PNDIS_PACKET_LIST packetList,
                        NDIS_SEND_PACKETS_FLAGS sendFlags)
{
    PDRIVER_CONTEXT drvCtx = &g_DriverContext;
    ULONG sent = 0;
    ULONG dropped = 0;

    DPRINT(DBG_TX, "OctnicSendPackets: %u packets", packetList->NumberOfPackets);
    UNREFERENCED_PARAMETER(miniportContext);
    UNREFERENCED_PARAMETER(sendFlags);

    /* Iterate over the packet list */
    PNDIS_PACKET_LIST current = packetList;
    while (current != NULL) {
        /* Each entry in the linked list is an NBL */
        PNET_BUFFER_LIST nbl = current->PacketList;

        /* Process each NetBuffer in this NBL */
        for (PNET_BUFFER netBuffer = NdisGetFirstNetBuffer(nbl);
             netBuffer != NULL;
             netBuffer = NdisGetNextNetBuffer(netBuffer)) {

            /* Round-robin across ports */
            PPORT_CONTEXT portCtx = &drvCtx->ports[current->CurrentPacket % drvCtx->numPorts];

            NTSTATUS status = OctnicTransmitFrame(portCtx, netBuffer);
            if (status == STATUS_SUCCESS) {
                sent++;
            } else if (status == STATUS_BUFFER_TOO_SMALL || status == STATUS_TRY_AGAIN) {
                dropped++;
                /* Ring full — free this packet */
                NdisMDiscardNetBufferList(nbl);
                break;
            } else {
                dropped++;
                NdisMDiscardNetBufferList(nbl);
                break;
            }
        }

        current = current->Next;
    }

    DPRINT(DBG_TX, "OctnicSendPackets: sent=%u dropped=%u", sent, dropped);
}

/* ============================================================
 * Transmit a single NetBuffer — CAS slot claim + PIO write
 * ============================================================ */
NTSTATUS OctnicTransmitFrame(PPORT_CONTEXT portCtx, PNET_BUFFER netBuffer)
{
    PDRIVER_CONTEXT drvCtx = (PDRIVER_CONTEXT)((CHAR*)portCtx - FIELD_OFFSET(DRIVER_CONTEXT, ports[0]));
    ULONG len;

    /* Get buffer length */
    len = NdisGetNetBufferLength(netBuffer);
    if (len == 0 || len > BUF_SZ) {
        DPRINT(DBG_ERROR, "OctnicTransmitFrame: invalid len %lu", len);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    /* CAS-claim a slot — parallel TX producer */
    ULONG txClaim = portCtx->txClaim;
    ULONG txConsCache = portCtx->txConsCache;

    /* Check if ring is full */
    if ((txClaim - txConsCache) >= RING_SZ) {
        /* Read current card consumer */
        ULONG tc = OctnicReadCtrl(portCtx, C_TX_CONS);
        portCtx->txConsCache = tc;

        if ((txClaim - tc) >= RING_SZ) {
            DPRINT(DBG_TX, "OctnicTransmitFrame: ring full");
            return STATUS_BUFFER_TOO_SMALL;
        }
    }

    /* Atomic CAS to claim slot */
    LONG oldVal = InterlockedCompareExchange(
        (LONG volatile*)&portCtx->txClaim,
        (LONG)(txClaim + 1),
        (LONG)txClaim
    );

    if (oldVal != (LONG)txClaim) {
        /* CAS failed — retry with new value */
        return STATUS_TRY_AGAIN;
    }

    /* Calculate slot index and actual length */
    ULONG slot = txClaim & RING_MASK;
    ULONG actualLen = (len > BUF_SZ) ? BUF_SZ : len;

    /* PIO write: host writes frame data into BAR2 TX buffer */
    PVOID txBuf = (PBYTE)portCtx->portBase + TXBUF_OFF + slot * BUF_SZ;
    ULONG dataLen = 0;
    UCHAR* data = NdisGetNetBufferContents(netBuffer, &dataLen);
    if (data != NULL && dataLen >= actualLen) {
        RtlCopyMemory(txBuf, data, actualLen);
    }

    /* Memory barrier before writing descriptor */
    _mm_mfence();

    /* Write descriptor: length */
    struct octshm_desc* desc = (struct octshm_desc*)((PBYTE)portCtx->portBase + TXDESC_OFF + slot * 8);
    desc->len = actualLen;

    /* Memory barrier before writing phase */
    _mm_mfence();

    /* Write descriptor: phase (last write to mark slot as ready) */
    desc->flags = TX_PHASE(txClaim);

    /* Final memory barrier */
    _mm_mfence();

    /* Update statistics */
    portCtx->txProdShadow = txClaim + 1;

    DPRINT(DBG_TX, "OctnicTransmitFrame: slot=%lu len=%lu phase=%u",
           slot, actualLen, TX_PHASE(txClaim));

    return STATUS_SUCCESS;
}

/* ============================================================
 * TX cleanup — called during halt or reset
 * ============================================================ */
VOID OctnicTxCleanup(PPORT_CONTEXT portCtx)
{
    DPRINT(DBG_TX, "OctnicTxCleanup: clearing TX state for port%lu", portCtx->idx);

    /* Reset TX producer counter */
    portCtx->txClaim = 0;
    portCtx->txProdShadow = 0;
    portCtx->txConsCache = 0;

    /* Clear host ready to stop TX */
    OctnicWriteCtrl(portCtx, C_HOST_READY, 0);
}
