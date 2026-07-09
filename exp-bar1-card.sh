#!/bin/sh
# EXPERIMENT 1 (card side): program BAR1 inbound window index0 to map host BAR2
# offset 0 -> card DRAM phys 0x02000000, after planting a magic value there.
# If the host can then read that magic through resource2, the "all-0xFF BAR wall"
# is broken from the card side. Leaves the card at the u-boot prompt (no reset
# after) so the host read sees the programmed window.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-exp1.log
BAUD=115200
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV $BAUD raw -echo 2>/dev/null
: > $LOG
( timeout 45 cat $DEV > $LOG ) & LP=$!
sleep 0.2
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.2}"; }
cmd "write64 0x0000000002000000 0xCAFEBABEDEADBEEF"
cmd "read64  0x0000000002000000"
cmd "write64 0x00011800C00000A8 0x8B"
cmd "read64  0x00011800C00000A8"
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== EXP1 CARD ===================="
cat -v $LOG | grep -v "^Octeon snic10e# $" | tail -40
echo "==================== END ===================="
