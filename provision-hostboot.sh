#!/bin/bash
# provision-hostboot.sh — one-time serial reprovision of the card's u-boot env so the no-serial
# host boot (octboot) becomes RELIABLE instead of a race.
#
# THE RACE it fixes: on cold boot the card's u-boot programs the PEM window then `sleep 25` then
# `bootoctlinux`. If the host hasn't pushed an image into the window within those 25s (slow POST),
# u-boot jumps to a stale entry -> cores wedge -> no PCIe reset recovers it (only a power cycle).
# Widening the sleep to 120s means the host ALWAYS reaches octboot while u-boot is still waiting,
# and octboot (push-if-window-live) drops the image straight in — no SBR, no race, no wedge.
#
# The u-boot env lives in NAND and is only reachable from the serial console (fw_setenv can't
# touch it — see docs/FLASHING §5), so this must be run once over serial. It ONLY changes the
# sleep value inside bootcmd; wa/wb/wc/wd (the PEM-window program) and bootdelay=1 are left as-is.
#
# PREREQ: the card must respond to SBR (running OpenWrt, or fresh at u-boot) — a garbage-wedged
# card won't reach the prompt; power-cycle (host reboot) first, let the no-serial service bring it
# up, THEN run this. Reversible: re-run with SLEEP=25 (or restore-bootapp.sh for the OEM env).
set -u
DEV=${DEV:-$(ls /dev/serial/by-id/*FT232* /dev/serial/by-id/*if00* /dev/ttyUSB0 2>/dev/null | head -1)}
BAUD=${BAUD:-115200}
SLEEP=${SLEEP:-120}                       # new u-boot pre-boot wait (was 25)
BOOTOCT=${BOOTOCT:-0x20010000}            # image entry inside the window (window base 0x20000000 + OFF 0x10000)
RGO=${RGO:-3}
LOG=${LOG:-/home/nico/Desktop/cavium/.cav-provision.log}
[ "$(id -u)" = 0 ] || exec sudo DEV="$DEV" BAUD="$BAUD" SLEEP="$SLEEP" BOOTOCT="$BOOTOCT" RGO="$RGO" LOG="$LOG" "$0" "$@"
[ -n "$DEV" ] && [ -e "$DEV" ] || { echo "[FAIL] no serial device (set DEV=/dev/ttyUSB0); is the FT232 plugged in?"; exit 1; }

CARD=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
[ -n "$CARD" ] || { echo "[FAIL] Cavium 177d:0092 not on PCI bus"; exit 1; }
BRIDGE=$(basename "$(dirname "$(readlink -f /sys/bus/pci/devices/0000:$CARD)")" | sed 's/^0000://')
echo "[*] card=$CARD bridge=$BRIDGE dev=$DEV sleep=$SLEEP"

stty -F "$DEV" "$BAUD" cs8 -cstopb -parenb clocal -crtscts raw -echo 2>/dev/null \
  || { echo "[FAIL] stty on $DEV"; exit 1; }
: > "$LOG"
( timeout 45 cat "$DEV" > "$LOG" 2>&1 ) & LP=$!
sleep 0.3

# reset the card to u-boot, then flood CR to interrupt the (bootdelay=1) autoboot at the prompt
echo "[*] SBR + CR-flood to catch u-boot prompt..."
setpci -s "$BRIDGE" BRIDGE_CONTROL=40:40; sleep 0.2; setpci -s "$BRIDGE" BRIDGE_CONTROL=00:40; sleep 1
for i in $(seq 1 700); do printf '\r' > "$DEV"; sleep 0.03; done
sleep 1
if ! grep -aqiE 'Clearing DRAM|Octeon snic10e#|U-Boot' "$LOG"; then
  echo "[FAIL] no u-boot banner/prompt — card is wedged or not responding to SBR."
  echo "       Power-cycle (host reboot), let the card come up, then re-run this."
  kill $LP 2>/dev/null; exit 2
fi
echo "[*] at u-boot prompt, writing env (bootcmd sleep -> ${SLEEP}s)..."

send() { printf '%s\r\n' "$1" > "$DEV"; sleep "${2:-1.5}"; }
# u-boot 2012 quirks (see cavium-noserial-boot memory): escape ';' as '\;', NO surrounding quotes,
# keep the line short enough for CBSIZE (only the sleep number changed vs the working bootcmd).
send "printenv bootcmd" 1.2                                   # capture the BEFORE value into the log
# NOTE: u-boot 2012 runs only the FIRST arg of `run a b c d` — each var needs its own `run`
# (cavium-noserial-boot memory). Keep the four separate `run` calls exactly like the OEM bootcmd.
send "setenv bootcmd run wa \\; run wb \\; run wc \\; run wd \\; sleep ${SLEEP} \\; flush_l2c \\; flush_dcache \\; bootoctlinux ${BOOTOCT} numcores=8 endbootargs console=ttyS0,115200 octeon-ethernet.receive_group_order=${RGO}"
send "saveenv" 5                                              # persist to NAND
send "printenv bootcmd" 1.2                                   # capture the AFTER value
send "printenv bootdelay" 1.2
kill $LP 2>/dev/null; wait $LP 2>/dev/null

echo "==================== SERIAL OUTPUT (tail) ===================="
cat -v "$LOG" | grep -v '^Octeon snic10e# $' | tail -40
echo "============================================================="
if grep -aq "sleep ${SLEEP}" "$LOG"; then
  echo "[ OK ] bootcmd now waits ${SLEEP}s — no-serial boot is now race-free. Reboot to verify."
else
  echo "[WARN] could not confirm 'sleep ${SLEEP}' in bootcmd readback — check the log above."
fi
