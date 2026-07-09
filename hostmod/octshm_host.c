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
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/skbuff.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/hwmon.h>

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
#define C_RESV0      0x38	/* card board temp, millidegC (fed by card /proc/octshm/temp) */
#define C_RESV1      0x3c	/* card Octeon die temp, millidegC */

static unsigned long base = 0xf4000000;
module_param(base, ulong, 0444);
static int poll_us = 200;
module_param(poll_us, int, 0644);
static int rxthreads = 1;	/* hrx only: N parallel drain threads, each owning slots
				 * s where s%N==tid. Card fills the single ring in claim order;
				 * a per-slot phase in RAM lets threads drain disjoint slots in
				 * parallel (no in-order single-drain wall). Must be a power of 2
				 * that divides RING_SZ (1/2/4/8) so slot ownership is stable. */
module_param(rxthreads, int, 0444);
static int dma;
module_param(dma, int, 0444);
static int rxbatch = 1;		/* deliver a whole drain batch via netif_receive_skb_list
				 * (one stack entry for N frames) instead of per-skb */
module_param(rxbatch, int, 0644);
static int lockfree;		/* match card lockfree=1: read the per-slot desc PHASE bit
				 * to tell ready, instead of the card's monotonic rx_prod */
module_param(lockfree, int, 0444);
static int ztx;			/* match card ztx=1: place TX frame in the coherent host
				 * pool (card DMA-reads it) instead of PIO'ing over PCIe */
module_param(ztx, int, 0444);
static int hrx;			/* match card hrx=1: read the RX {len,phase} descriptor from
				 * an 8-byte header the card DPI'd into local host RAM (no per-frame
				 * PCIe MMIO desc read). Data follows the header at +HRX_HDR. */
module_param(hrx, int, 0444);
#define HRX_HDR 8
#define RX_PHASE(rc)  (((rc) >> 7) & 1u)	/* log2(RING_SZ)=7; matches card RX_PHASE */

static void __iomem *win;
static struct pci_dev *pdev;
static void *tx_dma_va, *rx_dma_va;	/* host coherent packet pools (DMA mode) */
static dma_addr_t tx_dma_bus, rx_dma_bus;
#define POOL_SZ (RING_SZ * BUF_SZ)
static struct net_device *ndev;
static struct task_struct *poll_thread;
static struct task_struct *poll_threads[8];
static u32 rc_pub[8];			/* per-thread RX consumer position (stride rxthreads) */

/* index shadows: host owns tx_prod + rx_cons, so keep them in RAM and only
 * MMIO-read the card-owned counters. Cuts PCIe reads per packet. xmit is
 * serialized by the qdisc lock and poll by the timer, so no extra locking. */
static u32 tp_shadow;		/* host TX producer */
static u32 tc_cache;		/* last observed card TX consumer */
static u32 rc_shadow;		/* host RX consumer */

static inline u32 rd(u32 off)          { return readl(win + off); }
static inline void wr(u32 off, u32 v)  { writel(v, win + off); }
static inline u64 rd64(u32 off)        { return readq(win + off); }	/* whole desc in 1 MMIO */
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
	if (hrx) {				/* seed each slot's header phase to "not ready"
						 * (opposite of first expected) so the drain waits
						 * until the card actually fills the slot. */
		u32 s;
		for (s = 0; s < RING_SZ; s++)
			*(__le32 *)((u8 *)rx_dma_va + s * BUF_SZ + 4) =
				cpu_to_le32(RX_PHASE(s) ^ 1u);
	}
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
	if (ztx && dma && tx_dma_va)	/* zero-copy: local copy into coherent pool, card
					 * DPI-reads it -> no host PIO byte-push over PCIe */
		memcpy(tx_dma_va + slot * BUF_SZ, skb->data, len);
	else				/* PIO: host posted-write into card BAR window */
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

/* Drain up to `budget` frames from the card->host RX ring into the stack.
 * Returns the number consumed. */
