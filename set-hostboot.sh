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
# bridge BDF moves across reseats/re-enumeration (was 00:03.0, now 00:01.0) -- detect it
CARD=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
BR=$(basename "$(dirname "$(readlink -f /sys/bus/pci/devices/0000:$CARD)")" | sed 's/^0000://')
setpci -s "$BR" BRIDGE_CONTROL=40:40
sleep 0.12
setpci -s "$BR" BRIDGE_CONTROL=00:40
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
