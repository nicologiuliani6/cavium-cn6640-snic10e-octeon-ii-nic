# Cavium CN6640-SNIC10E — revival log

Full log of diagnosing a Cavium Octeon II SFP+ card and trying to make it a working
10G NIC under Linux. Honest record: what worked, what didn't, and why.

---

## The card

| | |
|---|---|
| **Model** (back sticker) | `CN6640-SNIC10E-1.1-G` |
| **Board id** (serial console) | `SNIC10E rev major:4 minor:1`, serial `4.1G1634-GB003886` |
| **SoC** | Cavium Octeon II **CN6640** — 8-core MIPS64 @800 MHz (pass 1.2) |
| **PCI ID** | `177d:0092`, subsystem `177d:0001` |
| **RAM** | 2 GiB DDR3 (667 MHz / 1334 DDR) |
| **Storage** | 8 MiB NOR flash + 1 GiB NAND |
| **PHY** | Vitesse VSC8488 (dual 10G) |
| **Ports** | 2× SFP+ (XAUI interfaces 0 and 1) |
| **PCIe** | Gen2 x4, sits on host bridge `00:03.0`, endpoint `04:00.0` |

---

## Goal

Make it a 100%-working 10G SFP+ NIC: the `liquidio` driver loads on the host,
`ethX` interfaces appear, plug an SFP+/DAC and use it.

---

## TL;DR verdict

- The card is **healthy**: genuine Cavium u-boot in NOR, PCIe trains 5GT/s x4, the
  genuine LiquidIO NIC firmware **boots and initialises** on it.
- The stock `liquidio` driver still can't drive it (its soft-reset + flash-handshake
  boot model doesn't fit this OEM card), **but the "all-`0xFF` BAR wall" that was thought
  to make a host NIC architecturally impossible turned out to be FALSE — it was just
  unconfigured PEM inbound registers, and it has now been BROKEN.** See
  "★ BAR WALL BROKEN" below: the host can now read+write card DRAM (BAR2) and read the
  SLI CSRs (BAR0), a full bidirectional host↔card channel — the foundation for a real
  host NIC. The earlier "both BARs read `0xFF`" was because the OEM boot-app never
  programmed `PEMX_P2N_BARx_START` / `PEMX_BAR1_INDEXx`; these are writable from the card
  side via u-boot `write64`, no NDA firmware needed.
- Working outcome achieved: **OpenWrt (snic10e port) runs on the card as a standalone
  2-port 10G device — both SFP+ ports link at 10G Full duplex and pass traffic** over a
  DAC loopback (ping 0% loss, iperf3 971 Mbit/s single-stream, CPU-bound not link-bound).
  See "OpenWrt on the card — WORKING 10G SFP+" below. Host OS untouched, fully reversible
  (runs from RAM). Reachable over the SFP+ network path, not over PCIe.

---

## How it was diagnosed (the journey)

### 1. Software-only revival is impossible from the host
The Linux `liquidio` driver fails every probe:
```
LiquidIO 0000:04:00.0: Board not responding        (early attempts)
LiquidIO 0000:04:00.0: Soft reset failed           (later, consistently)
probe ... failed with error -12
```
With Secure Boot off and lockdown off, host BAR0 mmap reads return all-`0xFF`.
Nothing on the host side fixes this — it needs on-card work (serial).

### 2. Serial console (the breakthrough)
- Adapter: **FT232RL USB-TTL set to 3.3 V** → `/dev/ttyUSB0`, **115200 8N1**.
- Wired to header **J1** (6-pin). Pins found by a zero-risk scan (listen-only),
  automated in `probe.sh` / `find-rx-loop.sh`:
  - **card TX** = the J1 pin that streams boot text into our RX.
  - **card RX** = the adjacent pin (found by hammering keys until autoboot stops).
  - GND = PCIe metal bracket. **Never** connect the adapter's VCC.
- Card reset from Linux without touching hardware: **PCIe Secondary Bus Reset** on the
  parent bridge — `setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep .12; BRIDGE_CONTROL=00:40`.

### 3. The NOR holds a genuine Cavium u-boot
```
U-Boot 2012.04.01  (Octeon SDK 3.0.0-p1 build 495, Mar 06 2014)
bootcmd  = chpart boot-app; fsload boot-app; bootoct $(loadaddr) numcores=8
bootdelay= 10
mtdparts = 3m(bootloaders)ro, 4m(boot-app), 1m(boot-env)ro
stdin    = serial,pci,bootcmd       # u-boot also listens on a PCI command channel
```
Full env saved in **`printenv-card.txt`**. So the card was never "bricked" — it
autoboots an OEM Simple-Executive app (`boot-app`, brings up XAUI) that does **not**
do the LiquidIO host handshake → "Board not responding".