static int oct_drain(int tid, int budget)
{
	u32 stride = rxthreads;
	u32 rp = lockfree ? 0 : rd(C_RX_PROD);
	u32 rc = rc_pub[tid];
	int n = 0;
	LIST_HEAD(rx_list);

	while (n < budget) {
		u32 slot = rc & RING_MASK;
		u32 len;
		struct sk_buff *skb;

		if (hrx) {
			/* descriptor lives in LOCAL host RAM (card DPI'd it, phase last).
			 * No PCIe MMIO: read {len,phase} from the slot's 8-byte header. */
			u8 *h = (u8 *)rx_dma_va + slot * BUF_SZ;
			u32 phase = le32_to_cpu(*(__le32 *)(h + 4));

			if (phase != RX_PHASE(rc))
				break;			/* not filled yet */
			rmb();				/* phase seen => data landed */
			len = le32_to_cpu(*(__le32 *)h);
		} else if (lockfree) {
			/* one 64-bit desc read = {len, flags}. Ready iff the slot's phase
			 * bit matches what this consumer position expects (virtio-style). */
			u64 desc = rd64(RXDESC_OFF + slot * 8);

			if (((u32)(desc >> 32) & 1u) != RX_PHASE(rc))
				break;			/* stale phase: not filled yet */
			len = (u32)desc;
		} else {
			if (rc == rp)
				break;
			len = rd(RXDESC_OFF + slot * 8);
		}

		if (len && len <= BUF_SZ) {
			skb = netdev_alloc_skb(ndev, len + 2);
			if (skb) {
				skb_reserve(skb, 2);
				if (dma)	/* card DMA-wrote into local coherent RAM */
					memcpy(skb_put(skb, len),
					       rx_dma_va + slot * BUF_SZ + (hrx ? HRX_HDR : 0),
					       len);
				else
					memcpy_fromio(skb_put(skb, len),
						      win + RXBUF_OFF + slot * BUF_SZ, len);
				skb->protocol = eth_type_trans(skb, ndev);
				ndev->stats.rx_packets++;
				ndev->stats.rx_bytes += len;
				if (rxbatch)		/* batch: one stack entry per drain */
					list_add_tail(&skb->list, &rx_list);
				else
					netif_receive_skb(skb);	/* BH disabled by caller */
			} else {
				ndev->stats.rx_dropped++;
			}
		}
		rc += stride;			/* next slot THIS thread owns */
		n++;
	}
	rc_pub[tid] = rc;
	{	/* publish consumer = min across threads (contiguous consumed floor);
		 * values stay within a batch of each other so signed diff is safe */
		u32 m = rc_pub[0], i;

		for (i = 1; i < stride; i++)
			if ((s32)(rc_pub[i] - m) < 0)
				m = rc_pub[i];
		if (m != rc_shadow) {
			rc_shadow = m;
			wr(C_RX_CONS, m);
		}
	}
	if (!list_empty(&rx_list))
		netif_receive_skb_list(&rx_list);
	return n;
}

/* Busy-poll kthread: drains continuously (no ~1ms timer/jiffy granularity that
 * capped throughput). Backs off only when the ring is empty. */
static int poll_fn(void *arg)
{
	int tid = (int)(long)arg;
	unsigned int idle = 0;

	while (!kthread_should_stop()) {
		int did;

		local_bh_disable();		/* netif_receive_skb needs BH context */
		did = oct_drain(tid, 64);
		local_bh_enable();
		if (did) {
			idle = 0;
			/* small gap so the C_RX_PROD MMIO polling doesn't saturate the
			 * PCIe link and starve the card's inbound DMA writes */
			usleep_range(poll_us, poll_us * 2);
		} else if (++idle < 8) {
			cpu_relax();
		} else {
			usleep_range(20, 60);
		}
	}
	return 0;
}

