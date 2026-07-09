# Performance

Measured with `iperf3 -P8`, MTU 9000, on a fresh card boot, over a DAC to an HP NC523 peer
(each peer port in its own netns — see [USAGE → test rig](USAGE.md#test-rig-netns)).

## Per-port, one port at a time

| port | FWD (host → peer) | REV (peer → host) |
|---|---|---|
| `oct0` (xaui0) | 7.75 Gb/s | 7.74 Gb/s |
| `oct1` (xaui1) | 7.28 Gb/s | 6.91 Gb/s |

Both directions near line rate. Ping 0% loss.

## Simultaneous — the card is the ceiling

The bottleneck is the card's 8-core Octeon + its PCIe bandwidth, **shared across all active
flows** — you do not get 2 × 10 G by summing the ports.

| load | aggregate |
|---|---|
| both ports, FWD only, simultaneous | 3.18 + 3.14 = **6.3 Gb/s** |
| both ports, full bidirectional (4 streams) | **~6.4 Gb/s** |

Full 4-way breakdown (oct0+oct1, TX+RX all at once):

| port | TX (uscita) | RX (entrata) |
|---|---|---|
| `oct0` | 0.44 Gb/s | 2.87 Gb/s |
| `oct1` | 0.48 Gb/s | 2.58 Gb/s |

Under full contention **RX wins and TX starves**: RX rides the card's DPI DMA engine while
TX is host PIO, so the shared budget goes to RX. If you need high TX and high RX at once,
use one port per direction (TX on one, RX on the other) rather than full-duplex on both.

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
