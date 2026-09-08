#!/bin/bash
# cavium-up.sh — unattended full bring-up after a HOST boot.
# The card runs its OS from RAM, so a host power-cycle wipes it. This re-pushes the image
# over PCIe and brings up the 10G NIC. Auto-detects the card's PCI location.
#
# NO SERIAL by default: octboot boots the card via a persisted self-programming
# u-boot bootcmd + a host BAR2 image push (no console). The image's baked /etc/rc.local
# self-loads octcarrier + octshm_card + the temp daemon, so the card side needs no serial.
# If boot-nsr fails and an FT232 console is present, it falls back to the serial boot-clean.
#
# Logs to /var/log/cavium-up.log. Safe to re-run by hand: `sudo bash scripts/cavium-up.sh`.
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
REPO="$(dirname "$DIR")"
LOG=/var/log/cavium-up.log
exec > >(tee -a "$LOG") 2>&1
echo "==================== cavium-up $(date) ===================="

if ! lspci -d 177d:0092 >/dev/null 2>&1 || [ -z "$(lspci -d 177d:0092)" ]; then
  echo "FATAL: Cavium 177d:0092 not on PCI bus"; exit 1
fi

# Detach any stale host NIC driver FIRST: octboot SBRs the card and pushes a fresh image over
# BAR2, and a still-loaded octnic keeps hammering BAR2 with sync reads during that reset/push —
# it can corrupt the push and risks a host stall on the reset. No-op on a cold boot (not loaded).
rmmod octnic 2>/dev/null || true

# 1) boot the card — no serial first (retry), then serial fallback if a console exists
booted=0
for t in 1 2 3; do
  echo "--- no-serial boot attempt $t ---"
  if bash "$REPO/octboot"; then booted=1; NOSER=1; echo "card booted no-serial (try $t)"; break; fi
  echo "no-serial boot $t failed"; sleep 3
done
if [ "$booted" != 1 ]; then
  DEV=$(ls /dev/serial/by-id/*FT232* /dev/serial/by-id/*if00* 2>/dev/null | head -1)
  [ -n "$DEV" ] || DEV=$(ls /dev/ttyUSB* 2>/dev/null | head -1)
  if [ -n "$DEV" ]; then
    export DEV; echo "--- fallback: serial boot via $DEV ---"
    for t in 1 2 3; do
      if bash "$DIR/boot-clean.sh"; then booted=1; echo "card booted via serial (try $t)"; break; fi
      sleep 3
    done
  fi
fi
[ "$booted" = 1 ] || { echo "FATAL: card did not boot"; exit 2; }
sleep 8							# let the card's rc.local self-configure

# 2) host side: load octnic and bring oct0/oct1 up (the card's baked rc.local has already
#    configured the card end by now).
echo "--- nic-up (host path) ---"
bash "$DIR/nic-up.sh"
echo "==================== done $(date) ===================="
