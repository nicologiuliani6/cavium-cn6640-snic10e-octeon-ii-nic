# cavium-cn6640-snic10e-octeon-ii-nic

**Project page:** [nicologiuliani.site/docs/cavium-cn6640-snic10e](https://nicologiuliani.site/docs/cavium-cn6640-snic10e/) — what it is, quick start, FAQ.

**How it was built:** [the write-up](https://nicologiuliani.site/blog/cavium-cn6640-bar2-datapath/)

**Questions, or testing on your own board?** [Open an issue](https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/issues)
or [a discussion](https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/discussions), or email
[me@nicologiuliani.site](mailto:me@nicologiuliani.site).

**Sponsor:** [github.com/sponsors/nicologiuliani6](https://github.com/sponsors/nicologiuliani6)

Multi-OS driver stack and boot tooling for the **Cavium CN6640-SNIC10E** (**Linux** and **Windows** today, BSD planned)
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
> [PERFORMANCE](docs/PERFORMANCE.md), measured on Linux). The card boots with no serial cable; the
> host brings both NICs up automatically at boot.

| OS | Driver | State |
|---|---|---|
| **Linux** | `linuxmod/` (kernel module, DKMS) | working, hardware-validated |
| **Windows** 10/11 x64 | `winmod/` (NDIS 6.20 miniport, test-signed) | working: `oct0` + `oct1` in Device Manager and Network Connections, DHCP/ping/internet verified through the card, card boots from Windows at every start |
| **BSD** | `bsdmod/` | **not implemented** — the installer cancels there |
## Components

### Host Drivers (per OS)

| Name | Side | Role |
|---|---|---|
| `octnic` (`linuxmod/octnic.c`) | Linux (kmod) | Registers `oct0`/`oct1` over the card's BAR2 window. Auto-discovers the card (`modprobe octnic ports=2`). |
| `octnic` (`winmod/octnic.sys`) | Windows (NDIS) | NDIS 6.20 miniport, same BAR2 shared-memory protocol. Two adapters (port 0 = PCI function, port 1 = companion root device). Prebuilt in releases, see `winmod/README.md`. |
| `octnic` (`bsdmod/`) | BSD (planned) | Placeholder for FreeBSD/OpenBSD/DragonFly ifnet driver. |

### Card Modules (shared across all hosts)

| Name | Side | Role |
|---|---|---|
| `octshm_card` (`cardmod/octshm_card.c`) | card (kmod) | Card end of the shared-memory datapath: per-port rings, DPI RX, XAUI uplink tap. |
| `octcarrier` (`cardmod/octcarrier.c`) | card (kmod) | Un-gates `xaui0`/`xaui1` TX on the QLogic DAC (`cvmx_helper_link_set`). |

### Boot & Orchestration

| Name | Side | Role |
|---|---|---|
| `octboot` | Linux host (bash) | Host **bootloader**: SBR the card, restore BARs, push OpenWrt image into card DRAM over BAR2. No serial. |
| `octboot.ps1` | Windows host (PowerShell) | Windows equivalent: pushes the image through the driver's control device (`\\.\octnic`), SBR fallback, waits for the heartbeat; no-op if the card is already up. |
| `scripts/cavium-up.sh` / `scripts/nic-up.sh` | Linux | Orchestrate full bring-up: boot card, load `octnic`, bring ports up. |

Linux runs entirely hands-off via `system/cavium-nic.service`; Windows via the boot task `octnic-boot` (`winmod/octnic-start.ps1`). Both are no-ops when the card is already running, so a dual-boot machine never has the two OSes fight over the card.

## Install (all operating systems)

One command per OS — the same guided flow, asking for confirmation before every step that changes something:

| OS | Command |
|---|---|
| Linux | `sudo ./install.sh` |
| Windows | `install.bat` (double-click; asks for Administrator) |
| BSD | not implemented yet — the installer **cancels** with a message |

What the installer does:

1. **Is the card on the PCI bus?** (`177d:0092`) — if not, it stops (or asks whether to install the driver anyway).
2. **Installs the driver** for your OS (Linux: `octnic` via DKMS + configs + autostart service; Windows: trusts the test certificate, installs `octnic`, creates the second NIC, names them `oct0`/`oct1`).
3. **Is the card already flashed?** i.e. is its u-boot environment provisioned for host boot (probed over PCIe, the same check `octboot` uses). If yes → no serial cable needed, it carries on.
4. **If not flashed:** looks for the USB-serial (FT232) adapter, tests the serial link (optionally resetting the card to see the u-boot banner), and — only after you confirm — **flashes the u-boot environment** (`saveenv` to NAND, once; reversible with `scripts/restore-bootapp.sh`). If no adapter is found or the link does not work it stops and tells you; the driver stays installed and nothing is written to the card.
5. **Boots the card** and brings up `oct0` + `oct1` (Linux: `cavium-nic.service`; Windows: boot task `octnic-boot`).

`--yes` (Windows: `-Yes`) answers yes to every question. Without a terminal, only the driver steps run unattended; reset/flash steps need `--yes`. Windows needs **Secure Boot off** (test-signing) and one reboot the first time if test-signing was off; on Linux Secure Boot must be off too (kernel lockdown blocks `setpci`/BAR access; "Above 4G decoding" is *not* required). A modern Linux (developed on 6.14, `dkms` recommended) and, only for the one-time flash, a USB-serial (FT232) adapter on the card's console header.

The card's OS image is downloaded by the installers/`octboot` from the latest release (`openwrt-octeon-generic-snic10e-initramfs-kernel.bin`); to build it yourself see [FLASHING §1–2](docs/FLASHING.md).

### Linux, by hand

```bash
git clone https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic
cd cavium-cn6640-snic10e-octeon-ii-nic
curl -LO https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/releases/latest/download/openwrt-octeon-generic-snic10e-initramfs-kernel.bin
sudo ./install.sh                       # driver + service (+ flash, guided)
sudo ./scripts/card-prep-hostboot.sh    # = the flash step on its own (serial cable, once)
sudo systemctl start cavium-nic         # every boot, no serial: pushes the image, brings up oct0 + oct1
ip -br addr show oct0
```

Manual equivalent of the last step: `sudo ./octboot` then `sudo modprobe octnic ports=2`. Building the image yourself: [FLASHING §1–2](docs/FLASHING.md).

### Windows, by hand

See [`winmod/README.md`](winmod/README.md): `octboot.ps1` (boot the card), `octnic-provision.ps1` (the serial flash step), `Get-NetAdapter oct0, oct1`.
Card temperatures and an estimated card power draw show up in plain `sensors` as
`cavium_card` once `octnic` is loaded — see [USAGE](docs/USAGE.md#card-temperature-and-power).

`octnic` also watches the card every 2 s. If the card resets/hangs (e.g. it overheated
because its fan is unplugged) or crosses `temp_crit_mc` (default 100 °C), it logs a
`KERN_EMERG` alert — broadcast to every terminal by journald/rsyslog on any distro —
and drops carrier on oct0/oct1. Warning level: `temp_warn_mc` (85 °C). Both are module
params, writable under `/sys/module/octnic/parameters/`.

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
| Written by the AI, run on the one reference card (the "working" status above): `linuxmod/octnic.c`, `cardmod/octshm_card.c`, `cardmod/octcarrier.c`, `octboot`, `install.sh`, `install.bat`, `scripts/*.sh`, `system/cavium-nic.service` | hardware-validated on this card, never in the field |
| Windows driver: `winmod/octnic.*` — NDIS 6.20 miniport, same BAR2 protocol, two adapters, control device for booting the card | tested on the reference card from Windows (Device Manager OK, DHCP/ping/internet); prebuilt in every release + `install.bat`; the serial flash step (`octnic-provision.ps1`) is a port of the Linux script and untested on Windows hardware |

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
