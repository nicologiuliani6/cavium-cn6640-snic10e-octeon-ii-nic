// SPDX-License-Identifier: GPL-2.0
/* octshm_host: host side of the Cavium shared-memory NIC. Registers netdev
 * "oct0" over the card's 4MB DRAM window exposed at PCIe BAR2 (host phys base,
 * default 0xf4000000, set via setpci before load). TX writes the host->card
 * ring; a poll timer drains the card->host RX ring into the stack. CPU MMIO
 * only -- no bus mastering, so it cannot DMA into host RAM (safe/reversible).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/timer.h>
#include <linux/skbuff.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>

#define OCTSHM_MAGIC 0x4f435348
#define RING_SZ      128
#define RING_MASK    (RING_SZ - 1)
#define BUF_SZ       9216
#define CTRL_OFF     0x000000
#define TXDESC_OFF   0x001000
#define RXDESC_OFF   0x002000
#define TXBUF_OFF    0x100000
#define RXBUF_OFF    0x280000
#define WIN_SZ       (4u << 20)

/* ctrl field byte offsets within the window */
#define C_MAGIC      0x00
#define C_VERSION    0x04
#define C_CARD_READY 0x08
#define C_HOST_READY 0x0c
#define C_TX_PROD    0x10
#define C_TX_CONS    0x14
#define C_RX_PROD    0x18
#define C_RX_CONS    0x1c
#define C_DMA_ENABLE 0x24
#define C_TX_DMA_BASE 0x28
#define C_RX_DMA_BASE 0x30

static unsigned long base = 0xf4000000;
module_param(base, ulong, 0444);
static int poll_us = 200;
module_param(poll_us, int, 0644);
static int dma;
module_param(dma, int, 0444);

static void __iomem *win;
static struct pci_dev *pdev;
static void *tx_dma_va, *rx_dma_va;	/* host coherent packet pools (DMA mode) */
static dma_addr_t tx_dma_bus, rx_dma_bus;
#define POOL_SZ (RING_SZ * BUF_SZ)
static struct net_device *ndev;
static struct timer_list poll_timer;

/* index shadows: host owns tx_prod + rx_cons, so keep them in RAM and only
 * MMIO-read the card-owned counters. Cuts PCIe reads per packet. xmit is
 * serialized by the qdisc lock and poll by the timer, so no extra locking. */
static u32 tp_shadow;		/* host TX producer */
static u32 tc_cache;		/* last observed card TX consumer */
static u32 rc_shadow;		/* host RX consumer */

static inline u32 rd(u32 off)          { return readl(win + off); }
static inline void wr(u32 off, u32 v)  { writel(v, win + off); }
static inline void wr64(u32 off, u64 v){ writeq(v, win + off); }

static int octdma_alloc(void)
{
	pdev = pci_get_device(0x177d, 0x0092, NULL);
	if (!pdev) { pr_err("octshm_host: EP 177d:0092 not found\n"); return -ENODEV; }
	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64))) {
		pci_dev_put(pdev); return -EIO;
	}
	tx_dma_va = dma_alloc_coherent(&pdev->dev, POOL_SZ, &tx_dma_bus, GFP_KERNEL);
	rx_dma_va = dma_alloc_coherent(&pdev->dev, POOL_SZ, &rx_dma_bus, GFP_KERNEL);
	if (!tx_dma_va || !rx_dma_va) return -ENOMEM;
	pci_set_master(pdev);			/* card masters host RAM (IOMMU-contained) */
	wr64(C_TX_DMA_BASE, (u64)tx_dma_bus);
	wr64(C_RX_DMA_BASE, (u64)rx_dma_bus);
	wmb();
	wr(C_DMA_ENABLE, 1);			/* tell card to switch to DMA */
	wmb();
	pr_info("octshm_host: DMA on, tx_bus=0x%llx rx_bus=0x%llx\n",
		(u64)tx_dma_bus, (u64)rx_dma_bus);
	return 0;
}
static void octdma_free(void)
{
	if (!pdev) return;
	wr(C_DMA_ENABLE, 0);
	pci_clear_master(pdev);
	if (tx_dma_va) dma_free_coherent(&pdev->dev, POOL_SZ, tx_dma_va, tx_dma_bus);
	if (rx_dma_va) dma_free_coherent(&pdev->dev, POOL_SZ, rx_dma_va, rx_dma_bus);
	pci_dev_put(pdev);
}

