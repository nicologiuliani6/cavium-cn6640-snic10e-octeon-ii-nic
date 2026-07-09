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

static char *dev = "xaui0";
module_param(dev, charp, 0444);
static int ipd_port;			/* xaui0 = interface 0 port 0 = ipd_port 0 */
module_param(ipd_port, int, 0444);
static unsigned int period_ms = 200;
module_param(period_ms, uint, 0444);

static struct net_device *nd;
static struct timer_list t;

static void force_up(void)
{
	union cvmx_helper_link_info li;

	li.u64 = 0;
	li.s.link_up = 1;
	li.s.full_duplex = 1;
	li.s.speed = 10000;
	cvmx_helper_link_set(ipd_port, li);
	if (nd && !netif_carrier_ok(nd))
		netif_carrier_on(nd);
}

static void tick(struct timer_list *unused)
{
	force_up();
	mod_timer(&t, jiffies + msecs_to_jiffies(period_ms));
}

static int __init m_init(void)
{
	nd = dev_get_by_name(&init_net, dev);
	if (!nd) {
		pr_err("octcarrier: %s not found\n", dev);
		return -ENODEV;
	}
	force_up();
	timer_setup(&t, tick, 0);
	mod_timer(&t, jiffies + msecs_to_jiffies(period_ms));
	pr_info("octcarrier: forcing link_set(up)+carrier for %s (ipd_port %d)\n",
		dev, ipd_port);
	return 0;
}

static void __exit m_exit(void)
{
	del_timer_sync(&t);
	if (nd)
		dev_put(nd);
	pr_info("octcarrier: released %s\n", dev);
}
module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
