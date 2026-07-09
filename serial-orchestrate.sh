#!/bin/sh
# Sole owner of the serial port for the ordered-preloaded attempt.
# CRITICAL: no background reader (cat) may run during the ymodem `sb` transfer,
# or it steals the 'C' handshake bytes and the transfer fails. So we only capture
# the serial in short timed bursts, never during sb.
DEV=/dev/ttyUSB0
ELF=/srv/tftp/lio_210sv.elf
LOG=/home/nico/Desktop/cavium/.cav-orch.log
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
rm -f /tmp/claude-1000/CARD_READY /tmp/claude-1000/GO_BOOTOCT
stty -F $DEV 115200 raw -echo 2>/dev/null
: > $LOG
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.0}"; }

echo "[orch] reset -> u-boot (no reader; just hammer Enter)"
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 220 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
cmd "setenv bootdelay -1"
cmd "write64 0x00011800C0000080 0x00000000F8000000"
cmd "write64 0x00011800C0000088 0x00000000F4000000"

echo "[orch] loady firmware to RAM (sb OWNS serial, no competing reader)"
printf 'loady 0x21000000\r\n' > $DEV
sleep 2
sb "$ELF" < $DEV > $DEV
echo "[orch] sb exit $?"
sleep 2

echo "[orch] verify ELF magic in RAM"
( timeout 5 cat $DEV > $LOG 2>&1 ) & V=$!
sleep 0.3
printf '\r\n' > $DEV; sleep 0.3
printf 'read64 0x0000000021000000\r\n' > $DEV; sleep 2
kill $V 2>/dev/null; wait $V 2>/dev/null
echo "---- verify ----"; cat -v $LOG | grep -vE '^Octeon snic10e# $' | tail -6

echo "[orch] CARD_READY; waiting host GO..."
touch /tmp/claude-1000/CARD_READY
w=0; while [ ! -f /tmp/claude-1000/GO_BOOTOCT ] && [ $w -lt 240 ]; do sleep 0.5; w=$((w+1)); done

echo "[orch] bootoct now"
: > $LOG
( timeout 30 cat $DEV > $LOG 2>&1 ) & C=$!
sleep 0.3
printf 'bootoct 0x21000000 coremask=0xff\r\n' > $DEV
sleep 28
kill $C 2>/dev/null; wait $C 2>/dev/null
echo "==================== ORCH BOOTOCT SERIAL ===================="
cat -v $LOG | grep -vE '^Octeon snic10e# $' | tail -40
echo "==================== END ===================="