static int oct_open(struct net_device *dev)
{
	tp_shadow = rd(C_TX_PROD);		/* sync shadows to current ring */
	tc_cache  = rd(C_TX_CONS);
	/* snap RX consumer to the card's current producer (drop any stale/desynced
	 * count). Avoids an infinite poll spin if C_RX_CONS holds garbage. */
	rc_shadow = rd(C_RX_PROD);
	if (hrx)			/* hrx ring is fresh at 0 (card realigns claim at arm) */
		rc_shadow = 0;
	wr(C_RX_CONS, rc_shadow);
	wr(C_HOST_READY, 1);
	netif_start_queue(dev);

	/* multi-thread drain only where slots carry an independent per-slot phase */
	if (!hrx && !lockfree)
		rxthreads = 1;
	if (rxthreads < 1)
		rxthreads = 1;
	if (rxthreads > 8)
		rxthreads = 8;
	while (rxthreads & (rxthreads - 1))	/* floor to power of 2 (divides RING_SZ) */
		rxthreads &= rxthreads - 1;
	{
		int i;

		for (i = 0; i < rxthreads; i++)
			rc_pub[i] = rc_shadow + i;	/* thread i owns slots ==i (mod rxthreads) */
		for (i = 0; i < rxthreads; i++) {
			poll_threads[i] = kthread_run(poll_fn, (void *)(long)i,
						      "octshm-rx/%d", i);
			if (IS_ERR(poll_threads[i])) {
				poll_threads[i] = NULL;
				break;
			}
		}
		if (!poll_threads[0]) {
			wr(C_HOST_READY, 0);
			netif_stop_queue(dev);
			return -ENOMEM;
		}
	}
	return 0;
}
static int oct_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	{
		int i;

		for (i = 0; i < 8; i++)
			if (poll_threads[i]) {
				kthread_stop(poll_threads[i]);
				poll_threads[i] = NULL;
			}
	}
	if (poll_thread) {
		kthread_stop(poll_thread);
		poll_thread = NULL;
	}
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

/* hwmon: expose the card's temps (fed by the card into ctrl->resv over BAR2) so
 * host `sensors` shows them as "cavium_card": temp1=board, temp2=Octeon die. */
static struct device *hwmon_dev;
static const char * const oct_temp_label[] = { "board", "octeon-die" };

static umode_t oct_hwmon_visible(const void *d, enum hwmon_sensor_types type,
				 u32 attr, int ch)
{
	return 0444;
}
static int oct_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	*val = (long)(s32)rd(channel == 0 ? C_RESV0 : C_RESV1);	/* millidegC */
	return 0;
}
static int oct_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
				 u32 attr, int channel, const char **str)
{
	if (type != hwmon_temp || attr != hwmon_temp_label || channel > 1)
		return -EOPNOTSUPP;
	*str = oct_temp_label[channel];
	return 0;
}
static const struct hwmon_ops oct_hwmon_ops = {
	.is_visible = oct_hwmon_visible,
	.read = oct_hwmon_read,
	.read_string = oct_hwmon_read_string,
};
static const struct hwmon_channel_info *oct_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL,
				 HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};
static const struct hwmon_chip_info oct_hwmon_chip = {
	.ops = &oct_hwmon_ops,
	.info = oct_hwmon_info,
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

	ret = register_netdev(ndev);
	if (ret) {
		free_netdev(ndev);
		iounmap(win);
		return ret;
	}
	pr_info("octshm_host: registered %s\n", ndev->name);

	if (!pdev)					/* hwmon needs a parent dev */
		pdev = pci_get_device(0x177d, 0x0092, NULL);
	if (pdev) {
		hwmon_dev = hwmon_device_register_with_info(&pdev->dev,
				"cavium_card", NULL, &oct_hwmon_chip, NULL);
		if (!IS_ERR(hwmon_dev))
			pr_info("octshm_host: hwmon cavium_card registered\n");
		else
			hwmon_dev = NULL;
	}
	return 0;
}

static void __exit octshm_host_exit(void)
{
	if (hwmon_dev)
		hwmon_device_unregister(hwmon_dev);
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
