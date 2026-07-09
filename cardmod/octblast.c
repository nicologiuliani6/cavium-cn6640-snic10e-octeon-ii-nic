// SPDX-License-Identifier: GPL-2.0
/* octblast: minimal kernel packet blaster. Spawns `threads` kthreads that transmit a
 * reused skb on `dev` (default xaui1) as fast as the driver accepts, to feed the RX
 * datapath at ~line rate (xaui1 -> DAC -> xaui0 -> octshm tap -> DPI -> host oct0),
 * independent of iperf/TCP so we can find the true RX-datapath ceiling. Measure the
 * result on the host with `ip -s link show oct0` (rx bytes/s). */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/delay.h>

static char *dev = "xaui1";
module_param(dev, charp, 0444);
static int len = 1500;			/* L2 payload length */
module_param(len, int, 0444);
static int threads = 2;
module_param(threads, int, 0444);
static int secs = 5;
module_param(secs, int, 0444);

#define MAXT 8
static struct task_struct *th[MAXT];
static struct net_device *ndev;
static atomic64_t total_pkts = ATOMIC64_INIT(0);

#define FLOWS 32		/* distinct src ports per thread -> spread over RX groups */

/* build one eth+IPv4+UDP frame with the given src port (varied 5-tuple lets the octeon
 * PIP grptag hash spread frames across receive groups/cores; L2-only = one group). */
static struct sk_buff *build_frame(int hlen, u16 sp)
{
	struct sk_buff *skb = alloc_skb(len + hlen + 64, GFP_KERNEL);
	u8 *d, *ip, *udp;
	struct ethhdr *e;
	u16 iplen = len - ETH_HLEN;

	if (!skb)
		return NULL;
	skb_reserve(skb, hlen);
	d = skb_put(skb, len);
	e = (struct ethhdr *)d;
	ip = d + ETH_HLEN;
	udp = ip + 20;
	eth_broadcast_addr(e->h_dest);
	memcpy(e->h_source, ndev->dev_addr, ETH_ALEN);
	e->h_proto = htons(ETH_P_IP);
	memset(ip, 0, 28);
	ip[0] = 0x45; ip[2] = iplen >> 8; ip[3] = iplen & 0xff;
	ip[8] = 64; ip[9] = 17;
	ip[12] = 10; ip[13] = 9; ip[14] = 9; ip[15] = 100;
	ip[16] = 10; ip[17] = 9; ip[18] = 9; ip[19] = 1;
	udp[0] = sp >> 8; udp[1] = sp & 0xff;
	udp[2] = 0x13; udp[3] = 0x89;
	udp[4] = (iplen - 20) >> 8; udp[5] = (iplen - 20) & 0xff;
	memset(d + ETH_HLEN + 28, 0xA5, len - ETH_HLEN - 28);
	skb->dev = ndev;
	skb->protocol = htons(ETH_P_IP);
	skb_reset_mac_header(skb);
	return skb;
}

static int blast_fn(void *arg)
{
	long id = (long)arg;
	unsigned long tend = jiffies + secs * HZ;
	u64 n = 0;
	int hlen = LL_RESERVED_SPACE(ndev);
	u16 sp = 2000 + (id << 6);	/* per-thread port base, ++ per pkt -> many flows */

	/* NOTE: alloc a fresh skb per packet. skb_get()-reuse of a pool CRASHES the octeon
	 * TX path (driver holds/mangles the skb), so pay the per-pkt alloc. */
	while (!kthread_should_stop() && time_before(jiffies, tend)) {
		struct sk_buff *skb = build_frame(hlen, sp++);

		if (!skb) { usleep_range(10, 30); continue; }
		if (dev_queue_xmit(skb) == NETDEV_TX_OK)
			n++;
		if ((n & 0x3FF) == 0)
			cond_resched();
	}
	atomic64_add(n, &total_pkts);
	return 0;
}

static int __init blast_init(void)
{
	int i;

	ndev = dev_get_by_name(&init_net, dev);
	if (!ndev) { pr_err("octblast: dev %s not found\n", dev); return -ENODEV; }
	if (threads < 1) threads = 1;
	if (threads > MAXT) threads = MAXT;
	pr_info("octblast: blasting %s len=%d threads=%d for %ds\n", dev, len, threads, secs);
	for (i = 0; i < threads; i++) {
		th[i] = kthread_create(blast_fn, (void *)(long)i, "octblast/%d", i);
		if (!IS_ERR(th[i]))
			wake_up_process(th[i]);
		else
			th[i] = NULL;
	}
	return 0;
}

static void __exit blast_exit(void)
{
	int i;
	u64 tot;

	for (i = 0; i < MAXT; i++)
		if (!IS_ERR_OR_NULL(th[i]))
			kthread_stop(th[i]);
	tot = atomic64_read(&total_pkts);
	pr_info("octblast: sent %llu pkts (~%llu Mbit in %ds = %llu Mbit/s)\n",
		tot, (tot * len * 8) / 1000000, secs,
		secs ? (tot * len * 8) / 1000000 / secs : 0);
	if (ndev)
		dev_put(ndev);
}
module_init(blast_init);
module_exit(blast_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("minimal packet blaster to feed the RX datapath");
