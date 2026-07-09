# Architecture

```
        HOST                         PCIe                        CARD (OpenWrt from RAM)
  ┌───────────────┐                                        ┌───────────────────────────┐
  │  octnic        │   BAR2 window (64 MiB)                │  octshm_card               │
  │  oct0 / oct1   │◄────────shared-memory rings──────────►│  per-port rings            │
  │  (netdevs)     │                                        │  DPI RX engine            │
  │                │   TX: host PIO into BAR window         │  XAUI tap (packet_type)   │
  │                │   RX: card DPI DMA into host RAM       │                           │
  └───────────────┘                                        │  octcarrier               │
                                                            │  xaui0/xaui1 TX un-gate   │
                                                            └──────────┬────────────────┘
                                                                       │ XAUI + DAC
                                                                       ▼  peer 10 GbE
```

## The channel: PEM inbound windows

The card exposes its DRAM to the host through the Octeon **PEM** inbound BARs. The OEM
boot-app never programmed them, so both BARs read all-`0xFF` (the "BAR wall"). Programming
`PEMX_P2N_BARx_START` + `PEMX_BAR1_INDEXx` from the card (u-boot `write64`, or from the
kernel module) maps card physical memory into the host's BAR2. This is the whole
foundation — a plain memory window, no NDA firmware.

## Shared-memory NIC (`octshm`)

The BAR2 window holds, per port, a fixed layout: a **control page** (magic, version,
ready/heartbeat, ring producer/consumer indices, DMA bases, temp), a **TX descriptor
ring**, an **RX descriptor ring**, and packet buffers.

- **Ring format** is virtio-style: fixed-size (128-slot) rings with a **phase bit** per
  slot (`(index >> 7) & 1`) so producer and consumer can tell a fresh slot from a stale one
  without a lock (`lockfree=1`).
- **Per-port regions.** With `ports=2` each port gets its own 4 MiB region and its own
  `PEMX_BAR1_INDEX` (IDX0 → port0, IDX1 → port1). A single 8 MiB allocation is **not**
  used — it exceeds the buddy allocator `MAX_ORDER` (top order-10 = 4 MiB) and fails; two
  4 MiB allocations mapped to consecutive BAR index windows look contiguous to the host.

### RX (card → host) — DMA

The card's **DPI** (DMA Packet Interface) engine copies received frames into host memory.
In **`hrx`** mode the card DPI-writes an 8-byte `{len, phase}` header + the frame into a
host-RAM RX pool; the host reads the header locally (no per-frame MMIO descriptor read
across PCIe), which is what lifts RX to ~10 G line rate. Eight POW-group RX IRQs are spread
across cores on the card so RX NAPI runs multi-core. See [DMA-DESIGN](DMA-DESIGN.md).

### TX (host → card) — PIO

The host posts frames by writing directly into the BAR2 TX buffer (write-combining), with a
CAS-claimed slot + per-slot TX phase so multiple host TX queues (`ntxq`) can fill the single
ring in parallel; the card drains by phase. TX is PIO (the inbound-DPI/zero-copy path
wedges on this board), so under full bidirectional contention TX yields to the DMA'd RX
(see [PERFORMANCE](PERFORMANCE.md)).

## XAUI uplink (`octcarrier`)

The card bridges each shared-memory ring to a physical XAUI port. On the QLogic DAC the
Vitesse PHY keeps the XAUI TX gated until `cvmx_helper_link_set(ipd_port, up)` is called;
`octcarrier` does exactly that for `xaui0` (`ipd_port=0`) and `xaui1` (`ipd_port=16`). Each
port maps 1:1 to a host netdev, like the OEM 2-port LiquidIO.

## Host driver (`octnic`)

`octnic` auto-discovers the card (PCI `177d:0092`), maps the BAR2 window, verifies the
per-port magic, and registers `oct0`/`oct1`. It carries the port pointer in `netdev_priv`,
runs `rxthreads` parallel RX drain kthreads per port, and registers an hwmon device for the
card temperatures. `base=0` (default) means no hard-coded address — `modprobe octnic
ports=2` just works.

## Boot (`octboot`)

`octboot` is the host bootloader: Secondary Bus Reset → restore BARs → wait for the card's
u-boot to program its window → push the ~21 MiB OpenWrt image into card DRAM over BAR2 (with
a word-fix pass for write-combining corruption) → the card's `bootcmd` boots it → poll BAR2
for the NIC heartbeat (magic + card_ready). No serial. Provisioning of the persistent
u-boot env is described in [FLASHING](FLASHING.md).

## What is deliberately not done

- **No inbound-DPI / zero-copy TX** — it wedges this board; TX stays PIO.
- **No flashing** — the NIC role runs entirely from RAM; the card is untouched and the host
  path is out-of-tree.
- **Serial-free *first* install** — prototyped (`octconsole`, u-boot PCI console injection)
  but shelved; provisioning still needs the serial cable once.
