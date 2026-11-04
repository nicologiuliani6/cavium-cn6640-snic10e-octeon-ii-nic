#!/bin/bash
# install.sh — GLOBAL installer for the Cavium CN6640 SNIC10E NIC stack (Linux today;
# Windows = install.bat, BSD not implemented yet -> this script cancels there).
#
# What it does, asking for confirmation before every step that changes something:
#   1. is the card on the PCI bus?
#   2. install the host driver (octnic via dkms) + configs + autostart service
#   3. is the card already flashed (u-boot env provisioned for host boot, see docs/FLASHING.md)?
#        yes -> done: `systemctl start cavium-nic` boots it with no serial cable
#        no  -> 4
#   4. find the USB-serial adapter, test it, and (after confirmation) flash the u-boot env
#      with scripts/card-prep-hostboot.sh, then boot the card
#
#   sudo ./install.sh            # interactive
#   sudo ./install.sh --start    # also boot the card + bring the NICs up now
#   sudo ./install.sh --yes      # answer yes to every question (incl. the flash step) — unattended
# Without a terminal (no tty) the driver steps run as before; the reset/flash steps are
# skipped unless --yes is given.
set -u
REPO="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
START=0; YES=0
for a in "$@"; do
  case "$a" in
    --start) START=1 ;;
    -y|--yes) YES=1 ;;
    -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
  esac
done
say(){ printf '[ \033[32m*\033[0m ] %s\n' "$1"; }
warn(){ printf '[ \033[33m!\033[0m ] %s\n' "$1"; }
die(){ printf '[ \033[31mFAIL\033[0m ] %s\n' "$1" >&2; exit 1; }
# ask "question" [soft]  — soft questions default to yes when there is no terminal (legacy unattended use)
ask(){
  if [ "$YES" = 1 ]; then printf '[ ? ] %s -> yes (--yes)\n' "$1"; return 0; fi
  if [ ! -t 0 ]; then
    if [ "${2:-}" = soft ]; then printf '[ ? ] %s -> yes (no terminal)\n' "$1"; return 0; fi
    printf '[ ? ] %s -> no (no terminal; use --yes)\n' "$1"; return 1
  fi
  printf '[ ? ] %s [y/N] ' "$1"; read -r _a; case "$_a" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

# --- OS gate: this installer is global, the drivers are not (yet) ---
OS=$(uname -s 2>/dev/null || echo unknown)
case "$OS" in
  Linux) ;;
  FreeBSD|OpenBSD|NetBSD|DragonFly)
    die "$OS: the BSD driver is not implemented yet (see bsdmod/README.md) — installation cancelled." ;;
  MINGW*|MSYS*|CYGWIN*)
    die "Windows detected: run install.bat from a normal Windows prompt (or double-click it)." ;;
  *) die "unsupported OS '$OS' (Linux only for now; Windows: install.bat) — installation cancelled." ;;
esac
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

