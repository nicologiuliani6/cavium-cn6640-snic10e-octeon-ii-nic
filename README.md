# cavium-cn6640-snic10e-octeon-ii-nic

Out-of-tree Linux driver stack and boot tooling for the **Cavium CN6640-SNIC10E**
(Octeon II CN6640, PCI `177d:0092`), exposing the card as **two independent 10 GbE
host interfaces** (`oct0`, `oct1`) over a reverse-engineered PCIe BAR2 shared-memory
datapath. No vendor NDA firmware.

> Status: **working.** Both ports link at 10 G on a DAC and pass traffic bidirectionally —
> TX at **line-rate 10 GbE** (9.7–9.8 Gb/s, zero-copy datapath) and RX at **8.1–8.8 Gb/s** (see
> [PERFORMANCE](docs/PERFORMANCE.md)). Card boots with no serial cable; the host brings both
> NICs up automatically at boot.

---

## What this is

The SNIC10E is an OEM Cavium "SmartNIC": an 8-core Octeon II SoC with two SFP+ ports,
meant to run vendor LiquidIO firmware. The stock `liquidio` driver cannot drive this OEM
board (its soft-reset + flash-handshake boot model doesn't fit it), and both PCIe BARs read
back all-`0xFF` out of the box.

That "BAR wall" is **unprogrammed PEM inbound registers**, not a hardware limit. Programming
`PEMX_P2N_BARx_START` / `PEMX_BAR1_INDEXx` from the card side (plain u-boot `write64`, no
firmware) opens a full bidirectional host↔card channel over PCIe.
On top of that channel this repo builds a real NIC:

- the card runs **OpenWrt from RAM** and moves packets between its XAUI ports and a
  shared-memory ring exposed to the host over **BAR2**;
- the host driver **`octnic`** maps that ring and registers `oct0` / `oct1`;
- RX (card→host) is DMA'd by the Octeon **DPI** engine; TX (host→card) is PIO into the
  BAR window. See [ARCHITECTURE](docs/ARCHITECTURE.md).

The card OS is never flashed: it is pushed into DRAM at every boot and gone at power-off. The
one permanent write is the card's u-boot environment (`saveenv` to NAND, once, so the host can
boot it without a serial cable) — `scripts/restore-bootapp.sh` puts the stock env back. The
host side is out-of-tree modules + scripts (see [USAGE → uninstall](docs/USAGE.md#uninstall)).

---

## Components

| Name | Side | Role |
|---|---|---|
| `octboot` | host (bash) | Host **bootloader**: SBR the card, restore BARs, push the OpenWrt image into card DRAM over BAR2, wait for the NIC heartbeat. No serial. |
| `octnic` (`hostmod/octnic.c`) | host (kmod) | Registers `oct0`/`oct1` over the card's BAR2 window. Auto-discovers the card (`modprobe octnic ports=2`). |
| `octshm_card` (`cardmod/`) | card (kmod) | Card end of the shared-memory datapath: per-port rings, DPI RX, XAUI uplink tap. |
| `octcarrier` (`cardmod/`) | card (kmod) | Un-gates `xaui0`/`xaui1` TX on the QLogic DAC (`cvmx_helper_link_set`). |
| `scripts/cavium-up.sh` / `scripts/nic-up.sh` | host | Orchestrate a full bring-up: boot the card, load `octnic`, bring both ports up. |

Run entirely hands-off by the `system/cavium-nic.service` systemd unit.

---

## Quick start

```bash
git clone https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic
cd cavium-cn6640-snic10e-octeon-ii-nic

# 1) the card's OS image, prebuilt — octboot looks for it here in the repo root
curl -LO https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/releases/latest/download/openwrt-octeon-generic-snic10e-initramfs-kernel.bin

# 2) the host side: builds + installs octnic (DKMS), drops the configs, enables autostart
sudo ./install.sh

# 3) first time on this machine only — persist the card's u-boot env (serial cable, once)
sudo ./scripts/card-prep-hostboot.sh

# 4) every boot, no serial: pushes the image over PCIe, brings up oct0 + oct1
sudo systemctl start cavium-nic
ip -br addr show oct0
```

Step 4 is what the `cavium-nic` service runs at every host boot, so after step 3 the card
comes up on its own. Steps 1–3 are once per machine.

**Building the image yourself** instead of step 1 (needed only if you change the card side):
[FLASHING §1–2](docs/FLASHING.md). `octboot` also takes `IMG=<path>` or finds the image in an
OpenWrt build tree under `$HOME`.

Manual equivalent of step 4:

```bash
sudo ./octboot                        # boot the card (no serial)
sudo modprobe octnic ports=2          # oct0 + oct1 appear
# assign IPs / bridge / use like any NIC
```

Card temperatures and an estimated card power draw show up in plain `sensors` as
`cavium_card` once `octnic` is loaded — see [USAGE](docs/USAGE.md#card-temperature-and-power).

Full details: **[docs/](docs/README.md)** — [FLASHING](docs/FLASHING.md) ·
[USAGE](docs/USAGE.md) · [HARDWARE](docs/HARDWARE.md) ·
[ARCHITECTURE](docs/ARCHITECTURE.md) · [PERFORMANCE](docs/PERFORMANCE.md).

---

## Repository layout

```
install.sh          one-shot host installer (module via DKMS + configs + service)
octboot             host bootloader: boots the card over PCIe, no serial
hostmod/            octnic.c — host kernel module (native build, DKMS)
                    power-model-check.py — self-check for the power1 model arithmetic
cardmod/            octshm_card.c, octcarrier.c — card kernel modules (cross-built)
scripts/            cavium-up.sh, nic-up.sh      — bring-up orchestration
                    card-prep-hostboot.sh        — first-time serial u-boot provisioning (once)
                    boot-clean.sh, cexec.sh      — serial fallbacks
                    restore-bootapp.sh           — revert the card to its stock OEM boot
openwrt/            snic10e.config, build-openwrt.sh, files/ — card image build + overlay
                    patches/, hostfix/ — upstream delta + host build shims (GPL source)
system/             cavium-nic.service, blacklist-liquidio.conf, 99-octnic-unmanaged.conf
docs/               see docs/README.md for the index
```

---

## Requirements

- The Cavium CN6640-SNIC10E card in a PCIe slot, with **Secure Boot off** (kernel lockdown
  blocks the `setpci`/BAR access this stack needs). "Above 4G decoding" is *not* required —
  see [HARDWARE → BIOS](docs/HARDWARE.md#bios).
- An SFP+ DAC (or optics) per port to whatever the card is cabled to.
- Host: modern Linux (developed on 6.14) with the matching kernel headers. `dkms` is optional
  but recommended — without it the module has to be rebuilt by hand after each kernel upgrade
  (`NODKMS=1` forces that path).
- The card image: prebuilt in the [releases](https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/releases),
  or an OpenWrt build tree to build it yourself.
- A USB-serial (FT232) adapter for the **one-time** u-boot provisioning only; normal
  operation is serial-free.

---

## Credits

- **OpenWrt SNIC10E support** — the released card image is built from
  [stintel/openwrt](https://codeberg.org/stintel/openwrt.git) branch `snic10e-5.10`
  (commit `7bbf4b7`), which brings up the board and its XAUI Ethernet; earlier work used
  [hurricos/openwrt @ `snic10e-ethernet`](https://git.laboratoryb.org/hurricos/openwrt/src/branch/snic10e-ethernet).
  This project runs that image on the card and builds the host datapath on top of it.
- Cavium/Marvell Octeon SDK (`cvmx_*` helpers) and the in-tree `liquidio` driver, used as
  reverse-engineering references for the SLI/DPI/PEM register layout.

## License

GPL-2.0 — see [LICENSE](LICENSE). The kernel modules carry SPDX headers; scripts and docs are
under the same license unless noted.

The card image attached to the [releases](https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/releases)
is a GPL-2.0 binary (OpenWrt + Linux). Its complete corresponding source is: upstream
[stintel/openwrt](https://codeberg.org/stintel/openwrt.git) at commit `7bbf4b7` (branch
`snic10e-5.10`), plus `openwrt/patches/` (the one local change to that tree),
`openwrt/snic10e.config` (the build config), `openwrt/files/` (the root overlay),
`cardmod/` (the card modules baked into it) and `openwrt/build-openwrt.sh` (the script that
builds it — `CLONE=1` clones and checks out the pinned commit for you).

## Disclaimer

Reverse-engineered and unofficial. This stack power-cycles and reprograms an OEM card over
PCIe, and a wedged card can hang the host under heavy load (see
[USAGE → troubleshooting](docs/USAGE.md#troubleshooting)) — recovery is a host reboot.
