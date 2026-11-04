/* SPDX-License-Identifier: GPL-2.0
 * octnic_oid.c — NDIS 6 OID request handler (synchronous, per adapter).
 */

#include "octnic.h"

const NDIS_OID g_SupportedOids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID, OID_GEN_VENDOR_DESCRIPTION, OID_GEN_VENDOR_DRIVER_VERSION,
    OID_GEN_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MAC_OPTIONS, OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_LINK_STATE, OID_GEN_STATISTICS,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER,
    OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION, OID_802_3_XMIT_MORE_COLLISIONS
};
const ULONG g_SupportedOidCount = sizeof(g_SupportedOids) / sizeof(g_SupportedOids[0]);

static const CHAR g_VendorDesc[] = "Cavium CN6640 SNIC10E Shared-Memory NIC";

static NDIS_STATUS OctnicQuery(POCT_ADAPTER a, PNDIS_OID_REQUEST req)
{
    NDIS_OID oid = req->DATA.QUERY_INFORMATION.Oid;
    PVOID buf = req->DATA.QUERY_INFORMATION.InformationBuffer;
    ULONG inLen = req->DATA.QUERY_INFORMATION.InformationBufferLength;
    PULONG written = &req->DATA.QUERY_INFORMATION.BytesWritten;
    PULONG needed = &req->DATA.QUERY_INFORMATION.BytesNeeded;

    const VOID* src = NULL;
    ULONG srcLen = 0;
    ULONG ul = 0;
    USHORT us;
    ULONG64 counter = 0;
    BOOLEAN isCounter = FALSE;
    BOOLEAN up = a->linkUp != 0;
    NDIS_LINK_STATE ls;
    NDIS_STATISTICS_INFO si;

    switch (oid) {
    case OID_GEN_SUPPORTED_LIST:
        src = g_SupportedOids; srcLen = g_SupportedOidCount * sizeof(NDIS_OID); break;
    case OID_GEN_HARDWARE_STATUS:      ul = NdisHardwareStatusReady; break;
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:         ul = NdisMedium802_3; break;
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_CURRENT_LOOKAHEAD:    ul = a->lookahead; break;
    case OID_GEN_MAXIMUM_FRAME_SIZE:   ul = OCTNIC_MTU; break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:   ul = OCTNIC_MAX_FRAME; break;
    case OID_GEN_LINK_SPEED:           ul = (ULONG)(OCTNIC_LINK_BPS / 100); break;   /* 100 bps units */
    case OID_GEN_TRANSMIT_BUFFER_SPACE:
    case OID_GEN_RECEIVE_BUFFER_SPACE: ul = RING_SZ * BUF_SZ; break;
    case OID_GEN_VENDOR_ID:            ul = 0x00FFFFFF; break;
    case OID_GEN_VENDOR_DESCRIPTION:   src = g_VendorDesc; srcLen = sizeof(g_VendorDesc); break;
    case OID_GEN_VENDOR_DRIVER_VERSION: ul = 0x00010001; break;
    case OID_GEN_DRIVER_VERSION:
        us = (USHORT)((NDIS_MINIPORT_MAJOR_VERSION << 8) | NDIS_MINIPORT_MINOR_VERSION);
        src = &us; srcLen = sizeof(us); break;
    case OID_GEN_CURRENT_PACKET_FILTER: ul = a->packetFilter; break;
    case OID_GEN_MAC_OPTIONS:
        ul = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
             NDIS_MAC_OPTION_NO_LOOPBACK; break;
    case OID_GEN_MEDIA_CONNECT_STATUS:
        ul = up ? NdisMediaStateConnected : NdisMediaStateDisconnected; break;
    case OID_GEN_LINK_STATE:
        RtlZeroMemory(&ls, sizeof(ls));
        ls.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        ls.Header.Revision = NDIS_LINK_STATE_REVISION_1;
        ls.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
        ls.MediaConnectState = up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
        ls.MediaDuplexState = up ? MediaDuplexStateFull : MediaDuplexStateUnknown;
        ls.XmitLinkSpeed = ls.RcvLinkSpeed = up ? OCTNIC_LINK_BPS : NDIS_LINK_SPEED_UNKNOWN;
        ls.PauseFunctions = NdisPauseFunctionsUnsupported;
        src = &ls; srcLen = sizeof(ls); break;
    case OID_GEN_STATISTICS:
        RtlZeroMemory(&si, sizeof(si));
        si.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        si.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
        si.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
        si.SupportedStatistics = NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV |
                                 NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT |
                                 NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_RCV |
                                 NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_XMIT |
                                 NDIS_STATISTICS_FLAGS_VALID_RCV_DISCARDS |
                                 NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR;
        si.ifHCInUcastPkts = a->rxPkts;
        si.ifHCInUcastOctets = a->rxBytes;
        si.ifHCOutUcastPkts = a->txPkts;
        si.ifHCOutUcastOctets = a->txBytes;
        si.ifInDiscards = a->rxNoBuf;
        si.ifOutErrors = a->txErrors;
        src = &si; srcLen = sizeof(si); break;
    case OID_GEN_XMIT_OK:       isCounter = TRUE; counter = a->txPkts; break;
    case OID_GEN_RCV_OK:        isCounter = TRUE; counter = a->rxPkts; break;
    case OID_GEN_XMIT_ERROR:    isCounter = TRUE; counter = a->txErrors; break;
    case OID_GEN_RCV_ERROR:     isCounter = TRUE; counter = 0; break;
    case OID_GEN_RCV_NO_BUFFER: isCounter = TRUE; counter = a->rxNoBuf; break;
    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS: ul = 0; break;
    case OID_802_3_PERMANENT_ADDRESS:
    case OID_802_3_CURRENT_ADDRESS:    src = a->mac; srcLen = ETH_ALEN; break;
    case OID_802_3_MAXIMUM_LIST_SIZE:  ul = 32; break;
    case OID_802_3_MULTICAST_LIST:     src = &ul; srcLen = 0; break;   /* empty list */
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }

    if (src == NULL) {
        if (isCounter && inLen >= sizeof(ULONG64)) { src = &counter; srcLen = sizeof(ULONG64); }
        else if (isCounter) { ul = (ULONG)counter; src = &ul; srcLen = sizeof(ULONG); }
        else { src = &ul; srcLen = sizeof(ULONG); }
    }

    if (inLen < srcLen) {
        *needed = srcLen;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    if (srcLen) RtlCopyMemory(buf, src, srcLen);
    *written = srcLen;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS OctnicSet(POCT_ADAPTER a, PNDIS_OID_REQUEST req)
{
    NDIS_OID oid = req->DATA.SET_INFORMATION.Oid;
    PVOID buf = req->DATA.SET_INFORMATION.InformationBuffer;
    ULONG inLen = req->DATA.SET_INFORMATION.InformationBufferLength;
    PULONG read = &req->DATA.SET_INFORMATION.BytesRead;
    PULONG needed = &req->DATA.SET_INFORMATION.BytesNeeded;

    switch (oid) {
    case OID_GEN_CURRENT_PACKET_FILTER: {
        ULONG f;
        if (inLen < sizeof(ULONG)) { *needed = sizeof(ULONG); return NDIS_STATUS_INVALID_LENGTH; }
        f = *(PULONG)buf;
        if (f & ~(NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST |
                  NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST |
                  NDIS_PACKET_TYPE_PROMISCUOUS))
            return NDIS_STATUS_INVALID_DATA;
        a->packetFilter = f;
        *read = sizeof(ULONG);
        return NDIS_STATUS_SUCCESS;
    }
    case OID_GEN_CURRENT_LOOKAHEAD:
        if (inLen < sizeof(ULONG)) { *needed = sizeof(ULONG); return NDIS_STATUS_INVALID_LENGTH; }
        a->lookahead = *(PULONG)buf;
        *read = sizeof(ULONG);
        return NDIS_STATUS_SUCCESS;
    case OID_802_3_MULTICAST_LIST:
        if (inLen % ETH_ALEN) return NDIS_STATUS_INVALID_LENGTH;
        *read = inLen;          /* any multicast delivered while MULTICAST filter set */
        return NDIS_STATUS_SUCCESS;
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

NDIS_STATUS OctnicOidRequest(NDIS_HANDLE ctx, PNDIS_OID_REQUEST req)
{
    POCT_ADAPTER a = (POCT_ADAPTER)ctx;

    switch (req->RequestType) {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
        return OctnicQuery(a, req);
    case NdisRequestSetInformation:
        return OctnicSet(a, req);
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}