### 4. The intended liquidio host-boot flow (from the kernel source)
Read from `drivers/net/ethernet/cavium/liquidio` (kernel 6.14):
`octeon_device_init` → `soft_reset` → wait DDR (needs module param `ddr_timeout`!) →
`octeon_wait_for_bootloader` (u-boot must set a PCI buffer owner = HOST) → send a
bootcmd over the PCI console → **download `lio_210sv_nic.bin` over PCIe** → boot it.
CN66XX always soft-resets + downloads (the "firmware preloaded" skip is CN23XX-only).

### 5. Where it hits the wall
- The host **soft reset does not work**: `lio_cn6xxx_soft_reset` writes
  `CIU_SOFT_RST` via a PCIe window; serial proves the Octeon never reboots → driver
  bails "Soft reset failed".
- We bypassed that: **booted the genuine NIC firmware locally** over serial
  (`loady` ymodem + `bootoct`, see `load-fw.sh`). Firmware ran and initialised the
  host PCIe path (`Interface 2 ... NPI`, `PCIPKOMAP`, `DMA Queues 0-7 initialized`).
- Built a **patched `liquidio`** (module param `preloaded=1` → skip
  reset/DDR/bootloader/download, attach to the already-running firmware). Built
  cleanly against the running kernel.
- On load it **oops'd** in `lio_cn6xxx_setup_pcie_mrrs`: the driver read
  `SLI_MAC_NUMBER` (BAR0 off `0x3E00`) = `0xFF` → `pcie_port=0xFF` →
  `SLI_S2M_PORTX_CTL(0xFF)` = offset `0x4D70` > the 16K BAR0 → out-of-map fault.
- Clean re-test (fresh reboot, fresh firmware, before any driver): **both BARs read
  all-`0xFF`** (BAR0 16K and BAR1 64M). The host literally cannot read the card.

**Root cause:** the SLI **inbound BAR windows** (BAR0→CSR, BAR1→DRAM) are never set up
for host access. Driver would set them by writing SLI CSRs through BAR0 — but BAR0 is
itself `0xFF` (chicken-and-egg), and the soft reset that would initialise it doesn't
work. Both symptoms, one root: this OEM card's on-card software never configures the
EP for host-driven LiquidIO. Only the genuine Marvell LiquidIO bootloader/firmware
does, and it's not available.

This matches the community verdict (OpenWrt forum thread 88461): secondhand SNIC10E
boards "never work as a liquidio host NIC".

---

## What is proven / what works

```
[ok ] serial console            J1, 115200 8N1, both directions
[ok ] genuine Cavium u-boot      healthy NOR, full env captured
[ok ] card reset from Linux      PCIe SBR on bridge 00:03.0
[ok ] NIC firmware boots         lio_210sv_nic.bin runs locally (bootoct)
[ok ] patched liquidio builds    preloaded=1, correct, builds against 6.14 headers
[NO ] host reads card over PCIe  BOTH BARs = 0xFF  <-- the hard wall
[NO ] host NIC (ethX)            blocked by the above (needs Marvell LiquidIO stack)
```

---

## Scripts (all in `~/Desktop/cavium/`)

| script | what it does |
|---|---|
| `probe.sh` | continuous listen-only scan to find the card-TX pin (no keypress) |
| `find-rx-loop.sh` | loop: reset + hammer keys until autoboot stops = found card-RX pin |
| `run-cmd.sh "<cmd>"` | reliable: reset, stop autoboot, run one u-boot command, dump output |
| `check-links.sh` | verify both serial directions (RX read / TX write) live |
| `set-hostboot.sh` | set `bootdelay=-1` + clear `bootcmd` + saveenv (host-boot ready) |
| `restore-bootapp.sh` | revert env to stock (`bootdelay=10` + original bootcmd) |
| `load-fw.sh` | ymodem (`loady`) the NIC ELF into RAM + `bootoct` it |
| `read-bar0.sh` | read BAR0 SLI registers from the host (the FF test) |
| `printenv-card.txt` | the card's full u-boot environment |
| `lio-build/` | patched liquidio source + built `.ko` (preloaded param) |

Firmware: `/srv/tftp/lio_210sv.elf` (ELF extracted from `lio_210sv_nic.bin`, offset 1312).

---

## Reproduce the key facts

