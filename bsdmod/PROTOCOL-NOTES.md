# PROTOCOL-NOTES.md - Cavium CN6640 SNIC10E FreeBSD Driver Notes

## 1. BAR2 Layout & Registers (from `docs/HARDWARE.md`, `docs/ARCHITECTURE.md`, `linuxmod/octnic.c`)
- BAR2 size: 64 MiB shared memory window.
- Control structures and ring descriptors mapped directly in BAR2.
- Two independent ports: `oct0` (Port 0) and `oct1` (Port 1).
- Register offsets and control page structures mirror `linuxmod/octnic.c`:
  - Heartbeat register, status/control flags, temperature monitoring (`temp_warn_mc`, `temp_crit_mc`).
  - TX ring and RX ring base pointers, head/tail pointers, phase bit protocol.

## 2. Ring Protocol (Virtio-style with Phase Bit)
- Ring size: 128 slots per ring (TX and RX for each port).
- Phase bit mechanism: each descriptor contains a phase bit that toggles with each wrap-around of the ring, ensuring synchronization between host and card without heavy locking.
- Descriptors carry buffer physical addresses, length, flags (e.g., ring wrap, completion).

## 3. TX & RX Mechanisms
- **TX**: PIO write-combining directly into BAR2 ring/buffers where supported by bus mapping, or standard mapped write.
- **RX**: Card performs DMA into a pre-allocated host RAM pool (`hrx`), managed via busdma (`bus_dma_tag_create`, `bus_dmamap_load`, etc.), delivering packets via `mbuf` clusters to the FreeBSD stack (`ifnet`).

## 4. Reset, Hang Detection & Temperature
- Periodic heartbeat monitoring. If card stops updating heartbeat or hangs, carrier is dropped (`if_set_link_state(ifp, LINK_STATE_DOWN)`), reset sequence is triggered.
- Temperature sensors exposed via `sysctl` nodes (`dev.octnic.0.temp`, etc.) using warn/crit thresholds.

## 5. Host Boot / SBR (`octboot` porting)
- Host initialization requires PCIe Secondary Bus Reset (SBR), BAR restoration, and loading OpenWrt image into card DRAM via BAR2.
- FreeBSD equivalent uses `pciconf`, `devctl`, and `/dev/devctl` or direct device control to perform SBR and map BAR2.

## 6. FreeBSD Kernel API Reference (`style(9)`)
- Network interface: `if_alloc(IFT_ETHER)`, `if_init_name`, `if_set_transmit`, `if_set_ioctl`, `ether_ifattach`, `if_inc_counter`.
- Bus/PCI: `pci_enable_busmaster`, `bus_alloc_resource_any`, `bus_space_read_*`, `bus_space_write_*`.
- DMA: `bus_dma_tag_create`, `bus_dmamap_create`, `bus_dmamap_load`.
- Headers required: `<sys/param.h>`, `<sys/systm.h>`, `<sys/kernel.h>`, `<sys/module.h>`, `<sys/bus.h>`, `<machine/bus.h>`, `<sys/rman.h>`, `<sys/socket.h>`, `<sys/sockio.h>`, `<sys/mbuf.h>`, `<sys/sysctl.h>`, `<net/if.h>`, `<net/if_var.h>`, `<net/ethernet.h>`, `<net/if_types.h>`, `<dev/pci/pcireg.h>`, `<dev/pci/pcivar.h>`, `device_if.h`, `bus_if.h`, `pci_if.h`.
