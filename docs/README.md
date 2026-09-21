# Documentation index

Start at the [project README](../README.md) for what this is and a quick start. These pages
are grouped by what you are trying to do.

## Get it running

| Page | Read it when |
|---|---|
| [FLASHING](FLASHING.md) | Building the modules and the card image, the one-time serial u-boot provisioning, and booting the card with `octboot`. |
| [USAGE](USAGE.md) | Day-to-day operation: the systemd autostart, `octnic` module parameters, temperatures and power in `sensors`, benchmarking, troubleshooting. |

## Reference

| Page | Contents |
|---|---|
| [HARDWARE](HARDWARE.md) | The board itself: SoC, PHY, PCIe BARs, the BAR0 window freeze hazard, serial pinout, XAUI `ipd_port` mapping. |
| [PERFORMANCE](PERFORMANCE.md) | Measured throughput, the tuning that got there, and how to reproduce the numbers. |

## How it works

| Page | Contents |
|---|---|
| [ARCHITECTURE](ARCHITECTURE.md) | The datapath end to end: the BAR2 shared-memory rings, the card and host modules, the boot flow. |
| [DMA-DESIGN](DMA-DESIGN.md) | Why RX is DPI-DMA and TX is PIO, and the constraints behind that split. |
