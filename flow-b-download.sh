#!/bin/sh
# Flow B: canonical liquidio driver-download. Card sits at u-boot; driver uses the
# bootloader PCI console (0x0006c000) to divert u-boot, then downloads + boots the
# firmware itself with driver-provided config -> firmware and driver are fully synced.
# No manual bootoct, no preloaded. This is the vendor-intended bring-up.
DEV=/dev/ttyUSB0
LB=/home/nico/Desktop/cavium/lio-build/drivers/net/ethernet/cavium/liquidio
CARD=02:00.0
BRIDGE=00:01.1
SLOG=/home/nico/Desktop/cavium/.cav-flowb.log
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
pkill -9 -f 'cat /dev/ttyUSB0' 2>/dev/null
rmmod liquidio 2>/dev/null; rmmod liquidio_core 2>/dev/null; rmmod octwin 2>/dev/null
stty -F $DEV 115200 raw -echo 2>/dev/null
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.0}"; }

echo "[1] SBR -> u-boot prompt (leave there; bootdelay -1)"
setpci -s $BRIDGE BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s $BRIDGE BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
cmd "setenv bootdelay -1"
# program PEM inbound windows so the host/driver can reach card mem (console @0x0006c000)
cmd "write64 0x00011800C0000080 0x00000000F8000000"
cmd "write64 0x00011800C0000088 0x00000000F4000000"

echo "[2] restore host BARs + BUS MASTER"
setpci -s $CARD BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s $CARD BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s $CARD COMMAND=0x06
echo "    BAR0=$(setpci -s $CARD BASE_ADDRESS_0) CMD=$(setpci -s $CARD COMMAND)"

echo "[3] capture serial (driver downloads+boots firmware over 60s)"
: > $SLOG
( timeout 70 cat $DEV > $SLOG 2>&1 ) &

echo "[4] insmod liquidio (driver-download flow, preloaded=0)"
dmesg -C
insmod "$LB/liquidio-core.ko" 2>&1
insmod "$LB/liquidio.ko" fw_type=nic console_bitmask=0x3 debug=1 2>&1
echo "    insmod liquidio ret=$?"
sleep 10

echo "==================== HOST DMESG ===================="
dmesg | grep -iE 'liquidio|octeon|bootloader|console|ddr|download|firmware|core|drv|nic|link|queue|fail|Board' | grep -vaiE 'Modules linked|Tainted' | tail -55
echo "==================== NETDEVS ===================="
ls /sys/class/net/ | grep -vE 'veth|docker|br-|^lo$|wlx|enp7|eno' || echo "(none new)"
echo "==================== CARD SERIAL (tail) ===================="
tail -40 $SLOG
echo "==================== END ===================="
