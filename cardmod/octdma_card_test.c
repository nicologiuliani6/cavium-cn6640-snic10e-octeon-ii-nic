// SPDX-License-Identifier: GPL-2.0
/* octdma_card_test: outbound-DMA proof, card side, instrumented + staged.
 *   mode=0: only program subids + print target VA + read a subid back (NO host
 *           access) — proves setup is safe.
 *   mode=1: mode0 + a single readback of host RAM (outbound READ).
 *   mode=2: mode0 + write DMA_MAGIC to host RAM (outbound WRITE).
 * Prints bracket each step so a hang pinpoints the offending access. Host IOMMU
 * contains any stray transfer (DMAR fault, not corruption).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx.h>

#define SUBIDX(i)   (CVMX_ADD_IO_SEG(0x00011F00000100E0ull) + ((i) & 31) * 16 - 16 * 12)
#define MEM_ACCESS_CTL CVMX_ADD_IO_SEG(0x00011F00000102F0ull)
#define MEM_BASE0   0x00011B0000000000ull
#define MEM_MASK36  0x0000000FFFFFFFFFull
#define DMA_MAGIC   0xD1A5F00DCAFEBABEull

static unsigned long test_busaddr;
module_param(test_busaddr, ulong, 0444);
static int mode;
module_param(mode, int, 0444);

static int __init m_init(void)
{
	u64 va;
	int i;

	if (!test_busaddr) {
		pr_err("octdma_card: pass test_busaddr=0x<hostIOVA>\n");
		return -EINVAL;
	}
	pr_info("octdma_card: START mode=%d busaddr=0x%lx\n", mode, test_busaddr);

	cvmx_write_csr(MEM_ACCESS_CTL, 127);	/* max_word=0, timer=127 (like RC init) */
	for (i = 12; i < 16; i++) {
		u64 v = (u64)(i - 12) | (1ull << 34) | (1ull << 36); /* ba,esw,esr */
		cvmx_write_csr(SUBIDX(i), v);
	}
	CVMX_SYNCW;
	pr_info("octdma_card: subids programmed; subid12=0x%llx\n",
		(unsigned long long)cvmx_read_csr(SUBIDX(12)));

	va = (1ull << 63) | MEM_BASE0 | (test_busaddr & MEM_MASK36);
	pr_info("octdma_card: access_va=0x%llx\n", (unsigned long long)va);

	if (mode >= 1) {
		u64 r;
		pr_info("octdma_card: about to READ host RAM...\n");
		r = *(volatile u64 *)va;
		pr_info("octdma_card: READ ok, got 0x%llx\n", (unsigned long long)r);
	}
	if (mode >= 2) {
		pr_info("octdma_card: about to WRITE host RAM...\n");
		*(volatile u64 *)va = DMA_MAGIC;
		CVMX_SYNCW;
		pr_info("octdma_card: WRITE issued\n");
	}
	pr_info("octdma_card: DONE mode=%d\n", mode);
	return 0;
}

static void __exit m_exit(void)
{
	pr_info("octdma_card: unloaded\n");
}
module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
