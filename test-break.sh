#!/bin/bash
# test-break.sh — manda un BREAK (segnale elettrico grezzo, forza linea a 0 per un tot)
# invece di byte veri. Molti u-boot fermano l'autoboot anche solo su BREAK.
# Se questo funziona ma i byte normali no = problema framing/baud.
# Se anche questo fallisce = linea TX->pin4 elettricamente morta.
set -u
DEV=${DEV:-/dev/ttyUSB0}
TMPD=/run/cavium; mkdir -p "$TMPD" 2>/dev/null || TMPD=/tmp
CARD=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
BRIDGE=$(basename "$(dirname "$(readlink -f /sys/bus/pci/devices/0000:$CARD)")" | sed 's/^0000://')

fuser -k -9 "$DEV" 2>/dev/null; sleep 0.4
timeout 3 stty -F "$DEV" 115200 cs8 -cstopb -parenb clocal -crtscts raw -echo

echo "[trace] reset card..."
( timeout 25 cat "$DEV" > "$TMPD/brk_boot.txt" 2>&1 ) &
setpci -s "$BRIDGE" BRIDGE_CONTROL=40:40; sleep 0.2; setpci -s "$BRIDGE" BRIDGE_CONTROL=00:40
sleep 1

echo "[trace] sending BREAK signal repeatedly during boot banner+countdown (~12s)..."
for i in $(seq 1 15); do
  python3 -c "
import fcntl,time
TIOCSBRK=0x5427; TIOCCBRK=0x5428
f=open('$DEV','wb')
fcntl.ioctl(f.fileno(), TIOCSBRK)
time.sleep(0.25)
fcntl.ioctl(f.fileno(), TIOCCBRK)
f.close()
"
  sleep 0.5
done

sleep 8
echo "=== full captured output ==="
tr -d '\r' < "$TMPD/brk_boot.txt"
