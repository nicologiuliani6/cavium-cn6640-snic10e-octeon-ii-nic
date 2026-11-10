/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Cavium CN6640 SNIC10E FreeBSD Driver Project
 */

#ifndef _OCTNIC_REGS_H_
#define _OCTNIC_REGS_H_

/* PCI vendor/device — match linuxmod/octnic.c and docs/HARDWARE.md */
#define OCTNIC_VENDOR_ID	0x177d
#define OCTNIC_DEVICE_ID	0x0092

/* IVSHMEM vendor/device for emulation testing (ivshmem-plain on QEMU) */
#define IVSHMEM_VENDOR_ID	0x1af4
#define IVSHMEM_DEVICE_ID	0x1110

#define OCTNIC_BAR			2
#define OCTNIC_MAX_PORTS		2
#define OCTNIC_RING_SIZE		128	/* 128 slots, log2 = 7 */

/* Shared-memory layout — byte-identical with linuxmod/octnic.c */
#define OCTNIC_MAGIC			0x4f435348u	/* "OCSH" */
#define OCTNIC_VERSION			2u

#define OCTNIC_CTRL_OFF			0x000000
#define OCTNIC_TXDESC_OFF		0x001000
#define OCTNIC_RXDESC_OFF		0x002000
#define OCTNIC_TXBUF_OFF		0x100000
#define OCTNIC_RXBUF_OFF		0x280000
#define OCTNIC_PORT_STRIDE		0x400000	/* 4 MB per port */
#define OCTNIC_BUF_SZ			9216

/* Control page field offsets (u32) — from linuxmod/octnic.c lines 40-54 */
#define C_MAGIC			0x00	/* magic number */
#define C_VERSION		0x04	/* protocol version */
#define C_CARD_READY		0x08	/* card NIC ready */
#define C_HOST_READY		0x0c	/* host ready to exchange */
#define C_TX_PROD		0x10	/* card TX producer index */
#define C_TX_CONS		0x14	/* card TX consumer index */
#define C_RX_PROD		0x18	/* card RX producer index */
#define C_RX_CONS		0x1c	/* card RX consumer index */
#define C_HEARTBEAT		0x20	/* card worker0 loop counter */
#define C_DMA_ENABLE		0x24	/* 1 = DMA mode on */
#define C_TX_DMA_BASE		0x28	/* TX DMA base address (u64) */
#define C_RX_DMA_BASE		0x30	/* RX DMA base address (u64) */
#define C_RESV0			0x38	/* card board temp, millidegC */
#define C_RESV1			0x3c	/* card Octeon die temp, millidegC */
#define C_LINK			0x40	/* bit31=published, bit0=SFP+ link up */

/* Handshake offset used by octboot */
#define OCTNIC_HANDSHAKE_OFF	0x000100

/* Phase-bit helpers — log2(RING_SZ) = 7 */
#define TX_PHASE(tp)		(((tp) >> 7) & 1u)
#define RX_PHASE(rc)		(((rc) >> 7) & 1u)

#endif /* _OCTNIC_REGS_H_ */
