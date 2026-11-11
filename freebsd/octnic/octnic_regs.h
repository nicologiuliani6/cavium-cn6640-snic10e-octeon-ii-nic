/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * octnic_regs.h — register/offset definitions byte-identical to linuxmod/octnic.c
 * All numeric values copied from linuxmod/octnic.c with line references.
 */

#ifndef _OCTNIC_REGS_H_
#define _OCTNIC_REGS_H_

/* PCI vendor/device — linuxmod octnic.c line 654: pci_get_device(0x177d, 0x0092) */
#define OCTNIC_VENDOR_ID	0x177d
#define OCTNIC_DEVICE_ID	0x0092

#define OCTNIC_BAR			2	/* BAR2 = card DRAM window, linuxmod line 661 */
#define OCTNIC_MAX_PORTS		2	/* linuxmod line 37: MAXPORT 2 */
#define OCTNIC_RING_SIZE		128	/* linuxmod line 28: RING_SZ 128 */

/* Shared-memory layout — byte-identical with linuxmod/octnic.c lines 27-36 */
#define OCTNIC_MAGIC			0x4f435348u	/* linuxmod line 27: "OCSH" */
#define OCTNIC_VERSION			2u

#define OCTNIC_CTRL_OFF			0x000000	/* linuxmod line 31 */
#define OCTNIC_TXDESC_OFF		0x001000	/* linuxmod line 32 */
#define OCTNIC_RXDESC_OFF		0x002000	/* linuxmod line 33 */
#define OCTNIC_TXBUF_OFF		0x100000	/* linuxmod line 34 */
#define OCTNIC_RXBUF_OFF		0x280000	/* linuxmod line 35 */
#define OCTNIC_PORT_STRIDE		0x400000	/* linuxmod line 36: 4MB per port */
#define OCTNIC_BUF_SZ			9216	/* linuxmod line 30: BUF_SZ */

/* Control page field offsets (u32) — linuxmod octnic.c lines 40-54 */
#define C_MAGIC			0x00	/* linuxmod line 40: magic number */
#define C_VERSION		0x04	/* linuxmod line 41: protocol version */
#define C_CARD_READY		0x08	/* linuxmod line 42: card NIC ready */
#define C_HOST_READY		0x0c	/* linuxmod line 43: host ready to exchange */
#define C_TX_PROD		0x10	/* linuxmod line 44: card TX producer index */
#define C_TX_CONS		0x14	/* linuxmod line 45: card TX consumer index */
#define C_RX_PROD		0x18	/* linuxmod line 46: card RX producer index */
#define C_RX_CONS		0x1c	/* linuxmod line 47: card RX consumer index */
#define C_HEARTBEAT		0x20	/* linuxmod line 54: card worker0 loop counter */
#define C_DMA_ENABLE		0x24	/* linuxmod line 48: 1 = DMA mode on */
#define C_TX_DMA_BASE		0x28	/* linuxmod line 49: TX DMA base address (u64) */
#define C_RX_DMA_BASE		0x30	/* linuxmod line 50: RX DMA base address (u64) */
#define C_RESV0			0x38	/* linuxmod line 51: card board temp, millidegC */
#define C_RESV1			0x3c	/* linuxmod line 52: card Octeon die temp, millidegC */
#define C_LINK			0x40	/* linuxmod line 53: bit31=published, bit0=SFP+ link up */

/* Phase-bit helpers — log2(RING_SZ) = 7, linuxmod lines 66, 89 */
#define OCTNIC_TX_PHASE(tp)	(((tp) >> 7) & 1u)
#define OCTNIC_RX_PHASE(rc)	(((rc) >> 7) & 1u)

/* HRX mode: card DPI'd 8-byte {len, phase} header before frame data */
#define OCTNIC_HRX_HDR		8	/* linuxmod line 88 */

#endif /* _OCTNIC_REGS_H_ */
