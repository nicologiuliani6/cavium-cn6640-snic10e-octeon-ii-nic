#!/bin/bash
# test-txrx.sh — reset card, lascia bootare app vendor fino alla sua console,
# poi manda comando e guarda se echo/risposta arriva. Testa RX reale senza race.
set -u
DEV=${DEV:-/dev/ttyUSB0}
TMPD=/run/cavium; mkdir -p "$TMPD" 2>/dev/null || TMPD=/tmp
CARD=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
BRIDGE=$(basename "$(dirname "$(readlink -f /sys/bus/pci/devices/0000:$CARD)")" | sed 's/^0000://')

fuser -k -9 "$DEV" 2>/dev/null; sleep 0.4
timeout 3 stty -F "$DEV" 115200 cs8 -cstopb -parenb clocal -crtscts raw -echo

echo "[trace] reset + wait full boot to vendor console (no key-race, just let it boot)..."
( timeout 25 cat "$DEV" > "$TMPD/txrx_boot.txt" 2>&1 ) &
setpci -s "$BRIDGE" BRIDGE_CONTROL=40:40; sleep 0.2; setpci -s "$BRIDGE" BRIDGE_CONTROL=00:40
sleep 20   # let it fully autoboot to PP0:~CONSOLE-> undisturbed

echo "[trace] now sending test command 'help' to vendor console..."
( timeout 5 cat "$DEV" > "$TMPD/txrx_reply.txt" 2>&1 ) &
sleep 0.2
printf 'help\r\n' > "$DEV"
sleep 4

echo "=== boot phase (last 5 lines) ==="
tr -d '\r' < "$TMPD/txrx_boot.txt" | tail -5
echo "=== reply to 'help' (raw, should show echo/response if RX works) ==="
cat -A "$TMPD/txrx_reply.txt" | head -20
