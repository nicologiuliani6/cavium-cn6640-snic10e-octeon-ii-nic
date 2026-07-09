#!/bin/sh
# Reset card, stop autoboot, run a u-boot command, dump FULL raw reply.
# Usage: sudo sh run-cmd.sh "printenv"     (WAIT=6 sudo sh run-cmd.sh "..." for slow cmds)
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-run.log
BAUD=115200
WAIT=${WAIT:-5}
if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
CMD="$*"
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > $LOG
( timeout 35 cat $DEV > $LOG ) &
LP=$!
sleep 0.2
setpci -s 00:03.0 BRIDGE_CONTROL=40:40
sleep 0.12
setpci -s 00:03.0 BRIDGE_CONTROL=00:40
# hammer Enter ~12s to stop autoboot, then STOP hammering
t=0
while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5                      # let prompt settle, stop sending
# send the command once, give it time
if [ -n "$CMD" ]; then
  printf '%s\r\n' "$CMD" > $DEV
  sleep "$WAIT"
fi
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== FULL LOG (last 120 lines) ===================="
cat -v $LOG | grep -v "^Octeon snic10e# $" | tail -120
echo "==================== END ===================="
