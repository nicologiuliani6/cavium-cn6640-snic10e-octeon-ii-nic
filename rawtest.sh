#!/bin/sh
# Test raw: configura porta, scrive HELLO, legge indietro in hex. Bypassa loopback.sh.
DEV=/dev/ttyACM0
[ "$(id -u)" = 0 ] || { echo "usa: sudo sh rawtest.sh"; exit 1; }

stty -F "$DEV" 115200 cs8 -cstopb -parenb clocal -crtscts raw -echo
echo "### porta configurata, leggo 3s mentre scrivo HELLO ###"
timeout 3 cat "$DEV" | xxd &
sleep 0.3
printf 'HELLO\r' > "$DEV"
wait
echo "### fine, se sopra vuoto -> niente torna ###"
