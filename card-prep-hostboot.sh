#!/bin/bash
# card-prep-hostboot.sh — ONE-TIME serial provisioning of a fresh card's u-boot env so it can be
# booted from the host with NO serial cable ever again (octboot / cavium-nic.service).
#
# This is THE first-time setup step for a new card on a new machine. It writes, over the serial
# console, a persistent (saveenv -> NAND) self-programming u-boot environment:
#   bootdelay=1
#   wa/wb/wc/wd  = program the PEM inbound window (P2N_BAR0/BAR1 + 8x BAR1_IDX) so the host's
#                  BAR2 maps to card DRAM 0x20000000, then flush_l2c/flush_dcache to activate it
#   bootcmd      = run wa ; run wb ; run wc ; run wd ; sleep <N> ; flush ; bootoctlinux <entry> ...
# After this, every boot is host-only: octboot pushes the image over BAR2 and u-boot boots it.
#
# ROBUST BY CONSTRUCTION: the window is programmed to THIS machine's BIOS-assigned BAR0/BAR2
# (read live from sysfs, exactly like boot-clean.sh), and baked into the saved env. A different
# machine assigns different BARs -> just re-run this once there. NEVER hardcode BAR addresses:
# a reseat / re-enumeration moves them (this card went f4000000 -> c4000000) and a stale env
# points the card's decode where the host can't reach it.
#
# The env lives in NAND, reachable only from the serial console (fw_setenv can't touch it, see
# docs/FLASHING.md), so this must be run once over serial. Reversible: restore-bootapp.sh.
#
#   sudo ./card-prep-hostboot.sh            # provision (sleep 120), then boot with: octboot
#   sudo SLEEP=25 ./card-prep-hostboot.sh   # tighter u-boot pre-boot wait (faster, less margin)
set -u
DEV=${DEV:-$(ls /dev/serial/by-id/*FT232* /dev/serial/by-id/*if00* /dev/ttyUSB0 2>/dev/null | head -1)}
BAUD=${BAUD:-115200}
SLEEP=${SLEEP:-120}                        # u-boot pre-boot wait: host must reach octboot within it
ADDR=${ADDR:-0x20000000}                   # card DRAM load base (cavium-fast-bar2-boot memory)
OFF=${OFF:-0x10000}                        # image pushed OFF into the window (dodges bad 0x1c cell)
RGO=${RGO:-3}                              # octeon-ethernet.receive_group_order (8 POW groups)
LOG=${LOG:-/home/nico/Desktop/cavium/.cav-prep.log}
BADDR=$(printf '0x%X' $(( ADDR + OFF )))   # bootoctlinux entry = window base + OFF
[ "$(id -u)" = 0 ] || exec sudo DEV="$DEV" BAUD="$BAUD" SLEEP="$SLEEP" ADDR="$ADDR" OFF="$OFF" RGO="$RGO" LOG="$LOG" "$0" "$@"
[ -n "$DEV" ] && [ -e "$DEV" ] || { echo "[FAIL] no serial device (set DEV=/dev/ttyUSB0); FT232 plugged in?"; exit 1; }

# --- auto-detect PCI location + BIOS-assigned BARs (never hardcode) ---
CARD=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
[ -n "$CARD" ] || { echo "[FAIL] Cavium 177d:0092 not on PCI bus"; exit 1; }
BRIDGE=$(basename "$(dirname "$(readlink -f /sys/bus/pci/devices/0000:$CARD)")" | sed 's/^0000://')
RES=/sys/bus/pci/devices/0000:$CARD/resource
BAR0=$(awk 'NR==1{print $1}' "$RES")       # BAR0 base (16K CSR window)
BAR2=$(awk 'NR==3{print $1}' "$RES")       # BAR2 base (64M DRAM window)
P2N_BAR0=$(printf '0x%016X' $(( BAR0 )))
P2N_BAR1=$(printf '0x%016X' $(( BAR2 )))
RBASE=$(( ADDR >> 22 ))                     # 4MB-granule index base for BAR1_IDX
echo "[*] card=$CARD bridge=$BRIDGE BAR0=$P2N_BAR0 BAR2=$P2N_BAR1 sleep=$SLEEP entry=$BADDR"

# octnic does sync BAR2 reads; if it's loaded when we SBR the card the host can stall. Detach it.
rmmod octnic 2>/dev/null || true

stty -F "$DEV" "$BAUD" cs8 -cstopb -parenb clocal -crtscts raw -echo 2>/dev/null \
  || { echo "[FAIL] stty on $DEV"; exit 1; }
: > "$LOG"
( timeout 60 cat "$DEV" > "$LOG" 2>&1 ) & LP=$!
sleep 0.3

# reset the card to u-boot, then flood CR to interrupt the (bootdelay) autoboot at the prompt
echo "[*] SBR + CR-flood to catch u-boot prompt..."
setpci -s "$BRIDGE" BRIDGE_CONTROL=40:40; sleep 0.2; setpci -s "$BRIDGE" BRIDGE_CONTROL=00:40; sleep 1
for i in $(seq 1 700); do printf '\r' > "$DEV"; sleep 0.03; done
sleep 1
if ! grep -aqiE 'Clearing DRAM|Octeon snic10e#|U-Boot' "$LOG"; then
  echo "[FAIL] no u-boot banner/prompt — card wedged or not responding to SBR."
  echo "       Power-cycle the host (full poweroff), let it come up, then re-run this."
  kill $LP 2>/dev/null; exit 2
fi
echo "[*] at u-boot prompt, writing self-programming env..."

send() { printf '%s\r\n' "$1" > "$DEV"; sleep "${2:-1.2}"; }
# u-boot 2012 quirks (cavium-noserial-boot memory): escape ';' as '\;', NO surrounding quotes,
# keep each setenv line short (CBSIZE), and `run a b c d` runs only a -> four separate `run`.

# wa: program the two P2N BAR bases (host BAR0/BAR2 -> card sees the window)
send "setenv wa write64 0x00011800C0000080 ${P2N_BAR0} \\; write64 0x00011800C0000088 ${P2N_BAR1}"
# wb/wc/wd: the 8 BAR1_IDX entries (each maps one 4MB DRAM granule at ADDR+i*4MB), split to fit
# CBSIZE; wd finishes with the cache flushes that actually activate the window for host BAR2.
idx_cmd() {  # $1=index -> "write64 <reg> <val>"
  printf 'write64 0x%011X 0x%X' $((0x11800C00000A8 + $1*8)) $(( ((RBASE + $1)<<4) | 0xB ))
}
send "setenv wb $(idx_cmd 0) \\; $(idx_cmd 1) \\; $(idx_cmd 2)"
send "setenv wc $(idx_cmd 3) \\; $(idx_cmd 4) \\; $(idx_cmd 5)"
send "setenv wd $(idx_cmd 6) \\; $(idx_cmd 7) \\; flush_l2c \\; flush_dcache"
send "setenv bootdelay 1"
send "setenv bootcmd run wa \\; run wb \\; run wc \\; run wd \\; sleep ${SLEEP} \\; flush_l2c \\; flush_dcache \\; bootoctlinux ${BADDR} numcores=8 endbootargs console=ttyS0,115200 octeon-ethernet.receive_group_order=${RGO}"
send "saveenv" 5                                              # persist to NAND
send "printenv bootcmd" 1.2                                   # readback for the log
send "printenv wa" 1.2
send "printenv bootdelay" 1.2
kill $LP 2>/dev/null; wait $LP 2>/dev/null

echo "==================== SERIAL OUTPUT (tail) ===================="
cat -v "$LOG" | grep -v '^Octeon snic10e# $' | tail -30
echo "============================================================="
ok=1
grep -aq "bootoctlinux ${BADDR}" "$LOG" || { echo "[WARN] bootcmd readback missing 'bootoctlinux ${BADDR}'"; ok=0; }
grep -aq "0x00011800C0000088 ${P2N_BAR1}" "$LOG" || { echo "[WARN] wa readback missing P2N_BAR1 ${P2N_BAR1}"; ok=0; }
if [ "$ok" = 1 ]; then
  echo "[ OK ] env provisioned + saved. The card now boots no-serial."
  echo "       Boot it now:  sudo ./octboot     (or: sudo systemctl restart cavium-nic)"
  echo "       From here every host boot brings up oct0/oct1 with no serial cable."
else
  echo "[FAIL] could not confirm the env readback — check the serial log above and re-run."
  exit 3
fi
