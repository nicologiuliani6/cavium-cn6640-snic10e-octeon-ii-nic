// SPDX-License-Identifier: GPL-2.0
/* octnic: host side of the Cavium shared-memory NIC. Registers one netdev
 * per card port ("oct0", "oct1") over the card's DRAM window exposed at PCIe
 * BAR2. base=0 (default) auto-discovers the card's BAR2 via PCI 177d:0092, so
 * `modprobe octnic` needs no args; pass base= to force a specific address. Each
 * port owns a private 4MB region at PORT_STRIDE*i. TX writes the host->card
 * ring; per-port poll kthreads drain the card->host RX ring into the stack.
 * ports=1 is byte-identical to the original single-port build.
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
#define PORT_STRIDE  0x400000		/* per-port window region (4MB) */
#define MAXPORT      2

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

static unsigned long base;		/* 0 = auto-discover card BAR2 (177d:0092). override to force */
module_param(base, ulong, 0444);
static int ports = 1;		/* one netdev per card uplink (oct0<->xaui0, oct1<->xaui1) */
module_param(ports, int, 0444);
static int poll_us = 200;
module_param(poll_us, int, 0644);
static int ntxq = 1;		/* multi-queue TX: N netdev txqs -> N cores xmit in parallel.
				 * Single ring, CAS slot-claim + per-slot TX phase (host stamps after
				 * PIO, card drains by phase). Mirrors the rxthreads RX win for FWD. */
module_param(ntxq, int, 0444);
#define TX_PHASE(tp)  (((tp) >> 7) & 1u)	/* log2(RING_SZ)=7 */
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

static void __iomem *win;		/* whole BAR2 mapping (ports * 4MB) */
static struct pci_dev *pdev;
#define POOL_SZ (RING_SZ * BUF_SZ)

/* Per-port host state. index shadows: host owns tx_prod + rx_cons, so keep them
 * in RAM and only MMIO-read the card-owned counters. */
struct host_port {
	void __iomem *w;			/* win + idx*PORT_STRIDE */
	struct net_device *ndev;
	void *tx_dma_va, *rx_dma_va;		/* host coherent packet pools (DMA mode) */
	dma_addr_t tx_dma_bus, rx_dma_bus;
	struct task_struct *poll_threads[8];
	u32 rc_pub[8];				/* per-thread RX consumer (stride rxthreads) */
	atomic_t tx_claim;			/* parallel TX producer (CAS-claimed) */
	u32 tp_shadow;				/* host TX producer */
	u32 tc_cache;				/* last observed card TX consumer */
	u32 rc_shadow;				/* host RX consumer */
	int idx;
};
static struct host_port hp[MAXPORT];
#define PORT(dev) (*(struct host_port **)netdev_priv(dev))

/* per-poll-thread argument: which port + which stride slot */
struct poll_arg { struct host_port *p; int tid; };
static struct poll_arg poll_args[MAXPORT][8];

static inline u32 prd(struct host_port *p, u32 off)          { return readl(p->w + off); }
static inline void pwr(struct host_port *p, u32 off, u32 v)  { writel(v, p->w + off); }
static inline u64 prd64(struct host_port *p, u32 off)        { return readq(p->w + off); }
static inline void pwr64(struct host_port *p, u32 off, u64 v){ writeq(v, p->w + off); }

static int octdma_alloc(struct host_port *p)
{
	if (!pdev) {
		pdev = pci_get_device(0x177d, 0x0092, NULL);
		if (!pdev) { pr_err("octnic: EP 177d:0092 not found\n"); return -ENODEV; }
		if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64)))
			return -EIO;
	}
	p->tx_dma_va = dma_alloc_coherent(&pdev->dev, POOL_SZ, &p->tx_dma_bus, GFP_KERNEL);
	p->rx_dma_va = dma_alloc_coherent(&pdev->dev, POOL_SZ, &p->rx_dma_bus, GFP_KERNEL);
	if (!p->tx_dma_va || !p->rx_dma_va) return -ENOMEM;
	pci_set_master(pdev);			/* card masters host RAM (IOMMU-contained) */
	if (hrx) {				/* seed each slot's header phase to "not ready"
						 * (opposite of first expected) so the drain waits
						 * until the card actually fills the slot. */
		u32 s;
		for (s = 0; s < RING_SZ; s++)
			*(__le32 *)((u8 *)p->rx_dma_va + s * BUF_SZ + 4) =
				cpu_to_le32(RX_PHASE(s) ^ 1u);
	}
	pwr64(p, C_TX_DMA_BASE, (u64)p->tx_dma_bus);
	pwr64(p, C_RX_DMA_BASE, (u64)p->rx_dma_bus);
	wmb();
	pwr(p, C_DMA_ENABLE, 1);		/* tell card to switch to DMA */
	wmb();
	pr_info("octnic: port%d DMA on, tx_bus=0x%llx rx_bus=0x%llx\n",
		p->idx, (u64)p->tx_dma_bus, (u64)p->rx_dma_bus);
	return 0;
}
static void octdma_free(struct host_port *p)
{
	if (!pdev) return;
	pwr(p, C_DMA_ENABLE, 0);
	wmb();
	msleep(100);	/* let the card disarm + in-flight DPI writes drain before the
			 * coherent pools are freed (else a late DMA lands in reused RAM) */
	if (p->tx_dma_va) dma_free_coherent(&pdev->dev, POOL_SZ, p->tx_dma_va, p->tx_dma_bus);
	if (p->rx_dma_va) dma_free_coherent(&pdev->dev, POOL_SZ, p->rx_dma_va, p->rx_dma_bus);
	p->tx_dma_va = p->rx_dma_va = NULL;
}

