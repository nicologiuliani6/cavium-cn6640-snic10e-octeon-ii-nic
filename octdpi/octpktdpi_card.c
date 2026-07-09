// SPDX-License-Identifier: GPL-2.0
/* octpktdpi_card: enable the DPI PACKET-output engine (engine 5) on CN66xx so PKO
 * output to the PCI ports (32-35 / npi0-3) is DMA'd to the host SLI output queue.
 * octdpi_test set DPI_DMA_CONTROL.pkt_en but left DPI_DMA_ENGX_EN(5)=0 (engine off),
 * so the packet path was never active. Enable engine 5 + arm SLI outbound. Then a
 * frame sent out npi0 (PKO port 32) should land in the host octoq_host OQ ring.
 * Best-guess config (CN66xx card->host packet setup is HRM/firmware-only). Read-mostly:
 * enables the engine + prints DPI/SLI state; does NOT itself post a host address, so
 * a stray DMA can only target the SLI OQ ring the host already pinned. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx.h>
#include <asm/octeon/cvmx-fpa.h>

#define IOSEG(a) CVMX_ADD_IO_SEG(a)
#define DPI_CTL           0x0001DF0000000040ull
#define DPI_DMA_CONTROL   0x0001DF0000000048ull
#define DPI_REQ_GBL_EN    0x0001DF0000000050ull
#define DPI_DMA_ENGX_EN(e) (0x0001DF0000000080ull + ((e) & 7) * 8)
#define DPI_ENGX_BUF(e)    (0x0001DF0000000880ull + ((e) & 7) * 8)
#define DPI_NCBX_CFG0     0x0001DF0000000800ull
#define DPI_SLI_PRTX_CFG(p) (0x0001DF0000000900ull + ((p) & 3) * 8)
#define SLI_MEM_ACCESS_CTL 0x00011F00000102F0ull
#define SLI_MEM_SUBIDX(i)  (0x00011F00000100E0ull + ((u64)((i)&31))*16 - 16*12)

static int es = 1;
module_param(es, int, 0444);

static u64 rd(u64 a){ return cvmx_read_csr(IOSEG(a)); }
static void wr(u64 a, u64 v){ cvmx_write_csr(IOSEG(a), v); }

static int __init m_init(void)
{
	int e;
	u64 dc;

	pr_info("octpktdpi: BEFORE CTL=%llx DMA_CTL=%llx REQ_GBL=%llx ENG5_EN=%llx ENG5_BUF=%llx\n",
		rd(DPI_CTL), rd(DPI_DMA_CONTROL), rd(DPI_REQ_GBL_EN),
		rd(DPI_DMA_ENGX_EN(5)), rd(DPI_ENGX_BUF(5)));

	/* global DPI PCIe request enable + NCB outstanding (needed for any egress) */
	wr(DPI_NCBX_CFG0, 0x3full);
	wr(DPI_REQ_GBL_EN, 0xffffffffffffffffull);
	CVMX_SYNCW;

	/* PCIe port sizing (mrrs/mps/molr) so DPI egress TLPs are legal */
	wr(DPI_SLI_PRTX_CFG(0), (32ull << 8) | (1ull << 7) | (1ull << 4) | (1ull << 3) | 3ull);

	/* SLI outbound must be armed (same as octshm/octdpi) for card->host writes */
	wr(SLI_MEM_ACCESS_CTL, 127);
	{ int i; for (i = 12; i < 16; i++)
		wr(0x00011F00000100E0ull + ((u64)(i & 31))*16 - 16*12,
		   (u64)(i - 12) | ((u64)(es & 3) << 34) | ((u64)(es & 3) << 36)); }
	CVMX_SYNCW;

	/* engine FIFO layout: eng0-4 = 2 blks (base 0,2,4,6,8), eng5(packets) = 6 blks base10 */
	for (e = 0; e < 5; e++)
		wr(DPI_ENGX_BUF(e), ((u64)(e * 2) << 4) | 2);
	wr(DPI_ENGX_BUF(5), (10ull << 4) | 6);

	/* KEY: enable engine 5 (packet output). octdpi left this 0 -> packet path dead. */
	for (e = 0; e < 5; e++)
		wr(DPI_DMA_ENGX_EN(e), 0xff);
	wr(DPI_DMA_ENGX_EN(5), 0xff);
	CVMX_SYNCW;

	/* DMA_CONTROL: pkt_hp(57) pkt_en(56) dma_enb[53:48]=0x3f dwb_denb(32) o_es(15) o_mode(14) */
	dc = rd(DPI_DMA_CONTROL);
	dc |= (1ull << 57) | (1ull << 56) | (0x3full << 48) | (1ull << 32)
	    | (1ull << 15) | (1ull << 14);
	wr(DPI_DMA_CONTROL, dc);
	wr(DPI_CTL, 0x3);		/* en=1 clk=1 */
	CVMX_SYNCW;
	mdelay(2);

	pr_info("octpktdpi: AFTER  CTL=%llx DMA_CTL=%llx REQ_GBL=%llx ENG5_EN=%llx PRTX0=%llx\n",
		rd(DPI_CTL), rd(DPI_DMA_CONTROL), rd(DPI_REQ_GBL_EN),
		rd(DPI_DMA_ENGX_EN(5)), rd(DPI_SLI_PRTX_CFG(0)));
	pr_info("octpktdpi: packet engine 5 ENABLED. Now TX a frame out npi0 (PKO port32); host octoq_host should receive.\n");
	return 0;
}
static void __exit m_exit(void) { pr_info("octpktdpi: unloaded (DPI left enabled)\n"); }
module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Enable CN66xx DPI packet-output engine for PKO->host");
