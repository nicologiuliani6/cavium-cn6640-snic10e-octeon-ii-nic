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
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx.h>
#include <asm/octeon/cvmx-pip.h>	/* cvmx_pip_get_port_status (RX drop profiling) */

#define PEM_P2N_BAR0   0x00011800C0000080ull
#define PEM_P2N_BAR1   0x00011800C0000088ull
#define PEM_BAR1_IDX0  0x00011800C00000A8ull
#define PEM_BAR1_IDX1  0x00011800C00000B0ull	/* second 4MB granule (port1) */
#define WIN_ORDER      10			/* 4MB (1 port); +1 => 8MB for 2 ports */

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
#define ZC_HDR         64			/* zc: bytes copied linear (eth+ip+tcp) so the
						 * octeon-ethernet ip_hdr()/csum block parses OK;
						 * payload beyond this is zero-copy frag */

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
static int hrx;				/* host-RAM RX descriptors: DPI-write an 8-byte
					 * {len,phase} header ahead of each frame's data, so
					 * the host reads descriptors from local RAM (no per-frame
					 * PCIe MMIO). Header rides the SAME DPI op as the data
					 * (posted last = lands after data) -> ordering-safe. */
module_param(hrx, int, 0444);
#define HRX_HDR 8
static int bench;			/* >0: run a raw-DPI throughput benchmark for N sec */
module_param(bench, int, 0444);
static int blen = 1500;			/* benchmark transfer size (bytes) */
module_param(blen, int, 0444);
/* Multi-port: one host netdev per card uplink (oct0<->xaui0, oct1<->xaui1),
 * each owning a private 4MB window region at PORT_STRIDE*i. ports=1 keeps
 * port0 byte-identical to the single-port build. uplink is a comma-list. */
static int ports = 1;
module_param(ports, int, 0444);

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

/* ---- DPI hardware-DMA offload for the RX copy (dma=2). The DPI engine moves the
 * frame card-L2 -> host RAM in hardware, freeing the 2 card cores from the per-byte
 * SLI-mem-access copy that caps CPU mode (~2.2G). DPI register config + OUTBOUND
 * instruction format reverse-engineered from the vendor firmware CSR dump (proven
 * by octdpi_test: wrote byte-swapped DPI_MAGIC into a host IOVA). Instruction FIFO
 * (chunk chaining + FPA recycle) managed by the kernel's cvmx-cmd-queue. ---- */
#include <asm/octeon/cvmx-fpa.h>
#include <asm/octeon/cvmx-cmd-queue.h>
#define DPIR_CTL       0x0001DF0000000040ull
#define DPIR_DMA_CTL   0x0001DF0000000048ull
#define DPIR_GBL_EN    0x0001DF0000000050ull
#define DPIR_NCBX_CFG  0x0001DF0000000800ull
#define DPIR_PRTX0     0x0001DF0000000900ull
#define DPIR_ENGBUF(e) (0x0001DF0000000880ull + ((e)&7)*8)
#define DPIR_ENGEN(e)  (0x0001DF0000000080ull + ((e)&7)*8)
#define DPIR_DBELL(q)  (0x0001DF0000000200ull + ((q)&7)*8)
#define DPIR_IBUFF(q)  (0x0001DF0000000280ull + ((q)&7)*8)
#define DPIR_COUNTS(q) (0x0001DF0000000300ull + ((q)&7)*8)
#define DPI_POOL   7
#define DPI_PSIZE  1024			/* FPA block bytes per DPI instr chunk */
#define DPI_NBLK   256			/* donate 256KB of chunks to the pool */
#define DPI_NQ     4			/* DMA queues 4..7 (avoid PKO-owned low queues) */
#define DPI_Q0     4
extern __cvmx_cmd_queue_all_state_t *__cvmx_cmd_queue_state_ptr;
static unsigned long dpi_region;
static spinlock_t dpi_lock[DPI_NQ];
/* RX-outbound DPI queue picker: the ptype tap runs on cpu0 only, so a cpu-id pick
 * would hammer queue 0 alone (1 of 4 DPI engines). Round-robin spreads each frame
 * across all 4 queues -> 4 engines DMA in parallel. DPI_NQ is a power of 2. */
static atomic_t dpi_rr = ATOMIC_INIT(0);
#define DPI_RR_K() ((u32)atomic_inc_return(&dpi_rr) & (DPI_NQ - 1))
/* RX profiling counters (racy += across NAPI cores: approximation is fine).
 * Worker 0 mirrors them into the port0 ctrl page at +0x100 each heartbeat so the
 * host can read them over BAR2 (mmap resource2). Cycle unit = 1.6GHz core clock. */
static u64 rxp_frames, rxp_cyc, rxp_cyc_dpi, rxp_fallback, rxp_ringfull;
#define RXPROF_OFF 0x100
static int dpiwait = 1;		/* 1: wait for dbell drain per pkt; 0: async (faster) */
module_param(dpiwait, int, 0444);
static int linrx;		/* 1: linearize jumbo RX skb -> one contiguous DPI op
				 * (fewer DPI descriptors than head+N-frag scatter) */
module_param(linrx, int, 0444);
static int rxdrop;		/* 1: after the tap delivers a frame to the host, mark it
				 * PACKET_OTHERHOST so the card stack drops it at ip_rcv instead
				 * of walking IP/route to an inevitable drop (xaui has no IPs in
				 * NIC mode) -- saves per-frame CPU on the RX-capture cores. */
module_param(rxdrop, int, 0444);
static int l2ca = 1;		/* BAR1 window CA bit. 1 (historic): host PIO writes ALLOCATE
				 * in the shared 2MB L2 -> a full-rate TX blast cycling the 1.15MB
				 * TX buffer evicts the whole cache and the RX capture path goes
				 * DRAM-latency-bound (RX 8.9 -> ~2G with ANY TX, step-like).
				 * 0: PIO writes bypass L2 -- but BREAKS coherency under load
				 * (port wedge). Keep 1; use wpar instead. */
module_param(l2ca, int, 0444);
static int wpar;		/* L2 way-partition mask for the IOB (0 = off). E.g. wpar=0x3
				 * limits IOB (host PIO writes and all IO-side allocation) to L2
				 * ways 0-1 (256KB), so a TX blast can't evict the RX capture
				 * working set from the other 14 ways. Coherency is unaffected
				 * (WPAR only constrains ALLOCATION, lookups still hit all ways).
				 * The duplex-collapse lever to test on the next boot budget. */
module_param(wpar, int, 0444);
#define L2C_WPAR_IOBX(i) (0x0001180080840200ull + ((i) & 1) * 8)
static int ztx;			/* 1: zero-copy TX. host->card frame is DMA'd (DPI INBOUND)
				 * from host RAM straight into the card skb, instead of the
				 * host PIO'ing it byte-by-byte over PCIe into the card window.
				 * Frees the host CPU (the real host->card TX cost). dma=2 only. */
module_param(ztx, int, 0444);
static int zc;			/* 1: zero-copy TX. Instead of alloc a 9KB skb + memcpy the
				 * frame out of the card window, build a tiny skb (14B eth header
				 * only) + a paged frag pointing straight AT the window slot, so
				 * PKO gathers the body by DMA -- no big alloc, no memcpy. The slot
				 * is reused only after PKO frees the skb (skb->destructor bumps a
				 * per-worker completion count that gates tx_cons). Profiling showed
				 * the 9216B netdev_alloc_skb (~30us) + cold-cache memcpy (~12.7us)
				 * were ~43 of the ~50us/frame TX budget -> this is the 10G lever. */
module_param(zc, int, 0444);
static int bindcpu;		/* 1: pin worker[i] to cpu i (kthread_bind). With nworkers <
				 * ncpus this frees the top cores for the RX NAPI softirq so a
				 * full-rate zc TX blast can't starve RX capture off xaui (RX
				 * collapses to ~0.6G under TX otherwise -- pure CPU contention). */
module_param(bindcpu, int, 0444);

