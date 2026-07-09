// SPDX-License-Identifier: GPL-2.0
/* octshm_card M3: shared-memory NIC, card side, bridged to a real SFP+ port.
 * Allocates a 4MB window in card DRAM, maps it into the PCIe host via the PEM
 * BAR1 window (ES=1 => byte-identical to the LE host; metadata is little-endian).
 * A kthread drains the host->card TX ring and transmits each frame out the
 * uplink netdev (default xaui0). An rx_handler on the uplink copies inbound
 * frames into the card->host RX ring. Net effect: host "oct0" == the SFP+ port.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/rtnetlink.h>
#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx.h>

#define PEM_P2N_BAR0   0x00011800C0000080ull
#define PEM_P2N_BAR1   0x00011800C0000088ull
#define PEM_BAR1_IDX0  0x00011800C00000A8ull
#define WIN_ORDER      10			/* 4MB */

/* host PCIe BAR base addresses (BIOS may reassign across host reboots) */
static unsigned long host_bar0 = 0xf8000000;
module_param(host_bar0, ulong, 0444);
static unsigned long host_bar1 = 0xf4000000;
module_param(host_bar1, ulong, 0444);

#define OCTSHM_MAGIC   0x4f435348		/* "OCSH" */
#define OCTSHM_VER     2
#define RING_SZ        128			/* power of two */
#define RING_MASK      (RING_SZ - 1)
#define BUF_SZ         9216

#define CTRL_OFF       0x000000
#define TXDESC_OFF     0x001000
#define RXDESC_OFF     0x002000
#define TXBUF_OFF      0x100000
#define RXBUF_OFF      0x280000

struct octshm_ctrl {
	__le32 magic, version, card_ready, host_ready;
	__le32 tx_prod, tx_cons, rx_prod, rx_cons;
	__le32 heartbeat, dma_enable;
	__le64 tx_dma_base, rx_dma_base;	/* host RAM bus addrs (DMA mode) */
	__le32 resv[3];
};
struct octshm_desc { __le32 len; __le32 flags; };

/* SLI outbound (card masters host RAM). Requires MEM_ACCESS_CTL + subids. */
#define SUBIDX(i)      (CVMX_ADD_IO_SEG(0x00011F00000100E0ull) + ((i)&31)*16 - 16*12)
#define MEM_ACCESS_CTL CVMX_ADD_IO_SEG(0x00011F00000102F0ull)
#define MEM_BASE0      0x00011B0000000000ull
#define MEM_MASK36     0x0000000FFFFFFFFFull

static char *uplink = "xaui0";
module_param(uplink, charp, 0444);
static int dma;
module_param(dma, int, 0444);
static int es = 1;			/* outbound endian-swap mode (subid esr/esw) */
module_param(es, int, 0444);
static struct net_device *up_dev;
static int dma_ready;
static u64 tx_dma_base, rx_dma_base;

static inline u64 host_va(u64 busaddr)
{
	return (1ull << 63) | MEM_BASE0 | (busaddr & MEM_MASK36);
}
static void octdma_setup(void)
{
	int i;
	cvmx_write_csr(MEM_ACCESS_CTL, 127);
	for (i = 12; i < 16; i++)
		cvmx_write_csr(SUBIDX(i),
			       (u64)(i - 12)
			       | ((u64)(es & 3) << 34)		/* esw */
			       | ((u64)(es & 3) << 36));	/* esr */
	CVMX_SYNCW;
	pr_info("octshm: octdma_setup es=%d\n", es);
}
/* copy len bytes host RAM -> local (card reads host) */
static void host_read(u64 busaddr, void *dst, u32 len)
{
	u64 va = host_va(busaddr);
	u32 i;
	for (i = 0; i + 8 <= len; i += 8)
		*(u64 *)((u8 *)dst + i) = *(volatile u64 *)(va + i);
	for (; i < len; i++)
		*((u8 *)dst + i) = *(volatile u8 *)(va + i);
}
/* copy len bytes local -> host RAM (card writes host) */
static void host_write(u64 busaddr, const void *src, u32 len)
{
	u64 va = host_va(busaddr);
	u32 i;
	for (i = 0; i + 8 <= len; i += 8)
		*(volatile u64 *)(va + i) = *(u64 *)((u8 *)src + i);
	for (; i < len; i++)
		*(volatile u8 *)(va + i) = *((u8 *)src + i);
	CVMX_SYNCW;
}

