#!/bin/sh
# Load OpenWrt octeon initramfs ELF via u-boot loady at 115200 (no baud switch,
# reliable - mirrors the working load-fw.sh) and boot with bootoctlinux + console.
# Runs from RAM (initramfs) - non-destructive. Transfer ~37 min at 115200.
DEV=/dev/ttyUSB0
IMG=/home/nico/openwrt/bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin
ADDR=0x20000000
LOG=/home/nico/Desktop/cavium/.cav-owrt.log
if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
stty -F $DEV 115200 raw -echo 2>/dev/null

echo "[*] reset card to u-boot prompt..."
setpci -s 00:01.1 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:01.1 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1

echo "[*] loady $ADDR at 115200 (no baud switch) ..."
printf 'loady %s\r\n' "$ADDR" > $DEV
sleep 2
echo "[*] ymodem send 25MB (~37 min)..."
sb -q "$IMG" < $DEV > $DEV
echo "[*] ymodem done exit $?"
sleep 2

echo "[*] bootoctlinux (capture first)..."
: > $LOG
( timeout 50 cat $DEV > $LOG ) &
LP=$!
sleep 0.3
printf 'bootoctlinux %s numcores=8 endbootargs console=ttyS0,115200\r\n' "$ADDR" > $DEV
sleep 48
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== OPENWRT BOOT ===================="
cat -v $LOG | grep -v "^Octeon snic10e# $" | tail -70
echo "==================== END ===================="