```bash
# get a u-boot prompt and read the environment
sudo sh ~/Desktop/cavium/run-cmd.sh "printenv"

# boot the genuine NIC firmware on the card (serial, ~2 min)
sudo sh ~/Desktop/cavium/load-fw.sh

# prove the host cannot read the card (both BARs 0xFF)
sudo sh ~/Desktop/cavium/read-bar0.sh
```

---

## OpenWrt on the card — WORKING 10G SFP+ ✅

Built and booted the **`snic10e` OpenWrt port** (Stintel `snic10e-5.10`, kernel 5.10.149,
with the snic10e DTS + QLM/SerDes + Vitesse VSC848X PHY init) on the card, from RAM over
serial — no flashing, fully reversible, host OS untouched. **Both 10G SFP+ ports link and
pass traffic.**

```
OpenWrt SNAPSHOT, r0-7bbf4b7                         → full root shell on serial console
Vitesse VSC848X ...:00:00 configured for 10G Passive Copper   ← PHY trains the DAC
xaui0: 10000 Mbps Full duplex, port 0, queue 0      ← SFP+ port 0 UP @ 10G
Vitesse VSC848X ...:00:01 configured for 10G Passive Copper
xaui1: 10000 Mbps Full duplex, port 16, queue 16    ← SFP+ port 1 UP @ 10G
```

### Test — port0 ↔ port1 DAC loopback
A 10G DAC connects the two SFP+ cages. To force traffic across the wire (both ports on one
host would otherwise short-circuit in-kernel), **xaui1 is pushed into a netns**; xaui0 stays
in the root ns. Then ping + iperf3 across the DAC (`owrt-test.sh`, `owrt-perf2.sh`):

```
ethtool xaui0:  Speed 10000Mb/s, Link detected: yes, 10000baseR_FEC
ping:           4/4, 0% loss, rtt avg 0.148 ms
TCP single:     971 Mbit/s   (clean, 1 MB cwnd)
TCP -P8:        515 Mbit/s   (worse — 454 retr, CPU-starved)
UDP @10G offer: 341 Mbit/s,  0/294210 lost (0%)
CPU:            Cavium Octeon II V0.2, 8 cores
```

**The link is genuine 10G** (PHY trains at 10G, ethtool reports 10 Gb/s, 0% UDP loss). The
~1 Gbit/s throughput ceiling is **not** the wire or PHY — it is the SoC doing *both* ends of
the loopback in software on one box (iperf3 client **and** server, TX **and** RX, on the same
8 MIPS cores). Parallel streams make it worse (CPU thrash), confirming CPU-bound, not
link-bound. A real external 10G peer (one direction only) would go higher; note the Octeon
Linux staging datapath still won't hit line-rate 10G without the cvmx hardware fast-path.

### How it was built (reusable)
Source: Stintel `snic10e-5.10` fork in `~/openwrt`. Config seed `snic10e.config`
(`CONFIG_TARGET_octeon_generic_DEVICE_snic10e=y`, `ROOTFS_INITRAMFS=y`, dropbear + iperf3 +
ethtool; **LuCI dropped** — its feed HEAD needs cmake 3.31, host has 3.24.2). Built via
`build-openwrt.sh` on Ubuntu 25.04, which needed several host-toolchain shims:
- gcc-14 default-errors → `host-ccwrap/{gcc,cc,g++,c++}` wrappers, **repointed the
  `staging_dir/host/bin/*` symlinks at them** (OpenWrt invokes those, not `$PATH`).
- Python 3.13 dropped `pipes`/`distutils` → `host-pyshim/pipes.py` + `PYTHONPATH`.
- `tools/elfutils/Makefile`: `HOST_CFLAGS += -Wno-error`.
- **Kernel patch `710-netdev-phy-of-Handle-nexus...` fixed**: 5.10.149 reordered the
  `of_mdio.c` includes, so its hunk-1 context (`module.h`) no longer matched — rewrote the
  hunk to anchor on `phy.h`/`phy_fixed.h` and corrected the `@@` line counts. This patch
  (with 711 vsc848x) is what makes the SFP+ PHYs train.

Output: `bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin` (21 MB).

### How it was booted (reusable)
`openwrt-boot.sh`: `loady 0x20000000` (ymodem at **115200**, no baud arg — a baud arg leaves
u-boot stuck at the high rate), `sb -q <image>`, then
`bootoctlinux 0x20000000 numcores=8 endbootargs console=ttyS0,115200`. Runs from initramfs
(RAM). 21 MB transfer ≈ 30 min at 115200. **Interfaces are reachable only over the SFP+
network path, not over PCIe** (the BAR wall above stands).

