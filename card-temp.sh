#!/bin/bash
# Read the Cavium card's on-board tmp421 sensor from the host over the serial console.
# ch1 = board/local, ch2 = Octeon die/remote. The die temp is the number that matters for
# the DRAM-degrades-under-load problem: keep it down or the DDR goes marginal and the fast
# reload stops landing the image. Run bare for one reading, or `card-temp.sh watch` to loop.
#   ponytail: reads via the serial console (needs OpenWrt up + serial free). Not wired into
#   host `sensors` -- that needs a host kernel hwmon driver fed by an always-on BAR2 data
#   channel; do that only once the NIC (octshm) is stable enough to host the channel.
DEV=/dev/ttyUSB0
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
read_once() {
  fuser -k "$DEV" 2>/dev/null; sleep 0.4
  stty -F "$DEV" 115200 raw -echo 2>/dev/null || { echo "card temp: serial not ready"; return 1; }
  local f=/tmp/card-temp.$$
  ( timeout 5 cat "$DEV" > "$f" 2>/dev/null ) &
  sleep 0.3
  printf '\n' > "$DEV"; sleep 0.3
  printf 'echo T1=$(cat /sys/class/hwmon/hwmon0/temp1_input) T2=$(cat /sys/class/hwmon/hwmon0/temp2_input)\n' > "$DEV"
  sleep 2.5
  local line t1 t2
  line=$(tr -d '\r' < "$f" | grep -aoE 'T1=[0-9]+ T2=[0-9]+' | tail -1); rm -f "$f"
  t1=${line#T1=}; t1=${t1%% *}; t2=${line##*T2=}
  [ -n "$t1" ] || { echo "card temp: no read (OpenWrt down or serial busy)"; return 1; }
  printf 'cavium-card\n  board:  %d.%d C\n  octeon: %d.%d C\n' \
    $((t1/1000)) $(( (t1%1000)/100 )) $((t2/1000)) $(( (t2%1000)/100 ))
}
if [ "$1" = watch ]; then
  while :; do clear; date; read_once || true; sleep "${2:-5}"; done
else
  read_once
fi
