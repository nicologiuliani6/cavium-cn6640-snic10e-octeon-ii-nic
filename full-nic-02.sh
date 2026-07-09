#!/bin/sh
# Stock liquidio bring-up, adapted to current topology: card 02:00.0, bridge 00:01.1.
# 1) SBR reset -> u-boot; 2) loady NIC firmware ELF to RAM; 3) restore host BARs +
# insmod patched liquidio (preloaded=1) in bg so it programs host DMA queues;
# 4) bootoct firmware into ready queues -> firmware<->driver handshake; 5) report.
DEV=/dev/ttyUSB0
ELF=/srv/tftp/lio_210sv.elf
LB=/home/nico/Desktop/cavium/lio-build/drivers/net/ethernet/cavium/liquidio
CARD=02:00.0
BRIDGE=00:01.1
SLOG=/home/nico/Desktop/cavium/.cav-full02.log
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
pkill -9 -f 'cat /dev/ttyUSB0' 2>/dev/null
rmmod liquidio 2>/dev/null; rmmod liquidio_core 2>/dev/null
rmmod octwin 2>/dev/null
stty -F $DEV 115200 raw -echo 2>/dev/null
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.0}"; }

echo "[1] SBR reset -> u-boot (bridge $BRIDGE)"
setpci -s $BRIDGE BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s $BRIDGE BRIDGE_CONTROL=00:40
# spam CR to interrupt u-boot autoboot
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
# confirm prompt
( timeout 3 cat $DEV > /tmp/claude-1000/ub.txt 2>&1 ) & printf '\r\n' > $DEV; sleep 3
grep -qi 'snic10e#\|Octeon' /tmp/claude-1000/ub.txt && echo "    u-boot prompt OK" || echo "    WARN: no prompt (tail: $(tail -c80 /tmp/claude-1000/ub.txt))"

cmd "setenv bootdelay -1"
cmd "write64 0x00011800C0000080 0x00000000F8000000"
cmd "write64 0x00011800C0000088 0x00000000F4000000"

echo "[2] loady firmware to RAM 0x21000000 (sb owns serial)"
printf 'loady 0x21000000\r\n' > $DEV; sleep 2
sb "$ELF" < $DEV > $DEV
echo "    sb exit $?"; sleep 2

echo "[3] restore host config BARs ($CARD) + insmod driver (bg)"
setpci -s $CARD BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s $CARD BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s $CARD COMMAND=0x06   # mem + BUS MASTER: firmware CORE_DRV_ACTIVE is an OQ DMA into host RAM
echo "    BAR0=$(setpci -s $CARD BASE_ADDRESS_0) BAR2=$(setpci -s $CARD BASE_ADDRESS_2) CMD=$(setpci -s $CARD COMMAND)"
dmesg -C
insmod "$LB/liquidio-core.ko" 2>&1
insmod "$LB/liquidio.ko" preloaded=1 fw_type=nic 2>&1 &
INS=$!
echo "[3] driver probing, 7s to program queues..."
sleep 7

echo "[4] bootoct firmware"
: > $SLOG
( timeout 45 cat $DEV > $SLOG 2>&1 ) &
sleep 0.3
printf 'bootoct 0x21000000 coremask=0xff\r\n' > $DEV
echo "[4] waiting driver handshake (up to 40s)..."
wait $INS 2>/dev/null
sleep 5

echo "==================== HOST DMESG ===================="
dmesg | grep -iE 'liquidio|octeon|nic|firmware|drv|CORE_DRV|link|queue' | tail -45
echo "==================== NETDEVS ===================="
ls /sys/class/net/ | grep -vE 'veth|docker|br-|^lo$|wlx|enp7|eno' || echo "(none new)"
echo "==================== CARD FW CONSOLE (tail) ===================="
tail -20 $SLOG
echo "==================== END ===================="