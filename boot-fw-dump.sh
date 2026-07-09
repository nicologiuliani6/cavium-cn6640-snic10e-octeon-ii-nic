#!/bin/sh
# SAFE firmware-register RE: boot vendor firmware (self-configures DPI/SLI/PKO for
# card->host packets), then READ its CSRs via the BAR0 window. COMMAND=0x02 (NO bus
# master) so the card cannot initiate any DMA = cannot wedge the host. Reads only.
DEV=/dev/ttyUSB0
ELF=/srv/tftp/lio_210sv.elf
CARD=02:00.0
BRIDGE=00:01.1
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
pkill -9 -f 'cat /dev/ttyUSB0' 2>/dev/null
rmmod octoq_host octwin liquidio liquidio_core 2>/dev/null
stty -F $DEV 115200 raw -echo 2>/dev/null
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.0}"; }

echo "[1] SBR -> u-boot"
setpci -s $BRIDGE BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s $BRIDGE BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1.5
cmd "setenv bootdelay -1"
cmd "write64 0x00011800C0000080 0x00000000F8000000"
echo "[2] loady firmware (~100s)"
printf 'loady 0x21000000\r\n' > $DEV; sleep 2
sb "$ELF" < $DEV > $DEV
echo "    sb exit $?"; sleep 2
echo "[3] bootoct firmware"
printf 'bootoct 0x21000000 coremask=0xff\r\n' > $DEV
sleep 14
echo "[4] restore host BAR0 (mem only, NO bus master) + dump firmware regs"
setpci -s $CARD BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s $CARD COMMAND=0x02
python3 /home/nico/Desktop/cavium/win_dump.py fw
echo "[5] SBR card -> idle u-boot (safe)"
setpci -s $BRIDGE BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s $BRIDGE BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 80 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
echo "==================== END ===================="
