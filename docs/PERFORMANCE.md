# Performance

Measured with `iperf3 -P8`, MTU 9000, on a fresh card boot, over a DAC to an HP NC523 peer
(each peer port in its own netns — see [USAGE → test rig](USAGE.md#test-rig-netns)).

## Per-port, one port at a time

| port | FWD (host → peer) | REV (peer → host) |
|---|---|---|
| `oct0` (xaui0) | **9.71 Gb/s** | 7.51 Gb/s |
| `oct1` (xaui1) | ~9.2 Gb/s | ~7.3 Gb/s |

FWD is **line-rate 10 GbE**. Ping 0% loss. (`oct0` FWD reached 9.71 Gb/s single-port and 9.22 Gb/s
in the shipped `ports=2` config.)

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

## Simultaneous (both ports, full bidirectional)

Full 4-way load = `oct0` TX+RX and `oct1` TX+RX all at once:

| port | TX (host → peer) | RX (peer → host) |
|---|---|---|
| `oct0` | 5.09 Gb/s | 2.85 Gb/s |
| `oct1` | 5.09 Gb/s | 2.59 Gb/s |

**Aggregate ≈ 15.6 Gb/s**, close to the PCIe Gen2 x4 full-duplex ceiling (~16 Gb/s per
direction). TX ~10.2 Gb/s + RX ~5.4 Gb/s across the two ports.

> **TX-starvation fix.** Earlier builds collapsed TX to ~0.45 Gb/s/port under this load
> (aggregate ~6.4 Gb/s): the card's worker loop drained RX and then *skipped* the TX drain
> whenever it had moved any RX, so a never-empty RX queue starved TX entirely. Bounding the
> RX batch and always falling through to the TX drain lifted TX to ~5.09 Gb/s/port with no
> RX cost — a 2.4× aggregate improvement. (commit `octshm_card: fix TX starvation`.)

The remaining ceiling is the card's 8-core Octeon + PCIe bandwidth, shared across all
flows — you still don't get 2 × 10 G by summing the ports, but both directions now coexist.

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
