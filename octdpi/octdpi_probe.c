// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/octeon/octeon.h>
#define IOSEG(a) CVMX_ADD_IO_SEG(a)
static int __init p_init(void){
	int i;
	for (i=0;i<8;i++)
		pr_info("octdpi_probe: Q%d IFLIGHT=%llx COUNTS=%llx\n", i,
			cvmx_read_csr(IOSEG(0x0001DF0000000A00ull + i*8)),
			cvmx_read_csr(IOSEG(0x0001DF0000000300ull + i*8)));
	return 0;
}
static void __exit p_exit(void){}
module_init(p_init); module_exit(p_exit);
MODULE_LICENSE("GPL");
