// SPDX-License-Identifier: GPL-2.0
/* octconsole: inject a command into the card's U-Boot **PCI console** over the
 * BAR0 window, so the card can be booted WITHOUT the serial cable.
 *
 * U-Boot exposes a fixed bootloader command buffer in low DRAM (the liquidio
 * "BOOTLOADER_PCI_READ_BUFFER" protocol, octeon_console.c):
 *   0x0006c000  owner  (be32): HOST=2 (u-boot ready for a cmd), OCTEON=1 (cmd posted)
 *   0x0006c004  len    (be32): command length
 *   0x0006c008  data   (bytes): command string
 * Handshake: wait owner==HOST -> write data, write len, write owner=OCTEON ->
 * wait owner==HOST (accepted). All scalar fields are big-endian in card memory.
 *
 * Values written through the window go out via writel (LE) and are read back by
 * the card as big-endian, so we pre-swap with cpu_to_be32(): the bytes land in
 * card memory in BE order. Same reasoning for reads (compare against cpu_to_be32).
 *
 * Reuses the CN6XXX BAR0 window mechanism from octwin.c. DRAM addresses use the
 * raw window (no CSR region OR). Load with cmd="..." to post one command. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>

#define WIN_WR_ADDR64  0x00
#define WIN_WR_DATA_LO 0x20
#define WIN_WR_DATA_HI 0x24
#define WIN_WR_MASK    0x30
#define WIN_RD_ADDR_LO 0x10
#define WIN_RD_ADDR_HI 0x14
#define WIN_RD_DATA_LO 0x40

#define BUF_OWNER 0x0006c000ull
#define BUF_LEN   0x0006c004ull
#define BUF_DATA  0x0006c008ull
#define OWNER_OCTEON 1u
#define OWNER_HOST   2u
#define MAX_CMD 247

static unsigned long bar0 = 0xf8000000;
module_param(bar0, ulong, 0444);
static char *cmd =
	"bootoctlinux 0x20010000 numcores=8 endbootargs console=ttyS0,115200 octeon-ethernet.receive_group_order=3";
module_param(cmd, charp, 0444);
static int wait_hundredths = 1500;	/* how long to wait for u-boot ready (x10ms) */
module_param(wait_hundredths, int, 0444);

static void __iomem *b0;

static u64 win_rd(u64 addr)
{
	writel((u32)(addr >> 32), b0 + WIN_RD_ADDR_HI);	/* raw: DRAM, no region OR */
	readl(b0 + WIN_RD_ADDR_HI);
	writel((u32)addr, b0 + WIN_RD_ADDR_LO);		/* LSB triggers read */
	readl(b0 + WIN_RD_ADDR_LO);
	return readq(b0 + WIN_RD_DATA_LO);
}
/* masked 8-byte write at aligned addr: mask selects bytes (bit i => byte i) */
static void win_wr_masked(u64 addr8, u64 mask, u32 lo, u32 hi)
{
	writeq(mask, b0 + WIN_WR_MASK);
	writeq(addr8, b0 + WIN_WR_ADDR64);
	writel(hi, b0 + WIN_WR_DATA_HI);
	readl(b0 + WIN_WR_DATA_HI);
	writel(lo, b0 + WIN_WR_DATA_LO);		/* LSB dword triggers */
	readl(b0 + WIN_WR_DATA_LO);
}

static u32 read_owner(void)
{
	/* owner is the low dword of the qword at 0x6c000, stored big-endian */
	return be32_to_cpu((__force __be32)(u32)(win_rd(BUF_OWNER) & 0xffffffff));
}

static int wait_owner_host(int hundredths)
{
	while (hundredths-- > 0) {
		if (read_owner() == OWNER_HOST)
			return 0;
		msleep(10);
	}
	return -ETIMEDOUT;
}

static int __init m_init(void)
{
	size_t len = strlen(cmd), i;
	u32 own;

	if (len > MAX_CMD) {
		pr_err("octconsole: cmd too long (%zu > %d)\n", len, MAX_CMD);
		return -EINVAL;
	}
	b0 = ioremap(bar0, 0x4000);
	if (!b0)
		return -ENOMEM;

	own = read_owner();
	pr_info("octconsole: owner=0x%x (HOST=2 means u-boot ready)\n", own);
	if (wait_owner_host(wait_hundredths)) {
		pr_err("octconsole: u-boot never signalled ready (owner=0x%x); is it at the PCI console?\n",
		       read_owner());
		iounmap(b0);
		return -EIO;
	}

	/* write command bytes at 0x6c008 in 8-byte chunks (LE pack -> BE card mem) */
	for (i = 0; i < len; i += 8) {
		u8 chunk[8] = { 0 };
		size_t n = min((size_t)8, len - i);

		memcpy(chunk, cmd + i, n);
		win_wr_masked(BUF_DATA + i, 0xffULL,
			      chunk[0] | chunk[1] << 8 | chunk[2] << 16 | chunk[3] << 24,
			      chunk[4] | chunk[5] << 8 | chunk[6] << 16 | chunk[7] << 24);
	}
	/* len at 0x6c004 = high dword of the 0x6c000 qword (mask 0xf0), big-endian */
	win_wr_masked(BUF_OWNER, 0xf0ULL, 0, (u32)cpu_to_be32((u32)len));
	wmb();
	/* hand the buffer to u-boot: owner=OCTEON at 0x6c000 (low dword, mask 0x0f) */
	win_wr_masked(BUF_OWNER, 0x0fULL, (u32)cpu_to_be32(OWNER_OCTEON), 0);
	wmb();

	if (wait_owner_host(200))
		pr_warn("octconsole: u-boot did not ack cmd (owner=0x%x)\n", read_owner());
	else
		pr_info("octconsole: posted \"%s\"\n", cmd);
	iounmap(b0);
	return 0;
}
static void __exit m_exit(void) { }
module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("inject a command into the Cavium U-Boot PCI console (no serial)");
