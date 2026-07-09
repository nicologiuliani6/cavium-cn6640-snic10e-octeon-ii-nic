#!/bin/sh
# Send one u-boot command to the card and print the reply.
# Card must already be at the "Octeon snic10e#" prompt (do NOT reset).
# Usage: sudo sh cmd.sh "printenv"
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-cmd.log
BAUD=115200
WAIT=${WAIT:-3}
if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > $LOG
( timeout $((WAIT+1)) cat $DEV > $LOG ) &
LP=$!
sleep 0.3
printf '\r\n' > $DEV        # wake prompt
sleep 0.3
printf '%s\r\n' "$*" > $DEV # the command
sleep "$WAIT"
kill $LP 2>/dev/null; wait $LP 2>/dev/null
cat -v $LOG
