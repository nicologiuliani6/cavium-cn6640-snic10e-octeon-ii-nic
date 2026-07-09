// SPDX-License-Identifier: GPL-2.0
/* octnpi: Path A egress proof (card side). Sends frames out the card's NPI/PCI
 * port (npi0) so PKO -> SLI output-queue DMAs them straight into a host-RAM ring,
 * with NO per-packet card CPU copy (unlike octshm's DPI-from-CPU path).
 *
 * The OpenWrt octeon-ethernet driver already exposes npi0..npi3 (CN66XX NPI probe
 * patched to return 4 PCI ports); xmit on npi0 uses cvm_oct_xmit -> PKO to the PCI
 * ipd_port -> SLI. The host side (octoq) programs+reads the SLI output queue.
 *
 * mode=test (default): a kthread transmits a magic test frame to npi0 every `ms`,
 *   so the host can prove PKO->SLI->host-RAM egress works with no wire traffic.
 * mode=fwd + src=xaui0: tap src RX and re-xmit each frame to npi0 (real egress).
 *
 * SAFETY: this runs entirely on the card. It never touches the host BAR, so it
 * cannot freeze the host. If it wedges the card, the host (which only reads its
 * own RAM ring) stays alive and can detach the card over config space.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/rtnetlink.h>
#include <asm/octeon/octeon.h>

/* SLI SCRATCH1 = card->host telemetry (host reads BAR0+0x3C0, one-shot, no DMA).
 * Packs: [63:32] frames xmit-OK, [31:16] frames dropped, [1] npi found, [0] npi open. */
#define OCTNPI_SCRATCH  0x00011F00000103C0ull	/* SCRATCH1: [63:32]=n_ok [31:16]=n_drop [0]=opened */
#define OCTNPI_SCRATCH2 0x00011F00000103D0ull	/* SCRATCH2: [63:32]=tx_pkts [31:16]=tx_err [15:0]=tx_drop */
static struct net_device *npi_dev;
static atomic_t n_ok = ATOMIC_INIT(0), n_drop = ATOMIC_INIT(0);
static int npi_opened;
static void report(void)
{
	struct rtnl_link_stats64 st = {0};
	u64 v = ((u64)atomic_read(&n_ok) << 32) |
		((u64)(atomic_read(&n_drop) & 0xffff) << 16) |
		(npi_opened ? 1u : 0u);
	cvmx_write_csr(CVMX_ADD_IO_SEG(OCTNPI_SCRATCH), v);
	if (npi_dev) {
		dev_get_stats(npi_dev, &st);
		cvmx_write_csr(CVMX_ADD_IO_SEG(OCTNPI_SCRATCH2),
			((u64)(st.tx_packets & 0xffffffff) << 32) |
			((u64)(st.tx_errors & 0xffff) << 16) |
			(u64)(st.tx_dropped & 0xffff));
	}
}

static char *dev = "npi0";	/* NPI/PCI netdev to egress on */
module_param(dev, charp, 0444);
static char *src = "";		/* fwd mode: xaui source to tap (empty => test mode) */
module_param(src, charp, 0444);
static int ms = 500;		/* test mode: inter-frame gap (ms) */
module_param(ms, int, 0444);
static int len = 64;		/* test frame length */
module_param(len, int, 0444);

#define OCTNPI_ETYPE 0x88b5	/* local-experimental ethertype */
#define OCTNPI_MAGIC 0x4f43504e	/* "OCPN" */

static struct net_device *npi_dev, *src_dev;
static struct task_struct *tx_thread;
static struct packet_type ptype;
static atomic_t counter = ATOMIC_INIT(0);

