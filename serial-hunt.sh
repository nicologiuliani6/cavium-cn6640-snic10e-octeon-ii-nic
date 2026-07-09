#!/bin/bash
# Live serial link hunter. Prints a status line every ~1s while you wiggle the J1 wires.
echo ">>> HUNT seriale /dev/ttyUSB0 @115200 - muovi i fili, Ctrl-C per fermare <<<"
DEV=/dev/ttyUSB0
fuser -k -9 "$DEV" 2>/dev/null
sleep 0.3
stty -F "$DEV" 115200 raw -echo 2>/dev/null
n=0
while true; do
  n=$((n+1))
  printf '\r\r\r' > "$DEV" 2>/dev/null
  data=$(timeout 1 cat "$DEV" 2>/dev/null)
  tot=${#data}
  clean=$(printf '%s' "$data" | tr -cd '[:print:]')
  cl=${#clean}
  if [ "$tot" -eq 0 ]; then
    echo "[$n] NIENTE .......... GND staccato? fili non toccano?"
  elif [ "$cl" -lt $(( tot / 2 )) ]; then
    echo "[$n] GARBAGE ($tot B) .. SCAMBIA pin4<->pin5"
  else
    echo "[$n] *** LINK OK *** :  $(printf '%s' "$data" | tr -d '\r\n' | cut -c1-55)"
  fi
done
