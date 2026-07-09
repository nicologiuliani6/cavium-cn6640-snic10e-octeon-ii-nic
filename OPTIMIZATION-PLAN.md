# octshm RX datapath — optimization plan (toward 10G)

Status 2026-07-03: octshm NIC works ~2.0G (reverse iperf) / ~3.4G (blaster datapath) in
loopback. Reverse iperf is capped by the **card's single-core TCP generation** (card is the
sender in the DAC loop) — NOT the datapath. With a real external 10G peer the peer generates
the traffic and the datapath limit (~3.4G today) is what matters.

## The multi-core RX hot path — false sharing is the prime suspect
With `octeon-ethernet.receive_group_order=3`, xaui0 RX is delivered on up to 8 cores (8 POW
groups, PIP flow-hash). But the octshm ptype tap `oct_rx_pack` funnels all cores through
shared state every packet → cacheline ping-pong that throttles the scaling:

1. `ctrl->resv[0]`/`resv[1]` per-packet writes — **REMOVED** (2026-07-03, de8aa315). Every core
   wrote the same cacheline every frame. Pure debug. Gone from hot path.
2. `rx_lock` taken TWICE per packet (claim, then ordered completion) — both are shared-cacheline
   spinlocks. This is the next target.
3. `rx_claim`, `ctrl->rx_cons` read, `ctrl->rx_prod` write, `rx_done[]` — all shared lines.

## Next step is a MEASUREMENT, not a blind rewrite
Before touching the ring, confirm on the (cooled) card, under an RGO=3 blast:
- per-core `/proc/stat` deltas — are multiple cores actually busy in softirq (RX distributed)?
- If yes → `rx_lock`/false-sharing is the cap → do the lock-free ring below.
- If no (one core hot) → PIP grptag isn't distributing → fix that first (CVMX_PIP_PRT_TAGX
  tag_type/tag_mode on the xaui0 ipd port; driver sets grptag but IRQ 121 dominated in one test
  — need to confirm 121 = xaui1 TX vs a single RX group).

## Lock-free ring design (only after the measurement says locking is the cap)
Goal: remove both `rx_lock` acquisitions from the per-packet path while keeping the host's
cheap single-MMIO `rx_prod` read (per-slot valid-flag polling over BAR2 = many slow PCIe reads
= worse, per the busy-poll finding — do NOT go there).
- Claim: `atomic_fetch_inc(&rx_claim)` — lock-free. PROBLEM: drops on ring-full leave a "hole"
  in claim numbers that stalls the in-order `rx_prod` publish. Fix: a dropped/oversize slot must
  still be marked done (len=0) so the publish loop can advance past it; host already skips len=0.
- Completion/publish: keep a tiny lock (or a cmpxchg loop) ONLY for advancing `rx_prod` over
  contiguously-done slots — this is inherently serial for an in-order single-pointer protocol.
  Net: 1 tiny locked region per packet instead of 2, and lock-free claim.
- Bigger win (more work + matched host change): per-core RX sub-rings (each core its own slot
  range → zero cross-core sharing); host drains N sub-rings round-robin. Eliminates false sharing
  entirely. Highest ceiling, highest risk.

## IMPLEMENTED (offline, ready to test — all behind flags, baseline intact)
Built 2026-07-03 while the card cooled. Each is a module param so we A/B on the bench.

| Item | Where | Flag | Default |
|---|---|---|---|
| Remove per-pkt debug false-sharing | card tap | (always) | on |
| RX batching (netif_receive_skb_list) | host drain | `rxbatch` | 1 |
| DPI linearize jumbo -> 1 contiguous op | card tap | `linrx` | 0 |
| **Lock-free RX ring (phase-bit)** | card+host | `lockfree` | 0 |
| **Zero-copy TX (DPI INBOUND)** | card+host | `ztx` | 0 |

Zero-copy TX = host->card frame is DMA'd (DPI INBOUND, `host_read_dpi`, type=1 mirror of the
proven outbound) from the coherent host pool straight into the card skb — no host `memcpy_toio`
PIO over PCIe. Host CPU freed (the real host->card cost). Needs dma=2 (DPI up). `host_read_dpi`
blocks until the frame lands, then xmit; the 128-slot ring means the host can't reuse the slot
before the DMA finishes, so tx_cons advances under lock and the DMA runs outside it. Test
FORWARD iperf (host->card, non -R) with `ztx=1` both sides vs baseline PIO. UNTESTED (DPI inbound
never run in-datapath) — short run first, wedge risk.

Lock-free ring = the "lock removal x2" + most of "multi-queue vera": card claims a slot with
an atomic CAS (clean drop on full), then stamps a PHASE bit in desc.flags LAST; host reads the
phase (one 64-bit `readq` of the desc = len+flags) to tell ready vs stale — NO rx_lock, NO
rx_prod, cores never serialize on completion. Correctness checked on paper: phase seeded to 1
(≠ first expected 0); card writes len before flags (wmb) so a phase-match guarantees len valid;
readq can only see (new len,new flags) or (*,old flags)->retry, never (old len,new flags).

### Test matrix (when card is up + cool)
0. FIRST measure the gate: RGO=3 blast, per-core `/proc/stat` deltas. Multiple cores in softirq?
   - card: `insmod octshm.ko dma=2 dpiwait=0` (baseline), host: `insmod octshm_host.ko dma=1`.
1. baseline reverse iperf + blaster oct0_rx (record).
2. `rxbatch` on vs off (host reload) — expect small host-side gain.
3. `linrx=1` (card reload) — jumbo DPI.
4. **`lockfree=1` BOTH sides** (card `insmod ... lockfree=1`, host `insmod ... lockfree=1`) —
   the big one. Test with a SHORT blast first (wedge risk on any concurrency bug), then iperf.
   Compare oct0_rx vs baseline especially under RGO=3 multi-core.

## NOT yet coded (design only)
- DPI doorbell batching (accumulate N pkts' descriptors, one doorbell) — fiddly with per-cpu queues.
- skb recycle pool / build_skb zero-copy RX (host) — needs a destructor/page-flip.

## Reminders / hazards
- Card DRAM degrades under sustained load/crash cycles (thermal) → fast-boot then can't land the
  image (crc unstable). Only real cooldown / power-cycle fixes it. See [[cavium-dram-degrades-under-load]].
- Test lock changes with a SHORT blast first; a bad concurrent RX path wedges the card (needs
  SBR + fast-boot recovery).
- skb-reuse in the blaster CRASHES octeon TX — keep per-packet alloc.