/* per-DPI-queue ring of 8-byte {len,phase} headers (DMA source for hrx). A ring of
 * 64 gives ample headroom: the header is consumed by the DPI well before we wrap.
 * MUST be kmalloc'd (linear map) not a module-static array (vmalloc space, where
 * virt_to_phys is invalid -> DPI would read the wrong physaddr). Flat [k*64+hi]. */
static struct octshm_desc *hrx_hdr;
static u32 hrx_hi[8];
#define HRX_H(k, hi) (&hrx_hdr[(k) * 64 + (hi)])
#define ZTX_RINGD 128		/* ztx: max outstanding per DPI queue (seq-source ring) */
static u64 *ztx_done, *ztx_src;	/* ztx completion + marker-source cells (see ztx_post) */

static int dpi_nic_init(void)
{
	static const u32 fw_buf[6] = {0x04, 0x44, 0x82, 0xa1, 0xb1, 0xc4};
	static const u32 fw_en[6]  = {0x21, 0x42, 0x84, 0x08, 0x10, 0x00};
	int e, i, k;

	/* best-effort reset of any prior (possibly wedged) DPI state before reconfig */
	cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_CTL), 0);
	for (k = 0; k < 8; k++)
		cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_IBUFF(k)), 0);
	CVMX_SYNCW;

	dpi_region = __get_free_pages(GFP_KERNEL, get_order(DPI_PSIZE * DPI_NBLK));
	if (!dpi_region)
		return -ENOMEM;
	if (hrx && !hrx_hdr)	/* kmalloc (linear map): virt_to_phys valid for DPI src */
		hrx_hdr = kzalloc(DPI_NQ * 64 * sizeof(*hrx_hdr), GFP_KERNEL);
	if (ztx && !ztx_done) {	/* pipelined-TX completion cells (linear map for DPI) */
		ztx_done = kzalloc(DPI_NQ * sizeof(u64), GFP_KERNEL);
		ztx_src  = kzalloc(DPI_NQ * ZTX_RINGD * sizeof(u64), GFP_KERNEL);
		if (!ztx_done || !ztx_src)
			return -ENOMEM;
	}
	for (i = 0; i < DPI_NBLK; i++)
		cvmx_fpa_free((void *)(dpi_region + (unsigned long)i * DPI_PSIZE),
			      DPI_POOL, 0);
	CVMX_SYNCWS;
	cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_NCBX_CFG), 0x20ull);
	cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_GBL_EN), 0xffull);
	cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_PRTX0), 0x2000ull);
	for (e = 0; e < 6; e++) {
		cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_ENGBUF(e)), fw_buf[e]);
		cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_ENGEN(e)), fw_en[e]);
	}
	for (k = 0; k < DPI_NQ; k++) {
		int q = DPI_Q0 + k;
		__cvmx_cmd_queue_state_t *qs;
		void *buf;

		spin_lock_init(&dpi_lock[k]);
		/* replicate cvmx_cmd_queue_initialize (not exported): first chunk +
		 * state, so the inline cvmx_cmd_queue_write can chunk-chain for us. */
		qs = __cvmx_cmd_queue_get_state(CVMX_CMD_QUEUE_DMA(q));
		buf = cvmx_fpa_alloc(DPI_POOL);
		if (!buf)
			return -ENOMEM;
		memset(qs, 0, sizeof(*qs));
		qs->fpa_pool = DPI_POOL;
		qs->pool_size_m1 = (DPI_PSIZE >> 3) - 1;
		qs->base_ptr_div128 = cvmx_ptr_to_phys(buf) / 128;
		__cvmx_cmd_queue_state_ptr->ticket[
			__cvmx_cmd_queue_get_index(CVMX_CMD_QUEUE_DMA(q))] = 0;
		/* point the DPI instruction FIFO at the same first chunk */
		cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_IBUFF(q)),
			       ((u64)(DPI_PSIZE / 8) << 48) |
			       (cvmx_ptr_to_phys(buf) & 0x0000000FFFFFFF80ull));
	}
	{	/* firmware DMA_CONTROL: commit_mode(58) pkt_en(56) dma_enb=0x1f(48)
		 * dwb_denb(32) dwb_ichk(23) fpa_que(20) o_add1(19) o_es=1(15) o_mode(14) */
		u64 c = cvmx_read_csr(CVMX_ADD_IO_SEG(DPIR_DMA_CTL));
		c |= (1ull << 58) | (1ull << 56) | (0x1full << 48) | (1ull << 32)
		   | ((u64)(DPI_PSIZE / 128) << 23) | ((u64)DPI_POOL << 20)
		   | (1ull << 19) | (1ull << 15) | (1ull << 14);
		cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_DMA_CTL), c);
	}
	cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_CTL), 0x3);
	CVMX_SYNCW;
	pr_info("octshm: DPI RX offload up: %d queues, DMA_CTL=0x%llx\n", DPI_NQ,
		(unsigned long long)cvmx_read_csr(CVMX_ADD_IO_SEG(DPIR_DMA_CTL)));
	return 0;
}

/* DMA one frame card-L2 -> host RAM via the DPI engine. Blocks until the DPI
 * queue drains (dbell+fcnt == 0) so the caller may publish the slot safely. */
#define DPI_MAXLEN 8000		/* DPI local-size field is 13 bits (<8192). For a bigger
				 * frame (jumbo) use two local source pointers (nfst=2) so
				 * each stays under the limit; one pcie dest (16-bit length). */

static void host_write_dpi_h(u64 busaddr, const void *src, u32 len, u32 phase)
{
	int k = DPI_RR_K(), q = DPI_Q0 + k;
	u64 srcp = virt_to_phys((void *)src);
	u32 doff = hrx ? HRX_HDR : 0;		/* data sits after the header in hrx mode */
	u64 cmd[5];
	unsigned long fl;
	int spins, nw;

	if (len > 2 * DPI_MAXLEN) {		/* >16000: shouldn't happen, CPU fallback */
		host_write(busaddr + doff, src, len);
		return;
	}
	spin_lock_irqsave(&dpi_lock[k], fl);
	/* Jumbo (>13-bit): post TWO proven single-fragment (nfst=1) instructions,
	 * splitting the frame at DPI_MAXLEN. Each is the exact format validated in
	 * isolation, so no unproven multi-pointer layout to wedge the engine. */
	{
		u32 off = 0, nposted = 0;
		int ok = 1;

		while (ok && off < len) {
			u32 chunk = len - off;
			if (chunk > DPI_MAXLEN)
				chunk = DPI_MAXLEN;
			cmd[0] = ((u64)1 << 44) | ((u64)1 << 40);
			cmd[1] = ((u64)(chunk & 0x1FFF) << 40) |
				 ((srcp + off) & 0x0000000FFFFFFFFFull);
			cmd[2] = ((u64)chunk & 0xFFFFull) << 48;
			cmd[3] = busaddr + doff + off;
			if (cvmx_cmd_queue_write(CVMX_CMD_QUEUE_DMA(q), 0, 4, cmd) !=
			    CVMX_CMD_QUEUE_SUCCESS) {
				ok = 0;		/* queue full: full CPU fallback below */
				break;
			}
			off += chunk;
			nposted += 4;
		}
		if (ok && hrx && hrx_hdr) {	/* header LAST: posted after data -> DPI FIFO
					 * lands it after the data, so host seeing the
					 * phase == data ready */
			u32 hi = hrx_hi[k]; u64 hp;
			HRX_H(k, hi)->len = cpu_to_le32(len);
			HRX_H(k, hi)->flags = cpu_to_le32(phase);
			hrx_hi[k] = (hi + 1) & 63;
			hp = virt_to_phys(HRX_H(k, hi));
			cmd[0] = ((u64)1 << 44) | ((u64)1 << 40);
			cmd[1] = ((u64)(8 & 0x1FFF) << 40) | (hp & 0x0000000FFFFFFFFFull);
			cmd[2] = ((u64)8 & 0xFFFFull) << 48;
			cmd[3] = busaddr;			/* header at slot offset 0 */
			if (cvmx_cmd_queue_write(CVMX_CMD_QUEUE_DMA(q), 0, 4, cmd) !=
			    CVMX_CMD_QUEUE_SUCCESS)
				ok = 0;		/* header MUST land or the host slot
						 * stalls forever: fall back whole-frame */
		}
		if (nposted) {		/* ring what was queued -- content-identical to the
					 * CPU fallback, so a late DPI landing is harmless */
			CVMX_SYNCWS;
			cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_DBELL(q)), nposted);
		}
		if (dpiwait && nposted) {
			for (spins = 0; spins < 20000; spins++) {
				if ((cvmx_read_csr(CVMX_ADD_IO_SEG(DPIR_COUNTS(q))) &
				     0x00000000FFFFFFFFull) == 0)
					break;
				cpu_relax();
			}
		}
		spin_unlock_irqrestore(&dpi_lock[k], fl);
		if (!ok) {
			struct octshm_desc hdr;

			rxp_fallback++;
			host_write(busaddr + doff, src, len);
			if (hrx) {
				hdr.len = cpu_to_le32(len);
				hdr.flags = cpu_to_le32(phase);
				host_write(busaddr, &hdr, 8);	/* phase header LAST */
			}
		}
	}
	(void)nw;
}

