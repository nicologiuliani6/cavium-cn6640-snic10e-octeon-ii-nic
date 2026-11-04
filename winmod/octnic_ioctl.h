/* SPDX-License-Identifier: GPL-2.0
 * octnic_ioctl.h â€” control device \\.\octnic (used by octboot.ps1 to boot the card
 * from Windows, the equivalent of Linux octboot's /sys/.../resource2 mmap).
 */
#pragma once

#ifndef CTL_CODE
#include <winioctl.h>
#endif

#define OCT_DEVICE_TYPE   0x8F00u
#define IOCTL_OCT_STATUS  CTL_CODE(OCT_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OCT_WRITE   CTL_CODE(OCT_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OCT_READ    CTL_CODE(OCT_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_OCT_SBR     CTL_CODE(OCT_DEVICE_TYPE, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define OCT_OFF_SBR       0xFFFFFFFCu   /* IOCTL_OCT_WRITE at this offset = secondary bus reset (payload OCT_SBR) */
#define OCT_MAX_XFER      (1024u * 1024u)

#pragma pack(push, 4)
typedef struct _OCT_STATUS {
    ULONG ready;        /* BAR2 with octshm magic found */
    ULONG nCand;        /* large memory BARs mapped */
    ULONG candLen;      /* length of candidate 0 */
    ULONG w0;           /* BAR2[0x0]    (magic when OS is up) */
    ULONG w8;           /* BAR2[0x8]    (card_ready) */
    ULONG w100;         /* BAR2[0x100]  (GO!! flag) */
    ULONG w10000;       /* BAR2[0x10000] (image area / marker) */
    ULONG hb;           /* heartbeat of port 0 (when ready) */
    ULONG armed0, armed1;
    ULONG link0, link1;
} OCT_STATUS;

typedef struct _OCT_XFER {      /* IOCTL_OCT_WRITE input header (data follows) / IOCTL_OCT_READ input */
    ULONG offset;
    ULONG len;
} OCT_XFER;
typedef struct _OCT_SBR {       /* IOCTL_OCT_SBR input: PCI location of the card and its upstream bridge */
    ULONG bridgeBus, bridgeSlot;   /* slot = device | (function << 5) */
    ULONG cardBus, cardSlot;
} OCT_SBR;
#pragma pack(pop)
