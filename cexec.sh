#!/bin/bash
# Reliable card serial exec: persistent background reader for whole sequence.
# Usage: sudo cexec.sh 'cmd1' 'cmd2' ...   (each sent with 1.2s gap)
DEV=${DEV:-/dev/ttyUSB0}
BAUD=${BAUD:-115200}
mkdir -p /tmp/claude-1000
fuser -k "$DEV" 2>/dev/null; pkill -9 -f "cat $DEV" 2>/dev/null; sleep 0.3
stty -F "$DEV" "$BAUD" cs8 -cstopb -parenb clocal -crtscts raw -echo 2>/dev/null || exit 1
LOG=/tmp/claude-1000/cexec.log; : > "$LOG"
cat "$DEV" > "$LOG" 2>/dev/null &
RP=$!
sleep 0.4
printf '\r\n' > "$DEV"; sleep 0.5
for c in "$@"; do
  printf '%s\r\n' "$c" > "$DEV"
  sleep 1.6
done
sleep 1.2
kill $RP 2>/dev/null
tr -d '\r' < "$LOG"