static netdev_tx_t oct_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct host_port *p = PORT(dev);
	u32 tp, slot, len = skb->len;

	/* CAS-claim a slot: parallel across txqs, and on full DROP without claiming
	 * (no hole -- the card drains by phase and would stall on an unfilled slot). */
	for (;;) {
		tp = (u32)atomic_read(&p->tx_claim);
		if ((tp - READ_ONCE(p->tc_cache)) >= RING_SZ) {
			u32 tc = prd(p, C_TX_CONS);

			WRITE_ONCE(p->tc_cache, tc);
			if ((tp - tc) >= RING_SZ) {
				dev->stats.tx_dropped++;
				dev_kfree_skb_any(skb);
				return NETDEV_TX_OK;
			}
		}
		if ((u32)atomic_cmpxchg(&p->tx_claim, (int)tp, (int)(tp + 1)) == tp)
			break;
	}
	if (len > BUF_SZ)
		len = BUF_SZ;
	slot = tp & RING_MASK;
	if (ztx && dma && p->tx_dma_va)	/* zero-copy: local copy into coherent pool, card
					 * DPI-reads it -> no host PIO byte-push over PCIe */
		memcpy(p->tx_dma_va + slot * BUF_SZ, skb->data, len);
	else				/* PIO: host posted-write into card BAR window */
		memcpy_toio(p->w + TXBUF_OFF + slot * BUF_SZ, skb->data, len);
	wmb();
	pwr(p, TXDESC_OFF + slot * 8, len);			/* desc len */
	wmb();							/* len+data before phase */
	pwr(p, TXDESC_OFF + slot * 8 + 4, TX_PHASE(tp));	/* phase LAST = slot ready to xmit */
	wmb();
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* Drain up to `budget` frames from a port's card->host RX ring into the stack.
 * Returns the number consumed. */
