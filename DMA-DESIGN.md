# octshm NIC — DMA path design (M4, execute after IOMMU enabled)

Goal: replace host-CPU PIO copies with the card mastering host RAM, to reach
multi-Gbit / 10G. Blocked until the user enables IOMMU (one reboot) so a stray
card write faults instead of corrupting host RAM.

## Prereqs (user does once)
```bash
sudo sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="/GRUB_CMDLINE_LINUX_DEFAULT="intel_iommu=on iommu=pt /' /etc/default/grub
sudo update-grub && sudo reboot
```
After reboot: host reboot likely power-cycles the PCIe slot → card resets to OEM
→ re-boot OpenWrt (`openwrt-boot-fast.sh`, ~4min), re-transfer octshm.ko.
Verify IOMMU: `ls /sys/bus/pci/devices/0000:04:00.0/iommu_group` must exist.

## Outbound addressing (card → host RAM), from cvmx-pcie.c
- Card physical address that generates a PCIe TLP to host bus address H:
  `card_phys = (1ull<<63) | ((u64)subid << 34) | (H & 0x3FFFFFFFF)`
  (bit63 = mem-access region; bits[38:34]=subid select; low 34 = offset)
- subid = 12 + (H >> 34); u-boot pre-programs SUBID 12..15 for PEM port0 with
  ba = 0,1,2,3 → each covers a 16GB PCIe window; ba = PCIe addr bits[63:34].
- Endian: esr=esw=1 (_CVMX_PCIE_ES=1) → byte-identical to LE host (same as our
  inbound BAR ES=1). Good.
- CSR: `CVMX_PEXP_SLI_MEM_ACCESS_SUBIDX(i) = CVMX_ADD_IO_SEG(0x00011F00000100E0)
  + (i&31)*16 - 16*12`. Fields (cn63/66 _s): ba:30@[41:12], port:3, nmerge,
  esr:2, esw:2, wtype:2, rtype:2, zero:1.
- Access from Linux module: `cvmx_write64_uint64(CVMX_ADD_IO_SEG(card_phys), v)`
  / `cvmx_read64_uint64(...)`. VERIFY subids are still valid in EP mode; if not,
  program SUBID 12 (ba=0) ourselves in the module.
- Bus-master: `setpci -s 04:00.0 COMMAND=0x06` (mem+master) on host, or the card
  sets its own EP command reg. Enable ONLY after IOMMU confirmed.

## Datapath v2 (DMA)
Control (indices, small) stays PIO in the inbound BAR window (works today).
Packet DATA moves by card-mastered copy into/out of a host coherent region:

1. Host: `dma_alloc_coherent(dev, REGION, &busaddr)` for a packet area (e.g. TX
   256*9216 + RX 256*9216). Publish `busaddr` + size into the ctrl block (new
   le64 field). Map it as the netdev's packet buffers (host CPU touches only its
   own coherent RAM — cache-coherent, fast — never the BAR for payload).
2. Card TX (host→wire): worker reads host TX buffer via outbound
   (`memcpy(local, CVMX_ADD_IO_SEG(card_phys_for(busaddr+txoff)), len)` — card
   READ of host RAM), builds skb, dev_queue_xmit(xaui0).
3. Card RX (wire→host): ptype handler DMA-writes the frame to host RX buffer
   (`memcpy(CVMX_ADD_IO_SEG(card_phys_for(busaddr+rxoff)), l2, len)` — card
   WRITE to host RAM, the fast posted direction), bumps rx_prod.
4. Host RX: read packet straight from its own coherent RAM (no BAR read!). This
   kills the non-posted-PCIe-read bottleneck that caps PIO RX today.
5. Later: use the DPI DMA engine (cvmx-dpi-defs.h) for true async offload +
   NAPI + MSI-X IRQ (card→host doorbell) instead of poll, for 10G line rate.

## Safety gates (must hold)
- No bus-master until `ls .../iommu_group` exists.
- Card only ever forms card_phys from `busaddr` values the host published; clamp
  offset < published size. A bug then faults in the IOMMU, not corrupts RAM.
- Keep PIO path as fallback (module param `dma=0` default).

## Expected win
PIO ceiling today ~400 Mbit/s (host CPU BAR copies). DMA removes host-CPU copies
+ non-posted reads → target multi-Gbit, up to ~10G with DPI+NAPI+IRQ (PCIe x4
gen2 ≈14 Gbit usable).
