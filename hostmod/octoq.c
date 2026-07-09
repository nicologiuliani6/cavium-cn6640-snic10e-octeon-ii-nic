// SPDX-License-Identifier: GPL-2.0
/* octoq: host side of the SLI packet OUTPUT queue (card->host hardware DMA) for
 * Path A. The host allocates a descriptor ring + data buffers in host RAM,
 * programs the card's SLI_OQ registers (BAR0 writes), and the card's SLI output
 * engine DMAs each packet PKO'd to the PCI port straight into the next buffer
 * (info {length,rh} at the front, then data) -- no CPU copy on the card.
 *
 * SAFETY: the poll loop reads ONLY host RAM (the info->length the card DMA'd), never
 * the card BAR. A wedged card therefore cannot livelock the host with completion
 * timeouts. The only card-BAR access is one-shot config writes at load + a couple of
 * readbacks on the (freshly booted, healthy) card, and the quiesce writes at unload.
 * Derived from the earlier octoq_host.c proof, with the PKTS_SENT poll removed.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/timer.h>
#include <linux/delay.h>

/* SLI PKT/OQ register offsets in BAR0 (CN6XXX) */
#define SLI_PKT_OUT_ENB        0x1010
#define SLI_PKT_SLIST_ROR      0x1030
#define SLI_PKT_SLIST_NS       0x1040
#define SLI_PKT_SLIST_ES64     0x1050
#define SLI_PKT_DPADDR         0x1080
#define SLI_PKT_DATA_OUT_ROR   0x1090
#define SLI_PKT_DATA_OUT_NS    0x10A0
#define SLI_PKT_DATA_OUT_ES64  0x10B0
#define SLI_PKT_OUT_BMODE      0x10D0
#define SLI_PKT_PCIE_PORT64    0x10E0
#define SLI_OQ_WMARK           0x1180
#define OQ_OFFSET              0x10
#define SLI_OQ_BUFF_INFO_SIZE(q) (0x0C00 + (q) * OQ_OFFSET)
#define SLI_OQ_BASE_ADDR64(q)    (0x1400 + (q) * OQ_OFFSET)
#define SLI_OQ_PKT_CREDITS(q)    (0x1800 + (q) * OQ_OFFSET)
#define SLI_OQ_SIZE(q)           (0x1C00 + (q) * OQ_OFFSET)
#define SLI_OQ_PKTS_SENT(q)      (0x2400 + (q) * OQ_OFFSET)
#define SLI_PKT_CTL              0x1220	/* global packet control (RING_EN, backpressure) */
#define SLI_PKT_IPTR             0x1070	/* info-pointer mode per ring */

struct oct_droq_desc { __le64 buffer_ptr; __le64 info_ptr; };
struct oct_droq_info { __le64 length; __le64 rh; };  /* front of each buffer */

static unsigned long bar0 = 0xf8000000;
module_param(bar0, ulong, 0444);
static int q;            /* output queue number (npi0 -> OQ0) */
module_param(q, int, 0444);
static int ndesc = 128;
module_param(ndesc, int, 0444);
static int bufsz = 2048;
module_param(bufsz, int, 0444);

static void __iomem *b0;
static struct pci_dev *pdev;
static struct oct_droq_desc *ring;
static dma_addr_t ring_dma;
static void *pool;
static dma_addr_t pool_dma;
static struct timer_list t;
static u32 read_idx;
static u32 pkts;

static inline u32 rd(u32 o)          { return readl(b0 + o); }
static inline void wr(u32 o, u32 v)  { writel(v, b0 + o); }
static inline void wr64(u32 o, u64 v){ writeq(v, b0 + o); }

/* RAM-ONLY poll: the card DMA's {length,...} to the front of each buffer. We detect a
 * new packet by info->length becoming non-zero -- no BAR read, so a wedged card can't
 * hang us. Refill credits + advance; leave PKTS_SENT untouched (credits gate the card). */
static void poll(struct timer_list *tp)
{
	int budget = 256;

	while (budget--) {
		struct oct_droq_info *info =
			(struct oct_droq_info *)((u8 *)pool + (size_t)read_idx * bufsz);
		u64 len = le64_to_cpu(READ_ONCE(info->length));
		u8 *data = (u8 *)info + sizeof(*info);

		if (!len)
			break;				/* nothing new at this slot */
		pkts++;
		if (pkts <= 8 || (pkts & 0x3ff) == 0)
			pr_info("octoq: PKT #%u q%d idx%u len=%llu rh=0x%llx data=%02x%02x%02x%02x %02x%02x%02x%02x\n",
				pkts, q, read_idx, (unsigned long long)len,
				(unsigned long long)le64_to_cpu(info->rh),
				data[0], data[1], data[2], data[3],
				data[4], data[5], data[6], data[7]);
		WRITE_ONCE(info->length, 0);		/* clear for reuse (host RAM) */
		wmb();
		read_idx = (read_idx + 1) % ndesc;
		wr(SLI_OQ_PKT_CREDITS(q), 1);		/* refill 1 buffer (BAR write, posted) */
	}
	mod_timer(&t, jiffies + msecs_to_jiffies(2));
}

