#!/bin/sh
# Read-only recon of the Octeon SLI/PEM endpoint state from the CARD side via
# u-boot read64. Tells us whether the OEM firmware left the BAR1 inbound windows
# disabled (why the host sees all-0xFF). Zero risk: only reads CSRs.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-recon.log
BAUD=115200
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > $LOG
( timeout 60 cat $DEV > $LOG ) & LP=$!
sleep 0.2
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
send(){ printf 'echo %s\r\n' "$1" > $DEV; sleep 0.6; printf '%s\r\n' "$2" > $DEV; sleep 1.0; }
send TAG_BAR1_IDX0   "read64 0x00011800C00000A8"
send TAG_BAR1_IDX1   "read64 0x00011800C00000B0"
send TAG_BAR1_IDX2   "read64 0x00011800C00000B8"
send TAG_MAC_NUMBER  "read64 0x00011F0000013E00"
send TAG_CTL_STATUS  "read64 0x00011F0000010570"
send TAG_S2M_PORT0   "read64 0x00011F0000013D80"
send TAG_S2M_PORT1   "read64 0x00011F0000013D90"
send TAG_CTL_PORT0   "read64 0x00011F0000010050"
send TAG_DONE        "version"
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== CSR RECON ===================="
cat -v $LOG | grep -v "^Octeon snic10e# $" | tail -80
echo "==================== END ===================="
