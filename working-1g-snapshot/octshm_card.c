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
static struct task_struct *worker;
static u32 beat;

/* RX: uplink inbound frame -> card->host RX ring. Registered as an ETH_P_ALL
 * packet_type (like tcpdump), which every netif_receive_skb path invokes. The
 * skb is shared (not owned) here: read it, copy into the ring, do not free. */
static struct packet_type oct_ptype;

static int oct_rx_pack(struct sk_buff *skb, struct net_device *dev,
		       struct packet_type *pt, struct net_device *orig)
{
	unsigned char *l2;
	u32 len, rp, rc;

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
	rp = le32_to_cpu(ctrl->rx_prod);
	rc = le32_to_cpu(ctrl->rx_cons);
	if (len && len <= BUF_SZ && (rp - rc) < RING_SZ) {
		u32 rs = rp & RING_MASK;
		if (dma_ready)			/* DMA: write frame straight to host RAM */
			host_write(rx_dma_base + (u64)rs * BUF_SZ, l2, len);
		else
			memcpy(rxbuf + rs * BUF_SZ, l2, len);
		rxd[rs].len = cpu_to_le32(len);
		rxd[rs].flags = 0;
		wmb();
		ctrl->rx_prod = cpu_to_le32(rp + 1);
		wmb();
	}
out:
	kfree_skb(skb);					/* drop our ref from deliver_skb */
	return 0;
}

/* TX: drain host->card TX ring, transmit each frame out the uplink. */
static int worker_fn(void *unused)
{
	while (!kthread_should_stop()) {
		u32 tp, tc;
		int did = 0;

		if (dma && !dma_ready && le32_to_cpu(ctrl->dma_enable)) {
			u64 tb = le64_to_cpu(ctrl->tx_dma_base);
			u64 rb = le64_to_cpu(ctrl->rx_dma_base);
			/* guard: never DMA to a null/absurd host addr (would wedge PEM) */
			if (rb && tb && rb < (1ull << 40) && tb < (1ull << 40)) {
				tx_dma_base = tb;
				rx_dma_base = rb;
				octdma_setup();
				dma_ready = 1;
				pr_info("octshm: DMA on, tx_base=0x%llx rx_base=0x%llx\n",
					(unsigned long long)tb, (unsigned long long)rb);
			}
		}
		tp = le32_to_cpu(ctrl->tx_prod);
		tc = le32_to_cpu(ctrl->tx_cons);

		while (tc != tp) {
			u32 ts = tc & RING_MASK;
			u32 len = le32_to_cpu(txd[ts].len);
			struct sk_buff *skb;

			if (len >= ETH_HLEN && len <= BUF_SZ && up_dev) {
				skb = netdev_alloc_skb(up_dev, len + NET_IP_ALIGN);
				if (skb) {
					skb_reserve(skb, NET_IP_ALIGN);
					/* TX stays PIO: host posted-wrote to the BAR window
					 * (fast). Card reads its own local DRAM here. */
					memcpy(skb_put(skb, len),
					       txbuf + ts * BUF_SZ, len);
					skb->dev = up_dev;
					skb_reset_mac_header(skb);
					skb->protocol =
						eth_hdr(skb)->h_proto;
					dev_queue_xmit(skb);
				}
			}
			tc++;
			ctrl->tx_cons = cpu_to_le32(tc);
			wmb();
			did = 1;
		}
		beat++;
		ctrl->heartbeat = cpu_to_le32(beat);
		wmb();
		if (!did) {
			usleep_range(20, 40);	/* fast TX pickup, ~30us vs 10ms */
		} else {
			cond_resched();
		}
	}
	return 0;
}

static int __init octshm_init(void)
{
	u64 idx;
	int ret;

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

	worker = kthread_run(worker_fn, NULL, "octshm");
	if (IS_ERR(worker)) {
		dev_remove_pack(&oct_ptype);
		dev_put(up_dev);
		__free_pages(win_pages, WIN_ORDER);
		return PTR_ERR(worker);
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
	if (!IS_ERR_OR_NULL(worker))
		kthread_stop(worker);
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