static void host_write_dpi(u64 busaddr, const void *src, u32 len)
{
	host_write_dpi_h(busaddr, src, len, 0);
}

/* Scatter-gather DPI: DMA a possibly non-linear skb (jumbo) to a contiguous host
 * region without a CPU copy. Posts one DPI instruction per skb segment (linear
 * head + each paged frag, each split to <=DPI_MAXLEN), one doorbell for the lot.
 * This is where the DPI beats the CPU: hardware gathers the frags. */
static int post_seg(int q, u32 *nposted, u64 sp, u32 seglen, u64 dstbase, u32 *dst)
{
	u32 off = 0;
	u64 cmd[4];

	while (off < seglen) {
		u32 c = min_t(u32, seglen - off, DPI_MAXLEN);

		cmd[0] = ((u64)1 << 44) | ((u64)1 << 40);
		cmd[1] = ((u64)(c & 0x1FFF) << 40) | ((sp + off) & 0x0000000FFFFFFFFFull);
		cmd[2] = ((u64)c & 0xFFFFull) << 48;
		cmd[3] = dstbase + *dst;
		if (cvmx_cmd_queue_write(CVMX_CMD_QUEUE_DMA(q), 0, 4, cmd) !=
		    CVMX_CMD_QUEUE_SUCCESS)
			return 0;	/* queue full: caller MUST fall back for the whole
					 * frame -- a silent partial post = corrupt frame
					 * (data holes) or a permanently stalled RX slot
					 * (missing phase header) on the host. */
		off += c;
		*dst += c;
		*nposted += 4;
	}
	return 1;
}

static void host_write_dpi_skb(u64 busaddr, struct sk_buff *skb, const void *l2,
			       u32 flen, u32 phase)
{
	int k = DPI_RR_K(), q = DPI_Q0 + k;
	u32 head = (u32)((const unsigned char *)skb->data -
			 (const unsigned char *)l2) + skb_headlen(skb);
	u32 dst = hrx ? HRX_HDR : 0, nposted = 0, i, spins;
	unsigned long fl;
	int ok;

	spin_lock_irqsave(&dpi_lock[k], fl);
	ok = post_seg(q, &nposted, virt_to_phys((void *)l2), head, busaddr, &dst);
	for (i = 0; ok && i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *f = &skb_shinfo(skb)->frags[i];

		ok = post_seg(q, &nposted, virt_to_phys(skb_frag_address(f)),
			      skb_frag_size(f), busaddr, &dst);
	}
	if (ok && hrx && hrx_hdr) {			/* header LAST -> lands after data (FIFO) */
		u32 hi = hrx_hi[k], hd = 0;
		HRX_H(k, hi)->len = cpu_to_le32(flen);
		HRX_H(k, hi)->flags = cpu_to_le32(phase);
		hrx_hi[k] = (hi + 1) & 63;
		ok = post_seg(q, &nposted, virt_to_phys(HRX_H(k, hi)), 8, busaddr, &hd);
	}
	if (nposted) {			/* ring whatever was queued (even on failure: the
					 * chunks carry the same bytes the CPU fallback will
					 * write -- content-identical, so a late DPI landing
					 * is harmless) */
		CVMX_SYNCWS;
		cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_DBELL(q)), nposted);
		if (dpiwait) {
			for (spins = 0; spins < 20000; spins++) {
				if ((cvmx_read_csr(CVMX_ADD_IO_SEG(DPIR_COUNTS(q))) &
				     0x00000000FFFFFFFFull) == 0)
					break;
				cpu_relax();
			}
		}
	}
	spin_unlock_irqrestore(&dpi_lock[k], fl);

	if (!ok) {
		/* DPI cmd queue full under load: CPU-write the WHOLE frame (segment
		 * walk), then the phase header LAST. Never silent-partial (corrupt
		 * frame) and never a missing header (host slot stalls forever). */
		u32 d = hrx ? HRX_HDR : 0;
		struct octshm_desc hdr;

		rxp_fallback++;
		host_write(busaddr + d, l2, head);
		d += head;
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			skb_frag_t *f = &skb_shinfo(skb)->frags[i];

			host_write(busaddr + d, skb_frag_address(f), skb_frag_size(f));
			d += skb_frag_size(f);
		}
		if (hrx) {
			hdr.len = cpu_to_le32(flen);
			hdr.flags = cpu_to_le32(phase);
			host_write(busaddr, &hdr, 8);	/* phase header LAST */
		}
	}
}

/* DPI INBOUND: DMA a frame host RAM -> card L2 (dst_phys), the reverse of host_write_dpi.
 * Same instruction layout, but word0 sets type=1 (bit54), word1 carries the LOCAL DEST
 * and word3 the host SOURCE IOVA (format proven in octdpi_test ib=1). BLOCKS until the
 * queue drains -- the caller (TX) must have the whole frame in the skb before it xmits.
 * Returns 0 on success, <0 if the frame couldn't be posted (caller falls back to PIO). */
static int host_read_dpi(u64 host_src, u64 dst_phys, u32 len)
{
	int k = raw_smp_processor_id() % DPI_NQ, q = DPI_Q0 + k;
	u64 cmd[4];
	unsigned long fl;
	int spins, rc = 0;
	u32 off = 0, nposted = 0;

	if (len > 2 * DPI_MAXLEN)
		return -1;
	spin_lock_irqsave(&dpi_lock[k], fl);
	while (off < len) {
		u32 chunk = len - off;

		if (chunk > DPI_MAXLEN)
			chunk = DPI_MAXLEN;
		cmd[0] = ((u64)1 << 54) | ((u64)1 << 44) | ((u64)1 << 40);	/* INBOUND: type[55:54]=1 (proven in octdpi_test ib=1; was 1<<48 => 100% drop) */
		cmd[1] = ((u64)(chunk & 0x1FFF) << 40) |
			 ((dst_phys + off) & 0x0000000FFFFFFFFFull);		/* local dest */
		cmd[2] = ((u64)chunk & 0xFFFFull) << 48;
		cmd[3] = host_src + off;					/* host source IOVA */
		if (cvmx_cmd_queue_write(CVMX_CMD_QUEUE_DMA(q), 0, 4, cmd) !=
		    CVMX_CMD_QUEUE_SUCCESS) {
			rc = -1;
			goto out;
		}
		off += chunk;
		nposted += 4;
	}
	CVMX_SYNCWS;
	cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_DBELL(q)), nposted);
	rc = -1;					/* assume timeout unless drained */
	for (spins = 0; spins < 200000; spins++) {	/* MUST land before xmit */
		if ((cvmx_read_csr(CVMX_ADD_IO_SEG(DPIR_COUNTS(q))) &
		     0x00000000FFFFFFFFull) == 0) {
			rc = 0;				/* fully DMA'd: safe to xmit */
			break;
		}
		cpu_relax();
	}
	/* on timeout rc stays -1 -> caller DROPS (never xmit a half-DMA'd frame = corruption) */
