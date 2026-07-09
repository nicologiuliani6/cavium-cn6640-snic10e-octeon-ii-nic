#!/bin/sh
# Continuous Octeon serial scan - NO keypress needed.
# Hold adapter RXD on one J1 pin at a time. GND on PCIe bracket. TXD unplugged.
# Watch the byte counts: when a pin shows >0 bytes + text, that's card TX.
# Stop with Ctrl-C.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-serial.log
BAUD=115200
trap 'echo; echo "stopped"; exit 0' INT TERM

if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
stty -F $DEV $BAUD raw -echo 2>/dev/null

echo "############################################################"
echo "# CONTINUOUS SCAN @ $BAUD  (Ctrl-C to stop)"
echo "# GND -> PCIe metal bracket | TXD -> unplugged"
echo "# Slowly hold RXD on each J1 pin; watch for bytes>0 + text"
echo "############################################################"

i=0
while true; do
  i=$((i+1))
  : > $LOG
  ( timeout 3 cat $DEV > $LOG ) &
  LP=$!
  sleep 0.2
  setpci -s 00:03.0 BRIDGE_CONTROL=40:40   # reset card -> u-boot prints
  sleep 0.12
  setpci -s 00:03.0 BRIDGE_CONTROL=00:40
  sleep 0.2
  printf "\r\n" > $DEV
  wait $LP
  n=$(wc -c < $LOG)
  if [ "$n" -gt 0 ]; then
    echo "===== cycle $i: $n BYTES !!! ====="
    cat -v $LOG | head -30
    echo "----- hex -----"
    head -c 80 $LOG | xxd
    echo "===== ^ THIS PIN = card TX. Keep it. Tell Claude. ====="
  else
    echo "cycle $i: 0 bytes (move/hold next pin)"
  fi
done
