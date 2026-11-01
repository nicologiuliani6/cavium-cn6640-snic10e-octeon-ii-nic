/* SPDX-License-Identifier: GPL-2.0
 * octnic_oid.c — Windows NDIS miniport driver for Cavium CN6640 SNIC10E.
 * OID query/set handlers for MAC address, statistics, media status, etc.
 */

#include "octnic.h"

/* ============================================================
 * Default MAC address generation
 * ============================================================ */
static VOID OctnicGenerateMacAddress(PPORT_CONTEXT pPort, PUCHAR macAddress)
{
    PDRIVER_CONTEXT drvCtx = (PDRIVER_CONTEXT)((CHAR*)pPort - FIELD_OFFSET(DRIVER_CONTEXT, ports[0]));

    macAddress[0] = 0x02;  /* Locally administered, unicast */
    macAddress[1] = 0xC4;

    ULONG hash = drvCtx->bar2Phys.LowPart ^ (drvCtx->bar2Phys.HighPart & 0xFFFFFFFF);
    macAddress[2] = (hash >> 24) & 0xFF;
    macAddress[3] = (hash >> 16) & 0xFF;
    macAddress[4] = (hash >> 8) & 0xFF;
    macAddress[5] = (hash + pPort->idx) & 0xFF;

    DPRINT(DBG_INIT, "OctnicGenerateMacAddress: port%lu MAC %02X:%02X:%02X:%02X:%02X:%02X",
           pPort->idx, macAddress[0], macAddress[1], macAddress[2],
           macAddress[3], macAddress[4], macAddress[5]);
}

/* ============================================================
 * OID Query handlers — NDIS 6.x signature:
 * NDIS_STATUS QueryInformation(MiniportAdapterHandle, Oid,
 *                              InformationBuffer, InformationBufferLength,
 *                              BytesWritten, BytesNeeded)
 * ============================================================ */
