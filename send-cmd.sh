#!/bin/sh
# Send u-boot commands to a card ALREADY sitting at the prompt (NO reset/SBR),
# so previously-programmed PEM/BAR state is preserved. Usage: sudo send-cmd.sh
# "cmd1" "cmd2" ...
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-send.log
BAUD=115200
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > $LOG
( timeout 30 cat $DEV > $LOG ) & LP=$!
sleep 0.3
printf '\r\n' > $DEV; sleep 0.5
for c in "$@"; do printf '%s\r\n' "$c" > $DEV; sleep 1.2; done
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== SEND-CMD ===================="
cat -v $LOG | grep -v "^Octeon snic10e# $" | tail -40
echo "==================== END ===================="