out:
	spin_unlock_irqrestore(&dpi_lock[k], fl);
	return rc;
}

/* --- pipelined inbound-DPI TX (ztx) ---------------------------------------------
 * The blocking host_read_dpi (wait DPIR_COUNTS==0 per frame) serialized TX to ~28M.
 * Instead: post the frame's INBOUND DMA + an INTERNAL "marker" that copies this frame's
 * seq into ztx_done[k] AFTER the data lands (same DPI queue -> single engine -> FIFO),
 * never block, and xmit each frame once ztx_done[k] >= its seq. Uncached (CKSEG1) access
 * to the seq cells so CPU<->DPI see each other's writes without cache games. */
/* 64-bit uncached XKPHYS (cca=2) so the completion cells work regardless of where in
 * card DRAM they land (CKSEG1 only maps the low 512MB). */
#define OCT_UNC(p) ((volatile u64 *)(0x9000000000000000ull | (u64)virt_to_phys((void *)(p))))
static u64  ztx_next[DPI_NQ];		/* producer seq (under dpi_lock[k]) */

static inline u64 ztx_completed(int k) { return *OCT_UNC(&ztx_done[k]); }

/* Post INBOUND DMA (host_src -> dst_phys, len) + FIFO completion marker on queue k.
 * Returns assigned seq (>0), or 0 if pipeline full / post failed. Never blocks. */
static u64 ztx_post(int k, u64 host_src, u64 dst_phys, u32 len)
{
	int q = DPI_Q0 + k;
	u64 seq, cmd[4], scell;
	u32 off = 0, nposted = 0;
	unsigned long fl;

	if (len == 0 || len > 2 * DPI_MAXLEN)
		return 0;
	spin_lock_irqsave(&dpi_lock[k], fl);
	seq = ztx_next[k] + 1;
	if (seq - ztx_completed(k) >= ZTX_RINGD - 8) {	/* backpressure: too many in flight */
		spin_unlock_irqrestore(&dpi_lock[k], fl);
		return 0;
	}
	while (off < len) {				/* INBOUND data, chunked to <8KB */
		u32 chunk = min_t(u32, len - off, DPI_MAXLEN);

		cmd[0] = ((u64)1 << 54) | ((u64)1 << 44) | ((u64)1 << 40);	/* type[55:54]=1 INBOUND */
		cmd[1] = ((u64)(chunk & 0x1FFF) << 40) | ((dst_phys + off) & 0x0000000FFFFFFFFFull);
		cmd[2] = ((u64)chunk & 0xFFFFull) << 48;
		cmd[3] = host_src + off;
		if (cvmx_cmd_queue_write(CVMX_CMD_QUEUE_DMA(q), 0, 4, cmd) != CVMX_CMD_QUEUE_SUCCESS)
			goto fail;
		off += chunk;
		nposted += 4;
	}
	/* INTERNAL marker: copy the 8-byte seq cell -> ztx_done[k] (lands after data, FIFO) */
	scell = (u64)k * ZTX_RINGD + (seq & (ZTX_RINGD - 1));
	*OCT_UNC(&ztx_src[scell]) = seq;
	cmd[0] = ((u64)2 << 54) | ((u64)1 << 44) | ((u64)1 << 40);	/* type[55:54]=2 INTERNAL */
	cmd[1] = ((u64)8 << 40) | (virt_to_phys(&ztx_src[scell]) & 0x0000000FFFFFFFFFull);
	cmd[2] = ((u64)8 << 40) | (virt_to_phys(&ztx_done[k])   & 0x0000000FFFFFFFFFull);
	if (cvmx_cmd_queue_write(CVMX_CMD_QUEUE_DMA(q), 0, 3, cmd) != CVMX_CMD_QUEUE_SUCCESS)
		goto fail;
	nposted += 3;
	CVMX_SYNCWS;
	cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_DBELL(q)), nposted);
	ztx_next[k] = seq;
	spin_unlock_irqrestore(&dpi_lock[k], fl);
	return seq;
fail:
	spin_unlock_irqrestore(&dpi_lock[k], fl);
	return 0;
}

#define MAX_WORKERS 8
#define MAXPORT 2
/* per-worker ztx pipeline: frames posted (DMA in flight), awaiting completion + xmit.
 * One DPI queue per worker (id % DPI_NQ) so FIFO head is always the oldest seq. */
struct ztx_pend { struct sk_buff *skb; struct net_device *dev; u32 seq; u8 k; };
#define ZTX_FIFO 256
static struct ztx_pend ztx_fifo[MAX_WORKERS][ZTX_FIFO];
static u32 ztx_h[MAX_WORKERS], ztx_t[MAX_WORKERS];

/* xmit every pipelined frame whose data has landed (head = oldest seq on this queue) */
static void ztx_drain(int id)
{
	while (ztx_h[id] != ztx_t[id]) {
		struct ztx_pend *e = &ztx_fifo[id][ztx_h[id] & (ZTX_FIFO - 1)];
		struct sk_buff *skb = e->skb;
		struct ethhdr *eh;

		if ((s64)(ztx_completed(e->k) - e->seq) < 0)
			break;				/* not landed yet */
		skb->dev = e->dev;
		skb_reset_mac_header(skb);
		skb->protocol = eth_hdr(skb)->h_proto;
		eh = eth_hdr(skb);
		if (ether_addr_equal_unaligned(eh->h_dest, e->dev->dev_addr) ||
		    is_broadcast_ether_addr(eh->h_dest)) {
			skb->pkt_type = PACKET_HOST;
			netif_rx(skb);
		} else {
			dev_queue_xmit(skb);
		}
		ztx_h[id]++;
	}
}
#define RX_PHASE(claimnum)  (((claimnum) >> 7) & 1u)	/* log2(RING_SZ)=7; flips each wrap */
#define TX_PHASE(tc)        (((tc) >> 7) & 1u)		/* host stamps this per TX slot */
/* Per-port ring state. The DPI engine + hrx_hdr ring + workers are SHARED
 * (one DPI); only the rings/netdev/dma-bases are per port. */
struct oct_port {
	struct page *pages;			/* own 4MB region (order WIN_ORDER) */
	u64 phys;				/* its card phys (for BAR1 index map) */
	u8 *base;				/* its kernel VA */
	struct octshm_ctrl *ctrl;
	struct octshm_desc *txd, *rxd;
	u8 *txbuf, *rxbuf;
	struct net_device *up_dev;
	struct packet_type ptype;
	u64 tx_dma_base, rx_dma_base;
	atomic_t rx_claim_a;			/* lockfree CAS slot claim */
	u32 rx_claim;				/* locked-path claim index */
	u8 rx_done[RING_SZ];			/* ordered-publish flags (non-lockfree) */
	u32 tx_cons_w[MAX_WORKERS];		/* per-worker TX consumer (stride nworkers) */
	int dma_ready;				/* 0 none, 1 SLI copy, 2 DPI */
	int idx;
};
static struct oct_port pv[MAXPORT];
static struct task_struct *workers[MAX_WORKERS];
static int nworkers = 2;	/* rev ~1.93G (rx_lock-driven); 2 = best fwd consistency */
module_param(nworkers, int, 0444);
static int rxwork;		/* offload RX DPI off the single cpu0 NAPI: the ptype tap just
				 * enqueues (it owns a deliver_skb ref), N workers dequeue + DPI in
				 * parallel across cores. Needs lockfree=1 (atomic claim) + more
				 * nworkers. Raw wire is 5.31G vs 2.75G octshm = this cpu0 DPI wall. */
