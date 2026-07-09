// SPDX-License-Identifier: GPL-2.0
/* octpko_card: card-side Phase-1 probe. Try to emit a test packet to the PCIe
 * PKO port (32) so it lands in the host SLI output queue (octoq_host). If the
 * host sees PKTS_SENT increment, the card->host hardware packet DMA works and we
 * can build the RX offload on it. If PKO to the PCI port isn't set up (OpenWrt
 * skips NPI on CN66xx), the send returns an error -> we then hand-roll the setup. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx.h>
#include <asm/octeon/cvmx-pko.h>
#include <asm/octeon/cvmx-fpa.h>
#include <asm/octeon/cvmx-helper.h>

static int port = 32;
module_param(port, int, 0444);
static int nsend = 4;
module_param(nsend, int, 0444);

static int __init m_init(void)
{
	int queue = cvmx_pko_get_base_queue(port);
	int nq = cvmx_pko_get_num_queues(port);
	int i;

	pr_info("octpko: port=%d base_queue=%d num_queues=%d\n", port, queue, nq);
	if (queue == CVMX_PKO_ILLEGAL_QUEUE) {
		pr_err("octpko: illegal queue for port %d\n", port);
		return -EINVAL;
	}

	for (i = 0; i < nsend; i++) {
		union cvmx_pko_command_word0 cmd;
		union cvmx_buf_ptr bp;
		cvmx_pko_status_t st;
		u8 *buf = cvmx_fpa_alloc(CVMX_FPA_PACKET_POOL);
		int len = 64;

		if (!buf) { pr_err("octpko: fpa_alloc fail\n"); return -ENOMEM; }
		memset(buf, 0, len);
		/* test ethernet frame: dst ff.., src 02:.., ethertype 0x88b5, marker */
		memset(buf, 0xff, 6);
		buf[6] = 0x02; buf[7] = 0xca; buf[8] = 0xfe;
		buf[12] = 0x88; buf[13] = 0xb5;
		buf[14] = 0xDE; buf[15] = 0xAD; buf[16] = 0xBE; buf[17] = 0xEF;
		buf[18] = (u8)i;

		bp.u64 = 0;
		bp.s.addr = cvmx_ptr_to_phys(buf);
		bp.s.pool = CVMX_FPA_PACKET_POOL;
		bp.s.size = len;

		cmd.u64 = 0;
		cmd.s.total_bytes = len;
		cmd.s.segs = 1;
		cmd.s.dontfree = 0;	/* let PKO free the buffer back to pool 0 */

		cvmx_pko_send_packet_prepare(port, queue, CVMX_PKO_LOCK_NONE);
		st = cvmx_pko_send_packet_finish(port, queue, cmd, bp,
						 CVMX_PKO_LOCK_NONE);
		pr_info("octpko: send %d -> status=%d (0=SUCCESS)\n", i, st);
	}
	return 0;
}
static void __exit m_exit(void) { pr_info("octpko: unloaded\n"); }
module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL");