static int oct_drain(struct host_port *p, int tid, int budget)
{
	u32 stride = rxthreads;
	u32 rp = lockfree ? 0 : prd(p, C_RX_PROD);
	u32 rc = p->rc_pub[tid];
	int n = 0;
	LIST_HEAD(rx_list);

	while (n < budget) {
		u32 slot = rc & RING_MASK;
		u32 len;
		struct sk_buff *skb;

		if (hrx) {
			/* descriptor lives in LOCAL host RAM (card DPI'd it, phase last).
			 * No PCIe MMIO: read {len,phase} from the slot's 8-byte header. */
			u8 *h = (u8 *)p->rx_dma_va + slot * BUF_SZ;
			u32 phase = le32_to_cpu(*(__le32 *)(h + 4));

			if (phase != RX_PHASE(rc))
				break;			/* not filled yet */
			rmb();				/* phase seen => data landed */
			len = le32_to_cpu(*(__le32 *)h);
		} else if (lockfree) {
			/* one 64-bit desc read = {len, flags}. Ready iff the slot's phase
			 * bit matches what this consumer position expects (virtio-style). */
			u64 desc = prd64(p, RXDESC_OFF + slot * 8);

			if (((u32)(desc >> 32) & 1u) != RX_PHASE(rc))
				break;			/* stale phase: not filled yet */
			len = (u32)desc;
		} else {
			if (rc == rp)
				break;
			len = prd(p, RXDESC_OFF + slot * 8);
		}

		if (len && len <= BUF_SZ) {
			skb = netdev_alloc_skb(p->ndev, len + 2);
			if (skb) {
				skb_reserve(skb, 2);
				if (dma)	/* card DMA-wrote into local coherent RAM */
					memcpy(skb_put(skb, len),
					       p->rx_dma_va + slot * BUF_SZ + (hrx ? HRX_HDR : 0),
					       len);
				else
					memcpy_fromio(skb_put(skb, len),
						      p->w + RXBUF_OFF + slot * BUF_SZ, len);
				skb->protocol = eth_type_trans(skb, p->ndev);
				p->ndev->stats.rx_packets++;
				p->ndev->stats.rx_bytes += len;
				if (rxbatch)		/* batch: one stack entry per drain */
					list_add_tail(&skb->list, &rx_list);
				else
					netif_receive_skb(skb);	/* BH disabled by caller */
			} else {
				p->ndev->stats.rx_dropped++;
			}
		}
		rc += stride;			/* next slot THIS thread owns */
		n++;
	}
	p->rc_pub[tid] = rc;
	{	/* publish consumer = min across threads (contiguous consumed floor);
		 * values stay within a batch of each other so signed diff is safe */
		u32 m = p->rc_pub[0], i;

		for (i = 1; i < stride; i++)
			if ((s32)(p->rc_pub[i] - m) < 0)
				m = p->rc_pub[i];
		if (m != p->rc_shadow) {
			p->rc_shadow = m;
			pwr(p, C_RX_CONS, m);
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
	struct poll_arg *pa = arg;
	struct host_port *p = pa->p;
	int tid = pa->tid;
	unsigned int idle = 0;

	while (!kthread_should_stop()) {
		int did;

		local_bh_disable();		/* netif_receive_skb needs BH context */
		did = oct_drain(p, tid, 64);
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
	struct host_port *p = PORT(dev);
	int i;

	p->tp_shadow = prd(p, C_TX_PROD);	/* sync shadows to current ring */
	p->tc_cache  = prd(p, C_TX_CONS);
	atomic_set(&p->tx_claim, 0);		/* fresh TX ring (card resets tx_cons + seeds phase at arm) */
	p->tc_cache = 0;
	/* snap RX consumer to the card's current producer (drop any stale/desynced
	 * count). Avoids an infinite poll spin if C_RX_CONS holds garbage. */
	p->rc_shadow = prd(p, C_RX_PROD);
	if (hrx)			/* hrx ring is fresh at 0 (card realigns claim at arm) */
		p->rc_shadow = 0;
	pwr(p, C_RX_CONS, p->rc_shadow);
	pwr(p, C_HOST_READY, 1);
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

	for (i = 0; i < rxthreads; i++)
		p->rc_pub[i] = p->rc_shadow + i;	/* thread i owns slots ==i (mod rxthreads) */
	for (i = 0; i < rxthreads; i++) {
		poll_args[p->idx][i].p = p;
		poll_args[p->idx][i].tid = i;
		p->poll_threads[i] = kthread_run(poll_fn, &poll_args[p->idx][i],
						 "octshm-rx%d/%d", p->idx, i);
		if (IS_ERR(p->poll_threads[i])) {
			p->poll_threads[i] = NULL;
			break;
		}
	}
	if (!p->poll_threads[0]) {
		pwr(p, C_HOST_READY, 0);
		netif_stop_queue(dev);
		return -ENOMEM;
	}
	return 0;
}
static int oct_stop(struct net_device *dev)
{
	struct host_port *p = PORT(dev);
	int i;

	netif_stop_queue(dev);
	for (i = 0; i < 8; i++)
		if (p->poll_threads[i]) {
			kthread_stop(p->poll_threads[i]);
			p->poll_threads[i] = NULL;
		}
	pwr(p, C_HOST_READY, 0);
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
 * host `sensors` shows them as "cavium_card": temp1=board, temp2=Octeon die.
 * Read from port0's ctrl page (the card daemon publishes there). */
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
	*val = (long)(s32)readl(hp[0].w + (channel == 0 ? C_RESV0 : C_RESV1));	/* millidegC */
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

static int nregistered;			/* how many netdevs got registered (for cleanup) */

static int __init octshm_host_init(void)
{
	u32 magic, cr;
	int ret, i;

	if (ports < 1) ports = 1;
	if (ports > MAXPORT) ports = MAXPORT;

	if (!base) {		/* auto-discover card BAR2 so `modprobe octnic` needs no args */
		struct pci_dev *pd = pci_get_device(0x177d, 0x0092, NULL);
		u16 cmd;

		if (!pd) {
			pr_err("octnic: CN6640 (177d:0092) not found\n");
			return -ENODEV;
		}
		base = pci_resource_start(pd, 2);	/* BAR2 = card DRAM window */
		pci_read_config_word(pd, PCI_COMMAND, &cmd);	/* ensure MEM+bus-master on */
		pci_write_config_word(pd, PCI_COMMAND,
				      cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
		pci_dev_put(pd);
		if (!base) {
			pr_err("octnic: BAR2 unassigned (enable Above-4G / re-scan)\n");
			return -ENODEV;
		}
	}

	win = ioremap_wc(base, (u32)ports * PORT_STRIDE);	/* WC: batch TX writes into big TLPs */
	if (!win)
		return -ENOMEM;
	for (i = 0; i < ports; i++) {
		hp[i].idx = i;
		hp[i].w = win + (unsigned long)i * PORT_STRIDE;
	}
	magic = readl(hp[0].w + C_MAGIC);
	cr = readl(hp[0].w + C_CARD_READY);
	if (magic != OCTSHM_MAGIC) {
		pr_err("octnic: bad magic 0x%08x at base 0x%lx (card module loaded? BAR2 set?)\n",
		       magic, base);
		iounmap(win);
		return -ENODEV;
	}
	pr_info("octnic: magic OK ver=%u card_ready=%u ports=%d\n",
		readl(hp[0].w + C_VERSION), cr, ports);

	if (ntxq < 1) ntxq = 1;
	if (ntxq > 8) ntxq = 8;

	for (i = 0; i < ports; i++) {
		struct host_port *p = &hp[i];

		if (i > 0 && readl(p->w + C_MAGIC) != OCTSHM_MAGIC) {
			pr_err("octnic: port%d magic bad -- card built with ports=%d?\n",
			       i, ports);
			ret = -ENODEV;
			goto err;
		}
		if (dma) {
			ret = octdma_alloc(p);
			if (ret)
				goto err;
		}
		p->ndev = alloc_etherdev_mq(sizeof(struct host_port *), ntxq);
		if (!p->ndev) { ret = -ENOMEM; goto err; }
		PORT(p->ndev) = p;
		netif_set_real_num_tx_queues(p->ndev, ntxq);	/* N txqs -> N cores xmit in parallel */
		snprintf(p->ndev->name, IFNAMSIZ, "oct%d", i);
		p->ndev->netdev_ops = &oct_ops;
		eth_hw_addr_random(p->ndev);
		p->ndev->mtu = 1500;
		p->ndev->max_mtu = 9000;		/* allow jumbo (BUF_SZ=9216) */

		ret = register_netdev(p->ndev);
		if (ret) {
			free_netdev(p->ndev);
			p->ndev = NULL;
			goto err;
		}
		nregistered++;
		pr_info("octnic: registered %s\n", p->ndev->name);
	}

	if (!pdev)					/* hwmon needs a parent dev */
		pdev = pci_get_device(0x177d, 0x0092, NULL);
	if (pdev) {
		hwmon_dev = hwmon_device_register_with_info(&pdev->dev,
				"cavium_card", NULL, &oct_hwmon_chip, NULL);
		if (!IS_ERR(hwmon_dev))
			pr_info("octnic: hwmon cavium_card registered\n");
		else
			hwmon_dev = NULL;
	}
	return 0;

err:
	for (i = 0; i < ports; i++) {
		if (hp[i].ndev) {
			if (i < nregistered)
				unregister_netdev(hp[i].ndev);
			free_netdev(hp[i].ndev);
			hp[i].ndev = NULL;
		}
		if (dma)
			octdma_free(&hp[i]);
	}
	if (pdev) { pci_clear_master(pdev); pci_dev_put(pdev); pdev = NULL; }
	iounmap(win);
	return ret;
}

static void __exit octshm_host_exit(void)
{
	int i;

	if (hwmon_dev)
		hwmon_device_unregister(hwmon_dev);
	for (i = 0; i < ports; i++) {
		if (hp[i].ndev) {
			unregister_netdev(hp[i].ndev);
			free_netdev(hp[i].ndev);
			hp[i].ndev = NULL;
		}
		if (dma)
			octdma_free(&hp[i]);
	}
	if (pdev) { pci_clear_master(pdev); pci_dev_put(pdev); }
	iounmap(win);
	pr_info("octnic: unloaded\n");
}
module_init(octshm_host_init);
module_exit(octshm_host_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("octnic: Cavium CN6640 shared-memory NIC (host side, oct0/oct1)");