module_param(rxwork, int, 0444);
#define RXQ_MAX 256		/* per-worker backlog cap; over -> deliver inline (no drop) */
static struct sk_buff_head rxq[MAX_WORKERS];
static DEFINE_SPINLOCK(rx_lock);	/* guards non-lockfree rx_done + rx_prod publish (all ports) */
/* lockfree=1: drop rx_lock entirely. Claim a slot with an atomic CAS (clean drop on
 * full, no hole), and instead of publishing a monotonic rx_prod under lock, stamp a
 * PHASE bit into the slot's desc.flags. The host reads the phase to tell ready vs stale
 * (virtio-style), so no completion lock and no cross-core rx_prod serialization. Matched
 * host change required (octshm_host lockfree=1). */
static int lockfree;
module_param(lockfree, int, 0444);
#define RXF_PHASE  0x1u
static u32 beat;
static struct task_struct *bench_thread;
static int dpi_up;			/* shared SLI+DPI engine level (0/1/2), init once */
static struct oct_port *port_of_dev(struct net_device *d)
{
	int i;

	for (i = 0; i < ports; i++)
		if (pv[i].up_dev == d)
			return &pv[i];
	return NULL;
}

/* Raw-DPI throughput benchmark: hammer host_write_dpi (card L2 -> host RAM) as fast as
 * possible for `bench` seconds, no network/tap/skb involved. Isolates the DPI engine's
 * real ceiling from the packet-path overhead, to decide where the 2.15G NIC cap lives. */
static int bench_fn(void *arg)
{
	unsigned long t0 = jiffies, tend = t0 + bench * HZ;
	u64 bytes = 0, cnt = 0;
	u32 i = 0;

	while (!kthread_should_stop() && time_before(jiffies, tend)) {
		host_write_dpi(pv[0].rx_dma_base + (u64)(i & 63) * BUF_SZ, pv[0].base, blen);
		bytes += blen;
		cnt++;
		i++;
		if ((cnt & 0x3FFF) == 0)
			cond_resched();
	}
	{
		u64 ms = jiffies_to_msecs(jiffies - t0);
		u64 mbit = ms ? (bytes * 8) / (ms * 1000) : 0;	/* Mbit/s */
		u64 kpps = ms ? cnt / ms : 0;			/* per ms ~= Kpps */

		pr_info("octshm BENCH: blen=%d %llu xfers in %llu ms = %llu Mbit/s, %llu Kpps (raw DPI)\n",
			blen, cnt, ms, mbit, kpps);
	}
	return 0;
}

/* RX: uplink inbound frame -> that port's card->host RX ring. Claim a slot, DMA
 * the frame to host, publish. Runs either inline in the ptype tap (cpu0 NAPI) or,
 * with rxwork=1, on a worker core (DPI parallelized off cpu0). Port resolved from
 * skb->dev so both the tap and the worker drain reach the right ring. */
static void oct_rx_deliver(struct sk_buff *skb)
{
	struct oct_port *p = port_of_dev(skb->dev);
	unsigned char *l2;
	u32 len, claim, rc, rs;
	unsigned long rxflags;
	u64 t0 = cvmx_get_cycle(), t1, t2;

	if (!p)
		return;
	l2 = skb_mac_header(skb);
	if (!l2 || l2 > skb->data)
		l2 = skb->data;
	len = skb->len + (u32)(skb->data - l2);		/* full L2 frame */
	if (p->dma_ready != 2 && skb_headlen(skb) < skb->len)	/* non-DPI needs linear */
		return;
	if (!(len && len <= BUF_SZ))
		return;

	/* Parallel RX DMA: the SLI mem-access "DMA" is a CPU copy over PCIe, so
	 * serializing every frame (old rx_lock around host_write) pinned reverse
	 * to one core. Instead take only a TINY lock to CLAIM a slot, do the
	 * heavy host_write OUTSIDE the lock (runs on all RX cores at once), then
	 * a TINY lock to publish rx_prod over contiguously-completed slots so the
	 * host still sees frames in order. */
	if (lockfree) {
		/* atomic CAS claim: on full, drop WITHOUT incrementing (no hole that
		 * would stall the phase sequence); else reserve the slot lock-free. */
		for (;;) {
			claim = (u32)atomic_read(&p->rx_claim_a);
			rc = le32_to_cpu(p->ctrl->rx_cons);
			if ((claim - rc) >= RING_SZ) {
				rxp_ringfull++;
				return;				/* ring full: clean drop */
			}
			if ((u32)atomic_cmpxchg(&p->rx_claim_a, (int)claim,
						(int)(claim + 1)) == claim)
				break;
		}
		rs = claim & RING_MASK;
	} else {
		spin_lock_irqsave(&rx_lock, rxflags);
		claim = p->rx_claim;
		rc = le32_to_cpu(p->ctrl->rx_cons);
		if ((claim - rc) >= RING_SZ) {		/* ring full: drop */
			spin_unlock_irqrestore(&rx_lock, rxflags);
			return;
		}
		rs = claim & RING_MASK;
		p->rx_claim = claim + 1;
		spin_unlock_irqrestore(&rx_lock, rxflags);
	}

	if (p->dma_ready == 2) {			/* DPI hardware DMA (copy offloaded) */
		if (linrx && skb_headlen(skb) < skb->len &&
		    skb_linearize(skb) == 0)
			l2 = skb_mac_header(skb);	/* head may have moved */
		t1 = cvmx_get_cycle();
		if (linrx && skb_headlen(skb) == skb->len)
			host_write_dpi_h(p->rx_dma_base + (u64)rs * BUF_SZ, l2, len,
					 hrx ? RX_PHASE(claim) : 0);
		else				/* scatter-gather head + frags */
			host_write_dpi_skb(p->rx_dma_base + (u64)rs * BUF_SZ, skb, l2,
					   len, hrx ? RX_PHASE(claim) : 0);
		t2 = cvmx_get_cycle();
		rxp_cyc_dpi += t2 - t1;
	}
	else if (p->dma_ready)			/* CPU SLI-mem-access copy (~2.2G cap) */
		host_write(p->rx_dma_base + (u64)rs * BUF_SZ + (hrx ? HRX_HDR : 0), l2, len);
	else
		memcpy(p->rxbuf + rs * BUF_SZ, l2, len);

	if (hrx) {
		/* descriptor rode the DPI into host RAM (phase last); host reads locally */
	} else if (lockfree) {
		/* lock-free completion: write len, then stamp the PHASE bit LAST so the
		 * host, seeing the new phase, is guaranteed the len+data are already there.
		 * No rx_prod, no completion lock -> cores never serialize here. */
		p->rxd[rs].len = cpu_to_le32(len);
		wmb();
		p->rxd[rs].flags = cpu_to_le32(RX_PHASE(claim));
		wmb();
	} else {
		p->rxd[rs].len = cpu_to_le32(len);
		p->rxd[rs].flags = 0;
		wmb();
		spin_lock_irqsave(&rx_lock, rxflags);	/* ordered completion */
		p->rx_done[rs] = 1;
		{
			u32 pr = le32_to_cpu(p->ctrl->rx_prod);
			while (pr != p->rx_claim && p->rx_done[pr & RING_MASK]) {
				p->rx_done[pr & RING_MASK] = 0;
				pr++;
			}
			p->ctrl->rx_prod = cpu_to_le32(pr);
		}
		wmb();
		spin_unlock_irqrestore(&rx_lock, rxflags);
	}
	rxp_cyc += cvmx_get_cycle() - t0;
	rxp_frames++;
}

/* ptype tap (cpu0 NAPI): filter to our uplink, then either enqueue to a worker
 * (rxwork) or deliver inline. We own a deliver_skb ref -> we free it (or the worker does). */
static int oct_rx_pack(struct sk_buff *skb, struct net_device *dev,
		       struct packet_type *pt, struct net_device *orig)
{
	struct oct_port *p = container_of(pt, struct oct_port, ptype);

