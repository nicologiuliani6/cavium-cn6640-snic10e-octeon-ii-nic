#!/bin/sh
# Single-process ordered-preloaded attempt (no two-script flag race).
# One owner of the serial port, used sequentially. Steps:
#  1) reset -> u-boot, set PEM windows + bootdelay -1
#  2) loady the NIC firmware into RAM (sb owns serial, no competing reader)
#  3) restore host config BARs, insmod liquidio preloaded=1 in background
#     (driver programs the host DMA queues, then waits 30s for the firmware)
#  4) bootoct the firmware so it comes up into the ready queues -> CORE_DRV_ACTIVE
#  5) report dmesg + netdevs
DEV=/dev/ttyUSB0
ELF=/srv/tftp/lio_210sv.elf
LB=/home/nico/Desktop/cavium/lio-build/drivers/net/ethernet/cavium/liquidio
SLOG=/home/nico/Desktop/cavium/.cav-full.log
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
pkill -9 -f 'cat /dev/ttyUSB0' 2>/dev/null
rmmod liquidio 2>/dev/null; rmmod liquidio_core 2>/dev/null
stty -F $DEV 115200 raw -echo 2>/dev/null
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.0}"; }

echo "[1] reset -> u-boot"
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 220 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
cmd "setenv bootdelay -1"
cmd "write64 0x00011800C0000080 0x00000000F8000000"
cmd "write64 0x00011800C0000088 0x00000000F4000000"

echo "[2] loady firmware to RAM (sb owns serial)"
printf 'loady 0x21000000\r\n' > $DEV; sleep 2
sb "$ELF" < $DEV > $DEV
echo "    sb exit $?"; sleep 2

echo "[3] restore host config BARs + insmod driver (bg)"
setpci -s 04:00.0 BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s 04:00.0 BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s 04:00.0 COMMAND=0x02
dmesg -C
insmod "$LB/liquidio-core.ko"
insmod "$LB/liquidio.ko" preloaded=1 fw_type=nic &
INS=$!
echo "[3] driver probing, wait 7s to program queues..."
sleep 7

echo "[4] bootoct firmware into ready queues"
: > $SLOG
( timeout 40 cat $DEV > $SLOG 2>&1 ) & C=$!
sleep 0.3
printf 'bootoct 0x21000000 coremask=0xff\r\n' > $DEV
echo "[4] waiting for driver handshake..."
wait $INS 2>/dev/null
sleep 3
kill $C 2>/dev/null; wait $C 2>/dev/null

echo "==================== DMESG ===================="
dmesg | grep -iE 'liquidio|octeon|core|nic|firmware|drv|eth' | tail -40
echo "==================== NETDEVS ===================="
ls /sys/class/net/ | grep -vE 'veth|docker|br-|^lo$|wlx|enp7' || echo "(no new eth)"
echo "==================== FW CONSOLE (bootoct) ===================="
cat -v $SLOG | grep -vE '^Octeon snic10e# $' | tail -20
echo "==================== END ===================="
