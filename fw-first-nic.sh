#!/bin/sh
# Instrumented stock-liquidio bring-up. Key differences vs full-nic-02.sh:
#  - COMMAND=0x06 (memory + BUS MASTER) so the card can DMA the handshake packet.
#  - liquidio console_bitmask=0xffffffff -> driver mirrors the firmware's PCI console
#    into host dmesg, revealing what the firmware does AFTER "DMA Queues initialized".
#  - dumps the FULL firmware serial console + full driver console, not a tail.
DEV=/dev/ttyUSB0
ELF=/srv/tftp/lio_210sv.elf
LB=/home/nico/Desktop/cavium/lio-build/drivers/net/ethernet/cavium/liquidio
CARD=02:00.0
BRIDGE=00:01.1
SLOG=/home/nico/Desktop/cavium/.cav-fwfirst.log
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
pkill -9 -f 'cat /dev/ttyUSB0' 2>/dev/null
rmmod liquidio 2>/dev/null; rmmod liquidio_core 2>/dev/null; rmmod octwin 2>/dev/null
stty -F $DEV 115200 raw -echo 2>/dev/null
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.0}"; }

echo "[1] SBR -> u-boot"
setpci -s $BRIDGE BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s $BRIDGE BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
cmd "setenv bootdelay -1"
cmd "write64 0x00011800C0000080 0x00000000F8000000"
cmd "write64 0x00011800C0000088 0x00000000F4000000"

echo "[2] loady firmware -> 0x21000000"
printf 'loady 0x21000000\r\n' > $DEV; sleep 2
sb "$ELF" < $DEV > $DEV
echo "    sb exit $?"; sleep 2

echo "[3] restore host BARs + BUS MASTER (0x06)"
setpci -s $CARD BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s $CARD BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s $CARD COMMAND=0x06
echo "    BAR0=$(setpci -s $CARD BASE_ADDRESS_0) BAR2=$(setpci -s $CARD BASE_ADDRESS_2) CMD=$(setpci -s $CARD COMMAND)"

echo "[4] insmod driver (programs host queues), then bootoct"
dmesg -C
insmod "$LB/liquidio-core.ko" 2>&1
insmod "$LB/liquidio.ko" preloaded=1 fw_type=nic console_bitmask=0xffffffff debug=1 2>&1 &
INS=$!
sleep 7
echo "[5] bootoct firmware; capture serial 75s"
: > $SLOG
( timeout 75 cat $DEV > $SLOG 2>&1 ) &
sleep 0.3
printf 'bootoct 0x21000000 coremask=0xff\r\n' > $DEV
wait $INS 2>/dev/null
sleep 8

echo "==================== FIRMWARE PCI CONSOLE (via driver, dmesg) ===================="
dmesg | grep -iE 'CONSOLE|liquidio|octeon|nic|core|drv|link|queue|fw|handshake|PHY|GMX|BGX' | tail -60
echo "==================== NETDEVS ===================="
ls /sys/class/net/ | grep -vE 'veth|docker|br-|^lo$|wlx|enp7|eno' || echo "(none new)"
echo "==================== FIRMWARE SERIAL CONSOLE (full, after DMA queues) ===================="
grep -aA200 'DMA Queues' $SLOG | head -80 || tail -60 $SLOG
echo "==================== END ===================="
