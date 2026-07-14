# Flashing & booting the card

Nothing is permanently flashed for the NIC role — the card runs OpenWrt **from RAM**. The
only persistent step is a one-time u-boot environment so the card can be booted from the
host with no serial cable. After that, every boot is: `octboot` pushes the image over PCIe
and the card runs it.

```
 (once)  serial → persist u-boot env  ─────────────┐
 (each)  octboot → SBR → push image via BAR2 → card runs OpenWrt from RAM → heartbeat
```

## 1. Build the modules

Host module (native, against the running kernel):

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

## 2. Build the OpenWrt image

The card image is the **[hurricos/openwrt `snic10e-ethernet`](https://git.laboratoryb.org/hurricos/openwrt/src/branch/snic10e-ethernet)**
port, plus this repo's overlay (`openwrt/files/`) which bakes in the card modules and an
`rc.local` that auto-loads the datapath at boot.

- `openwrt/snic10e.config` — kernel/config fragment for the target.
- `openwrt/files/` — root overlay: `/etc/rc.local` (loads `octcarrier` + `octshm_card`,
  spreads RX IRQs, starts the temp feed) and `/root/*.ko` (the baked card modules).
- `openwrt/build-openwrt.sh` — reference build invocation.

Result is an **initramfs** image, e.g.
`bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin` (~21 MiB).

> Rebaking after a card-module change: `make target/linux/install` alone does **not**
> re-apply the `files/` overlay. Copy the new `.ko` into
> `build_dir/.../root-octeon/root/`, remove the `vmlinux-initramfs*` + staged `.bin`
> stamps to force the initramfs cpio relink, then re-run the install. The initramfs is
> uncompressed inside the ELF — `grep -a` the `.bin` for a module string to confirm the
> embed.

## 3. One-time u-boot provisioning (serial, once) — `card-prep-hostboot.sh`

`octboot` relies on a persisted u-boot environment that, on reset, **programs the card's
PEM inbound window and then boots the pushed image**. Provision it **once** over the serial
console ([HARDWARE → serial](HARDWARE.md#serial-console-one-time-provisioning-only)):

```bash
sudo ./card-prep-hostboot.sh      # writes + saveenv the self-programming env, then: sudo ./octboot
```

That is the whole first-time step. The script auto-detects the card, the bridge, and the
serial port, resets the card to its u-boot prompt, and writes a persistent (`saveenv` → NAND)
environment:

- `bootdelay=1`
- `wa/wb/wc/wd` — program the PEM inbound window (`PEMX_P2N_BAR0/BAR1_START` + 8× `BAR1_INDEX`
  for the 64 MiB DRAM window), ending in `flush_l2c; flush_dcache` to activate it,
- `bootcmd = run wa ; run wb ; run wc ; run wd ; sleep <N> ; flush ; bootoctlinux 0x20010000
  numcores=8 endbootargs console=ttyS0,115200 octeon-ethernet.receive_group_order=3`

**The window is programmed to THIS machine's BIOS-assigned BAR0/BAR2, read live from sysfs —
never hardcoded.** BAR bases move across reseats and differ between machines (this card went
`f4000000` → `c4000000` after a reseat); a stale hardcode aims the card's decode where the
host can't reach it. Because provisioning is per-machine anyway, baking in the real BARs makes
it correct by construction: on a different machine, just run `card-prep-hostboot.sh` once there.

`sleep <N>` defaults to **120 s** (race-free: the host always reaches `octboot` while u-boot is
still waiting). Tighten with `sudo SLEEP=25 ./card-prep-hostboot.sh` for a faster boot with less
margin — or after the fact with `provision-hostboot.sh` (which only re-tunes the sleep value).

Related helpers: `restore-bootapp.sh` reverts to the stock boot-app autoboot; `set-hostboot.sh`
is the (legacy) liquidio host-boot prep, not this env.

> The fully serial-**free** first install (injecting these commands over the u-boot PCI
> console instead of a cable) is prototyped but shelved — see `_lab/hostmod-dead/octconsole.c`.

## 4. Boot it (each time, no serial)

```bash
sudo ./octboot
# [ OK ] CN6640 detected
# [ OK ] Secondary Bus Reset
# [ OK ] BAR restored
# [ OK ] Uploading OpenWrt...
# [ OK ] Booting...
# [ OK ] Heartbeat detected -- card ready. Load NIC: sudo modprobe octnic ports=2
```

If a serial cable *is* attached and `octboot` can't complete, `cavium-up.sh` falls back to
a serial boot via `cexec.sh` / `boot-clean.sh`.

Then bring up the NICs — see [USAGE](USAGE.md).

## 5. Where the u-boot env lives (and why serial is still needed to change it)

Short version: **the u-boot environment cannot be edited from Linux on this board — it is in
NAND, and `bootdelay=1` + the `octboot` bootcmd are already persisted there permanently.**

The obvious target is the NOR partition named `environment` (mtd2, 64 KiB, erasesize `0x2000`),
and `uboot-envtools` + `fw_setenv`/`fw_printenv` do work against it. But that partition is a
**decoy**: it ships blank, so u-boot falls back to its compiled-in default env there
(`bootcmd=bootp; … bootm`). A write to it round-trips fine but **u-boot never reads it**.

Proven by scanning every NOR partition (mtd0 `bootloader`, mtd1 `rootfs_data`, mtd2
`environment`) for a valid env whose `bootcmd` contains `bootoctlinux` (octboot's signature):
**none found**. So u-boot's real env — the one `octboot` depends on (`bootdelay=1`, the
`wa/wb/wc/wd` PEM-window vars, `bootcmd = run wa … ; bootoctlinux …`) — lives in the 1 GiB
**NAND**, which this OpenWrt kernel does not expose as an mtd. `fw_setenv` from Linux has no
path to it.

That env is already **permanent**: it was written once via the serial console (`saveenv`) and
is empirically re-proven every boot — `octboot` requires `bootdelay=1` and boots the card every
time. So there is nothing left to persist for `bootdelay`; it is definitive.

`/root/envdiag.sh` (run from `rc.local` ~20 s after boot) confirms this at runtime and reports
over a clean control-page channel — it writes a verdict to `/proc/octshm/env`, which
`octshm_card` mirrors into the shared ctrl page at **offset `0x200`**. Read it from the host:

```bash
sudo python3 - <<'PY'
import mmap,os
m=mmap.mmap(os.open("/sys/bus/pci/devices/0000:03:00.0/resource2",os.O_RDWR),4096,mmap.MAP_SHARED)
print(bytes(m[0x200:0x300]).split(b"\0",1)[0].decode())
PY
# ENV_NAND: u-boot env in NAND (not fw_setenv-reachable). bootdelay=1 already permanent (octboot). NOR /dev/mtd2=writable decoy.
```

**To actually change the u-boot env** (custom `bootcmd`, `bootdelay`, etc.) you still need the
serial console once — see §3. There is no serial-free path on this board: the writable NOR env
is ignored by u-boot, and the effective NAND env is not reachable from Linux.

> The `/proc/octshm/env` ⇄ BAR2 `0x200` control-page channel itself is reusable for any
> card-side → host status reporting where there is no serial/login (same pattern as the temp
> feed at `/proc/octshm/temp`).
