#!/bin/sh
# freebsd/install.sh — build and install the if_octnic FreeBSD kernel module.
# Usage: sh install.sh [--dry-run] [--uninstall]
# Env:   SYSDIR=/usr/src/sys (kernel sources matching the running release)
# Note:  card boot is NOT implemented on FreeBSD yet; this only installs the driver.
set -eu

DRY=0
UNINST=0
for a in "$@"; do
    case "$a" in
        --dry-run) DRY=1 ;;
        --uninstall) UNINST=1 ;;
        -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

say() { printf '[ * ] %s\n' "$1"; }
die() { printf '[FAIL] %s\n' "$1" >&2; exit 1; }
run() { if [ "$DRY" = 1 ]; then echo "+ $*"; else "$@"; fi; }

HERE=$(cd "$(dirname "$0")" && pwd)

[ "$(uname -s)" = FreeBSD ] || die "FreeBSD only (uname: $(uname -s))"
if [ "$(id -u)" != 0 ] && [ "$DRY" = 0 ]; then exec sudo "$0" "$@"; fi

if [ "$UNINST" = 1 ]; then
    run kldunload if_octnic 2>/dev/null || true
    run rm -f /boot/modules/if_octnic.ko /usr/local/etc/rc.d/cavium_nic
    if [ -f /boot/loader.conf ]; then
        run sed -i '' '/^if_octnic_load=/d' /boot/loader.conf
    fi
    say "uninstalled"
    exit 0
fi

SYSDIR=${SYSDIR:-/usr/src/sys}
if [ ! -d "$SYSDIR" ]; then
    REL=$(uname -r | sed 's/-.*//')
    die "kernel sources not found at $SYSDIR. Get them: fetch https://download.freebsd.org/releases/amd64/amd64/${REL}-RELEASE/src.txz && tar -xf src.txz -C /"
fi

say "building if_octnic.ko against $SYSDIR"
run make -C "$HERE/octnic" SYSDIR="$SYSDIR" clean >/dev/null 2>&1 || true
run make -C "$HERE/octnic" SYSDIR="$SYSDIR" || die "build failed"

say "installing module and rc.d script"
run install -m 555 "$HERE/octnic/if_octnic.ko" /boot/modules/
run install -m 555 "$HERE/rc.d/cavium_nic" /usr/local/etc/rc.d/cavium_nic

if [ ! -f /boot/loader.conf ] || ! grep -q '^if_octnic_load=' /boot/loader.conf; then
    run sh -c 'echo if_octnic_load=\"YES\" >> /boot/loader.conf'
fi

if [ "$DRY" = 0 ]; then
    kldstat -q -m if_octnic || kldload if_octnic || die "kldload if_octnic failed"
    say "module loaded; oct interfaces: $(ifconfig -l | tr ' ' '\n' | grep '^oct' | tr '\n' ' ' || true)"
fi

say "installed. Card boot on FreeBSD is not implemented yet: see freebsd/README.md"
