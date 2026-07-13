# Performance

Measured with `iperf3 -P8`, MTU 9000, on a fresh card boot, over a DAC to a 10 GbE peer (our
dev-only test peer here was an HP NC523 in the same host, each port in its own netns — see
[USAGE → test rig](USAGE.md#test-rig-netns)); the peer is not part of the deliverable.

## Per-port, one port at a time

| port | FWD (host → peer) | REV (peer → host) |
|---|---|---|
| `oct0` (xaui0) | **9.71 Gb/s** | **8.10 Gb/s** |
| `oct1` (xaui1) | **9.81 Gb/s** | **8.82 Gb/s** |

FWD is **line-rate 10 GbE** and REV is at ~85–90 % of wire. Ping 0% loss. (Shipped `ports=2`
config, single fresh boot, one measurement pass.)

> **RX 5.4 → 8.9 Gb/s: the capture cores were the wall.** RX frames die *between* PIP and the
> driver tap (the POW/NAPI capture layer) when the NAPI cores can't keep up — proven with UDP
> probes (38 % loss while the card's own counters showed PIP drops = 0, DPI queue ~empty, and
> deliver cost only 0.6 µs/frame). RX scales with the number of NAPI cores: 2 cores = 5.4 G,
> 4 = 8.2, 6 = 8.9, 8 = 7.2 (cross-core contention). Meanwhile the zero-copy TX path needs only
> **two** worker cores for line rate. The shipped split is therefore `nworkers=2` (cpu0-1) +
> RX POW IRQs pinned to cpu2-7 (**6 NAPI cores**) — set by `bindcpu=1` + rc.local.

> **Zero-copy TX = the 10G lever.** Reaching line rate took two card-side fixes:
>
> 1. **PCIe MaxPayload (MPS).** Card is Gen2 ×4 (`lspci`: `5GT/s Width x4`) → ~16 Gb/s usable per
>    direction, so 10 Gb/s per port one-way is within budget. A Secondary Bus Reset leaves the card's
>    MPS at 128 B while the bridge is 256 B; `octboot` re-matches the card to **256 B**
>    (`setpci … CAP_EXP+8.w`) → `oct0` FWD 7.75 → 8.29 Gb/s.
> 2. **Zero-copy TX (`zc=1`, now default).** The old TX path did a `netdev_alloc_skb(9216)` + a 9 KB
>    cold-cache `memcpy` of the PCIe-written window into the skb for *every* frame (~43 µs/frame) —
>    that per-packet CPU cost, not the link or the DMA engine (which benches > 14 Gb/s), was the wall
>    at 8.3 Gb/s. `zc` instead copies only the **64 B of L2-L4 headers** linear and hands PKO a
>    **frag pointing straight at the window slot**, so the NIC DMA-gathers the 9 KB payload with no
>    big alloc and no payload copy. Slot reuse is safe by ring depth (128 slots) — PKO drains at wire
>    (9.89 Gb/s) faster than the host fills over PCIe (≤ 8.4 Gb/s), so a slot is always done
>    transmitting before the host wraps to it. Result: `oct0` FWD **8.29 → 9.71 Gb/s**.

> **Getting the header split right mattered.** A first cut put only the 14 B Ethernet header linear and
> the IP+TCP headers in the frag; the octeon-ethernet TX path reads `ip_hdr(skb)` from the *linear*
> region for its checksum-offload decision, so it read garbage, set a bad `ipoffp1`, and PKO recomputed
> the checksum at the wrong offset — corrupting every frame (141 Mb/s + a retransmit storm). Copying a
> full 64 B header (eth+ip+tcp) linear fixed it. A completion-gating scheme via `skb->destructor` also
> throttled TX to 299 Mb/s through the driver's lazy `tx_free_list`; dropping it for ring-depth safety
> is what unlocked line rate.

## Double (two streams at once, same direction)

| load | result |
|---|---|
| **2-port TX** (`oct0`+`oct1` FWD) | 4.91 + 5.66 = **10.6 Gb/s** aggregate |
| **2-port RX** (`oct0`+`oct1` REV) | 6.24 + 3.97 = **10.2 Gb/s** aggregate |

Both single-direction aggregates now sit at ~10.5 Gb/s (2-port RX was 5–7 before the NAPI-core
split — it nearly doubled).

> **2-port TX ~10.5 Gb/s is the host-PIO wall, not a tuning gap.** The host fills the TX rings
> with CPU stores through a write-combining BAR2 mapping; x86 WC buffers flush as **64-byte**
> PCIe TLPs, whose header overhead caps the Gen2 ×4 link at ~73 % efficiency ≈ 11.7 Gb/s
> theoretical — the measured 10.5 is ~90 % of that, with the host CPUs far from saturated
> (mpstat ~36 % idle during the blast). Beating it would need ≥ 256 B TLPs, i.e. card-pulled DMA
> (the `ztx` inbound-DPI path) — which is read-latency-bound at ~6.6 Gb/s and loses. 10.5 stands.

## Full-duplex (TX and RX at once) — the open front

Any substantial *real* TX traffic collapses RX — even across ports (`oct0` TX + `oct1` RX:
9.5 + 2.2 Gb/s), and near-independently of the TX rate (paced TX at 4 G still leaves RX at
2.75). A long elimination session (all runtime experiments, counters on the card) ruled out:

- **PCIe direction contention** — a synthetic 100 %-duty BAR2 write flood has *zero* RX impact;
- **ACK drops at the host TX ring** — `tx_dropped = 0` under duplex;
- **DPI engine starvation** — DPI doorbell backlog stays ≤ 36 under duplex;
- **PIP/RED capture drops** — PIP drop counters stay 0;
- **host qdisc ACK queuing** — `fq` helps only marginally (RX 0.13 → 0.73 Gb/s) and its single
  qdisc lock costs TX 3.5 Gb/s;
- **L2 thrash via the BAR1 CA bit** — plausible (host PIO writes allocate in the shared 2 MB L2,
  and a TX blast cycles a 1.15 MB window), but `l2ca=0` breaks store/load coherency under load
  (port wedge) — documented dead end as tried.

What the counters *do* show under duplex: the card keeps **delivering** ~130 k frames/s (9.3 Gb/s
raw) while goodput is ~0 — an out-of-order/retransmit storm seeded by RX-ring-full drops.
**L2 way-partitioning** (`wpar=0x3`, `L2C_WPAR_IOB`) was tested and is **neutral** — also ruled out.

**The quantified truth (UDP probes, no TCP dynamics):** the card sustains **TX 8.7 + RX 4.8 =
13.5 Gb/s aggregate full-duplex** — RX halves under any big TX (the card-global step: DRAM/IOB
bandwidth, ~39 Gb/s of combined window traffic under duplex) but does *not* collapse. What
collapses is **TCP**: the RX side runs at ~46 % frame loss at that operating point, and TCP
goodput dies under that loss. Even with both directions app-paced, TCP holds at most
~**4.0 + 2.8 = 6.8 Gb/s** balanced. So:

| duplex mode | aggregate |
|---|---|
| UDP / loss-tolerant | **13.5 Gb/s** (8.7 TX + 4.8 RX) |
| TCP, both sides paced | ~6.8 Gb/s (4.0 + 2.8) |
| TCP, unpaced (default) | ~9.6 Gb/s (TX-priority, RX ~0) |

## Takeaways

- **TX line-rate per port** (9.7–9.8 Gb/s) *and* **RX at 85–90 % of wire** (8.1–8.8 Gb/s), one
  direction at a time — a genuine 10 GbE dual NIC out of an OEM SmartNIC with no vendor firmware.
- **~10.5 Gb/s aggregate in either single direction** with both ports loaded — the host-PIO
  64 B-TLP wall (TX) and its RX counterpart, ~80 % of the Gen2 ×4 usable budget.
- **Full-duplex under load is the one open front** (card-global step effect, see above).
- Numbers are for a **freshly booted** card. A card that has been driven hard can wedge and
  report degraded/zero throughput until re-booted with `octboot` (see
  [USAGE → troubleshooting](USAGE.md#troubleshooting)).

## Reproduce

```bash
sudo systemctl restart cavium-nic     # fresh boot + both ports up
# port 0
sudo ip netns exec nc  iperf3 -s -B 10.9.9.2 &
sudo iperf3 -c 10.9.9.2 -B 10.9.9.1 -P8 -t10        # -R for reverse
# port 1
sudo ip netns exec nc2 iperf3 -s -B 10.9.10.2 &
sudo iperf3 -c 10.9.10.2 -B 10.9.10.1 -P8 -t10
```
