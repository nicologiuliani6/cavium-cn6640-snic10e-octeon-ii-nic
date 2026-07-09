#!/bin/sh
# Boot the snic10e OpenWrt image from RAM, but FIRST program the PEM inbound
# windows in u-boot. Test whether the windows survive the Linux boot (i.e. whether
# the octeon kernel leaves the EP PEM alone). If host BAR access still works after
# Linux is up + xaui0/1 at 10G, then the card can be a NIC without a card-side
# module doing the window setup.
DEV=/dev/ttyUSB0
IMG=/home/nico/openwrt/bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin
ADDR=0x20000000
LOG=/home/nico/Desktop/cavium/.cav-owrtwin.log
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV 115200 raw -echo 2>/dev/null

echo "[*] reset to u-boot prompt..."
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5

echo "[*] program PEM inbound windows before boot..."
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.0}"; }
cmd "write64 0x00011800C0000080 0x00000000F8000000"   # P2N_BAR0_START
cmd "write64 0x00011800C0000088 0x00000000F4000000"   # P2N_BAR1_START
cmd "write64 0x0000000002000000 0xCAFEBABEDEADBEEF"   # magic in DRAM
cmd "write64 0x00011800C00000A8 0x8B"                 # BAR1_INDEX0 -> DRAM 0x02000000

echo "[*] loady $ADDR at 115200 (~30 min)..."
printf 'loady %s\r\n' "$ADDR" > $DEV
sleep 2
sb -q "$IMG" < $DEV > $DEV
echo "[*] ymodem done exit $?"
sleep 2

echo "[*] bootoctlinux..."
: > $LOG
( timeout 90 cat $DEV > $LOG ) & LP=$!
sleep 0.3
printf 'bootoctlinux %s numcores=8 endbootargs console=ttyS0,115200\r\n' "$ADDR" > $DEV
sleep 88
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== BOOT (last 40) ===================="
cat -v $LOG | tail -40
echo "==================== END ===================="