NDIS_STATUS OctnicQueryInformation(NDIS_HANDLE miniportContext, NDIS_OID oid,
                                   PVOID buffer, ULONG bufferLength,
                                   PULONG bytesWritten, PULONG bytesNeeded)
{
    PDRIVER_CONTEXT drvCtx = &g_DriverContext;

    DPRINT(DBG_TRACE, "OctnicQueryInformation: OID 0x%08X", oid);
    UNREFERENCED_PARAMETER(miniportContext);

    if (bytesWritten) *bytesWritten = 0;
    if (bytesNeeded) *bytesNeeded = 0;

    switch (oid) {
        case OID_802_3_CURRENT_ADDRESS:
            if (bufferLength < ETH_ALEN) {
                *bytesNeeded = ETH_ALEN;
                return NDIS_STATUS_INVALID_LENGTH;
            }
            OctnicGenerateMacAddress(&drvCtx->ports[0], (PUCHAR)buffer);
            if (bytesWritten) *bytesWritten = ETH_ALEN;
            break;

        case OID_802_3_MAX_MULTICAST_LIST:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = 0;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_802_3_MULTICAST_LIST:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = 0;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_802_3_MAXIMUM_SIZE:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = BUF_SZ;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_HARDWARE_STATUS:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = NDIS_HW_STATUS_READY;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_LINK_STATE:
            if (bufferLength < sizeof(NDIS_LINK_STATE)) {
                *bytesNeeded = sizeof(NDIS_LINK_STATE);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            {
                PNDIS_LINK_STATE linkState = (PNDIS_LINK_STATE)buffer;
                ULONG linkCtrl = OctnicReadCtrl(&drvCtx->ports[0], C_LINK);

                linkState->MediaConnectState = (linkCtrl & 0x80000000u) ?
                    (linkCtrl & 1 ? MediaConnectStateConnected : MediaConnectStateDisconnected) :
                    MediaConnectStateConnected;
                linkState->MediaDuplexState = MediaDuplexStateFull;
                linkState->XmitLinkSpeeds = SPEED_10000;
                linkState->RcvLinkSpeeds = SPEED_10000;
                linkState->PauseFunctions = NDIS_PAUSE_FUNCTIONS_BOTH;
                linkState->AutoNegParameters = 0;
            }
            if (bytesWritten) *bytesWritten = sizeof(NDIS_LINK_STATE);
            break;

        case OID_GEN_TRANSMIT_BUFFER_SPACE:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = RING_SZ * BUF_SZ;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_RECEIVE_BUFFER_SPACE:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = RING_SZ * BUF_SZ;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_VENDOR_DESCRIPTION:
            {
                const CHAR* desc = "Cavium CN6640 SNIC10E Shared-Memory NIC";
                ULONG len = (ULONG)(strlen(desc) + 1) * sizeof(CHAR);
                if (bufferLength < len) {
                    *bytesNeeded = len;
                    return NDIS_STATUS_INVALID_LENGTH;
                }
                RtlCopyMemory(buffer, desc, len);
                if (bytesWritten) *bytesWritten = len;
            }
            break;

        case OID_GEN_VENDOR_DRIVER_VERSION:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = 0x0100;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = BUF_SZ;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_LINK_SPEED:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = 10000000000ULL;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_XMIT_OK:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = drvCtx->ports[0].txProdShadow;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_RCV_OK:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = drvCtx->ports[0].rxConsShadow;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_RCV_ERROR:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = 0;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        case OID_GEN_RCV_NO_BUFFER:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            *(PULONG)buffer = 0;
            if (bytesWritten) *bytesWritten = sizeof(ULONG);
            break;

        default:
            DPRINT(DBG_TRACE, "OctnicQueryInformation: unsupported OID 0x%08X", oid);
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    return NDIS_STATUS_SUCCESS;
}

/* ============================================================
 * OID Set handlers — NDIS 6.x signature:
 * NDIS_STATUS SetInformation(MiniportAdapterHandle, Oid,
 *                            InformationBuffer, InformationBufferLength,
 *                            BytesRead, BytesNeeded)
 * ============================================================ */
NDIS_STATUS OctnicSetInformation(NDIS_HANDLE miniportContext, NDIS_OID oid,
                                 PVOID buffer, ULONG bufferLength,
                                 PULONG bytesRead, PULONG bytesNeeded)
{
    UNREFERENCED_PARAMETER(miniportContext);

    DPRINT(DBG_TRACE, "OctnicSetInformation: OID 0x%08X", oid);

    if (bytesRead) *bytesRead = 0;
    if (bytesNeeded) *bytesNeeded = 0;

    switch (oid) {
        case OID_802_3_RCV_ERROR_ALIGNMENT:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (bytesRead) *bytesRead = sizeof(ULONG);
            break;

        case OID_802_3_ONE_ERR_COL:
            if (bufferLength < sizeof(ULONG)) {
                *bytesNeeded = sizeof(ULONG);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (bytesRead) *bytesRead = sizeof(ULONG);
            break;

        case OID_GEN_CURRENT_POWER_STATE:
            if (bufferLength < sizeof(POWER_STATE)) {
                *bytesNeeded = sizeof(POWER_STATE);
                return NDIS_STATUS_INVALID_LENGTH;
            }
            if (bytesRead) *bytesRead = sizeof(POWER_STATE);
            break;

        default:
            DPRINT(DBG_TRACE, "OctnicSetInformation: unsupported OID 0x%08X", oid);
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    return NDIS_STATUS_SUCCESS;
}

/* ============================================================
 * Statistics query
 * ============================================================ */
VOID OctnicQueryStatistics(PDRIVER_CONTEXT drvCtx, PNDIS_MINIPORT_STATISTICS stats)
{
    RtlZeroMemory(stats, sizeof(NDIS_MINIPORT_STATISTICS));

    for (ULONG i = 0; i < drvCtx->numPorts && i < MAX_PORTS; i++) {
        PPORT_CONTEXT portCtx = &drvCtx->ports[i];
        stats->IfInUcastPkts += portCtx->rxConsShadow;
        stats->IfOutUcastPkts += portCtx->txProdShadow;
    }

    DPRINT(DBG_TRACE, "OctnicQueryStatistics: queried stats");
}