### Persistence (optional, next)
Everything so far runs from RAM (reboot = clean, nothing on the card changed). To make it
permanent, flash the squashfs sysupgrade to the 1 GiB NAND from the booted OpenWrt. Deferred
— not done here to keep the card's original OEM boot-app intact and the setup reversible.

## ★ BAR WALL BROKEN — host↔card PCIe channel works ✅

The whole project's central "impossible" verdict — *both PCI BARs read all-`0xFF`, so the
host can never reach the chip* — was **wrong**. It was not a silicon lock; the OEM boot-app
simply **never programmed the PEM (PCIe MAC) inbound address windows**. They are writable
from the card side, and once programmed the host reads and writes the card freely.

### The mechanism
The genuine Cavium u-boot has `read64`/`write64` (+ `md64`/`mw64`) — read/write any 64-bit
CSR by physical address. Reading the PEM inbound registers from the card side showed the
smoking gun:

```
PEMX_P2N_BAR0_START (0x00011800C0000080) = 0   ← BAR0 inbound base unset
PEMX_P2N_BAR1_START (0x00011800C0000088) = 0   ← BAR1(64M) inbound base unset
PEMX_BAR1_INDEX0    (0x00011800C00000A8) = 0   ← window→DRAM map disabled
```

With those zero, the PEM matches no host memory access → every read returns `0xFF`. The fix
is three card-side writes plus restoring the host-side config BARs (an SBR reset wipes both
the PEM regs and the EP config BARs, so program-then-read must not re-reset — use
`send-cmd.sh`, which drives the already-at-prompt card without an SBR):

```
# card side (u-boot), 64MB DRAM window = BAR2/resource2:
write64 0x00011800C0000088 0xF4000000       # P2N_BAR1_START = host BAR2 base (64M-aligned)
write64 0x0000000002000000 0xCAFEBABEDEADBEEF# plant a magic in card DRAM
write64 0x00011800C00000A8 0x8B             # BAR1_INDEX0: off0→DRAM 0x02000000, enable
                                            #   0x8B = ((0x02000000>>22)<<4) | 0xB
                                            #   0xB = CA<<3 | 64BIT_SWAP<<1 | VALID
# card side, 16KB SLI-CSR window = BAR0/resource0:
write64 0x00011800C0000080 0xF8000000       # P2N_BAR0_START = host BAR0 base

# host side (SBR reset the EP config BARs to 0 → restore + enable mem decode):
setpci -s 04:00.0 BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s 04:00.0 BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s 04:00.0 COMMAND=0x02
```

### Proven results
```
card→host  (BAR2): host mmap resource2 off0  = 0xCAFEBABEDEADBEEF  ← the planted magic
host→card  (BAR2): host writes 0x1122334455667788 → card read64 0x02000100 reads it back
host→CSR   (BAR0): host mmap resource0 reads SLI_CTL_STATUS@0x570=0x82002,
                   MAC_NUMBER@0x3E00=0, S2M_PORT0@0x3D80=0x2  (all match card-side reads)
```

A **full bidirectional host↔card shared-memory channel** (64 MB DRAM window) plus host access
to the SLI control registers. That is exactly the primitive set a PCIe NIC is built on:
put TX/RX descriptor rings + buffers in the DRAM window, doorbell via a CSR, and a card-side
program shuttles frames between the rings and `xaui0/1`.

### Scripts
`recon-csr.sh`, `recon-pem.sh` (read-only CSR dumps), `exp-bar1b-card.sh` (BAR1 bring-up),
`send-cmd.sh` (no-reset u-boot sender — preserves PEM state between steps), `read-bar2.py` /
`host-write-bar2.py` (host mmap of the window). `read64`/`write64` are the poke tool — no
cross-compile needed.

### What's left for a full `ethX` NIC (the remaining hard part)
1. **Persist the window setup without serial.** An SBR/boot re-zeroes the PEM regs and the
   octeon Linux PCIe init would re-touch the SLI, so the setup must be done by whatever owns
   the datapath last — i.e. a **card-side kernel module** (uses `cvmx_write_csr`) once the
   card's OpenWrt is up, not u-boot.
2. **Ring protocol** in the DRAM window (TX host→card, RX card→host, head/tail, doorbell).
3. **Card-side module**: program the windows, poll TX ring → transmit on `xaui0`; receive on
   `xaui0` → RX ring → signal host (MSI or host-poll).
4. **Host driver**: a netdev that maps BAR2/BAR0, moves skbs through the rings, presents
   `ethX`.

Items 2–4 are essentially writing an `ivshmem-net`-style driver pair for this card — long,
but no longer blocked by anything architectural. The wall that made it impossible is gone.
