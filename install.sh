#!/bin/bash
# install.sh — one-shot host installer for the Cavium CN6640 SNIC10E NIC stack.
# Builds + installs the host kernel module, drops the system configs, installs and
# enables the autostart service. After this + a card already provisioned (one-time
# u-boot env, see docs/FLASHING.md), `sudo systemctl start cavium-nic` gives you
# oct0/oct1 with no serial cable.
#
#   sudo ./install.sh          # build+install, enable service (does not boot the card)
#   sudo ./install.sh --start  # also boot the card + bring the NICs up now
set -u
REPO="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
START=0; [ "${1:-}" = "--start" ] && START=1
say(){ printf '[ \033[32m*\033[0m ] %s\n' "$1"; }
die(){ printf '[ \033[31mFAIL\033[0m ] %s\n' "$1" >&2; exit 1; }

# 1) deps
say "checking host deps"
miss=
for c in setpci lspci make gcc ip awk python3 depmod modprobe; do
  command -v "$c" >/dev/null 2>&1 || miss="$miss $c"
done
[ -d "/lib/modules/$(uname -r)/build" ] || miss="$miss kernel-headers(/lib/modules/$(uname -r)/build)"
[ -z "$miss" ] || die "missing:$miss  (Debian/Ubuntu: apt install pciutils build-essential linux-headers-\$(uname -r))"

# 2) build + install the host module (native, against the running kernel)
say "building host module (octnic, octoq)"
# Makefile uses M=$(PWD), so build from inside hostmod (not `make -C`).
( cd "$REPO/hostmod" && make clean ) >/dev/null 2>&1 || true
( cd "$REPO/hostmod" && make ) >/tmp/octnic-build.log 2>&1 || { cat /tmp/octnic-build.log; die "hostmod build failed"; }
DEST="/lib/modules/$(uname -r)/extra"
install -d "$DEST"
install -m644 "$REPO/hostmod/octnic.ko" "$DEST/" || die "install octnic.ko"
[ -f "$REPO/hostmod/octoq.ko" ] && install -m644 "$REPO/hostmod/octoq.ko" "$DEST/"
depmod -a
say "installed octnic.ko -> $DEST  (modprobe octnic)"

# 3) system configs: keep stock liquidio off, keep NM from flushing oct* IPs
say "installing system configs"
install -m644 "$REPO/system/blacklist-liquidio.conf" /etc/modprobe.d/ 2>/dev/null \
  && rmmod liquidio 2>/dev/null; true
if [ -d /etc/NetworkManager/conf.d ]; then
  install -m644 "$REPO/system/99-octnic-unmanaged.conf" /etc/NetworkManager/conf.d/ 2>/dev/null \
    && systemctl reload NetworkManager 2>/dev/null; true
fi

# 4) autostart service, with ExecStart pinned to THIS repo location
say "installing cavium-nic.service (ExecStart -> $REPO/cavium-up.sh)"
sed "s#^ExecStart=.*#ExecStart=$REPO/cavium-up.sh#" "$REPO/system/cavium-nic.service" \
  > /etc/systemd/system/cavium-nic.service || die "write service"
systemctl daemon-reload
systemctl enable cavium-nic.service >/dev/null 2>&1 && say "service enabled (starts at boot)"

# 5) card image check (built OpenWrt initramfs; octboot needs it)
IMG=$(ls "$REPO"/openwrt/bin/targets/octeon/generic/*snic10e-initramfs-kernel.bin \
        /home/*/openwrt/bin/targets/octeon/generic/*snic10e-initramfs-kernel.bin 2>/dev/null | head -1)
if [ -n "$IMG" ]; then say "card image found: $IMG"
else printf '[ \033[33m!\033[0m ] card image not built yet -> run ./openwrt/build-openwrt.sh (or set IMG=... for octboot)\n'; fi

# 6) card present?
if lspci -d 177d:0092 >/dev/null 2>&1 && [ -n "$(lspci -d 177d:0092)" ]; then
  say "CN6640 present on PCI"
  if [ "$START" = 1 ]; then
    say "booting card + bringing up NICs (systemctl start cavium-nic)"
    systemctl start cavium-nic.service && say "done -> check: ip -br addr show oct0" \
      || die "start failed (see /var/log/cavium-up.log)"
  fi
else
  printf '[ \033[33m!\033[0m ] CN6640 (177d:0092) not on the bus — plug the card / enable Above-4G BIOS\n'
fi

echo
say "installed. Next:"
echo "    sudo systemctl start cavium-nic     # boot card + oct0/oct1 (no serial)"
echo "    ip -br addr show oct0"
echo
printf '[ \033[33mnote\033[0m ] A brand-new card needs its one-time u-boot env provisioned first\n'
printf '           (serial, once) — see docs/FLASHING.md. Cards already provisioned skip this.\n'
