#!/bin/sh
# DECISIVE, low-risk test: does card->host SLI packet-output DMA work on this EP at all?
# Boot the vendor firmware (it retries CORE_DRV_ACTIVE = a card->host packet into an OQ).
# Arm the HARDENED octoq_host (pinned buffer, safe unload) on OQ0 and watch for ANY packet.
#  arrives  -> card->host SLI DMA WORKS -> 10G reachable (need card-side posting for our pkts)
#  nothing  -> SLI card->host DMA dead on this EP -> 2.2G CPU-copy is the ceiling
# Read-only on the card side (octwin/octoq reads + firmware boot); no risky card->host from us.
DEV=/dev/ttyUSB0
ELF=/srv/tftp/lio_210sv.elf
CARD=02:00.0
BRIDGE=00:01.1
HM=/home/nico/Desktop/cavium/hostmod
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
cmd "write64 0x00011800C0000088 0x00000000F4000000"

echo "[2] loady firmware (~100s)"
printf 'loady 0x21000000\r\n' > $DEV; sleep 2
sb "$ELF" < $DEV > $DEV
echo "    sb exit $?"; sleep 2

echo "[3] bootoct firmware"
printf 'bootoct 0x21000000 coremask=0xff\r\n' > $DEV
sleep 12   # let firmware init + begin CORE_DRV_ACTIVE retries

echo "[4] restore host BARs + BUS MASTER, arm hardened octoq_host on OQ0"
setpci -s $CARD BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s $CARD BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s $CARD COMMAND=0x06
dmesg -C
insmod "$HM/octoq_host.ko" q=0 ndesc=256 bufsz=2048
sleep 15   # catch firmware CORE_DRV_ACTIVE retries

echo "==================== octoq_host (did a packet arrive?) ===================="
dmesg | grep -i octoq | tail -12
echo "==================== scan ALL OQ PKTS_SENT via BAR0 ===================="
python3 - <<'PY'
import mmap,os,struct
sz=os.path.getsize('/sys/bus/pci/devices/0000:02:00.0/resource0')
fd=os.open('/sys/bus/pci/devices/0000:02:00.0/resource0',os.O_RDWR)
m=mmap.mmap(fd,sz,mmap.MAP_SHARED)
rd=lambda o: struct.unpack('<I',m[o:o+4])[0]
print("OQ_PKTS_SENT q0..3:", [hex(rd(0x2400+q*0x10)) for q in range(4)])
print("OQ_PKT_CREDITS q0..3:", [hex(rd(0x1800+q*0x10)) for q in range(4)])
m.close();os.close(fd)
PY
echo "[5] safe teardown: rmmod octoq_host (hardened), SBR card to idle u-boot"
rmmod octoq_host 2>/dev/null
sleep 1
setpci -s $BRIDGE BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s $BRIDGE BRIDGE_CONTROL=00:40
t=0; while [ $t -lt 100 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
echo "==================== END ===================="
