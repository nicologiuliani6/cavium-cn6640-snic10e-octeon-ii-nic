// SPDX-License-Identifier: GPL-2.0
/* octdpi_test: prove the Octeon II DPI DMA engine can do an OUTBOUND transfer
 * (local L2/DRAM -> host PCIe RAM) asynchronously, offloading the copy from the
 * CPU. Isolated from the NIC. Pass test_busaddr=<host IOVA> (from octdma_test on
 * the host). Card builds a DPI DMA instruction, rings the doorbell, and the DPI
 * engine writes DPI_MAGIC into the host buffer. Logs DPI_DMAX_COUNTS + errors.
 *
 * Instruction format (FreeBSD octeon-sdk cvmx-dma-engine.h), OUTBOUND:
 *   word0 header : nfst first(local) ptrs, nlst last(PCIe) ptrs, type=OUTBOUND
 *   word1 internal: {size:13 @40, addr:36 @0}  (local source, phys)
 *   word2 pcie_len: {len0:16 @0}               (fragment length)
 *   word3 pcie_adr: raw 64-bit host bus address (IOVA)
 * doorbell = 4 words.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx-fpa.h>	/* DPI needs an FPA pool to recycle instr chunks */
#include <asm/addrspace.h>		/* CKSEG1ADDR: uncached access */

#define IOSEG(a) CVMX_ADD_IO_SEG(a)
#define DPI_POOL 7			/* FPA pool for DPI instruction chunks (kernel uses 0/1) */
#define BLK      1024			/* FPA block size (bytes): csize=BLK/8, dwb_ichk=BLK/128 */
#define NBLK     16
#define DPI_CTL            0x0001DF0000000040ull
#define DPI_DMA_CONTROL    0x0001DF0000000048ull
#define DPI_DMA_ENGX_EN(e) (0x0001DF0000000080ull + ((e) & 7) * 8)
#define DPI_DMAX_DBELL(q)  (0x0001DF0000000200ull + ((q) & 7) * 8)
#define DPI_DMAX_IBUFF(q)  (0x0001DF0000000280ull + ((q) & 7) * 8)
#define DPI_DMAX_COUNTS(q) (0x0001DF0000000300ull + ((q) & 7) * 8)
#define DPI_ENGX_BUF(e)    (0x0001DF0000000880ull + ((e) & 7) * 8)
#define DPI_DMAX_ERR_RSP   0x0001DF00000003A0ull	/* DPI_DMAX_ERR_RSP_STATUS(0) approx */

#define DPI_MAGIC 0xD1A5F00DCAFEBABEull
#define XFER_LEN  64

static unsigned long test_busaddr;
module_param(test_busaddr, ulong, 0444);
static int q = 7;			/* DMA queue to use (avoid kernel-owned q0) */
module_param(q, int, 0444);
static int ob;				/* 1 = OUTBOUND (L2->host PCIe) to test_busaddr */
module_param(ob, int, 0444);
static int ib;				/* 1 = INBOUND (host PCIe->L2) from test_busaddr */
module_param(ib, int, 0444);
static int lport;			/* OUTBOUND PCIe port (header lport[57:56]) */
module_param(lport, int, 0444);
static int lenhi = 1;			/* 1: len0 @[63:48] (BE), 0: len @[15:0] */
module_param(lenhi, int, 0444);
static int amode;			/* pcie addr form: 0=raw IOVA,1=MEM_BASE0|ba,2=(1<<63)|MEM_BASE0|ba */
module_param(amode, int, 0444);
static int cpurd;			/* 1 = CPU-read host via SLI window (diagnostic, no DPI) */
module_param(cpurd, int, 0444);

static void *chunk;		/* DPI instruction FIFO chunk (from FPA pool) */
static unsigned long fpa_region;	/* backing memory donated to the FPA pool */