static int __init m_init(void)
{
	int i;

	pdev = pci_get_device(0x177d, 0x0092, NULL);
	if (!pdev) { pr_err("octoq: EP 177d:0092 not found\n"); return -ENODEV; }
	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64))) {
		pci_dev_put(pdev); return -EIO;
	}
	b0 = ioremap(bar0, 0x4000);
	if (!b0) { pci_dev_put(pdev); return -ENOMEM; }

	ring = dma_alloc_coherent(&pdev->dev, (size_t)ndesc * sizeof(*ring),
				  &ring_dma, GFP_KERNEL);
	pool = dma_alloc_coherent(&pdev->dev, (size_t)ndesc * bufsz,
				  &pool_dma, GFP_KERNEL);
	if (!ring || !pool) { pr_err("octoq: no dma mem\n"); return -ENOMEM; }
	memset(pool, 0, (size_t)ndesc * bufsz);		/* zero info->length everywhere */

	for (i = 0; i < ndesc; i++) {
		ring[i].buffer_ptr = cpu_to_le64(pool_dma + (size_t)i * bufsz);
		ring[i].info_ptr   = 0;			/* info prepended in buffer */
	}
	pci_set_master(pdev);

	/* global OQ config (LE host) */
	/* SLI_PKT_CTL: for <=4 rings RING_EN(bit4) must be OFF, and no per-port
	 * backpressure (low 4 bits). liquidio's setup_pkt_ctl_regs -- OpenWrt never
	 * runs it, so the SLI output engine was left in a state where PKO'd packets
	 * never made it into the OQ ring. */
	{
		u64 pktctl = readq(b0 + SLI_PKT_CTL);

		pktctl &= ~(1ull << 4);		/* RING_EN off (<=4 rings) */
		pktctl &= ~0xFull;		/* no per-port backpressure */
		writeq(pktctl, b0 + SLI_PKT_CTL);
	}
	wr(SLI_PKT_IPTR, 0);			/* info prepended in the data buffer */
	wr64(SLI_PKT_PCIE_PORT64, 0);
	wr64(SLI_OQ_WMARK, 0);
	wr(SLI_PKT_OUT_BMODE, 0);
	wr(SLI_PKT_DPADDR, 0xFFFFFFFF);
	wr(SLI_PKT_SLIST_ROR, 0); wr(SLI_PKT_SLIST_NS, 0);
	wr64(SLI_PKT_SLIST_ES64, 0);
	wr(SLI_PKT_DATA_OUT_ROR, 0); wr(SLI_PKT_DATA_OUT_NS, 0);
	wr64(SLI_PKT_DATA_OUT_ES64, 0x5555555555555555ULL);

	/* per-queue */
	wr64(SLI_OQ_BASE_ADDR64(q), (u64)ring_dma);
	wr(SLI_OQ_SIZE(q), ndesc);
	wr(SLI_OQ_BUFF_INFO_SIZE(q), bufsz);
	wr(SLI_PKT_OUT_ENB, rd(SLI_PKT_OUT_ENB) | (1u << q));
	wmb();
	wr(SLI_OQ_PKT_CREDITS(q), ndesc);

	/* one-shot readbacks (healthy freshly-booted card) to confirm config took */
	pr_info("octoq: q%d ring_dma=0x%llx pool_dma=0x%llx ndesc=%d bufsz=%d\n",
		q, (u64)ring_dma, (u64)pool_dma, ndesc, bufsz);
	pr_info("octoq: readback BASE=0x%llx SIZE=0x%x BUFF=0x%x ENB=0x%x CRED=0x%x\n",
		readq(b0 + SLI_OQ_BASE_ADDR64(q)), rd(SLI_OQ_SIZE(q)),
		rd(SLI_OQ_BUFF_INFO_SIZE(q)), rd(SLI_PKT_OUT_ENB),
		rd(SLI_OQ_PKT_CREDITS(q)));

	timer_setup(&t, poll, 0);
	mod_timer(&t, jiffies + msecs_to_jiffies(5));
	return 0;
}

static void __exit m_exit(void)
{
	del_timer_sync(&t);
	/* quiesce the OQ BEFORE freeing host DMA memory (a stray card->host DMA into
	 * freed RAM corrupts the box): disable, zero base, drop credits, drain. */
	wr(SLI_PKT_OUT_ENB, rd(SLI_PKT_OUT_ENB) & ~(1u << q));
	wr64(SLI_OQ_BASE_ADDR64(q), 0);
	wr(SLI_OQ_SIZE(q), 0);
	wr(SLI_OQ_PKT_CREDITS(q), 0);
	wmb();
	msleep(100);
	pci_clear_master(pdev);
	if (ring) dma_free_coherent(&pdev->dev, (size_t)ndesc * sizeof(*ring), ring, ring_dma);
	if (pool) dma_free_coherent(&pdev->dev, (size_t)ndesc * bufsz, pool, pool_dma);
	iounmap(b0);
	pci_dev_put(pdev);
	pr_info("octoq: unloaded (%u pkts)\n", pkts);
}
module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Path A host SLI output-queue reader (RAM-only poll, no BAR read storm)");
