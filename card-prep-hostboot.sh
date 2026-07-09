#!/bin/sh
# Put the card at the u-boot prompt in HOST-BOOT mode for the liquidio driver's
# normal download flow: stay at prompt (bootdelay -1) so u-boot answers the
# host PCI-console bootloader handshake, and program the PEM inbound windows so
# the host BAR0/BAR2 access (used by the driver to download firmware + poke CSRs)
# works. Leaves the card at the prompt (no boot).
DEV=/dev/ttyUSB0
BAUD=115200
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > /home/nico/Desktop/cavium/.cav-prep.log
( timeout 40 cat $DEV > /home/nico/Desktop/cavium/.cav-prep.log ) & LP=$!
sleep 0.2
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.0}"; }
cmd "setenv bootdelay -1"
cmd "write64 0x00011800C0000080 0x00000000F8000000"   # P2N_BAR0_START
cmd "write64 0x00011800C0000088 0x00000000F4000000"   # P2N_BAR1_START
cmd "read64  0x00011800C0000080"
cmd "read64  0x00011800C0000088"
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== CARD PREP ===================="
cat -v /home/nico/Desktop/cavium/.cav-prep.log | grep -v "^Octeon snic10e# $" | tail -25
echo "==================== END ===================="