# --- helpers ---
card_bdf(){ lspci -d 177d:0092 2>/dev/null | awk '{print $1}' | head -1; }
find_image(){
  ls "$REPO"/*snic10e-initramfs-kernel.bin \
     "$REPO"/openwrt/bin/targets/octeon/generic/*snic10e-initramfs-kernel.bin \
     /home/*/openwrt/bin/targets/octeon/generic/*snic10e-initramfs-kernel.bin 2>/dev/null | head -1
}
# osready = card OS up | os = magic but not ready | live = u-boot waiting with the BAR2 window programmed
# (== env provisioned) | dead = no window (stock env, wedged, or powered down). Same probe as octboot.
probe_card(){
  setpci -s "$1" COMMAND=0x06 2>/dev/null
  CARD="$1" python3 - <<'PY'
import mmap,os
try:
    fd=os.open("/sys/bus/pci/devices/0000:%s/resource2"%os.environ["CARD"],os.O_RDWR)
    m=mmap.mmap(fd,64*1024*1024,mmap.MAP_SHARED)
    if int.from_bytes(bytes(m[0:4]),"little")==0x4f435348:
        print("osready" if int.from_bytes(bytes(m[8:12]),"little") else "os")
    else:
        m[0x10000:0x10008]=b"NSRLIVE0"; m.flush()
        print("live" if bytes(m[0x10000:0x10008])==b"NSRLIVE0" else "dead")
except Exception:
    print("dead")
PY
}
find_serial(){ ls /dev/serial/by-id/*FT232* /dev/serial/by-id/*if00* /dev/ttyUSB0 2>/dev/null | head -1; }
# serial_check DEV SBR(0|1) -> uboot | console | silent
serial_check(){
  _dev=$1; _tmp=$(mktemp)
  stty -F "$_dev" 115200 cs8 -cstopb -parenb clocal -crtscts raw -echo 2>/dev/null || { echo silent; rm -f "$_tmp"; return; }
  ( timeout 45 cat "$_dev" > "$_tmp" 2>/dev/null ) & _lp=$!
  sleep 0.3
  if [ "$2" = 1 ]; then
    rmmod octnic 2>/dev/null || true
    _br=$(basename "$(dirname "$(readlink -f "/sys/bus/pci/devices/0000:$CARD")")" | sed 's/^0000://')
    setpci -s "$_br" BRIDGE_CONTROL=40:40; sleep 0.2; setpci -s "$_br" BRIDGE_CONTROL=00:40; sleep 1
    _n=700
  else _n=40; fi
  _i=0; while [ "$_i" -lt "$_n" ]; do printf '\r' > "$_dev" 2>/dev/null; sleep 0.03; _i=$((_i+1)); done
  sleep 1; kill $_lp 2>/dev/null; wait $_lp 2>/dev/null
  if grep -aqiE 'Clearing DRAM|Octeon snic10e#|U-Boot' "$_tmp"; then echo uboot
  elif [ -s "$_tmp" ]; then echo console
  else echo silent; fi
  rm -f "$_tmp"
}

# ---------- 1) card present? ----------
command -v lspci >/dev/null 2>&1 || die "lspci missing (Debian/Ubuntu: apt install pciutils)"
CARD=$(card_bdf)
if [ -n "$CARD" ]; then say "CN6640 found on the PCI bus ($CARD)"
else
  warn "CN6640 (177d:0092) not on the PCI bus — check the card is seated in a PCIe slot"
  ask "Install the driver anyway?" soft || die "cancelled: no card"
fi

# ---------- 2) driver ----------
ask "Install the host driver (octnic via dkms), configs and the cavium-nic autostart service?" soft || die "cancelled by user"
install_driver(){
# 1) deps
say "checking host deps"
miss=
for c in setpci lspci make gcc ip awk python3 depmod modprobe; do
  command -v "$c" >/dev/null 2>&1 || miss="$miss $c"
done
[ -d "/lib/modules/$(uname -r)/build" ] || miss="$miss kernel-headers(/lib/modules/$(uname -r)/build)"
[ -z "$miss" ] || die "missing:$miss  (Debian/Ubuntu: apt install pciutils build-essential linux-headers-\$(uname -r))"
# Secure Boot puts the kernel in lockdown: setpci + the BAR mmaps return EPERM and unsigned
# out-of-tree modules are refused. Warn early — it is a BIOS setting, nothing to rebuild.
grep -q '\[integrity\]\|\[confidentiality\]' /sys/kernel/security/lockdown 2>/dev/null \
  && printf '[ \033[33m!\033[0m ] kernel lockdown is ON (Secure Boot) — setpci/BAR access will fail; disable Secure Boot in the BIOS\n'

# 2) build + install the host module (native, against the running kernel).
# With dkms present the module is registered so it rebuilds itself on every kernel
# upgrade; without it we fall back to a plain build into /lib/modules/<ver>/extra,
# which has to be re-run by hand after a kernel update. NODKMS=1 forces the fallback.
VER=$(awk -F'"' '/^PACKAGE_VERSION=/{print $2}' "$REPO/linuxmod/dkms.conf")
if command -v dkms >/dev/null 2>&1 && [ "${NODKMS:-0}" != 1 ]; then
  say "building host module (octnic $VER, dkms)"
  # drop EVERY registered octnic version (also stale/broken ones), else dkms skips the install
  for v in $(dkms status 2>/dev/null | awk -F'[/,: ]+' '$1=="octnic"{print $2}' | sort -u); do
    dkms remove -m octnic -v "$v" --all >/dev/null 2>&1 || true
  done
  rm -rf /usr/src/octnic-*
  rm -rf "/usr/src/octnic-$VER"
  # drop any octnic.ko left by an earlier non-dkms install, else dkms flags the mismatch
  rm -f "/lib/modules/$(uname -r)/extra/octnic.ko"
  install -d "/usr/src/octnic-$VER"
  install -m644 "$REPO/linuxmod/octnic.c" "$REPO/linuxmod/Makefile" "$REPO/linuxmod/dkms.conf" \
    "/usr/src/octnic-$VER/" || die "stage dkms source"
  dkms install -m octnic -v "$VER" >/tmp/octnic-build.log 2>&1 \
    || { tail -20 /tmp/octnic-build.log; die "dkms build failed"; }
  say "installed octnic.ko via dkms (rebuilds on kernel upgrade)"
else
  say "building host module (octnic, no dkms)"
  # Makefile uses M=$(PWD), so build from inside linuxmod (not `make -C`).
  ( cd "$REPO/linuxmod" && make clean ) >/dev/null 2>&1 || true
  ( cd "$REPO/linuxmod" && make ) >/tmp/octnic-build.log 2>&1 || { cat /tmp/octnic-build.log; die "linuxmod build failed"; }
  DEST="/lib/modules/$(uname -r)/extra"
  install -d "$DEST"
  install -m644 "$REPO/linuxmod/octnic.ko" "$DEST/" || die "install octnic.ko"
  depmod -a
  say "installed octnic.ko -> $DEST  (modprobe octnic; re-run after a kernel upgrade)"
fi

# 3) system configs: keep stock liquidio off, sensors config
say "installing system configs"
install -m644 "$REPO/system/blacklist-liquidio.conf" /etc/modprobe.d/ 2>/dev/null \
  && rmmod liquidio 2>/dev/null; true
# oct0/oct1 are DHCP clients now (nic-up.sh): drop the old "unmanaged" pin from earlier installs
rm -f /etc/NetworkManager/conf.d/99-octnic-unmanaged.conf
# lm-sensors config so `sensors` shows the card's temps (cavium_card hwmon driver)
if [ -d /etc/sensors.d ]; then
  install -m644 "$REPO/system/cavium-card" /etc/sensors.d/ 2>/dev/null; true
fi

# 4) autostart service, with ExecStart pinned to THIS repo location
say "installing cavium-nic.service (ExecStart -> $REPO/scripts/cavium-up.sh)"
sed "s|@REPO@|$REPO|g" "$REPO/system/cavium-nic.service" \
  > /etc/systemd/system/cavium-nic.service || die "write service"
systemctl daemon-reload
systemctl enable cavium-nic.service >/dev/null 2>&1 && say "service enabled (starts at boot)"
}
install_driver

# ---------- 3) already flashed? ----------
FLASHED=0
if [ -n "$CARD" ]; then
  STATE=$(probe_card "$CARD"); say "card state: $STATE"
  if [ "$STATE" = dead ]; then
    warn "the card does not answer: its u-boot is not provisioned for host boot, or it is stuck/powered down"
    IMG=$(find_image)
    if [ -n "$IMG" ] && ask "Try to boot it with octboot (SBR + image push, 1-3 min) to find out if it is already flashed?"; then
      if "$REPO/octboot"; then STATE=osready; say "octboot booted the card: it was already flashed"; fi
    fi
  fi
  case "$STATE" in osready|os|live) FLASHED=1; say "card already flashed (u-boot env provisioned): no serial needed" ;; esac

  # ---------- 4) not flashed -> serial -> flash ----------
  if [ "$FLASHED" = 0 ]; then
    warn "card not flashed (or not reachable) — looking for the USB-serial adapter"
    DEV=$(find_serial)
    if [ -z "$DEV" ]; then
      warn "no USB-serial adapter found. Plug the FT232 adapter into the card console header and this PC,"
      warn "then run ./install.sh again. The driver is installed; nothing was written to the card."
    else
      say "serial adapter found: $DEV"
      if ask "Test the serial link on $DEV (sends a few carriage returns)?"; then
        SER=$(serial_check "$DEV" 0); say "serial link: $SER"
        if [ "$SER" = silent ] && ask "Silent. Reset the card (SBR) and test again — it prints the u-boot banner at reset?"; then
          SER=$(serial_check "$DEV" 1); say "serial link after reset: $SER"
        fi
        case "$SER" in
          uboot|console)
            say "serial link works"
            if ask "Flash the card now? This writes the u-boot environment to its NAND (persistent; undo: scripts/restore-bootapp.sh)."; then
              DEV="$DEV" "$REPO/scripts/card-prep-hostboot.sh" && { FLASHED=1; say "card flashed"; } || warn "flashing failed — see the output above"
            fi ;;
          *) warn "the serial link does not work (wiring, port, or 115200 8N1) — fix it and run ./install.sh again" ;;
        esac
      fi
    fi
  fi
fi

# ---------- 5) card image + first boot ----------# 5) card image check (built OpenWrt initramfs; octboot needs it)
IMG=$(ls "$REPO"/*snic10e-initramfs-kernel.bin \
        "$REPO"/openwrt/bin/targets/octeon/generic/*snic10e-initramfs-kernel.bin \
        /home/*/openwrt/bin/targets/octeon/generic/*snic10e-initramfs-kernel.bin 2>/dev/null | head -1)
if [ -n "$IMG" ]; then say "card image found: $IMG"
else printf '[ \033[33m!\033[0m ] no card image -> download it from the GitHub release into %s/, or build it with ./openwrt/build-openwrt.sh\n' "$REPO"; fi
if [ "$FLASHED" = 1 ] && { [ "$START" = 1 ] || ask "Boot the card and bring up oct0/oct1 now (systemctl start cavium-nic)?"; }; then
  say "booting card + bringing up NICs (systemctl start cavium-nic)"
  systemctl start cavium-nic.service && say "done -> check: ip -br addr show oct0" \
    || die "start failed (see /var/log/cavium-up.log)"
fi

echo
say "installed. Next:"
echo "    sudo systemctl start cavium-nic     # boot card + oct0/oct1 (no serial)"
echo "    ip -br addr show oct0"
echo
[ "$FLASHED" = 1 ] || { printf '[ \033[33mnote\033[0m ] The card is not flashed yet: it needs its one-time u-boot env provisioned\n'
                         printf '           (USB-serial, once) — run ./install.sh again with the adapter, or see docs/FLASHING.md.\n'; }