static struct page *win_pages;
static u8 *win;
static u64 win_phys;
static struct octshm_ctrl *ctrl;
static struct octshm_desc *txd, *rxd;
static u8 *txbuf, *rxbuf;
#define MAX_WORKERS 8
static struct task_struct *workers[MAX_WORKERS];
static int nworkers = 2;	/* rev ~1.93G (rx_lock-driven); 2 = best fwd consistency */
module_param(nworkers, int, 0444);
static DEFINE_SPINLOCK(tx_lock);	/* serializes TX slot claim + copy + tx_cons */
static DEFINE_SPINLOCK(rx_lock);	/* guards rx_claim + ordered rx_prod publish */
static u32 rx_claim;			/* RX slot claim index (ahead of published rx_prod) */
static u8 rx_done[RING_SZ];		/* per-slot DMA-complete flags for ordered publish */
static u32 beat;

/* RX: uplink inbound frame -> card->host RX ring. Registered as an ETH_P_ALL
 * packet_type (like tcpdump), which every netif_receive_skb path invokes. The
 * skb is shared (not owned) here: read it, copy into the ring, do not free. */
static struct packet_type oct_ptype;

static int oct_rx_pack(struct sk_buff *skb, struct net_device *dev,
		       struct packet_type *pt, struct net_device *orig)
{
	unsigned char *l2;
	u32 len, claim, rc, rs;
	unsigned long rxflags;

	if (skb->dev != up_dev)
		goto out;
	ctrl->resv[0] = cpu_to_le32(le32_to_cpu(ctrl->resv[0]) + 1);	/* fire count */
	l2 = skb_mac_header(skb);
	if (!l2 || l2 > skb->data)
		l2 = skb->data;
	len = skb->len + (u32)(skb->data - l2);		/* full L2 frame */
	ctrl->resv[1] = cpu_to_le32(len);
	if (skb_headlen(skb) < skb->len)			/* need linear src */
		goto out;
	if (!(len && len <= BUF_SZ))
		goto out;

	/* Parallel RX DMA: the SLI mem-access "DMA" is a CPU copy over PCIe, so
	 * serializing every frame (old rx_lock around host_write) pinned reverse
	 * to one core. Instead take only a TINY lock to CLAIM a slot, do the
	 * heavy host_write OUTSIDE the lock (runs on all RX cores at once), then
	 * a TINY lock to publish rx_prod over contiguously-completed slots so the
	 * host still sees frames in order. */
	spin_lock_irqsave(&rx_lock, rxflags);
	claim = rx_claim;
	rc = le32_to_cpu(ctrl->rx_cons);
	if ((claim - rc) >= RING_SZ) {		/* ring full: drop */
		spin_unlock_irqrestore(&rx_lock, rxflags);
		goto out;
	}
	rs = claim & RING_MASK;
	rx_claim = claim + 1;
	spin_unlock_irqrestore(&rx_lock, rxflags);

	if (dma_ready)				/* heavy copy OUTSIDE lock -> parallel */
		host_write(rx_dma_base + (u64)rs * BUF_SZ, l2, len);
	else
		memcpy(rxbuf + rs * BUF_SZ, l2, len);
	rxd[rs].len = cpu_to_le32(len);
	rxd[rs].flags = 0;
	wmb();

