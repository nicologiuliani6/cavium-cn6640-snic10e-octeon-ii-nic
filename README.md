# cavium-cn6640-snic10e-octeon-ii-nic

Out-of-tree Linux driver stack and boot tooling for the **Cavium CN6640-SNIC10E**
(Octeon II CN6640, PCI `177d:0092`), exposing the card as **two independent 10 GbE
host interfaces** (`oct0`, `oct1`) over a reverse-engineered PCIe BAR2 shared-memory
datapath. No vendor NDA firmware. Host OS untouched, fully reversible.

> Status: **working.** Both ports link at 10 G on a DAC and pass traffic bidirectionally —
> TX at **line-rate 10 GbE** (`oct0` FWD 9.71 Gb/s, zero-copy datapath), RX ~7.5 Gb/s (see
> [PERFORMANCE](docs/PERFORMANCE.md)). Card boots with no serial cable; the host brings both
> NICs up automatically at boot.

---

## What this is

The SNIC10E is an OEM Cavium "SmartNIC": an 8-core Octeon II SoC with two SFP+ ports,
meant to run vendor LiquidIO firmware. The stock `liquidio` driver cannot drive this OEM
board (its soft-reset + flash-handshake boot model doesn't fit it), and for a long time
the card looked architecturally unusable as a plain host NIC because both PCIe BARs read
back all-`0xFF`.

That "BAR wall" turned out to be **unprogrammed PEM inbound registers**, not a hardware
limit. Programming `PEMX_P2N_BARx_START` / `PEMX_BAR1_INDEXx` from the card side (plain
u-boot `write64`, no firmware) opens a full bidirectional host↔card channel over PCIe.
On top of that channel this repo builds a real NIC:

- the card runs **OpenWrt from RAM** and moves packets between its XAUI ports and a
  shared-memory ring exposed to the host over **BAR2**;
- the host driver **`octnic`** maps that ring and registers `oct0` / `oct1`;
- RX (card→host) is DMA'd by the Octeon **DPI** engine; TX (host→card) is PIO into the
  BAR window. See [ARCHITECTURE](docs/ARCHITECTURE.md).

Everything is reversible: the card runs entirely from RAM (nothing is flashed for the NIC
role), and the host side is out-of-tree modules + scripts.

---

## Components

| Name | Side | Role |
|---|---|---|
| `octboot` | host (bash) | Host **bootloader**: SBR the card, restore BARs, push the OpenWrt image into card DRAM over BAR2, wait for the NIC heartbeat. No serial. |
| `octnic` (`hostmod/octnic.c`) | host (kmod) | Registers `oct0`/`oct1` over the card's BAR2 window. Auto-discovers the card (`modprobe octnic ports=2`). |
| `octshm_card` (`cardmod/`) | card (kmod) | Card end of the shared-memory datapath: per-port rings, DPI RX, XAUI uplink tap. |
| `octcarrier` (`cardmod/`) | card (kmod) | Un-gates `xaui0`/`xaui1` TX on the QLogic DAC (`cvmx_helper_link_set`). |
| `cavium-up.sh` / `twocard-up.sh` | host | Orchestrate a full bring-up: boot the card, load `octnic`, wire up both ports. |

Run entirely hands-off by the `cavium-nic.service` systemd unit (see `system/`).

---

## Quick start

Assuming the card is already provisioned (one-time u-boot env, see [FLASHING](docs/FLASHING.md))
and the modules are built + installed:

```bash
sudo systemctl start cavium-nic      # boots the card + brings up oct0 and oct1
ip -br addr show oct0                 # 10 GbE host interface, port 0
ip -br addr show oct1                 # 10 GbE host interface, port 1
```

Manual equivalent:

```bash
sudo ./octboot                        # boot the card (no serial)
sudo modprobe octnic ports=2          # oct0 + oct1 appear
# assign IPs / bridge / use like any NIC
```

Full details: [FLASHING](docs/FLASHING.md) · [USAGE](docs/USAGE.md) ·
[HARDWARE](docs/HARDWARE.md) · [ARCHITECTURE](docs/ARCHITECTURE.md) ·
[PERFORMANCE](docs/PERFORMANCE.md).

---

## Repository layout

```
cardmod/     octshm_card.c, octcarrier.c   — card-side kernel modules (cross-built)
hostmod/     octnic.c                        — host-side kernel module (native build)
octboot                                      — host bootloader (no serial)
cavium-up.sh, twocard-up.sh, cexec.sh        — bring-up orchestration (+ serial fallback)
card-temp.sh                                 — card temperature feed
set-hostboot.sh, boot-clean.sh, restore-*.sh — one-time provisioning / fallbacks
openwrt/     snic10e.config, build-openwrt.sh, files/  — OpenWrt image build + overlay
system/      cavium-nic.service, blacklist-liquidio.conf, 99-octnic-unmanaged.conf
docs/        HARDWARE, FLASHING, USAGE, ARCHITECTURE, PERFORMANCE, DMA-DESIGN
```

The R&D history (experiments, logs, snapshots, the reverse-engineering reference tree)
lives outside the repo and is not published.

---

## Requirements

- The Cavium CN6640-SNIC10E card in a PCIe slot, **BIOS "Above 4G decoding" enabled**.
- A second 10 GbE peer for real traffic (another NIC + DAC per port; this repo uses an
  HP NC523 as the loopback peer).
- Host: modern Linux (developed on 6.14), an OpenWrt build tree for the card image.
- A USB-serial (FT232) adapter for the **one-time** u-boot provisioning only; normal
  operation is serial-free.

---

## Credits

- **[hurricos/openwrt @ `snic10e-ethernet`](https://git.laboratoryb.org/hurricos/openwrt/src/branch/snic10e-ethernet)**
  — the OpenWrt port that brings up the SNIC10E and its XAUI Ethernet; this project runs
  that image on the card and builds the host datapath on top of it.
- Cavium/Marvell Octeon SDK (`cvmx_*` helpers) and the in-tree `liquidio` driver, used as
  reverse-engineering references for the SLI/DPI/PEM register layout.

## License

GPL-2.0 for the kernel modules (see SPDX headers). Scripts and docs under the same repo
license unless noted.

## Disclaimer

Reverse-engineered, unofficial, no warranty. It power-cycles and reprograms an OEM card
over PCIe; a wedged card can hang the host under heavy load (see
[USAGE → troubleshooting](docs/USAGE.md#troubleshooting)). Use on hardware you can afford
to reset.