	if (skb->dev != p->up_dev)
		goto out;
	if (rxwork && p->dma_ready == 2) {
		int k = raw_smp_processor_id() % nworkers;

		if (skb_queue_len(&rxq[k]) < RXQ_MAX) {
			/* CLONE: after we return, the stack keeps pulling headers on THIS
			 * skb (data/mac_header shift) while a worker reads it async -> race
			 * -> bad len -> DPI overflow -> wedge. The clone freezes our metadata
			 * snapshot (data buffer shared read-only; pull only moves pointers). */
			struct sk_buff *c = skb_clone(skb, GFP_ATOMIC);

			if (c) {
				skb_queue_tail(&rxq[k], c);	/* worker frees the clone */
				goto out;
			}
		}					/* backlog full / clone fail: inline, no drop */
	}
	oct_rx_deliver(skb);
out:
	if (rxdrop)
		/* host has its copy (or the frame wasn't ours); tell the card stack to
		 * drop at ip_rcv instead of walking IP/route to an inevitable drop. */
		skb->pkt_type = PACKET_OTHERHOST;
	kfree_skb(skb);					/* drop our ref from deliver_skb */
	return 0;
}

/* TX: drain each port's host->card TX ring, transmit out that port's uplink. */
static int worker_fn(void *arg)
{
	long id = (long)arg;

	while (!kthread_should_stop()) {
		int pi, any_ready = 0, any2 = 0, did = 0;

		/* DMA activation: only worker 0 arms (single writer). Each port arms
		 * independently when its host sets dma_enable + valid bases. The SLI
		 * outbound + DPI engine are SHARED, so init them once (dpi_up). */
		if (id == 0 && dma) {
			for (pi = 0; pi < ports; pi++) {
				struct oct_port *p = &pv[pi];
				u64 tb, rb;
				u32 s;

				if (p->dma_ready && !le32_to_cpu(p->ctrl->dma_enable)) {
					/* host detached (octnic unload writes dma_enable=0):
					 * disarm so the next octnic load re-arms with its new
					 * pool addresses -- makes a host-side module reload a
					 * full ring-resync WITHOUT a card reboot. */
					p->dma_ready = 0;
					pr_info("octshm: port%d DMA off (host detached)\n", pi);
					continue;
				}
				if (p->dma_ready || !le32_to_cpu(p->ctrl->dma_enable))
					continue;
				tb = le64_to_cpu(p->ctrl->tx_dma_base);
				rb = le64_to_cpu(p->ctrl->rx_dma_base);
				if (!(rb && tb && rb < (1ull << 40) && tb < (1ull << 40)))
					continue;
				p->tx_dma_base = tb;
				p->rx_dma_base = rb;
				/* fresh ring: realign claim to 0 so the phase sequence matches
				 * the host's rc=0 fresh start; seed TX phases "not ready". */
				atomic_set(&p->rx_claim_a, 0);
				p->rx_claim = 0;
				p->ctrl->rx_cons = 0;
				p->ctrl->rx_prod = 0;
				for (s = 0; s < RING_SZ; s++)
					p->txd[s].flags = cpu_to_le32(TX_PHASE(s) ^ 1u);
				for (s = 0; s < (u32)nworkers && s < MAX_WORKERS; s++)
					p->tx_cons_w[s] = s;
				wmb();
				if (!dpi_up) {		/* shared engine: init once */
					octdma_setup();	/* SLI outbound (needed by DPI too) */
					dpi_up = 1;
					if (dma == 2 && dpi_nic_init() == 0)
						dpi_up = 2;
				}
				p->dma_ready = (dma == 2 && dpi_up == 2) ? 2 : 1;
				if (p->dma_ready == 2 && bench > 0 && !bench_thread)
					bench_thread = kthread_run(bench_fn, NULL,
								   "octshm-bench");
				pr_info("octshm: port%d DMA on (mode=%d) tx=0x%llx rx=0x%llx\n",
					pi, p->dma_ready, (unsigned long long)tb,
					(unsigned long long)rb);
			}
		}
		if (id == 0) {			/* heartbeat: single writer, all ports */
			beat++;
			for (pi = 0; pi < ports; pi++)
				pv[pi].ctrl->heartbeat = cpu_to_le32(beat);
			if (pv[0].ctrl) {	/* mirror RX profiling counters for the host
						 * (BAR2 ctrl page +0x100, 8 LE u64s) */
				__le64 *rp = (__le64 *)((u8 *)pv[0].ctrl + RXPROF_OFF);

				rp[0] = cpu_to_le64(rxp_frames);
				rp[1] = cpu_to_le64(rxp_cyc);
				rp[2] = cpu_to_le64(rxp_cyc_dpi);
				rp[3] = cpu_to_le64(rxp_fallback);
				rp[4] = cpu_to_le64(rxp_ringfull);
				if (dpi_up == 2 && (beat & 255) == 0) {
					/* sampled: deepest DPI dbell backlog (is the
					 * outbound DMA keeping up?) + PIP hard drops
					 * (frames lost BEFORE the tap = the TCP loss) */
					u64 d, m = le64_to_cpu(rp[5]);
					int qq;
					cvmx_pip_port_status_t ps;

					for (qq = 0; qq < DPI_NQ; qq++) {
						d = cvmx_read_csr(CVMX_ADD_IO_SEG(
							DPIR_COUNTS(DPI_Q0 + qq))) &
						    0xFFFFFFFFull;
						if (d > m)
							m = d;
					}
					rp[5] = cpu_to_le64(m);
					cvmx_pip_get_port_status(0, 0, &ps);
					rp[6] = cpu_to_le64(ps.dropped_packets);
					if (ports > 1) {
						cvmx_pip_get_port_status(16, 0, &ps);
						rp[7] = cpu_to_le64(ps.dropped_packets);
					}
				}
			}
		}

		for (pi = 0; pi < ports; pi++) {
			if (pv[pi].dma_ready)
				any_ready = 1;
			if (pv[pi].dma_ready == 2)
				any2 = 1;
		}

		/* RX offload: drain this core's queue, DPI each frame (parallel across
		 * workers -> off the single cpu0 NAPI). oct_rx_deliver resolves the port. */
		if (rxwork && any2) {
			struct sk_buff *rs;
			int b = 16;		/* bounded RX batch, then fall through to TX
					 * drain so heavy RX can't fully starve TX
					 * (was: drain 64 then `continue`, skipping TX
					 * -> TX collapsed to ~0.45G under contention). */

			while (b-- && (rs = skb_dequeue(&rxq[id]))) {
				oct_rx_deliver(rs);
				kfree_skb(rs);
				did = 1;
			}
		}

		if (!any_ready) {		/* wait for arm: txd phases seeded there */
			usleep_range(50, 100);
			continue;
		}
		/* phase-based TX drain: this worker owns slots where slot%nworkers==id,
		 * across every armed port. Host stamps txd[slot].flags (TX phase) LAST,
		 * after PIO+len, so a matching phase means the slot is fully written ->
		 * no tx_lock; workers drain disjoint slots + PKO-xmit in parallel. */
		for (pi = 0; pi < ports; pi++) {
			struct oct_port *p = &pv[pi];
			struct sk_buff *skb;
			u32 tc, ts, len, i, m;

			if (!p->dma_ready || !p->up_dev)
				continue;
			tc = p->tx_cons_w[id];
			ts = tc & RING_MASK;
			if (le32_to_cpu(p->txd[ts].flags) != TX_PHASE(tc))
				continue;			/* slot not filled yet */
			rmb();					/* phase seen => len+data landed */
			len = le32_to_cpu(p->txd[ts].len);

			if (zc && p->dma_ready) {
				/* zero-copy TX: small skb with L2-L4 headers copied linear (so
				 * cvm_oct_xmit's ip_hdr()/csum-offload block parses valid data) + a frag
				 * pointing straight at the window slot, so PKO DMA-gathers the payload --
				 * no 9KB alloc, no 9KB cold-cache memcpy. NO completion gate: the ring is
				 * 128 deep and PKO drains at wire (9.89G) faster than the host fills over
				 * PCIe (<=8.4G), so a slot is always long done transmitting before the host
				 * wraps back to it. get_page balances the frag's put_page on skb free.
				 * ponytail: relies on PKO-faster-than-host-fill; the copy path proves fill
				 * <= 8.4G < 9.89G wire, so it holds. Earlier destructor+counter gate
				 * throttled to 141/299Mbit via the tx_free_list lazy free -- removed. */
				struct ethhdr *eh;
				int hdr = len < ZC_HDR ? len : ZC_HDR;

				if (len < ETH_HLEN || len > BUF_SZ) {
					/* bad len: drop, advance below */
				} else if (!(skb = netdev_alloc_skb(p->up_dev,
							ZC_HDR + NET_IP_ALIGN + 32))) {
					break;			/* no mem: retry slot next pass */
				} else {
					skb_reserve(skb, NET_IP_ALIGN);
					memcpy(skb_put(skb, hdr), p->txbuf + ts * BUF_SZ, hdr);
					eh = (struct ethhdr *)skb->data;
					skb->dev = p->up_dev;
					skb_reset_mac_header(skb);
					skb_set_network_header(skb, ETH_HLEN);
					skb->protocol = eh->h_proto;
					if (ether_addr_equal_unaligned(eh->h_dest, p->up_dev->dev_addr) ||
					    is_broadcast_ether_addr(eh->h_dest)) {
						if (len > hdr)		/* local dst (rare): copy the rest */
							memcpy(skb_put(skb, len - hdr),
							       p->txbuf + ts * BUF_SZ + hdr, len - hdr);
						skb->pkt_type = PACKET_HOST;
						netif_rx(skb);
					} else if (len > hdr) {		/* jumbo: header copied, payload frag */
						get_page(p->pages);	/* frag holds a ref on the window */
						skb_add_rx_frag(skb, 0, p->pages,
								TXBUF_OFF + ts * BUF_SZ + hdr,
								len - hdr, len - hdr);
						dev_queue_xmit(skb);
					} else {			/* small frame fully linear: no frag */
						dev_queue_xmit(skb);
					}
				}
			} else if (ztx && p->dma_ready == 2) {
				/* pipelined inbound DPI: post the DMA now, xmit later in
				 * ztx_drain() once the marker confirms the frame landed. */
				int k = id & (DPI_NQ - 1);
				u64 seq;

				if (((ztx_t[id] + 1) & (ZTX_FIFO - 1)) == (ztx_h[id] & (ZTX_FIFO - 1)))
					break;			/* pipeline full: drain + retry slot */
				if (len >= ETH_HLEN && len <= BUF_SZ) {
					skb = netdev_alloc_skb(p->up_dev, BUF_SZ + NET_IP_ALIGN);
					if (!skb)
						break;		/* no mem: retry slot next pass */
					skb_reserve(skb, NET_IP_ALIGN);
					seq = ztx_post(k, p->tx_dma_base + (u64)ts * BUF_SZ,
						       virt_to_phys(skb_put(skb, len)), len);
					if (!seq) {		/* pipeline full / post failed: retry */
						dev_kfree_skb(skb);
						break;
					}
					ztx_fifo[id][ztx_t[id] & (ZTX_FIFO - 1)] =
						(struct ztx_pend){ .skb = skb, .dev = p->up_dev,
								   .seq = seq, .k = (u8)k };
					ztx_t[id]++;
				}				/* bad len: slot consumed, no post */
			} else if (len >= ETH_HLEN && len <= BUF_SZ &&
				   (skb = netdev_alloc_skb(p->up_dev, BUF_SZ + NET_IP_ALIGN))) {
				/* PIO copy path: host already wrote the frame into the window */
				struct ethhdr *eh;

				skb_reserve(skb, NET_IP_ALIGN);
				memcpy(skb_put(skb, len), p->txbuf + ts * BUF_SZ, len);
				skb->dev = p->up_dev;
				skb_reset_mac_header(skb);
				skb->protocol = eth_hdr(skb)->h_proto;
				eh = eth_hdr(skb);
				if (ether_addr_equal_unaligned(eh->h_dest, p->up_dev->dev_addr) ||
				    is_broadcast_ether_addr(eh->h_dest)) {
					skb->pkt_type = PACKET_HOST;
					netif_rx(skb);
				} else {
					dev_queue_xmit(skb);
				}
			}
			/* slot consumed: advance this worker's consumer. */
			p->tx_cons_w[id] = tc + nworkers;
			m = p->tx_cons_w[0];
			for (i = 1; i < (u32)nworkers; i++)
				if ((s32)(p->tx_cons_w[i] - m) < 0)
					m = p->tx_cons_w[i];
			p->ctrl->tx_cons = cpu_to_le32(m);
			did = 1;
		}
		ztx_drain(id);				/* xmit frames whose inbound DMA completed */
		if (did)
			cond_resched();
		else
			usleep_range(20, 40);
	}
	return 0;
}

