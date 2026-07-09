#!/bin/sh
# Boot the snic10e OpenWrt initramfs from RAM but at 921600 baud (8x faster than
# 115200 -> ~4min instead of ~30min). Session-only baud change (no saveenv), so a
# power cycle reverts to 115200. Falls nothing to flash; NOR/OEM untouched.
DEV=/dev/ttyUSB0
IMG=/home/nico/openwrt/bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin
ADDR=0x20000000
BR=921600
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

echo "[*] reset -> u-boot @115200"
stty -F $DEV 115200 raw -echo 2>/dev/null
setpci -s 00:01.1 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:01.1 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 220 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5

echo "[*] switch console to $BR"
printf 'setenv baudrate %s\r\n' "$BR" > $DEV
sleep 1.2
stty -F $DEV $BR raw -echo 2>/dev/null      # match new baud
printf '\r\n' > $DEV                          # u-boot 'press ENTER' to confirm
sleep 0.8
printf '\r\n' > $DEV
sleep 0.5

echo "[*] loady $ADDR @${BR} (~4 min)"
printf 'loady %s\r\n' "$ADDR" > $DEV
sleep 2
sb "$IMG" < $DEV > $DEV
echo "[*] sb exit $?"
sleep 2

echo "[*] bootoctlinux @${BR}"
LOG=/home/nico/Desktop/cavium/.cav-fastboot.log
: > $LOG
( timeout 60 cat $DEV > $LOG 2>&1 ) & C=$!
sleep 0.3
printf 'bootoctlinux %s numcores=8 endbootargs console=ttyS0,%s\r\n' "$ADDR" "$BR" > $DEV
sleep 58
kill $C 2>/dev/null; wait $C 2>/dev/null
echo "==================== FASTBOOT ===================="
cat -v $LOG | tail -20
echo "==================== END ===================="
