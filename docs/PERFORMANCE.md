# Performance

Measured with `iperf3 -P8`, MTU 9000, on a fresh card boot, over a DAC to an HP NC523 peer
(each peer port in its own netns — see [USAGE → test rig](USAGE.md#test-rig-netns)).

## Per-port, one port at a time

| port | FWD (host → peer) | REV (peer → host) |
|---|---|---|
| `oct0` (xaui0) | 8.29 Gb/s | 7.75 Gb/s |
| `oct1` (xaui1) | 7.28 Gb/s | 6.91 Gb/s |

Both directions near line rate. Ping 0% loss.

> **PCIe MaxPayload (MPS).** The card is Gen2 ×4 (`lspci`: `5GT/s Width x4`) → ~16 Gb/s usable per
> direction, so 10 Gb/s per port in one direction is within budget. A Secondary Bus Reset leaves the
> card's MPS at 128 B while the bridge is 256 B; `octboot` now re-matches the card to **256 B**
> (`setpci … CAP_EXP+8.w`), which lifted `oct0` FWD 7.75 → **8.29 Gb/s**. The remaining gap to line
> rate is the card's per-packet CPU cost (skb alloc + copy on TX, RX tap clone), not the PCIe link or
> the DMA engine (which benches > 14 Gb/s) — closing it needs a zero-copy card datapath.

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

- **~7.7 Gb/s per port** in isolation — a genuine 10 GbE-class host NIC out of an OEM
  SmartNIC with no vendor firmware.
- **~6.4 Gb/s aggregate** whichever way you split it — the SoC/PCIe ceiling, not the links.
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