/* Temp reporting: a card-side daemon writes "<board_mC> <die_mC>" (tmp421 hwmon
 * millidegrees) to /proc/octshm/temp; we stash them in ctrl->resv[0..1], which the
 * host reads over BAR2 and exposes via hwmon so `sensors` shows the card temps. */
static struct proc_dir_entry *proc_dir, *proc_temp;
static ssize_t temp_write(struct file *f, const char __user *ubuf, size_t n, loff_t *off)
{
	char buf[64];
	unsigned int board = 0, die = 0;

	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';
	if (sscanf(buf, "%u %u", &board, &die) >= 1 && pv[0].ctrl) {
		pv[0].ctrl->resv[0] = cpu_to_le32(board);
		pv[0].ctrl->resv[1] = cpu_to_le32(die);
		wmb();
	}
	return n;
}
static const struct proc_ops temp_pops = { .proc_write = temp_write };

/* Env discovery return channel: envdiag.sh (card-side NOR scan for the u-boot env
 * location) writes its result string to /proc/octshm/env; we stash it at ctrl+0x200
 * (clean: past RXPROF@0x100, before TXDESC@0x1000, never touched by NIC traffic).
 * Host reads it over BAR2 window +0x200. Serial-free fw_setenv discovery. */
#define ENV_OFF 0x200
static struct proc_dir_entry *proc_env;
static ssize_t env_write(struct file *f, const char __user *ubuf, size_t n, loff_t *off)
{
	if (n > 512)
		n = 512;
	if (pv[0].ctrl) {
		u8 *dst = (u8 *)pv[0].ctrl + ENV_OFF;
		if (copy_from_user(dst, ubuf, n))
			return -EFAULT;
		if (n < 512)
			dst[n] = '\0';
		wmb();
	}
	return n;
}
static const struct proc_ops env_pops = { .proc_write = env_write };

