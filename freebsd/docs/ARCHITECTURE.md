# Architecture

> **Status: not verified.** Written from the Linux driver and the FreeBSD port design. Not tested on a FreeBSD host or on hardware. See [README](../README.md).

## High-level diagram

```
HOST (FreeBSD kernel)                PCIe BAR2                       CARD (OpenWrt from RAM)
┌──────────────────────┐                              ┌─────────────────────────────────┐
│ if_octnic.ko         │   BAR2 window (64 MiB)       │ octshm_card (card kernel module)│
│ kld driver           ├──────── shared-memory rings ─┤  per-port TX/RX rings           │
│ oct0 / oct1 (ifnet)  │                              │  DPI RX engine                  │
│                      │   TX: host PIO into BAR buf  │  XAUI tap (packet_type)         │
│                      │   RX: card DPI DMA to host   │                                 │
└──────────────────────┘                              └───────────────┬─────────────────┘
                                                                      │ XAUI + DAC
                                                                      ▼  peer 10 GbE
```

## Shared-memory NIC

The card and host share a 64 MiB BAR2 window. Each port occupies a 4 MB (`0x400000`) stride:

| Offset (from port base) | Content |
|---|---|
| `0x000000` | Control page (256 B): magic, version, ready flags, ring indices, DMA bases, temperatures, link state |
| `0x001000` | TX descriptor ring: 128 entries x 8 bytes `{len: u32, phase: u32}` |
| `0x002000` | RX descriptor ring: 128 entries x 8 bytes `{len: u32, flags: u32}` |
| `0x100000` | TX buffer: ~4 MB, host PIO writes frames here (write-combining BAR) |
| `0x280000` | RX buffer: ~3.5 MB, card DPI DMA writes frames here |

Each descriptor entry is 8 bytes. The ring wraps at 128 entries (power-of-2 mask).

## Ring protocol

### Phase bit

The phase bit is the low bit of the 32-bit value at offset +4 in each descriptor. It
alternates between 0 and 1 for successive wraps of the ring, allowing the consumer to
distinguish "new" data from "stale" entries without consulting a shared index.

```
phase(entry N) = (N >> 7) & 1    // log2(RING_SIZE=128) = 7
```

A slot is free when `descriptor.phase == expected_phase`. The host increments its producer
counter (`op_tx_claim`) via `atomic_fetchadd_int` and checks the descriptor phase before
writing.

### TX flow (Host -> Card)

1. Host atomically claims a slot: `tp = atomic_fetchadd_int(&port->op_tx_claim, 1)`
2. Checks ring fullness against cached card consumer (`op_tc_cache`)
3. Verifies descriptor phase matches expected -- slot is free
4. Copies frame data into BAR2 TX buffer via `bus_space_write_region_1` (PIO)
5. Writes `{len, phase}` to the descriptor ring via `pwr()` (MMIO write)
6. Memory barriers ensure card sees consistent state

With `ztx=1`, frames are copied into a coherent DMA pool instead of BAR2, and the card
DPI-reads them directly (zero-copy on the card side).

### RX flow (Card -> Host)

**HRX mode** (`hrx=1`): The card DPI-writes an 8-byte `{len: u32, phase: u32}` header into
host RAM ahead of each frame. The host reads descriptors from local RAM (no PCIe MMIO),
then copies frame data from the coherent DMA pool (or BAR2 RX buffer) into an `mbuf`.

**Non-HRX mode**: The host reads the RX descriptor ring via MMIO (`prd64`) to get `{len, flags}`.

The drain is handled by taskqueue callbacks (`octnic_drain`), invoked periodically by the
watchdog timer. In multi-thread mode (`rxthreads > 1`), threads are stride-partitioned by
thread ID across the ring.

### Slot management

- **TX slots**: claimed via CAS/atomic counter, ready when phase matches. Card consumes by
  reading the phase bit and advancing its TX consumer index.
- **RX slots**: card fills and sets phase bit. Host reads phase to detect new data, advances
  consumer shadow (`op_rc_pub`), and writes back `C_RX_CONS` to the control page when
  progress is made.

## RX: DPI DMA + busy-poll

The card's DPI (Direct Packet Insert) engine DMA-writes received frames into host RAM. The
FreeBSD driver uses taskqueue callbacks (`octnic_drain`) rather than kernel threads, invoked
periodically by a `callout` timer (1 Hz). Each callback drains up to 64 slots.

In `hrx=1` mode, descriptors are in local host RAM (no per-frame PCIe MMIO), making the
poll path extremely fast. The `lockfree` mode reads the per-slot phase bit instead of the
shared card RX producer index, enabling safe multi-threaded drain without contention.

## TX: PIO fill + CAS slot claiming

TX frames are written via posted writes into the BAR2 TX buffer. The host uses write-combining
memory (BAR2 is mapped as WC-equivalent via `bus_space`). Slot claiming uses atomic
`atomic_fetchadd_int` on a shared counter (`op_tx_claim`), providing lock-free parallel TX
across multiple queues.

The card drains TX slots by phase bit, supporting zero-copy PKO gather when `ztx=1`.

## Differences from the Linux driver

| Feature | Linux (`linuxmod/octnic.c`) | FreeBSD (`freebsd/octnic/if_octnic.c`) |
|---|---|---|
| Framework | Kernel module + netdev | kld driver + ifnet |
| TX callback | `ndo_start_xmit` | `if_settransmitfn` |
| RX | kthread + `local_bh_disable` | taskqueue callback + callout timer |
| Memory mapping | `ioremap_wc()` | `bus_space_map()` via `rman_get_bustag/handle` |
| Scheduling | `kthread_run()` + `schedule_delayed_work` | `callout_reset()` + `taskqueue_enqueue()` |
| Parameters | `module_param()` | `SYSCTL_INT(_kern, ...)` |
| HWMON | Linux hwmon subsystem | `device_printf()` to dmesg |
| Multi-queue | `alloc_etherdev_mq()` | `ntxq` sysctl (single TX path currently) |
| Locking | `spinlock_t` | `struct mtx` |
| DMA | `dma_alloc_coherent()` | `bus_dmamem_alloc()` + `bus_dmamap_load()` |
| Module params | `modprobe octnic param=value` | `sysctl kern.octnic.param=value` |

The shared-memory protocol, register offsets, and ring format are **byte-identical** between
the two drivers. All numeric constants in `freebsd/octnic/octnic_regs.h` are copied directly
from `linuxmod/octnic.c` with line references.

## Boot flow

The `octboot` tool is Linux-only today (setpci, sysfs, resource2). A FreeBSD equivalent is not implemented yet. The steps below are the Linux flow:

1. **SBR** (Secondary Bus Reset) to clear the card
2. Restore BARs via `setpci`
3. Push OpenWrt image to card DRAM at offset `0x10000` over BAR2
4. Write `GO!!` marker at `+0x100`
5. Wait for heartbeat counter to increment (card OS running)

The rc.d script (`rc.d/cavium_nic`) wraps this: it runs `octboot`, then brings up both
interfaces with `ifconfig oct0/1 up`.
