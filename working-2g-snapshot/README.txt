Cavium SNIC10E host NIC — MULTI-WORKER + RX-LOCK snapshot (2026-07-02)
=====================================================================
Throughput (jumbo 9000, iperf3, DAC loopback xaui0<->xaui1):
  Reverse (card->host, RX-DMA):  ~1.93-1.95 Gbit/s  (was 858M single-worker)
  Forward (host->card, PIO):     ~1.0-1.87 Gbit/s   (variable, PIO-bound)
  Bidirectional:                 ~1.8 Gbit/s aggregate
STABLE: no datapath death across many heavy runs, ping 0%.

KEY FIX vs working-1g-snapshot:
  1. cardmod: N TX worker kthreads (default nworkers=4), claim+copy+tx_cons
     under tx_lock in strict order (race-free ring), skb-alloc+xmit outside lock.
  2. cardmod: oct_rx_pack RX-ring producer now under rx_lock. This was a
     LATENT race (present even single-worker) — multiple TX workers -> concurrent
     loopback RX softirq on many cores -> two frames grabbed the same slot ->
     rx_prod desync -> datapath death. The lock fixed stability AND lifted
     reverse from 858M to ~1.95G (was retransmit-limited by the corruption).

nworkers tuning (module param, default 4):
  1 = single-worker (== working-1g-snapshot behavior), 4 = best balance,
  6/8 = no gain (reverse RX-bound ceiling, forward tx_lock-bound).

Restore: ../restore-1g.sh (transfers this cardmod/octshm_card.ko, default nw4).
Deep fallback if ever unstable: ../working-1g-snapshot/ (single-worker, bulletproof).
DPI hardware DMA: dead end — engine consumes instructions but never executes
transfers (exhaustive attempt: SDK format, full init, uncached coherency all ruled
out). Needs Cavium HRM operational sequence not in public sources. See memory.
