// SPDX-License-Identifier: GPL-2.0
/* octcarrier: force an xaui/XFI port's link UP in hardware so TX un-gates.
 *
 * The octeon-ethernet link poll leaves the GMX/PKO TX path DISABLED for a port
 * whose Vitesse PHY reports link-down (foreign QLogic DAC): cvmx_helper_link_set
 * is only ever called with link_up=0, so frames never leave the MAC (tx_packets
 * stays 0) even if we force netif_carrier on. Here we call cvmx_helper_link_set
 * ourselves with a real 10G/full/up state to enable the hardware TX datapath,
 * and also flip netif_carrier so Linux dequeues. A timer re-asserts both against
 * the driver poll. RX on this port is already proven clean, so this only adds
 * the missing TX enable. Safe probe: if the PHY TX line is truly dead, nothing
 * reaches the peer and tx_packets still won't climb. */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/timer.h>
#include <asm/octeon/cvmx.h>
#include <asm/octeon/cvmx-helper.h>

/* Up to MAXP ports forced up. dev/ipd_port are comma-lists, e.g. dev=xaui0,xaui1
 * ipd_port=0,16 -- one entry per port. Single values keep the old 1-port behaviour. */
#define MAXP 4
static char *dev = "xaui0";
module_param(dev, charp, 0444);
static char *ipd_port = "0";		/* xaui0 = ipd_port 0; xaui1 = 16 on CN66xx XAUI */
module_param(ipd_port, charp, 0444);
static unsigned int period_ms = 200;
module_param(period_ms, uint, 0444);

static struct net_device *nd[MAXP];
static int ipd[MAXP];
static int nports;
static struct timer_list t;

static void force_up(void)
{
	union cvmx_helper_link_info li;
	int i;

	li.u64 = 0;
	li.s.link_up = 1;
	li.s.full_duplex = 1;
	li.s.speed = 10000;
	for (i = 0; i < nports; i++) {
		cvmx_helper_link_set(ipd[i], li);
		if (nd[i] && !netif_carrier_ok(nd[i]))
			netif_carrier_on(nd[i]);
	}
}

static void tick(struct timer_list *unused)
{
	force_up();
	mod_timer(&t, jiffies + msecs_to_jiffies(period_ms));
}

static int __init m_init(void)
{
	char *dp = dev, *ip = ipd_port, *tok;

	while ((tok = strsep(&dp, ",")) && nports < MAXP) {
		char *itok = strsep(&ip, ",");
		struct net_device *n = dev_get_by_name(&init_net, tok);

		if (!n) {
			pr_err("octcarrier: %s not found\n", tok);
			continue;
		}
		nd[nports] = n;
		ipd[nports] = itok ? (int)simple_strtol(itok, NULL, 0) : 0;
		pr_info("octcarrier: forcing link up for %s (ipd_port %d)\n",
			tok, ipd[nports]);
		nports++;
	}
	if (!nports)
		return -ENODEV;
	force_up();
	timer_setup(&t, tick, 0);
	mod_timer(&t, jiffies + msecs_to_jiffies(period_ms));
	return 0;
}

static void __exit m_exit(void)
{
	int i;

	del_timer_sync(&t);
	for (i = 0; i < nports; i++)
		if (nd[i])
			dev_put(nd[i]);
	pr_info("octcarrier: released %d port(s)\n", nports);
}
module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
