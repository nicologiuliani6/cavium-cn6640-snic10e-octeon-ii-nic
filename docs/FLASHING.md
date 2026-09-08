# Flashing & booting the card

One thing **is** written to the card permanently: the u-boot environment, `saveenv`'d into
NAND once by `scripts/card-prep-hostboot.sh` (§3), so the card can be booted from the host
with no serial cable. It replaces the stock boot-app autoboot env;
`scripts/restore-bootapp.sh` writes the stock one back.

Nothing else is flashed — no bootloader, no firmware, no rootfs. The OS the NIC runs is
pushed into DRAM on every boot and disappears at power-off: `octboot` pushes the image over
PCIe and the card runs OpenWrt **from RAM**.

```
 (once)  serial → persist u-boot env  ─────────────┐
 (each)  octboot → SBR → push image via BAR2 → card runs OpenWrt from RAM → heartbeat
```

Two ways to get the pieces:

- **Prebuilt** — download the card image from a
  [release](https://github.com/nicologiuliani6/cavium-cn6640-snic10e-octeon-ii-nic/releases)
  into the repo root and run `sudo ./install.sh` for the host module. Skip to §3.
- **From source** — build the host module, the card modules and the OpenWrt image yourself:
  §1 and §2 below. Needed only if you change the card side.

## 1. Build the modules (from source)

Host module — `install.sh` does this for you (with DKMS when available); by hand, against the
running kernel:

```bash
cd hostmod && make            # -> octnic.ko
sudo install -D -m0644 octnic.ko /lib/modules/$(uname -r)/extra/octnic.ko
sudo depmod -a                # enables `modprobe octnic`
```

Card modules (cross-built against the OpenWrt Octeon kernel). Point `KDIR`/toolchain at
your OpenWrt build tree:

```bash
KDIR=<openwrt>/build_dir/target-mips64_octeonplus_64_musl/linux-octeon_generic/linux-<ver>
TC=<openwrt>/staging_dir/toolchain-mips64_octeonplus_64_gcc-*_musl
export PATH=$TC/bin:$PATH STAGING_DIR=<openwrt>/staging_dir
cd cardmod
make -C $KDIR M=$PWD ARCH=mips CROSS_COMPILE=mips64-openwrt-linux-musl- \
     octshm_card.ko octcarrier.ko
```

## 2. Build the OpenWrt image (from source)

The prebuilt image in the releases is this same build; `octboot` takes whichever it finds
(repo root, an OpenWrt build tree under `$HOME`, or `IMG=<path>`).

The card image is the OpenWrt SNIC10E port —
[stintel/openwrt](https://codeberg.org/stintel/openwrt.git), branch `snic10e-5.10`, commit
`7bbf4b7`, which is what the released image was built from — plus `openwrt/patches/` (one
local change to that tree) and this repo's overlay (`openwrt/files/`), which bakes in the card
modules and an `rc.local` that auto-loads the datapath at boot.

```bash
OPENWRT_DIR=~/openwrt CLONE=1 ./openwrt/build-openwrt.sh   # clones the pinned commit, patches, builds
```

- `openwrt/snic10e.config` — kernel/config fragment for the target.
- `openwrt/files/` — root overlay: `/etc/rc.local` (loads `octcarrier` + `octshm_card`,
  spreads RX IRQs, starts the temp feed) and `/root/*.ko` (the baked card modules — build
  them with step 1 and drop them in; the binaries themselves are not tracked).

  The arguments `rc.local` passes are the tuned configuration behind the numbers in
  [PERFORMANCE](PERFORMANCE.md):

  | module | argument | why |
  |---|---|---|
  | `octcarrier` | `dev=xaui0,xaui1 ipd_port=0,16` | un-gate both XAUI TX paths |
  | `octshm_card` | `ports=2 uplink=xaui0,xaui1` | one shared-memory ring pair per SFP+ port |
  | | `dma=2 hrx=1 dpiwait=0` | DPI RX into host RAM, async doorbell |
  | | `zc=1 nworkers=2 bindcpu=1` | zero-copy PKO TX gather on two pinned workers |
  | | `lockfree=1 rxdrop=1` | phase-bit rings, drop after the host tap |

  Card-side parameters not in that list (`bench`, `blen`, `linrx`, `l2ca`, `wpar`, `ztx`,
  `rxwork`, `es`) are measurement or tuning knobs; the defaults are what ships.
- `openwrt/patches/` — the local delta against the upstream commit; `build-openwrt.sh` applies it.
- `openwrt/hostfix/` — host-side build shims (gcc-14 warning downgrades, a `pipes` stub for
  Python 3.13) that OpenWrt 5.10's host tools need on a current distro.
- `openwrt/build-openwrt.sh` — the build itself: clone/pin, patch, config, `make`.

Result is an **initramfs** image, e.g.
`bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin` (~21 MiB).

> Rebaking after a card-module change: `make target/linux/install` alone does **not**
> re-apply the `files/` overlay. Copy the new `.ko` into
> `build_dir/.../root-octeon/root/`, remove the `vmlinux-initramfs*` + staged `.bin`
> stamps to force the initramfs cpio relink, then re-run the install. The initramfs is
> uncompressed inside the ELF — `grep -a` the `.bin` for a module string to confirm the
> embed.

## 3. One-time u-boot provisioning (serial, once) — `scripts/card-prep-hostboot.sh`

`octboot` relies on a persisted u-boot environment that, on reset, **programs the card's
PEM inbound window and then boots the pushed image**. Provision it **once** over the serial
console ([HARDWARE → serial](HARDWARE.md#serial-console-one-time-provisioning-only)):

```bash
sudo ./scripts/card-prep-hostboot.sh   # writes + saveenv the self-programming env, then: sudo ./octboot
```

The script auto-detects the card, the bridge, and the serial port, resets the card to its
u-boot prompt, and writes a persistent (`saveenv` → NAND) environment:

- `bootdelay=1`
- `wa/wb/wc/wd` — program the PEM inbound window (`PEMX_P2N_BAR0/BAR1_START` + 8× `BAR1_INDEX`
  for the 64 MiB DRAM window), ending in `flush_l2c; flush_dcache` to activate it,
- `bootcmd = run wa ; run wb ; run wc ; run wd ; sleep <N> ; flush ; bootoctlinux 0x20010000
  numcores=8 endbootargs console=ttyS0,115200 octeon-ethernet.receive_group_order=3`

The window is programmed to this machine's BIOS-assigned BAR0/BAR2, read live from sysfs. BAR
bases move across reseats and differ between machines (this card went `f4000000` → `c4000000`
after a reseat), and the card's decode has to point where the host can reach it — so
on another machine (or after a reseat that moves the BARs) run `card-prep-hostboot.sh` again.

`sleep <N>` defaults to **120 s**, which leaves the host time to reach `octboot` while u-boot
is still waiting. `sudo SLEEP=25 ./scripts/card-prep-hostboot.sh` boots faster with less
margin; re-running the script is also how you change it afterwards.

To undo everything on the card, `scripts/restore-bootapp.sh` reverts it to the stock boot-app
autoboot.

## 4. Boot it (each time, no serial)

```bash
sudo ./octboot
# output, abridged:
# [ OK ] CN6640 detected
# [ OK ] Secondary Bus Reset
# [ OK ] BAR restored
# [ OK ] Uploading OpenWrt...
# [ OK ] Booting...
# [ OK ] Heartbeat detected -- card ready. Load NIC: sudo modprobe octnic ports=2
```

If a serial cable *is* attached and `octboot` can't complete, `scripts/cavium-up.sh` falls back
to a serial boot via `scripts/boot-clean.sh` (`scripts/cexec.sh` is the manual equivalent for
running single commands on the card over the console).

Then bring up the NICs — see [USAGE](USAGE.md).

## 5. Where the u-boot env lives (and why serial is still needed to change it)

**The u-boot environment cannot be edited from Linux on this board: it lives in NAND.**

The apparent target is the NOR partition named `environment` (mtd2, 64 KiB, erasesize
`0x2000`), and `uboot-envtools` + `fw_setenv`/`fw_printenv` do work against it. That partition
is a decoy: it ships blank, so u-boot falls back to its compiled-in default env
(`bootcmd=bootp; … bootm`). A write to it round-trips fine, and u-boot never reads it.

Scanning every NOR partition (mtd0 `bootloader`, mtd1 `rootfs_data`, mtd2 `environment`) for a
valid env whose `bootcmd` contains `bootoctlinux` finds none. The env `octboot` depends on
(`bootdelay=1`, the `wa/wb/wc/wd` PEM-window vars, `bootcmd = run wa … ; bootoctlinux …`) is in
the 1 GiB **NAND**, which this OpenWrt kernel does not expose as an mtd, so `fw_setenv` has no
path to it. The env is persistent — written once via `saveenv` on the serial console — and the
card boots from it every time.

`/root/envdiag.sh` (run from `rc.local` ~20 s after boot) reports this at runtime over the
control page: it writes a verdict to `/proc/octshm/env`, which
`octshm_card` mirrors into the shared ctrl page at **offset `0x200`**. Read it from the host:

```bash
sudo python3 - <<'PY'   # BDF from: lspci -d 177d:0092
import mmap,os
m=mmap.mmap(os.open("/sys/bus/pci/devices/0000:02:00.0/resource2",os.O_RDWR),4096,mmap.MAP_SHARED)
print(bytes(m[0x200:0x300]).split(b"\0",1)[0].decode())
PY
# ENV_NAND: u-boot env in NAND (not fw_setenv-reachable). bootdelay=1 already permanent (octboot). NOR /dev/mtd2=writable decoy.
```

**Changing the u-boot env** (custom `bootcmd`, `bootdelay`, …) needs the serial console — see
§3. There is no serial-free path: the writable NOR env is ignored by u-boot, and the NAND env
it does read is not reachable from Linux.

> The `/proc/octshm/env` ⇄ BAR2 `0x200` control-page channel itself is reusable for any
> card-side → host status reporting where there is no serial/login (same pattern as the temp
> feed at `/proc/octshm/temp`).