static int __init octshm_init(void)
{
	u64 idx;
	int ret, i;

	if (ports < 1)
		ports = 1;
	if (ports > MAXPORT)
		ports = MAXPORT;

	/* One 4MB region per port. A single 8MB alloc would exceed the buddy
	 * MAX_ORDER (top order-10 = 4MB), so each port allocates its own region and
	 * gets its own BAR1 index -- the host sees them as one contiguous 8MB BAR
	 * via the two index mappings even though the card phys need not be adjacent. */
	for (i = 0; i < ports; i++) {
		struct oct_port *p = &pv[i];

		p->pages = alloc_pages(GFP_KERNEL, WIN_ORDER);
		if (!p->pages) {
			pr_err("octshm: port%d 4MB alloc failed\n", i);
			while (--i >= 0)
				__free_pages(pv[i].pages, WIN_ORDER);
			return -ENOMEM;
		}
		p->base = page_address(p->pages);
		p->phys = page_to_phys(p->pages);
		memset(p->base, 0, 1UL << (WIN_ORDER + PAGE_SHIFT));
		p->idx   = i;
		p->ctrl  = (struct octshm_ctrl *)(p->base + CTRL_OFF);
		p->txd   = (struct octshm_desc *)(p->base + TXDESC_OFF);
		p->rxd   = (struct octshm_desc *)(p->base + RXDESC_OFF);
		p->txbuf = p->base + TXBUF_OFF;
		p->rxbuf = p->base + RXBUF_OFF;
		atomic_set(&p->rx_claim_a, 0);
		p->ctrl->magic   = cpu_to_le32(OCTSHM_MAGIC);
		p->ctrl->version = cpu_to_le32(OCTSHM_VER);
		if (lockfree) {		/* seed rx desc phase = 1 so no slot reads ready early */
			int s;

			for (s = 0; s < RING_SZ; s++)
				p->rxd[s].flags = cpu_to_le32(RXF_PHASE);
		}
	}
	wmb();

	cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_P2N_BAR0), host_bar0);
	cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_P2N_BAR1), host_bar1);
	cvmx_write_csr(CVMX_ADD_IO_SEG(0x00011800C0000128ull), 0x10); /* BAR_CTL: bar1_siz=1 (64M) */
	/* map each port's 4MB region into consecutive 4MB BAR1 windows.
	 * l2ca=0 drops the CA bit: host PIO writes bypass the shared L2 (no
	 * allocation, still coherent) so a TX blast can't evict the RX path. */
	idx = ((pv[0].phys >> 22) << 4) | (1u << 1) | (l2ca ? 0x9 : 0x1);
	cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX0), idx);
	if (ports > 1) {
		u64 idx1 = ((pv[1].phys >> 22) << 4) | (1u << 1) | (l2ca ? 0x9 : 0x1);

		cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX1), idx1);
	}
	if (wpar) {	/* partition L2: IOB (PIO/IO allocation) confined to `wpar` ways */
		cvmx_write_csr(CVMX_ADD_IO_SEG(L2C_WPAR_IOBX(0)), (u64)(wpar & 0xFFFF));
		cvmx_write_csr(CVMX_ADD_IO_SEG(L2C_WPAR_IOBX(1)), (u64)(wpar & 0xFFFF));
		pr_info("octshm: L2C WPAR_IOB=0x%x (IOB L2 ways limited)\n", wpar & 0xFFFF);
	}
	CVMX_SYNCW;
	pr_info("octshm: P2N_BAR1=0x%llx BAR_CTL=0x%llx IDX0=0x%llx ports=%d (host_bar1=0x%lx)\n",
		(unsigned long long)cvmx_read_csr(CVMX_ADD_IO_SEG(PEM_P2N_BAR1)),
		(unsigned long long)cvmx_read_csr(CVMX_ADD_IO_SEG(0x00011800C0000128ull)),
		(unsigned long long)cvmx_read_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX0)),
		ports, host_bar1);

	/* resolve one uplink netdev per port from the comma-list (e.g. xaui0,xaui1) */
	{
		char names[64], *sp = names, *tok;

		strscpy(names, uplink, sizeof(names));
		for (i = 0; i < ports; i++) {
			tok = strsep(&sp, ",");
			pv[i].up_dev = (tok && *tok) ?
				dev_get_by_name(&init_net, tok) : NULL;
			if (!pv[i].up_dev) {
				pr_err("octshm: uplink #%d missing/not found (uplink=%s)\n",
				       i, uplink);
				while (--i >= 0)
					dev_put(pv[i].up_dev);
				cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX0), 0);
				if (ports > 1)
					cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX1), 0);
				for (i = 0; i < ports; i++)
					__free_pages(pv[i].pages, WIN_ORDER);
				return -ENODEV;
			}
		}
	}
	{
		int qi;

		for (qi = 0; qi < MAX_WORKERS; qi++)
			skb_queue_head_init(&rxq[qi]);
	}
	for (i = 0; i < ports; i++) {		/* one ptype tap per port (dev-filtered) */
		pv[i].ptype.type = htons(ETH_P_ALL);
		pv[i].ptype.dev  = pv[i].up_dev;
		pv[i].ptype.func = oct_rx_pack;
		dev_add_pack(&pv[i].ptype);
		rtnl_lock();			/* promisc: accept frames for host MAC */
		dev_set_promiscuity(pv[i].up_dev, 1);
		rtnl_unlock();
	}

	if (nworkers < 1) nworkers = 1;
	if (nworkers > MAX_WORKERS) nworkers = MAX_WORKERS;
	for (i = 0; i < nworkers; i++) {
		workers[i] = kthread_create(worker_fn, (void *)(long)i,
					    "octshm/%d", i);
		if (IS_ERR(workers[i])) {
			int j;

			ret = PTR_ERR(workers[i]);
			workers[i] = NULL;
			while (--i >= 0) kthread_stop(workers[i]);
			for (j = 0; j < ports; j++) {
				dev_remove_pack(&pv[j].ptype);
				rtnl_lock();
				dev_set_promiscuity(pv[j].up_dev, -1);
				rtnl_unlock();
				dev_put(pv[j].up_dev);
			}
			cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX0), 0);
			if (ports > 1)
				cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX1), 0);
			for (j = 0; j < ports; j++)
				__free_pages(pv[j].pages, WIN_ORDER);
			return ret;
		}
		/* bindcpu: pin worker[i] to cpu i so nworkers<ncpus leaves the top
		 * cores exclusively for the RX NAPI softirq (else the spinning TX
		 * workers starve RX capture -> RX ~0.6G under a TX blast). Default off
		 * = float (fine for one-directional TX). */
		if (bindcpu && i < num_possible_cpus())
			kthread_bind(workers[i], i);
		wake_up_process(workers[i]);
	}
	ret = 0; (void)ret;
	for (i = 0; i < ports; i++)
		pv[i].ctrl->card_ready = cpu_to_le32(1);
	wmb();
	proc_dir = proc_mkdir("octshm", NULL);
	if (proc_dir) {
		proc_temp = proc_create("temp", 0222, proc_dir, &temp_pops);
		proc_env  = proc_create("env", 0222, proc_dir, &env_pops);
	}
	pr_info("octshm M3: phys=0x%llx ring=%d ports=%d uplink=%s up\n",
		(unsigned long long)pv[0].phys, RING_SZ, ports, uplink);
	return 0;
}

static void __exit octshm_exit(void)
{
	int i;

	if (proc_temp)
		proc_remove(proc_temp);
	if (proc_env)
		proc_remove(proc_env);
	if (proc_dir)
		proc_remove(proc_dir);
	for (i = 0; i < MAX_WORKERS; i++)
		if (!IS_ERR_OR_NULL(workers[i]))
			kthread_stop(workers[i]);
	if (zc)
		msleep(200);		/* let in-flight zc frames drain off the NIC before we free
					 * the window pages. Their frags hold get_page refs, so a late
					 * free is refcount-safe anyway; this just avoids the churn. */
	for (i = 0; i < ports; i++) {
		if (!pv[i].up_dev)
			continue;
		dev_remove_pack(&pv[i].ptype);
		rtnl_lock();
		dev_set_promiscuity(pv[i].up_dev, -1);
		rtnl_unlock();
		dev_put(pv[i].up_dev);
		if (pv[i].ctrl)
			pv[i].ctrl->card_ready = 0;
	}
	for (i = 0; i < MAX_WORKERS; i++) {		/* free any skbs still queued */
		skb_queue_purge(&rxq[i]);
		while (ztx_h[i] != ztx_t[i])		/* free undrained pipelined TX skbs */
			dev_kfree_skb(ztx_fifo[i][ztx_h[i]++ & (ZTX_FIFO - 1)].skb);
	}
	cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX0), 0);
	if (ports > 1)
		cvmx_write_csr(CVMX_ADD_IO_SEG(PEM_BAR1_IDX1), 0);
	if (dpi_up == 2) {			/* quiesce DPI before dropping its chunks */
		int q;
		for (q = DPI_Q0; q < DPI_Q0 + DPI_NQ; q++)
			cvmx_write_csr(CVMX_ADD_IO_SEG(DPIR_IBUFF(q)), 0);
		CVMX_SYNCW;
		msleep(50);
	}
	kfree(hrx_hdr);
	kfree(ztx_done); kfree(ztx_src);
	for (i = 0; i < ports; i++)
		if (pv[i].pages)
			__free_pages(pv[i].pages, WIN_ORDER);
	pr_info("octshm: unloaded\n");
}
module_init(octshm_init);
module_exit(octshm_exit);
MODULE_LICENSE("GPL");
