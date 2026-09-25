# cavium-cn6640-snic10e-octeon-ii-nic

**Project page:** [nicologiuliani.site/docs/cavium-cn6640-snic10e](https://nicologiuliani.site/docs/cavium-cn6640-snic10e/) — what it is, quick start, FAQ.

**How it was built:** [the write-up](https://nicologiuliani.site/blog/cavium-cn6640-bar2-datapath/)

**Questions, or testing on your own board?** [Open an issue](https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/issues)
or [a discussion](https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/discussions), or email
[me@nicologiuliani.site](mailto:me@nicologiuliani.site).

**Sponsor:** [github.com/sponsors/nicologiuliani6](https://github.com/sponsors/nicologiuliani6)

Out-of-tree Linux driver stack and boot tooling for the **Cavium CN6640-SNIC10E**
(Octeon II CN6640, PCI `177d:0092`), exposing the card as **two independent 10 GbE
host interfaces** (`oct0`, `oct1`) over a reverse-engineered PCIe BAR2 shared-memory
datapath. No vendor NDA firmware.

The SNIC10E is an OEM Cavium "SmartNIC": an 8-core Octeon II SoC with two SFP+ ports,
meant to run vendor LiquidIO firmware. The stock `liquidio` driver cannot drive this OEM
board, and both PCIe BARs read back all-`0xFF` out of the box — that "BAR wall" is
**unprogrammed PEM inbound registers**, not a hardware limit. Programming
`PEMX_P2N_BARx_START` / `PEMX_BAR1_INDEXx` from the card side (plain u-boot `write64`, no
firmware) opens a full bidirectional host↔card channel over PCIe, and on top of that this
repo builds a real NIC: the card runs **OpenWrt from RAM** and moves packets between its
XAUI ports and a shared-memory ring exposed to the host over **BAR2**; the host driver
**`octnic`** maps that ring and registers `oct0` / `oct1`; RX (card→host) is DMA'd by the
Octeon **DPI** engine, TX (host→card) is PIO into the BAR window. See
[ARCHITECTURE](docs/ARCHITECTURE.md).

The card OS is never flashed: it is pushed into DRAM at every boot and gone at power-off.
The one permanent write is the card's u-boot environment (`saveenv` to NAND, once, so the
host can boot it without a serial cable) — `scripts/restore-bootapp.sh` puts the stock env
back.

> Status: **working.** Both ports link at 10 G on a DAC and pass traffic bidirectionally —
> TX at **line-rate 10 GbE** (9.7–9.8 Gb/s, zero-copy datapath) and RX at **8.1–8.8 Gb/s** (see
> [PERFORMANCE](docs/PERFORMANCE.md)). Card boots with no serial cable; the host brings both
> NICs up automatically at boot.

## Components

| Name | Side | Role |
|---|---|---|
| `octboot` | host (bash) | Host **bootloader**: SBR the card, restore BARs, push the OpenWrt image into card DRAM over BAR2, wait for the NIC heartbeat. No serial. |
| `octnic` (`hostmod/octnic.c`) | host (kmod) | Registers `oct0`/`oct1` over the card's BAR2 window. Auto-discovers the card (`modprobe octnic ports=2`). |
| `octshm_card` (`cardmod/`) | card (kmod) | Card end of the shared-memory datapath: per-port rings, DPI RX, XAUI uplink tap. |
| `octcarrier` (`cardmod/`) | card (kmod) | Un-gates `xaui0`/`xaui1` TX on the QLogic DAC (`cvmx_helper_link_set`). |
| `scripts/cavium-up.sh` / `scripts/nic-up.sh` | host | Orchestrate a full bring-up: boot the card, load `octnic`, bring both ports up. |

Run entirely hands-off by the `system/cavium-nic.service` systemd unit.

## Quick start

You need the card in a PCIe slot with **Secure Boot off** (kernel lockdown blocks the
`setpci`/BAR access this needs — "Above 4G decoding" is *not* required), a modern Linux
host (developed on 6.14, `dkms` recommended), and a USB-serial (FT232) adapter for the
**one-time** u-boot provisioning only — normal operation is serial-free.

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

## Not working or untested

- **Dual-port simultaneous load**: both ports work bidirectionally, but the card wedges
  under sustained dual-port traffic — fresh boot needed per test run.
- **FWD path (BAR2 PIO outbound) plateaus at ~3.1 Gb/s**: full 10G that direction needs
  inbound-DPI/`ztx`, which wedges the card — not solved.
- **`openwrt/build-openwrt.sh`** (from-scratch image rebuild): compiles, but isn't the exact
  path the shipped release image was verified against.
- **A wedged card needs a host reboot to recover** — no soft reset clears its RAM. Don't run
  this on a host you can't afford to reboot; see [USAGE → troubleshooting](docs/USAGE.md#troubleshooting).

## Documentation

[docs/](docs/README.md) · [FLASHING](docs/FLASHING.md) · [USAGE](docs/USAGE.md) ·
[HARDWARE](docs/HARDWARE.md) · [ARCHITECTURE](docs/ARCHITECTURE.md) ·
[DMA-DESIGN](docs/DMA-DESIGN.md) · [PERFORMANCE](docs/PERFORMANCE.md)

## Who did what

**Human (owner):** owns the card and the host it sits in; all physical work (seating the
card in a PCIe slot, wiring the FT232 serial adapter for the one-time u-boot provisioning,
power-cycling the card); chose the goals (drop vendor LiquidIO firmware, no NDA blobs, both
ports at line-rate 10G, serial-free boot); authorised every register write and load test the
AI ran on the card; called the runtime tests that ruled out dead-end theories (L2C
way-partitioning, MPS tuning) instead of accepting the first plausible explanation. Host
reboots are off-limits by owner directive — a wedged card is documented and left for a
non-destructive recovery, never "just reboot the host."

**AI (Claude):** everything else: reverse-engineering the PEM/BAR/DPI/SLI register layout
from BAR0 reads (no vendor SDK, no NDA firmware), the `octnic`/`octshm_card`/`octcarrier`
kernel modules, `octboot`, the bring-up and recovery scripts, the throughput tuning (zero-copy
TX, multi-core RX via POW-group IRQ spreading, the BAR2 profiling counters), and the
documentation. Ran on the one reference card under the owner's standing permission.

## Code provenance

| | Status |
|---|---|
| Unmodified upstream: OpenWrt ([stintel/openwrt](https://codeberg.org/stintel/openwrt.git) `snic10e-5.10`, commit `7bbf4b7`; earlier work [hurricos/openwrt @ `snic10e-ethernet`](https://git.laboratoryb.org/hurricos/openwrt/src/branch/snic10e-ethernet)), Linux mainline | widely used and tested by others |
| Cavium/Marvell Octeon SDK (`cvmx_*`) and the in-tree `liquidio` driver | reverse-engineering *reference* only — no code copied, no NDA firmware run |
| Written by the AI, run on the one reference card (the "working" status above): `hostmod/octnic.c`, `cardmod/octshm_card.c`, `cardmod/octcarrier.c`, `octboot`, `install.sh`, `scripts/*.sh`, `system/cavium-nic.service` | hardware-validated on this card, never in the field |

GPL-2.0 — see [LICENSE](LICENSE). The card image in the [releases](https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/releases)
is a GPL-2.0 binary (OpenWrt + Linux); its complete corresponding source is upstream
`stintel/openwrt` at commit `7bbf4b7` plus `openwrt/patches/`, `openwrt/snic10e.config`,
`openwrt/files/`, `cardmod/` (baked in) and `openwrt/build-openwrt.sh` (`CLONE=1` clones and
checks out the pinned commit for you).

## AI usage

Model: Claude, through Claude Code, across a multi-day, multi-session reverse-engineering and
driver-development effort plus several helper agents. Token count and cost were not tracked
precisely: the sessions processed on the order of tens of millions of tokens, mostly cached
context re-reads (an estimate, not a measurement). Exact figures: the owner's Claude usage
page.
