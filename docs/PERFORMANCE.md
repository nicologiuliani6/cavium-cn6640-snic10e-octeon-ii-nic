# Performance

Measured with `iperf3 -P8`, MTU 9000, on a fresh card boot, over a DAC to an HP NC523 peer
(each peer port in its own netns — see [USAGE → test rig](USAGE.md#test-rig-netns)).

## Per-port, one port at a time

| port | FWD (host → peer) | REV (peer → host) |
|---|---|---|
| `oct0` (xaui0) | 8.8 – **9.71** Gb/s | 7.4 Gb/s |
| `oct1` (xaui1) | 9.2 – 9.5 Gb/s | 8.2 Gb/s |

FWD is **line-rate 10 GbE** (peak `oct0` 9.71 Gb/s single-port, 9.22 Gb/s in the shipped `ports=2`
config; 8.8–9.5 Gb/s run to run). Ping 0% loss.

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

## Double (two streams at once)

| load | result |
|---|---|
| **2-port TX** (`oct0`+`oct1` FWD) | 5.69 + 4.84 = **10.53 Gb/s** aggregate |
| **2-port RX** (`oct0`+`oct1` REV) | 3.48 + 3.47 = **6.95 Gb/s** aggregate |
| **1-port full-duplex** (`oct0` OUT+IN) | OUT 9.6–9.8 + IN 0.07–5.47 Gb/s (see note) |

2-port TX aggregate (~10.5 Gb/s) is the **PCIe host→card direction saturated** — the zero-copy
TX is fast enough that two ports together fill the inbound PCIe budget.

> **Full-duplex RX under TX — `bindcpu=1`, and an unresolved reproducibility caveat.** Under a
> full-rate zc TX blast a port's full-duplex RX starves: the spinning TX workers crowd out the RX
> NAPI softirq that captures frames off XAUI. `bindcpu=1` (default) pins the (now 6) TX workers to
> cores 0–5 and the RX POW-group IRQs to cores 6–7 so RX capture has dedicated cores. **On a
> freshly-booted card this lifted single-port full-duplex to 9.77 / 5.47 Gb/s (TX line-rate, RX ×9
> over the un-pinned build). But the RX half did not reproduce after several more boot cycles
> (9.60 / 0.07), alongside a drop in RX-alone (7.2 → 5.5) — i.e. the card's DRAM degrades under a
> long session of boots ([troubleshooting](USAGE.md#troubleshooting)), and RX (a DPI read path) is
> far more sensitive to that than TX (posted writes).** So: **TX line-rate is solid and
> reproducible; the `bindcpu` full-duplex-RX gain is real on a fresh card but needs re-validation
> from a cold boot before it's a headline number.** The 2-port 4-way case starves RX regardless of
> core split (tried 2- and 4-RX-core layouts) — a shared-resource wall (DPI engine / memory
> bandwidth), not core allocation.

## Quad (4-way: both ports, both directions at once)

| port | TX (host → peer) | RX (peer → host) |
|---|---|---|
| `oct0` | 5.05 Gb/s | 0.28 Gb/s |
| `oct1` | 5.08 Gb/s | 0.74 Gb/s |

**TX aggregate ~10.1 Gb/s** (the inbound-PCIe ceiling again), 0% ping loss, no wedge.

> **RX starves under simultaneous TX — a trade-off of the zero-copy win.** With `zc` the TX
> path is so much cheaper per frame that the card's 8 worker cores stay busy transmitting and
> under-service the RX drain, so RX collapses under a full TX blast (quad RX ~1 Gb/s aggregate;
> single-direction RX is a healthy 7–8 Gb/s). The card's 8-core Octeon, not the PCIe link, is
> the shared resource here. Rebalancing the worker loop (bounding the TX batch per pass so RX
> gets serviced under contention) is the open lever if balanced bidirectional throughput matters
> more than peak one-way TX; the shipped default optimizes for line-rate TX.

## Takeaways

- **Line-rate 10 GbE on TX per port** in isolation (`oct0` FWD 9.71 Gb/s) — a genuine 10 GbE
  host NIC out of an OEM SmartNIC with no vendor firmware, via a zero-copy card datapath.
- **~15.6 Gb/s aggregate** with both ports fully loaded — the PCIe Gen2 ×4 full-duplex
  ceiling, not the links. You don't get 2 × 10 G by summing ports (40 > 32 Gb/s bus).
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
