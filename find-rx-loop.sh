#!/bin/sh
# Loop until card RX found. Keep RXD on card TX (reads). GND on bracket.
# Slowly move TXD across J1 pins; watch FAIL/PASS each cycle.
# On PASS: autoboot stopped -> dumps printenv and exits. Ctrl-C to stop.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-rx.log
BAUD=115200
trap 'echo; echo stopped; exit 0' INT TERM
if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
stty -F $DEV $BAUD raw -echo 2>/dev/null

echo "############################################################"
echo "# RX-FINDER LOOP @ $BAUD  (Ctrl-C to stop)"
echo "# RXD on card TX (fixed) | GND bracket | move TXD pin by pin"
echo "############################################################"

i=0
while true; do
  i=$((i+1))
  : > $LOG
  ( timeout 20 cat $DEV > $LOG ) &
  LP=$!
  sleep 0.2
  setpci -s 00:03.0 BRIDGE_CONTROL=40:40
  sleep 0.12
  setpci -s 00:03.0 BRIDGE_CONTROL=00:40
  # hammer Enter through the whole autoboot window
  t=0
  while [ $t -lt 280 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
  printf 'printenv\r\n' > $DEV; sleep 1.2
  kill $LP 2>/dev/null; wait $LP 2>/dev/null
  if grep -q "boot-app" $LOG; then
    echo "cycle $i: FAIL (autoboot ran) -> move TXD to next J1 pin"
  else
    echo ""
    echo "############################################################"
    echo "# cycle $i: PASS !!!  autoboot STOPPED -> card RX = current TXD pin"
    echo "############################################################"
    echo "----- console log -----"
    cat -v $LOG | tail -40
    echo "----- END. Keep TXD on this pin. Tell Claude. -----"
    exit 0
  fi
done