	spin_lock_irqsave(&rx_lock, rxflags);	/* ordered completion */
	rx_done[rs] = 1;
	{
		u32 p = le32_to_cpu(ctrl->rx_prod);
		while (p != rx_claim && rx_done[p & RING_MASK]) {
			rx_done[p & RING_MASK] = 0;
			p++;
		}
		ctrl->rx_prod = cpu_to_le32(p);
	}
	wmb();
	spin_unlock_irqrestore(&rx_lock, rxflags);
out:
	kfree_skb(skb);					/* drop our ref from deliver_skb */
	return 0;
}

/* TX: drain host->card TX ring, transmit each frame out the uplink. */
static int worker_fn(void *arg)
{
	long id = (long)arg;
	unsigned int idle = 0;		/* consecutive empty polls -> backoff */

	while (!kthread_should_stop()) {
		struct sk_buff *skb = NULL;
		u32 ts = 0, len = 0;
		int have = 0;
		unsigned long flags;

		/* DMA activation: only worker 0 arms it (single writer) */
		if (id == 0 && dma && !dma_ready &&
		    le32_to_cpu(ctrl->dma_enable)) {
			u64 tb = le64_to_cpu(ctrl->tx_dma_base);
			u64 rb = le64_to_cpu(ctrl->rx_dma_base);
			if (rb && tb && rb < (1ull << 40) && tb < (1ull << 40)) {
				tx_dma_base = tb;
				rx_dma_base = rb;
				octdma_setup();
				dma_ready = 1;
				pr_info("octshm: DMA on, tx_base=0x%llx rx_base=0x%llx\n",
					(unsigned long long)tb, (unsigned long long)rb);
			}
		}
		if (id == 0) {			/* heartbeat: single writer */
			beat++;
			ctrl->heartbeat = cpu_to_le32(beat);
		}

		/* cheap racy hint: skip alloc when the ring looks empty */
		if (le32_to_cpu(ctrl->tx_prod) == le32_to_cpu(ctrl->tx_cons) ||
		    !up_dev) {
			usleep_range(20, 40);
			continue;
		}
		(void)idle;
		/* pre-alloc skb OUTSIDE the lock (parallel across workers) */
		skb = netdev_alloc_skb(up_dev, BUF_SZ + NET_IP_ALIGN);
		if (!skb) { usleep_range(20, 40); continue; }

		/* critical section: claim one slot, copy it, advance tx_cons in
		 * strict order. Keeps the ring race-free (tx_cons only passes a
		 * slot after its data is fully copied out). Copy is cheap. */
		spin_lock_irqsave(&tx_lock, flags);
		{
			u32 tp = le32_to_cpu(ctrl->tx_prod);
			u32 tc = le32_to_cpu(ctrl->tx_cons);
			if (tc != tp) {
				ts  = tc & RING_MASK;
				len = le32_to_cpu(txd[ts].len);
				if (len >= ETH_HLEN && len <= BUF_SZ) {
					skb_reserve(skb, NET_IP_ALIGN);
					memcpy(skb_put(skb, len),
					       txbuf + ts * BUF_SZ, len);
					have = 1;
				}
				tc++;
				ctrl->tx_cons = cpu_to_le32(tc);
				wmb();
			}
		}
		spin_unlock_irqrestore(&tx_lock, flags);

		if (have) {			/* alloc-heavy xmit OUTSIDE lock */
			skb->dev = up_dev;
			skb_reset_mac_header(skb);
			skb->protocol = eth_hdr(skb)->h_proto;
			dev_queue_xmit(skb);
			cond_resched();
		} else {
			dev_kfree_skb(skb);	/* empty or bad-len slot */
		}
	}
	return 0;
}

