#!/bin/bash
# RX check semplice. Manda \r ogni giro, conta byte che tornano dalla card.
# Fili OK + card viva  -> "RX OK  N byte" (prompt/echo torna indietro)
# Filo staccato / GND   -> "RX -- niente"
DEV=/dev/ttyUSB0
stty -F "$DEV" 115200 raw -echo 2>/dev/null || { echo "porta $DEV assente"; exit 1; }
echo ">>> RX check $DEV @115200 - muovi i fili, Ctrl-C stop <<<"
n=0
while true; do
  n=$((n+1))
  printf 'version\r' > "$DEV" 2>/dev/null       # sollecita output card
  b=$(timeout 1 head -c 400 "$DEV" 2>/dev/null | wc -c)
  if [ "$b" -gt 0 ]; then echo "[$n] RX OK   $b byte"; else echo "[$n] RX --   niente"; fi
  sleep 0.2
done
