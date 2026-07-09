#!/bin/sh
# Read-only recon of the PEM (PCIe MAC) inbound BAR config from the card side.
# Checks whether PEMX_P2N_BAR1_START matches the host-assigned BAR2 base
# (0xf4000000) and whether BAR_CTL enables the index-mapped BAR1 window.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-reconpem.log
BAUD=115200
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > $LOG
( timeout 55 cat $DEV > $LOG ) & LP=$!
sleep 0.2
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
r(){ printf 'echo %s\r\n' "$1" > $DEV; sleep 0.5; printf 'read64 %s\r\n' "$2" > $DEV; sleep 0.9; }
r PEM_CTL_STATUS   0x00011800C0000000
r PEM_BAR_CTL      0x00011800C0000128
r PEM_BAR2_MASK    0x00011800C0000130
r PEM_P2N_BAR0     0x00011800C0000080
r PEM_P2N_BAR1     0x00011800C0000088
r PEM_P2N_BAR2     0x00011800C0000090
r PEM_BAR1_IDX0    0x00011800C00000A8
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== PEM RECON ===================="
cat -v $LOG | grep -v "^Octeon snic10e# $" | tail -70
echo "==================== END ===================="
