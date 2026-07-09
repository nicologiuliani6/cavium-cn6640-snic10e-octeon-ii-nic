#!/bin/sh
# Prep for the "correct ordering" preloaded attempt: put the card at the u-boot
# prompt with the PEM windows set AND the NIC firmware pre-loaded into RAM at
# 0x21000000 (via ymodem) but NOT booted. The host then loads the liquidio driver
# (preloaded=1) so it programs the host DMA queues, and only AFTER that we
# 'bootoct' the firmware (separate step) so it comes up with the queues already
# set up and sends CORE_DRV_ACTIVE into a ready DROQ.
DEV=/dev/ttyUSB0
ELF=/srv/tftp/lio_210sv.elf
BAUD=115200
ADDR=0x21000000
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV $BAUD raw -echo 2>/dev/null

echo "[*] SBR reset -> u-boot prompt"
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.0}"; }
echo "[*] bootdelay -1 + PEM windows"
cmd "setenv bootdelay -1"
cmd "write64 0x00011800C0000080 0x00000000F8000000"
cmd "write64 0x00011800C0000088 0x00000000F4000000"
echo "[*] loady firmware into RAM at $ADDR (~100s), NOT booting"
printf 'loady %s\r\n' "$ADDR" > $DEV
sleep 2
sb -q "$ELF" < $DEV > $DEV
echo "[*] ymodem done exit $?  (firmware now in RAM, card at prompt)"
sleep 1
