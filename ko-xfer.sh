#!/bin/sh
# Transfer a file to the card's running OpenWrt over the serial console using
# base64 (busybox), then verify md5. Usage: sudo ko-xfer.sh <localfile> <cardpath>
DEV=/dev/ttyUSB0
SRC="$1"; DST="${2:-/tmp/mod.ko}"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$SRC" ] || { echo "no such file $SRC"; exit 1; }
pkill -9 -f 'cat /dev/ttyUSB0' 2>/dev/null
stty -F $DEV ${BAUD:-921600} raw -echo 2>/dev/null
B64=$(mktemp); gzip -c "$SRC" | base64 > "$B64"   # gzip first: ~3x fewer lines over serial
HOSTMD5=$(md5sum "$SRC" | awk '{print $1}')
echo "[xfer] $SRC ($(wc -c <"$SRC") bytes) -> card:$DST  md5=$HOSTMD5  b64lines=$(wc -l <"$B64")"

# wake shell
printf '\r\n' > $DEV; sleep 0.4
# start receiver
printf 'base64 -d | gzip -d > %s\r\n' "$DST" > $DEV; sleep 0.6
# stream base64 (paced per line to avoid octeon serial RX drops)
while IFS= read -r line; do
  printf '%s\n' "$line" > $DEV
  sleep 0.004
done < "$B64"
sleep 0.5
printf '\004' > $DEV        # Ctrl-D -> EOF to base64
sleep 1.5
# verify
: > /tmp/claude-1000/xfer-verify.log
( timeout 6 cat $DEV > /tmp/claude-1000/xfer-verify.log ) &
sleep 0.3
printf 'md5sum %s\r\n' "$DST" > $DEV; sleep 2
wait 2>/dev/null
rm -f "$B64"
echo "==== card md5 (want $HOSTMD5) ===="
cat -v /tmp/claude-1000/xfer-verify.log | tail -6
