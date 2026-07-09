#!/bin/sh
# EXPERIMENT 2 (card side): full inbound-window bring-up for the 64MB BAR1 window.
#   PEMX_P2N_BAR1_START(0x88) = 0xf4000000  -> match host's assigned BAR2 base
#   DRAM 0x02000000 = magic
#   PEMX_BAR1_INDEX0(0xA8)   = 0x8B         -> BAR1 off0 -> DRAM 0x02000000, enable
# BAR_CTL already has bar1_siz=1 (64M) so no change. Leaves card at prompt.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.cav-exp2.log
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
cmd "write64 0x00011800C0000088 0x00000000F4000000"
cmd "read64  0x00011800C0000088"
cmd "write64 0x0000000002000000 0xCAFEBABEDEADBEEF"
cmd "write64 0x00011800C00000A8 0x8B"
cmd "read64  0x00011800C00000A8"
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== EXP2 CARD ===================="
cat -v $LOG | grep -v "^Octeon snic10e# $" | tail -40
echo "==================== END ===================="