/* build+send one test frame to npi0 */
static void send_test(void)
{
	struct sk_buff *skb;
	u8 *p;
	u32 c = (u32)atomic_inc_return(&counter);
	int flen = len < 64 ? 64 : len;

	skb = netdev_alloc_skb(npi_dev, flen + NET_IP_ALIGN);
	if (!skb)
		return;
	skb_reserve(skb, NET_IP_ALIGN);
	p = skb_put(skb, flen);
	memset(p, 0, flen);
	eth_broadcast_addr(p);				/* dst = broadcast */
	memcpy(p + 6, npi_dev->dev_addr, 6);		/* src = npi0 mac */
	p[12] = (OCTNPI_ETYPE >> 8) & 0xff;
	p[13] = OCTNPI_ETYPE & 0xff;
	/* payload: magic + counter (big-endian so host sees it clearly) */
	p[14] = (OCTNPI_MAGIC >> 24) & 0xff; p[15] = (OCTNPI_MAGIC >> 16) & 0xff;
	p[16] = (OCTNPI_MAGIC >> 8) & 0xff;  p[17] = OCTNPI_MAGIC & 0xff;
	p[18] = (c >> 24) & 0xff; p[19] = (c >> 16) & 0xff;
	p[20] = (c >> 8) & 0xff;  p[21] = c & 0xff;

	skb->dev = npi_dev;
	skb->protocol = htons(OCTNPI_ETYPE);
	skb_reset_mac_header(skb);
	if (dev_queue_xmit(skb) == NET_XMIT_DROP)
		atomic_inc(&n_drop);
	else
		atomic_inc(&n_ok);
	report();
}

static int tx_fn(void *arg)
{
	pr_info("octnpi: test-tx thread up, %dB frames every %dms to %s\n",
		len, ms, dev);
	while (!kthread_should_stop()) {
		send_test();
		msleep(ms);
	}
	return 0;
}

/* fwd mode: each frame received on src (xaui0) is re-xmitted out npi0 */
static int oct_rx(struct sk_buff *skb, struct net_device *d,
		  struct packet_type *pt, struct net_device *orig)
{
	struct sk_buff *tx;

	if (skb->dev != src_dev)
		goto drop;
	tx = skb_copy(skb, GFP_ATOMIC);
	if (tx) {
		tx->dev = npi_dev;
		skb_reset_mac_header(tx);
		dev_queue_xmit(tx);
	}
drop:
	kfree_skb(skb);
	return 0;
}

static int __init octnpi_init(void)
{
	int rc;

	npi_dev = dev_get_by_name(&init_net, dev);
	if (!npi_dev) {
		pr_err("octnpi: %s not found (npi ports not exposed?)\n", dev);
		return -ENODEV;
	}
	rtnl_lock();
	rc = dev_open(npi_dev, NULL);
	rtnl_unlock();
	if (rc) {
		pr_err("octnpi: %s open failed %d\n", dev, rc);
		dev_put(npi_dev);
		return rc;
	}
	npi_opened = 1;
	report();
	pr_info("octnpi: %s up (ifindex=%d mac=%pM)\n", dev, npi_dev->ifindex,
		npi_dev->dev_addr);

	if (src && *src) {			/* fwd mode: tap xaui source */
		src_dev = dev_get_by_name(&init_net, src);
		if (!src_dev) {
			pr_err("octnpi: src %s not found\n", src);
			dev_put(npi_dev);
			return -ENODEV;
		}
		rtnl_lock();
		dev_set_promiscuity(src_dev, 1);
		rtnl_unlock();
		ptype.type = htons(ETH_P_ALL);
		ptype.dev = src_dev;
		ptype.func = oct_rx;
		dev_add_pack(&ptype);
		pr_info("octnpi: fwd %s RX -> %s TX\n", src, dev);
	} else {				/* test mode: kthread of magic frames */
		tx_thread = kthread_run(tx_fn, NULL, "octnpi-tx");
		if (IS_ERR(tx_thread)) {
			tx_thread = NULL;
			dev_put(npi_dev);
			return -EIO;
		}
	}
	return 0;
}

static void __exit octnpi_exit(void)
{
	if (tx_thread)
		kthread_stop(tx_thread);
	if (src_dev) {
		dev_remove_pack(&ptype);
		rtnl_lock();
		dev_set_promiscuity(src_dev, -1);
		rtnl_unlock();
		dev_put(src_dev);
	}
	if (npi_dev)
		dev_put(npi_dev);
	pr_info("octnpi: unloaded\n");
}
module_init(octnpi_init);
module_exit(octnpi_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Path A egress: send frames out npi0 (PKO->SLI->host DMA)");
