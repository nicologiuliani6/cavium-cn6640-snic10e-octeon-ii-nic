// SPDX-License-Identifier: GPL-2.0
/* octwin: host-side arbitrary card-CSR access via the CN6XXX BAR0 window
 * registers (the liquidio mechanism: lio_pci_readq/writeq). Lets the host read
 * (and optionally write) any Octeon CSR on the card regardless of what OS the
 * card runs -- used to reverse-engineer the SLI/DPI config the genuine liquidio
 * firmware sets that OpenWrt does not. Read-only by default; writes gated by the
 * `wr_addr`/`wr_val` params so a dump can't disturb the card. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/pci.h>

/* CN6XXX BAR0 window register offsets */
#define WIN_WR_ADDR64 0x00
#define WIN_RD_ADDR64 0x10
#define WIN_WR_DATA_LO 0x20
#define WIN_WR_DATA_HI 0x24
#define WIN_WR_MASK    0x30
#define WIN_RD_DATA_LO 0x40
#define WIN_RD_DATA_HI 0x44

static unsigned long bar0 = 0xf8000000;
module_param(bar0, ulong, 0444);
static unsigned long long wr_addr;	/* if set, do a window write of wr_val -> wr_addr */
module_param(wr_addr, ullong, 0444);
static unsigned long long wr_val;
module_param(wr_val, ullong, 0444);
static unsigned long long rd_addr;	/* if set, read 8 dwords from here and dump (no write) */
module_param(rd_addr, ullong, 0444);

static void __iomem *b0;

#define WIN_RD_ADDR_LO 0x10
#define WIN_RD_ADDR_HI 0x14
static unsigned int rawrd;	/* 1 = no region OR (DRAM); 0 = CSR IO region */
module_param(rawrd, uint, 0444);
static u64 win_rd(u64 addr)
{
	u32 addrhi = rawrd ? (u32)(addr >> 32) : ((addr >> 32) | 0x00060000);
	writel(addrhi, b0 + WIN_RD_ADDR_HI);
	readl(b0 + WIN_RD_ADDR_HI);			/* ordering */
	writel(addr & 0xffffffff, b0 + WIN_RD_ADDR_LO);	/* LSB triggers read */
	readl(b0 + WIN_RD_ADDR_LO);
	return readq(b0 + WIN_RD_DATA_LO);
}
static void win_wr(u64 addr, u64 val)
{
	writeq(0xffULL, b0 + WIN_WR_MASK);
	writeq(addr, b0 + WIN_WR_ADDR64);
	writel(val >> 32, b0 + WIN_WR_DATA_HI);
	readl(b0 + WIN_WR_DATA_HI);		/* ordering */
	writel(val & 0xffffffff, b0 + WIN_WR_DATA_LO);	/* LSB triggers */
}

/* card CSR addresses (full NCB addresses, window translates) */
#define SLI_CTL_STATUS 0x00011F0000000570ULL
#define SLI_CTL_PORT0  0x00011F0000000050ULL
#define SLI_S2M_PORT0  0x00011F0000003D80ULL
#define DPI_CTL        0x0001DF0000000040ULL
#define DPI_SLI_PRTX0  0x0001DF0000000900ULL	/* verify offset */
#define DPI_REQ_GBL_EN 0x0001DF0000000050ULL

static int __init m_init(void)
{
	b0 = ioremap(bar0, 0x4000);
	if (!b0) return -ENOMEM;

	if (rd_addr) {
		int i;
		for (i = 0; i < 8; i++) {
			u64 a = rd_addr + i * 8;
			u64 v = win_rd(a);
			pr_info("octwin: [0x%llx] = 0x%016llx  '%c%c%c%c%c%c%c%c'\n",
				(u64)a, (u64)v,
				(int)((v>>0)&0xff)  >= 32 && ((v>>0)&0xff)  < 127 ? (int)((v>>0)&0xff)  : '.',
				(int)((v>>8)&0xff)  >= 32 && ((v>>8)&0xff)  < 127 ? (int)((v>>8)&0xff)  : '.',
				(int)((v>>16)&0xff) >= 32 && ((v>>16)&0xff) < 127 ? (int)((v>>16)&0xff) : '.',
				(int)((v>>24)&0xff) >= 32 && ((v>>24)&0xff) < 127 ? (int)((v>>24)&0xff) : '.',
				(int)((v>>32)&0xff) >= 32 && ((v>>32)&0xff) < 127 ? (int)((v>>32)&0xff) : '.',
				(int)((v>>40)&0xff) >= 32 && ((v>>40)&0xff) < 127 ? (int)((v>>40)&0xff) : '.',
				(int)((v>>48)&0xff) >= 32 && ((v>>48)&0xff) < 127 ? (int)((v>>48)&0xff) : '.',
				(int)((v>>56)&0xff) >= 32 && ((v>>56)&0xff) < 127 ? (int)((v>>56)&0xff) : '.');
		}
		return 0;
	}
	if (wr_addr) {
		pr_info("octwin: WRITE 0x%llx -> 0x%llx (was 0x%llx)\n",
			(u64)wr_val, (u64)wr_addr, win_rd(wr_addr));
		win_wr(wr_addr, wr_val);
		pr_info("octwin: now 0x%llx\n", win_rd(wr_addr));
		return 0;
	}
	pr_info("octwin: SLI_CTL_STATUS=0x%llx (expect ~0x82002)\n", win_rd(SLI_CTL_STATUS));
	pr_info("octwin: SLI_CTL_PORT0 =0x%llx\n", win_rd(SLI_CTL_PORT0));
	pr_info("octwin: SLI_S2M_PORT0 =0x%llx\n", win_rd(SLI_S2M_PORT0));
	pr_info("octwin: DPI_CTL       =0x%llx (expect 3)\n", win_rd(DPI_CTL));
	pr_info("octwin: DPI_SLI_PRTX0 =0x%llx (expect ~0x209b)\n", win_rd(DPI_SLI_PRTX0));
	pr_info("octwin: DPI_REQ_GBL_EN=0x%llx (expect 0xff)\n", win_rd(DPI_REQ_GBL_EN));
	return 0;
}
static void __exit m_exit(void) { iounmap(b0); pr_info("octwin: unloaded\n"); }
module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
