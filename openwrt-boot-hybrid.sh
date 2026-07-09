#!/bin/sh
# Transfer image at 921600 (fast), boot Linux with console=115200 (reliable read).
# No u-boot baud switch-back (that hung). bootoctlinux sent at 921600; Linux then
# talks 115200.
DEV=/dev/ttyUSB0
IMG=/home/nico/openwrt/bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin
ADDR=0x20000000
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

echo "[*] reset -> u-boot @115200"
stty -F $DEV 115200 raw -echo 2>/dev/null
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 220 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5

echo "[*] u-boot baud -> 921600 for transfer"
printf 'setenv baudrate 921600\r\n' > $DEV
sleep 1.2
stty -F $DEV 921600 raw -echo 2>/dev/null
printf '\r\n' > $DEV; sleep 0.6; printf '\r\n' > $DEV; sleep 0.5

echo "[*] loady @921600 (~4 min)"
printf 'loady %s\r\n' "$ADDR" > $DEV
sleep 2
sb "$IMG" < $DEV > $DEV
echo "[*] sb exit $?"
sleep 2

echo "[*] bootoctlinux (cmd @921600, Linux console=115200)"
printf 'bootoctlinux %s numcores=8 endbootargs console=ttyS0,115200\r\n' "$ADDR" > $DEV
sleep 3
stty -F $DEV 115200 raw -echo 2>/dev/null      # Linux talks 115200 now
LOG=/home/nico/Desktop/cavium/.hybridboot.log
: > $LOG
( timeout 75 cat $DEV > $LOG 2>&1 ) & C=$!
sleep 73
kill $C 2>/dev/null; wait $C 2>/dev/null
echo "==================== HYBRIDBOOT ===================="
cat -v $LOG | tr -d '\000' | grep -aE 'xaui0: 10000|Please press|Linux version|panic' | tail -8
echo "==================== END ===================="
