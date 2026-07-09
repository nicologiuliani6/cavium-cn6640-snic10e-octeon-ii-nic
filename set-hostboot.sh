#!/bin/sh
# Configure card u-boot to stay at prompt after reset (host-boot ready for liquidio).
# Sets bootdelay=-1 and clears bootcmd, then saveenv (persists in NOR env partition).
# Reversible: re-run restore-bootapp.sh to put it back.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-set.log
BAUD=115200
if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > $LOG
( timeout 40 cat $DEV > $LOG ) &
LP=$!
sleep 0.2
setpci -s 00:03.0 BRIDGE_CONTROL=40:40
sleep 0.12
setpci -s 00:03.0 BRIDGE_CONTROL=00:40
# hammer Enter ~12s to stop autoboot
t=0
while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1
send() { printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.5}"; }
echo "[*] at prompt, sending env changes..."
send "setenv bootdelay -1"
send "setenv bootcmd"
send "saveenv" 4
send "printenv bootdelay" 1.5
send "printenv bootcmd" 1.5
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== OUTPUT ===================="
cat -v $LOG | grep -v "^Octeon snic10e# $" | tail -60
echo "==================== END ===================="
