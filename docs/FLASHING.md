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

## 3. One-time u-boot provisioning (serial, once)

`octboot` relies on a persisted u-boot environment that, on reset, **programs the card's
PEM inbound window and then boots the pushed image**. Set it once over the serial console
([HARDWARE → serial](HARDWARE.md#serial-console-one-time-provisioning-only)):

The environment must, in `bootcmd`:

1. program the PEM inbound BARs so the host can reach card DRAM:
   - `write64 0x00011800C0000080 0x00000000F8000000`  (`PEMX_P2N_BAR0_START`)
   - `write64 0x00011800C0000088 0x00000000F4000000`  (`PEMX_P2N_BAR1_START`)
   - program `PEMX_BAR1_INDEX0` / `BAR_CTL` for the 64 MiB window
   - `flush_l2c; flush_dcache` to activate,
2. `sleep` long enough for the host to push the image over BAR2 (~25 s),
3. `bootoctlinux 0x20010000 numcores=8 endbootargs console=ttyS0,115200 octeon-ethernet.receive_group_order=3`

with `bootdelay=1` and `saveenv` to persist. `set-hostboot.sh` / `card-prep-hostboot.sh`
in this repo are the serial helpers used to poke these; `restore-bootapp.sh` reverts to the
stock boot-app.

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

## 5. Editing the u-boot env without serial (`fw_setenv`)

Once the card is running our image you can read **and persist** the u-boot environment with
no serial cable. The card's NOR exposes a partition named `environment` (mtd2, 64 KiB,
erasesize `0x2000`) — the u-boot `CONFIG_ENV` location. `uboot-envtools` is baked in, and
`/root/envdiag.sh` (run automatically from `rc.local` ~20 s after boot) auto-detects the
partition and writes `/etc/fw_env.config`, so `fw_setenv` / `fw_printenv` just work on the
card shell.

Because the card has no serial and no login, `envdiag.sh` reports its result over a clean
control-page channel: it writes a verdict string to `/proc/octshm/env`, which the card
module (`octshm_card`) mirrors into the shared ctrl page at **offset `0x200`**. Read it from
the host over BAR2:

```bash
sudo python3 - <<'PY'
import mmap,os
m=mmap.mmap(os.open("/sys/bus/pci/devices/0000:03:00.0/resource2",os.O_RDWR),4096,mmap.MAP_SHARED)
print(bytes(m[0x200:0x300]).split(b"\0",1)[0].decode())
PY
# FWENV_RW_OK /dev/mtd2 0x0 0x00002000 | probe=33.19 (env initialised, persists serial-free)
```

Verdicts: `FWENV_OK` (valid env already present), `FWENV_RW_OK` (env was blank, initialised
+ round-trip write/read proven), `FWENV_NOPART` (no `environment` partition),
`FWENV_WRITE_FAIL` (NOR write refused). **Confirmed on hardware: a `fw_setenv` write to mtd2
persists and reads back with no serial** — the env shipped blank (u-boot ran compiled
defaults), so the first write initialises it.

To **persist real settings** (e.g. a custom `bootcmd`/`bootdelay`) serial-free: add
`fw_setenv KEY VALUE` lines to `envdiag.sh`, rebake the image (§2), and `octboot` once. u-boot
reads this same partition on the next cold boot. This finally makes the one-time serial
provisioning in §3 optional for env tweaks (the *first* PEM/boot env still needs §3 once on a
virgin card, since fw_setenv needs the card already running our image).