static netdev_tx_t oct_xmit(struct sk_buff *skb, struct net_device *dev)
{
	u32 tp = tp_shadow;
	u32 slot, len = skb->len;

	if ((tp - tc_cache) >= RING_SZ) {	/* maybe full: refresh once */
		tc_cache = rd(C_TX_CONS);
		if ((tp - tc_cache) >= RING_SZ) {
			dev->stats.tx_dropped++;
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}
	}
	if (len > BUF_SZ)
		len = BUF_SZ;
	slot = tp & RING_MASK;
	/* TX always PIO: host posted-write to card BAR window (fast). */
	memcpy_toio(win + TXBUF_OFF + slot * BUF_SZ, skb->data, len);
	wmb();
	wr(TXDESC_OFF + slot * 8, len);
	wmb();					/* flush WC + desc before producer bump */
	tp_shadow = tp + 1;
	wr(C_TX_PROD, tp_shadow);
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void oct_poll(struct timer_list *t)
{
	u32 rp = rd(C_RX_PROD);
	u32 rc = rc_shadow;
	int quota = 128;

	while (rc != rp && quota--) {
		u32 slot = rc & RING_MASK;
		u32 len = rd(RXDESC_OFF + slot * 8);
		struct sk_buff *skb;

		if (len && len <= BUF_SZ) {
			skb = netdev_alloc_skb(ndev, len + 2);
			if (skb) {
				skb_reserve(skb, 2);
				if (dma)	/* card DMA-wrote into local coherent RAM */
					memcpy(skb_put(skb, len),
					       rx_dma_va + slot * BUF_SZ, len);
				else
					memcpy_fromio(skb_put(skb, len),
						      win + RXBUF_OFF + slot * BUF_SZ, len);
				skb->protocol = eth_type_trans(skb, ndev);
				ndev->stats.rx_packets++;
				ndev->stats.rx_bytes += len;
				netif_rx(skb);
			} else {
				ndev->stats.rx_dropped++;
			}
		}
		rc++;
	}
	if (rc != rc_shadow) {			/* publish consumer once per batch */
		rc_shadow = rc;
		wr(C_RX_CONS, rc);
	}
	mod_timer(&poll_timer, jiffies + max(1UL, usecs_to_jiffies(poll_us)));
}

static int oct_open(struct net_device *dev)
{
	tp_shadow = rd(C_TX_PROD);		/* sync shadows to current ring */
	tc_cache  = rd(C_TX_CONS);
	/* snap RX consumer to the card's current producer (drop any stale/desynced
	 * count). Avoids an infinite poll spin if C_RX_CONS holds garbage. */
	rc_shadow = rd(C_RX_PROD);
	wr(C_RX_CONS, rc_shadow);
	wr(C_HOST_READY, 1);
	netif_start_queue(dev);
	mod_timer(&poll_timer, jiffies + 1);
	return 0;
}
static int oct_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	del_timer_sync(&poll_timer);
	wr(C_HOST_READY, 0);
	return 0;
}

static const struct net_device_ops oct_ops = {
	.ndo_open       = oct_open,
	.ndo_stop       = oct_stop,
	.ndo_start_xmit = oct_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr   = eth_validate_addr,
};

static int __init octshm_host_init(void)
{
	u32 magic, cr;
	int ret;

	win = ioremap_wc(base, WIN_SZ);		/* WC: batch TX writes into big TLPs */
	if (!win)
		return -ENOMEM;
	magic = rd(C_MAGIC);
	cr = rd(C_CARD_READY);
	if (magic != OCTSHM_MAGIC) {
		pr_err("octshm_host: bad magic 0x%08x at base 0x%lx (card module loaded? BAR2 set?)\n",
		       magic, base);
		iounmap(win);
		return -ENODEV;
	}
	pr_info("octshm_host: magic OK ver=%u card_ready=%u\n", rd(C_VERSION), cr);

	if (dma) {
		ret = octdma_alloc();
		if (ret) { octdma_free(); iounmap(win); return ret; }
	}

	ndev = alloc_etherdev(0);
	if (!ndev) { iounmap(win); return -ENOMEM; }
	strscpy(ndev->name, "oct0", IFNAMSIZ);
	ndev->netdev_ops = &oct_ops;
	eth_hw_addr_random(ndev);
	ndev->mtu = 1500;
	ndev->max_mtu = 9000;			/* allow jumbo (BUF_SZ=9216) */
	timer_setup(&poll_timer, oct_poll, 0);

	ret = register_netdev(ndev);
	if (ret) {
		free_netdev(ndev);
		iounmap(win);
		return ret;
	}
	pr_info("octshm_host: registered %s\n", ndev->name);
	return 0;
}

static void __exit octshm_host_exit(void)
{
	unregister_netdev(ndev);
	if (dma)
		octdma_free();
	free_netdev(ndev);
	iounmap(win);
	pr_info("octshm_host: unloaded\n");
}
module_init(octshm_host_init);
module_exit(octshm_host_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cavium shared-memory NIC (host side)");
