/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Cavium CN6640 SNIC10E FreeBSD Driver Project
 */

#ifndef _OCTNIC_REGS_H_
#define _OCTNIC_REGS_H_

#define OCTNIC_VENDOR_ID        0x177d
#define OCTNIC_DEVICE_ID        0x0092

#define OCTNIC_BAR              2
#define OCTNIC_RING_SIZE        128
#define OCTNIC_MAX_PORTS        2

/* Control structure offsets and signatures matching linuxmod/octnic.c */
#define OCTNIC_MAGIC            0x43415649 /* "CAVI" */
#define OCTNIC_REG_MAGIC        0x00
#define OCTNIC_REG_STATUS       0x04
#define OCTNIC_REG_HEARTBEAT    0x08
#define OCTNIC_REG_TEMP         0x0c

#define OCTNIC_STATUS_RUNNING   0x01
#define OCTNIC_STATUS_INIT      0x02
#define OCTNIC_STATUS_ERROR     0x04

#endif /* _OCTNIC_REGS_H_ */
