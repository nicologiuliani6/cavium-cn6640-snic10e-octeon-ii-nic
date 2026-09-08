# DMA design — why RX is card-mastered and TX is not

[ARCHITECTURE](ARCHITECTURE.md) describes *what* the datapath does. This page is the
*why*: the two directions ended up asymmetric, and that asymmetry is the whole reason the
card reaches line rate.

## The constraint: non-posted PCIe reads

Writes across PCIe are **posted** — the writer fires and forgets. Reads are **non-posted**:
every read stalls on a round trip. A datapath is fast when each side only ever *writes*
across the bus, and slow the moment a per-frame *read* appears in the hot loop.

That single rule decides both directions:

| | who writes | who reads | across PCIe |
|---|---|---|---|
| **RX** (card → host) | card DPI writes host RAM | host reads its own RAM | writes only |
| **TX** (host → card) | host PIO writes the BAR window | card reads its own DRAM | writes only |

The two rejected variants are exactly the ones that put a read on the wire: a host that
reads RX descriptors out of the BAR window, and a card that DMA-*reads* host RAM for TX.

## RX: card-mastered DMA into host RAM (`hrx=1`)

The card's **DPI** engine copies each received frame into a host-RAM pool the host
published, and — in the same DPI operation, posted last so it lands after the payload — an
8-byte `{len, phase}` header ahead of it. The host polls that header in its **own** RAM, so
the per-frame descriptor read never crosses the bus. Without `hrx` the host had to MMIO-read
a descriptor per frame from the window; that was the RX wall.

The remaining RX cost is on the card: capture. Spreading eight POW-group RX IRQs across
cores (`receive_group_order=3`, IRQ affinity in the image's `rc.local`) is what took RX from
~5.4 to ~8.9 Gb/s — see [PERFORMANCE](PERFORMANCE.md).

### Outbound addressing (card → host RAM)

The card forms a physical address that turns into a PCIe TLP to host bus address `H`:

```
card_phys = (1ull << 63) | ((u64)subid << 34) | (H & 0x3FFFFFFFF)
```

- bit 63 selects the memory-access region, bits [38:34] pick the SUBID, the low 34 bits are
  the offset;
- `subid = 12 + (H >> 34)` — u-boot pre-programs SUBID 12..15 for PEM port 0 with
  `ba = 0,1,2,3`, each covering a 16 GiB PCIe window (`ba` = PCIe address bits [63:34]);
- `CVMX_PEXP_SLI_MEM_ACCESS_SUBIDX(i) = CVMX_ADD_IO_SEG(0x00011F00000100E0) + (i & 31)*16 - 16*12`,
  fields (`cn63/66 _s`): `ba:30@[41:12], port:3, nmerge, esr:2, esw:2, wtype:2, rtype:2`;
- endianness: `esr = esw = 1` (`_CVMX_PCIE_ES=1`) makes the stream byte-identical to a
  little-endian host, matching the inbound BAR window.

## TX: host PIO fill, no card-side DMA read

The host writes frames straight into the BAR2 TX buffer with write-combining, claiming
slots by CAS so several TX queues (`ntxq`) fill the one ring in parallel; the card drains by
phase bit and hands PKO a frag pointing at the window slot (`zc=1`) instead of copying.

The obvious-looking alternative — the card DMA-*reads* the frame out of host RAM (`ztx=1`,
inbound DPI) — is implemented and works, and still loses: it is read-latency-bound at
~6.6 Gb/s against ~7.75 Gb/s for plain PIO fill, so it stays off.

## Safety gates for card-mastered DMA

The card writing host RAM is exactly the operation that corrupts memory when it goes wrong,
so the rules are:

- no bus-master until the device has an IOMMU group
  (`ls /sys/bus/pci/devices/<BDF>/iommu_group`) — a stray card write then faults instead of
  landing in random host memory;
- the card only ever forms `card_phys` from bus addresses the host published in the control
  page, with the offset clamped to the published size;
- the PIO path stays the fallback (`dma=0` is the module default).

> Enable the IOMMU at **runtime** (`echo DMA > /sys/.../<BDF>/iommu_group/type`) rather than
> via a GRUB boot parameter on this host.
