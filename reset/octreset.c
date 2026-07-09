// SPDX-License-Identifier: GPL-2.0
/* octreset: write CIU_SOFT_RST to soft-reset the Octeon SoC back to u-boot.
 * Use to recover a wedged PEM (inbound BAR MAbort) without a host power-cycle. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/octeon/octeon.h>
#define CIU_SOFT_RST 0x0001070000000740ull
static int __init r_init(void)
{
	pr_emerg("octreset: CIU_SOFT_RST now\n");
	cvmx_write_csr(CVMX_ADD_IO_SEG(CIU_SOFT_RST), 1);
	return 0;
}
module_init(r_init);
MODULE_LICENSE("GPL");