static int __init octshm_init(void)
{
	u64 idx;
	int ret, i;

	win_pages = alloc_pages(GFP_KERNEL, WIN_ORDER);
	if (!win_pages)
		return -ENOMEM;
	win = page_address(win_pages);
	win_phys = page_to_phys(win_pages);
	memset(win, 0, 1UL << (WIN_ORDER + PAGE_SHIFT));

	ctrl  = (struct octshm_ctrl *)(win + CTRL_OFF);
	txd   = (struct octshm_desc *)(win + TXDESC_OFF);
	rxd   = (struct octshm_desc *)(win + RXDESC_OFF);
	txbuf = win + TXBUF_OFF;
	rxbuf = win + RXBUF_OFF;

	ctrl->magic   = cpu_to_le32(OCTSHM_MAGIC);
	ctrl->version = cpu_to_le32(OCTSHM_VER);
	wmb();

	idx = ((win_phys >> 22) << 4) | (1u << 1) | 0x9;	/* CA|ES=1|valid */
	cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_P2N_BAR0), host_bar0);
	cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_P2N_BAR1), host_bar1);
	cvmx_write_csr(CVMX_ADD_IO_SEG(0x00011800C0000128ull), 0x10); /* BAR_CTL: bar1_siz=1 (64M) */
	cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX0), idx);
	CVMX_SYNCW;
	pr_info("octshm: P2N_BAR1=0x%llx BAR_CTL=0x%llx IDX0=0x%llx (host_bar1=0x%lx)\n",
		(unsigned long long)cvmx_read_csr(CVMX_ADD_IO_SEG(PEM_P2N_BAR1)),
		(unsigned long long)cvmx_read_csr(CVMX_ADD_IO_SEG(0x00011800C0000128ull)),
		(unsigned long long)cvmx_read_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX0)),
		host_bar1);

	up_dev = dev_get_by_name(&init_net, uplink);
	if (!up_dev) {
		pr_err("octshm: uplink %s not found\n", uplink);
		__free_pages(win_pages, WIN_ORDER);
		return -ENODEV;
	}
	oct_ptype.type = htons(ETH_P_ALL);
	oct_ptype.dev  = up_dev;
	oct_ptype.func = oct_rx_pack;
	dev_add_pack(&oct_ptype);
	rtnl_lock();				/* promisc: accept frames for host MAC */
	dev_set_promiscuity(up_dev, 1);
	rtnl_unlock();

	if (nworkers < 1) nworkers = 1;
	if (nworkers > MAX_WORKERS) nworkers = MAX_WORKERS;
	for (i = 0; i < nworkers; i++) {
		workers[i] = kthread_create(worker_fn, (void *)(long)i,
					    "octshm/%d", i);
		if (IS_ERR(workers[i])) {
			ret = PTR_ERR(workers[i]);
			workers[i] = NULL;
			while (--i >= 0) kthread_stop(workers[i]);
			dev_remove_pack(&oct_ptype);
			dev_put(up_dev);
			__free_pages(win_pages, WIN_ORDER);
			return ret;
		}
		/* no kthread_bind: let the scheduler float workers so RX softirq
		 * and the card's own tx aren't starved on reverse traffic. */
		wake_up_process(workers[i]);
	}
	ret = 0; (void)ret;
	ctrl->card_ready = cpu_to_le32(1);
	wmb();
	pr_info("octshm M3: phys=0x%llx idx=0x%llx ring=%d uplink=%s up\n",
		(unsigned long long)win_phys, (unsigned long long)idx,
		RING_SZ, uplink);
	return 0;
}

static void __exit octshm_exit(void)
{
	int i;

	for (i = 0; i < MAX_WORKERS; i++)
		if (!IS_ERR_OR_NULL(workers[i]))
			kthread_stop(workers[i]);
	if (up_dev) {
		dev_remove_pack(&oct_ptype);
		rtnl_lock();
		dev_set_promiscuity(up_dev, -1);
		rtnl_unlock();
		dev_put(up_dev);
	}
	if (ctrl)
		ctrl->card_ready = 0;
	cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX0), 0);
	__free_pages(win_pages, WIN_ORDER);
	pr_info("octshm: unloaded\n");
}
module_init(octshm_init);
module_exit(octshm_exit);
MODULE_LICENSE("GPL");
