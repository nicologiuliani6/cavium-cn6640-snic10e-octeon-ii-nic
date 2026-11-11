# if_octnic — FreeBSD driver for Cavium CN6640-SNIC10E

**Status: in development. Not tested on hardware. Card boot on FreeBSD is not implemented yet.**

Kernel module `if_octnic` that exposes the card's two 10 GbE ports as `oct0` and `oct1`
(standard `ifnet` interfaces) over the BAR2 shared-memory window. It is a fresh port of
`linuxmod/octnic.c`; the old `bsdmod/` attempt was removed.

## What works today
- Source compiles with `clang -Wall -Werror` against the real FreeBSD kernel headers (see "Syntax check").
- `make` in `octnic/` builds `if_octnic.ko` on FreeBSD (needs matching `/usr/src`).
- `install.sh` builds, installs and loads the module; `rc.d/cavium_nic` loads/unloads it.

## What does NOT work yet
- **Card boot on FreeBSD.** The Linux `octboot` script is Linux-only (`setpci`, sysfs, `resource2` mmap).
  The rc.d script refuses to pretend otherwise and says so.
- **Hardware test.** No run on a real card yet.
- **Host tests.** No test harness yet.

## Build (on FreeBSD 14+)
```sh
fetch https://download.freebsd.org/releases/amd64/amd64/$(uname -r | sed 's/-.*//')-RELEASE/src.txz
tar -xf src.txz -C /            # gives /usr/src
cd freebsd/octnic && make       # -> if_octnic.ko
```

## Install
```sh
sudo sh freebsd/install.sh              # build, install to /boot/modules, load
sudo sh freebsd/install.sh --dry-run    # print actions only
sudo sh freebsd/install.sh --uninstall
```
Loading at boot: `if_octnic_load="YES"` in `/boot/loader.conf` (the installer adds it).

## Syntax check (Linux host, no FreeBSD VM)
Needs a FreeBSD source tree (`$FBSD_SRC`, e.g. a sparse or full clone of `freebsd-src`) and
the kernel-generated interface headers (`device_if.h`, `bus_if.h`, `pci_if.h`, built with
`sys/tools/makeobjops.awk -h` on the `.m` files) plus `machine` and `x86` links, placed in `$FBSD_INC`:
```sh
clang -target x86_64-unknown-freebsd -D_KERNEL -DKLD_MODULE -ffreestanding -nostdinc \
  -I$FBSD_INC -I$FBSD_SRC/sys -I$FBSD_SRC/sys/contrib/ck/include \
  -Wall -Werror -fsyntax-only freebsd/octnic/if_octnic.c
```

## Layout
- `octnic/if_octnic.c`, `octnic/octnic_regs.h`: driver.
- `octnic/Makefile`: kernel module build (`bsd.kmod.mk`, FreeBSD only).
- `rc.d/cavium_nic`: rc service (load/unload only, see above).
- `install.sh`: installer.
- `.github/workflows/release-freebsd.yml` (repo root): builds the `.ko` in a FreeBSD VM and publishes a pre-release on `freebsd-v*` tags.