static int __init dpi_init(void)
{
	u64 cp, hdr, dma_ctl, cnt0, cnt1;
	int i;

	if (!test_busaddr) {
		pr_err("octdpi: pass test_busaddr=0x<hostIOVA>\n");
		return -EINVAL;
	}

	/* Set up an FPA pool for the DPI: the engine recycles consumed instruction
	 * chunks back to DMA_CONTROL.fpa_que. Without a valid pool it fetches the
	 * instruction but never executes (fcnt stuck). Donate NBLK*BLK bytes of
	 * page-aligned (=>128B aligned) memory as BLK-sized buffers. */
	fpa_region = __get_free_pages(GFP_KERNEL, 2);	/* 16KB */
	if (!fpa_region) return -ENOMEM;
	for (i = 0; i < NBLK; i++)
		cvmx_fpa_free((void *)(fpa_region + (unsigned long)i * BLK), DPI_POOL, 0);
	CVMX_SYNCWS;

	/* allocate the instruction chunk FROM the pool (so the engine may free it) */
	chunk = cvmx_fpa_alloc(DPI_POOL);
	if (!chunk) { free_pages(fpa_region, 2); return -ENOMEM; }
	cp = virt_to_phys(chunk);
	if (cp & 0x7f) { pr_err("octdpi: chunk not 128B aligned 0x%llx\n", cp);
			 free_pages(fpa_region, 2); return -EINVAL; }

	pr_info("octdpi: FPA pool=%d chunk phys=0x%llx busaddr=0x%lx\n",
		DPI_POOL, cp, test_busaddr);
	pr_info("octdpi: KERNEL DPI state: CTL=%llx DMA_CTL=%llx SLI_PRT0=%llx SLI_PRT1=%llx ENGEN0=%llx\n",
		cvmx_read_csr(IOSEG(DPI_CTL)),
		cvmx_read_csr(IOSEG(DPI_DMA_CONTROL)),
		cvmx_read_csr(IOSEG(0x0001DF0000000900ull)),
		cvmx_read_csr(IOSEG(0x0001DF0000000908ull)),
		cvmx_read_csr(IOSEG(DPI_DMA_ENGX_EN(0))));
	{	/* which queues has the kernel configured? (non-zero IBUFF = in use) */
		int k;
		for (k = 0; k < 8; k++)
			pr_info("octdpi:  q%d IBUFF=%llx COUNTS=%llx\n", k,
				cvmx_read_csr(IOSEG(DPI_DMAX_IBUFF(k))),
				cvmx_read_csr(IOSEG(DPI_DMAX_COUNTS(k))));
	}

	/* DPI_REQ_GBL_EN (0x050): global enable for DPI PCIe REQUESTS. If 0, the
	 * engine does INTERNAL (L2) fine but OUTBOUND/INBOUND (need a PCIe request)
	 * are dropped/stall. Kernel leaves it 0 (never uses DPI PCIe). Enable all. */
	pr_info("octdpi: REQ_GBL_EN before=0x%llx NCB_CTL=0x%llx NCBX_CFG0=0x%llx DMA_CTL=0x%llx\n",
		cvmx_read_csr(IOSEG(0x0001DF0000000050ull)),
		cvmx_read_csr(IOSEG(0x0001DF0000000028ull)),
		cvmx_read_csr(IOSEG(0x0001DF0000000800ull)),
		cvmx_read_csr(IOSEG(0x0001DF0000000048ull)));
	/* DPI_NCBX_CFG.molr = max outstanding NCB load requests. If kernel left it 0,
	 * DPI cannot issue outbound NCB transactions -> engine stalls (fcnt stuck).
	 * Set molr max (0x3f). This is never set by Linux (no DPI PCIe DMA use). */
	cvmx_write_csr(IOSEG(0x0001DF0000000800ull), 0x20ull);	/* firmware value */
	CVMX_SYNCW;
	pr_info("octdpi: NCBX_CFG0 after=0x%llx\n",
		cvmx_read_csr(IOSEG(0x0001DF0000000800ull)));
	cvmx_write_csr(IOSEG(0x0001DF0000000050ull), 0xffull);	/* firmware value */
	CVMX_SYNCW;
	pr_info("octdpi: REQ_GBL_EN after=0x%llx\n",
		cvmx_read_csr(IOSEG(0x0001DF0000000050ull)));

	/* DPI PCIe port config (DPI_SLI_PRTX_CFG): mrrs=3, mrrs_lim, mps, mps_lim,
	 * molr=32. This gates DPI's own PCIe TLP egress (separate from CPU
	 * mem-access). Kernel left it 0x2000 (molr only). */
	pr_info("octdpi: PRTX_CFG(0) before=0x%llx\n",
		cvmx_read_csr(IOSEG(0x0001DF0000000900ull)));
	cvmx_write_csr(IOSEG(0x0001DF0000000900ull), 0x2000ull);	/* firmware exact: molr=32 only */
	CVMX_SYNCW;
	pr_info("octdpi: PRTX_CFG(0) after=0x%llx\n",
		cvmx_read_csr(IOSEG(0x0001DF0000000900ull)));

	/* SLI outbound must be armed for any card->host PCIe write (MEM_ACCESS_CTL
	 * + SUBIDX endian), same as the NIC's octdma_setup; DPI TLPs are dropped
	 * without it. */
	cvmx_write_csr(IOSEG(0x00011F00000102F0ull), 127);	/* SLI_MEM_ACCESS_CTL */
	for (i = 12; i < 16; i++)
		cvmx_write_csr(IOSEG(0x00011F00000100E0ull) + ((u64)(i & 31)) * 16 - 16 * 12,
			       (u64)(i - 12) | (1ull << 34) | (1ull << 36));	/* esw|esr */
	CVMX_SYNCW;

	/* cpurd: CPU reads host memory via the SLI window (same addr the NIC writes
	 * work with). Splits "DPI-specific" from "SLI read/completion path broken":
	 * if this returns the host INMAGIC, CPU PCIe reads work and the DPI stall is
	 * DPI-specific; if it hangs/garbage, the SLI read path is broken for all. */
	if (cpurd) {
		u64 va = (1ull << 63) | 0x00011B0000000000ull |
			 ((u64)test_busaddr & 0xFFFFFFFFFull);
		u64 val;
		pr_info("octdpi: cpurd about to read SLI va=0x%016llx\n", va);
		CVMX_SYNC;
		val = *(volatile u64 *)va;
		pr_info("octdpi: CPU host_read via SLI = 0x%016llx (expect ~0xC0FFEE.. or swap)\n",
			val);
		return 0;
	}

	/* --- full SDK-mirror DPI init --- */
	{
		int e;
		/* FIRMWARE engine FIFO buffer distribution (base<<4|blks, total 16 blks):
		 * eng0 base0 blks4, eng1 base4 blks4, eng2 base8 blks2, eng3 base10 blks1,
		 * eng4 base11 blks1, eng5 base12 blks4. */
		static const u32 fw_buf[6] = {0x04, 0x44, 0x82, 0xa1, 0xb1, 0xc4};
		for (e = 0; e < 6; e++)
			cvmx_write_csr(IOSEG(DPI_ENGX_BUF(e)), fw_buf[e]);
		/* instruction FIFO queue: csize=BLK/8 words [61:48], saddr phys[35:7] */
		cvmx_write_csr(IOSEG(DPI_DMAX_IBUFF(q)),
			       ((u64)(BLK / 8) << 48) | (cp & 0x0000000FFFFFFF80ull));
		/* FIRMWARE engine->queue assignment masks (each queue served by one engine):
		 * eng0=q0,5 eng1=q1,6 eng2=q2,7 eng3=q3 eng4=q4 eng5=off. */
		static const u32 fw_en[6] = {0x21, 0x42, 0x84, 0x08, 0x10, 0x00};
		for (e = 0; e < 6; e++)
			cvmx_write_csr(IOSEG(DPI_DMA_ENGX_EN(e)), fw_en[e]);
		/* DMA_CONTROL (cn61xx/cn63xx layout): pkt_hp(57) pkt_en(56)
		 * dma_enb[53:48]=0x1f dwb_denb(32) dwb_ichk[31:23]=BLK/128
		 * fpa_que[22:20]=POOL o_es(15) o_mode(14). fpa_que+dwb are the
		 * missing piece: engine recycles instr chunks to this pool. */
		/* FIRMWARE DMA_CONTROL bits (decoded from 0x051f00010088c000):
		 * commit_mode(58) pkt_en(56) dma_enb=0x1f(48) dwb_denb(32) dwb_ichk(23)
		 * o_add1(19) o_es=1(15) o_mode(14). NO pkt_hp(57) (I had it wrong).
		 * fpa_que=DPI_POOL (firmware used 0; we own pool 7). */
		dma_ctl = cvmx_read_csr(IOSEG(DPI_DMA_CONTROL));
		dma_ctl |= (1ull << 58) | (1ull << 56) | (0x1full << 48)
			 | (1ull << 32) | ((u64)(BLK / 128) << 23)
			 | ((u64)DPI_POOL << 20)
			 | (1ull << 19) | (1ull << 15) | (1ull << 14);
		cvmx_write_csr(IOSEG(DPI_DMA_CONTROL), dma_ctl);
		cvmx_write_csr(IOSEG(DPI_CTL), 0x3);		/* en=1, clk=1 */
		CVMX_SYNCW;
	}

	/* --- INTERNAL DMA using CHUNK-relative addresses (proven DPI-readable,
	 * since the engine successfully fetches instructions from the chunk).
	 * src = chunk+0x100 (magic), dst = chunk+0x200. Isolates FORMAT from
	 * addressing. Instruction still at chunk+0 (first 3 words). --- */
	{
		/* UNCACHED view of the chunk (CKSEG1): writes go straight to DRAM
		 * where the DPI reads them, and reads see what the DPI wrote. Kills
		 * cache-coherency ambiguity in both directions. */
		u64 *cu = (u64 *)(unsigned long)CKSEG1ADDR(cp);
		u64 srcp = cp + 0x100, dstp = cp + 0x200;
		int k;
		for (k = 0; k < XFER_LEN; k += 8) {
			cu[(0x100 + k) / 8] = DPI_MAGIC;
			cu[(0x200 + k) / 8] = 0xAAAAAAAAAAAAAAAAull;	/* sentinel */
		}
		if (ib) {
			/* INBOUND (type=1): host PCIe SOURCE -> L2 DEST. Same layout as
			 * OUTBOUND (first=internal local, last=pcie) but direction is
			 * PCIe->local. local dest = chunk+0x300 (pre-cleared to 0). */
			u64 dl = cp + 0x300;
			int j; for (j = 0; j < XFER_LEN; j += 8) cu[(0x300 + j) / 8] = 0;
			cu[0] = ((u64)1 << 54) | ((u64)(lport & 3) << 56)
				| ((u64)1 << 44) | ((u64)1 << 40);
			cu[1] = ((u64)XFER_LEN << 40) | (dl & 0x0000000FFFFFFFFFull);
			cu[2] = ((u64)XFER_LEN & 0xFFFFull) << 48;	/* len0 @[63:48] */
			cu[3] = (u64)test_busaddr;			/* host source IOVA */
			CVMX_SYNCW;
			pr_info("octdpi: INBOUND host IOVA=0x%lx -> L2 dst=0x%llx\n",
				test_busaddr, dl);
		} else if (ob) {
			/* OUTBOUND (type=0): L2 src -> host PCIe. nfst=1 local ptr,
			 * nlst=1 PCIe ptr = one length word + one raw IOVA word.
			 * doorbell = 4 words. Source = chunk+0x100 (magic). */
			cu[0] = ((u64)0 << 54) | ((u64)(lport & 3) << 56)
				| ((u64)1 << 44) | ((u64)1 << 40);
			cu[1] = ((u64)XFER_LEN << 40) | (srcp & 0x0000000FFFFFFFFFull);
			cu[2] = lenhi ? (((u64)XFER_LEN & 0xFFFFull) << 48)
				      : ((u64)XFER_LEN & 0xFFFFull);
			cu[3] = amode == 2 ? ((1ull << 63) | 0x00011B0000000000ull | ((u64)test_busaddr & 0xFFFFFFFFFull))
			      : amode == 1 ? (0x00011B0000000000ull | ((u64)test_busaddr & 0xFFFFFFFFFull))
			      : (u64)test_busaddr;	/* pcie addr form */
			CVMX_SYNCW;
			pr_info("octdpi: OUTBOUND src=0x%llx -> IOVA=0x%lx lport=%d lenhi=%d\n",
				srcp, test_busaddr, lport, lenhi);
		} else {
			hdr = ((u64)2 << 54) | ((u64)1 << 44) | ((u64)1 << 40);	/* INTERNAL */
			cu[0] = hdr;
			cu[1] = ((u64)XFER_LEN << 40) | (srcp & 0x0000000FFFFFFFFFull);
			cu[2] = ((u64)XFER_LEN << 40) | (dstp & 0x0000000FFFFFFFFFull);
			CVMX_SYNCW;
			pr_info("octdpi: INTERNAL(uncached) src=0x%llx dst=0x%llx cu=%p\n",
				srcp, dstp, cu);
		}
	}

	pr_info("octdpi: regs CTL=%llx DMA_CTL=%llx ENGEN0=%llx ENGBUF0=%llx IBUFF0=%llx\n",
		cvmx_read_csr(IOSEG(DPI_CTL)),
		cvmx_read_csr(IOSEG(DPI_DMA_CONTROL)),
		cvmx_read_csr(IOSEG(DPI_DMA_ENGX_EN(0))),
		cvmx_read_csr(IOSEG(DPI_ENGX_BUF(0))),
		cvmx_read_csr(IOSEG(DPI_DMAX_IBUFF(q))));
	cnt0 = cvmx_read_csr(IOSEG(DPI_DMAX_COUNTS(q)));
	pr_info("octdpi: SLI DATA_OUT_CNT before=0x%llx S2M_PORT0=0x%llx CTL_STATUS=0x%llx\n",
		cvmx_read_csr(IOSEG(0x00011F00000005F0ull)),
		cvmx_read_csr(IOSEG(0x00011F0000003D80ull)),
		cvmx_read_csr(IOSEG(0x00011F0000000570ull)));

	/* ring doorbell: INTERNAL=3 words, OUTBOUND/INBOUND=4 */
	cvmx_write_csr(IOSEG(DPI_DMAX_DBELL(q)), (ob || ib) ? 4 : 3);
	CVMX_SYNCW;
	mdelay(10);
	pr_info("octdpi: SLI DATA_OUT_CNT after=0x%llx (ucnt change => TLP hit SLI outbound)\n",
		cvmx_read_csr(IOSEG(0x00011F00000005F0ull)));
	cnt1 = cvmx_read_csr(IOSEG(DPI_DMAX_COUNTS(q)));	/* immediate */
	mdelay(50);
	if (ib) {	/* INBOUND: L2 dst chunk+0x300 should hold the host magic */
		u64 got = *(u64 *)(unsigned long)CKSEG1ADDR(cp + 0x300);
		pr_info("octdpi: INBOUND L2 dst[0]=0x%016llx %s (host had 0xC0FFEEC0FFEEC0FF)\n",
			got, got ? "*** DPI READ HOST OK ***" : "ZERO (no read)");
	} else {	/* INTERNAL/OUTBOUND: check chunk+0x200 */
		u64 got = *(u64 *)(unsigned long)CKSEG1ADDR(cp + 0x200);
		const char *v = got == DPI_MAGIC ? "*** ENGINE OK ***" :
				got == 0xAAAAAAAAAAAAAAAAull ? "UNTOUCHED (no write)" :
				"WROTE (but stale/coherency)";
		pr_info("octdpi: INTERNAL dst[0]=0x%016llx %s\n", got, v);
	}
	pr_info("octdpi: DBELL rung. COUNTS pre=%llx imm=%llx post=%llx\n",
		cnt0, cnt1, cvmx_read_csr(IOSEG(DPI_DMAX_COUNTS(q))));
	pr_info("octdpi: INT_REG=%llx REQ_ERR_RSP=%llx ERR_RSP0=%llx IFLIGHT0=%llx NADDR0=%llx\n",
		cvmx_read_csr(IOSEG(0x0001DF0000000008ull)),
		cvmx_read_csr(IOSEG(0x0001DF0000000058ull)),
		cvmx_read_csr(IOSEG(0x0001DF0000000A80ull)),
		cvmx_read_csr(IOSEG(0x0001DF0000000A00ull + (u64)q*8)),
		cvmx_read_csr(IOSEG(0x0001DF0000000380ull + (u64)q*8)));
	pr_info("octdpi: PEMX_INT_SUM(0)=0x%llx DPI_PKT_ERR_RSP=0x%llx DPI_CTL=0x%llx IFLIGHT(q)=0x%llx\n",
		cvmx_read_csr(IOSEG(0x00011800C0000428ull)),
		cvmx_read_csr(IOSEG(0x0001DF0000000078ull)),
		cvmx_read_csr(IOSEG(DPI_CTL)),
		cvmx_read_csr(IOSEG(0x0001DF0000000A00ull + (u64)q * 8)));
	pr_info("octdpi: DONE. check host buffer for 0x%016llx (byte-swapped by o_es)\n",
		DPI_MAGIC);
	return 0;
}

static void __exit dpi_exit(void)
{
	/* nothing rings DBELL(0) after unload, so the stale IBUFF is never walked;
	 * leave DPI_CTL alone (don't risk the PCIe/BAR path). */
	free_pages(fpa_region, 2);
	pr_info("octdpi: unloaded\n");
}
module_init(dpi_init);
module_exit(dpi_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Octeon DPI DMA engine outbound test");
