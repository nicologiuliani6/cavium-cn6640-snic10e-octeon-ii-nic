#!/bin/sh
# Reset Octeon, interrupt autoboot, dump env.
# Needs: GND on bracket, RXD on card TX pin (found), TXD on card RX (adjacent pin).
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-prompt.log
BAUD=115200
if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > $LOG
( timeout 22 cat $DEV > $LOG ) &
LP=$!
sleep 0.2
echo "[*] resetting card..."
setpci -s 00:03.0 BRIDGE_CONTROL=40:40
sleep 0.12
setpci -s 00:03.0 BRIDGE_CONTROL=00:40
echo "[*] spamming keys to stop autoboot (~12s)..."
t=0
while [ $t -lt 120 ]; do
  printf ' ' > $DEV       # any key stops Octeon autoboot
  sleep 0.1
  t=$((t+1))
done
echo "[*] sending printenv + bdinfo..."
printf '\r\n' > $DEV; sleep 0.5
printf 'printenv\r\n' > $DEV; sleep 1.5
printf 'bdinfo\r\n' > $DEV; sleep 1.5
wait $LP
echo "==================== CONSOLE LOG ===================="
cat -v $LOG
echo "==================== END ===================="
