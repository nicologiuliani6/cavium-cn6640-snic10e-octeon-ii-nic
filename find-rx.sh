#!/bin/sh
# Find Octeon card RX pin: reset, hammer keys, detect if autoboot stopped.
# Keep RXD on card TX (reads). Move TXD across other J1 pins. GND on bracket.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-rx.log
BAUD=115200
if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > $LOG
( timeout 20 cat $DEV > $LOG ) &
LP=$!
sleep 0.2
echo "[*] reset + hammering keys ~16s..."
setpci -s 00:03.0 BRIDGE_CONTROL=40:40
sleep 0.12
setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0
while [ $t -lt 320 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
# if we got a prompt, ask for env
printf 'printenv\r\n' > $DEV; sleep 1.5
wait $LP
echo "==================================================="
if grep -q "boot-app" $LOG; then
  echo ">>> FAIL: autoboot still ran (loaded boot-app). TXD pin is NOT card RX."
  echo ">>> Move TXD to the next J1 pin and run again."
else
  echo ">>> PASS?? autoboot did NOT load boot-app — likely STOPPED. Check log below for prompt."
fi
echo "----- tail of log -----"
cat -v $LOG | tail -25
