#!/bin/sh
# Phase 0: fast OpenWrt reload by pushing the 21MB image into card DRAM over PCIe BAR2
# (seconds) instead of 37min serial ymodem. Card must be at the u-boot prompt.
#   - u-boot maps DRAM 0x20000000..0x24000000 (32MB) into host BAR2 via BAR1_INDEX0..7
#   - host mmaps resource2 and writes the image bytes
#   - u-boot crc32 GATES the boot: only bootoctlinux if the DRAM copy matches the file
IMG=/home/nico/openwrt/bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin
DEV=/dev/ttyUSB0
CARD=02:00.0
BRIDGE=00:01.1
ADDR=${ADDR:-0x30000000}   # good DRAM bank (0x20000000 bank went bad this card)
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
SZ=$(stat -c%s $IMG); SZHEX=$(printf %x $SZ)
HOSTCRC=$(python3 -c "import zlib;print('%08x'%(zlib.crc32(open('$IMG','rb').read())&0xffffffff))")
pkill -9 -f 'cat /dev/ttyUSB0' 2>/dev/null
rmmod octwin octoq_host liquidio liquidio_core 2>/dev/null
stty -F $DEV 115200 raw -echo 2>/dev/null
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-0.8}"; }

echo "[1] ensure u-boot prompt (SBR if needed)"
printf '\r\n' > $DEV; sleep 0.5
( timeout 2 cat $DEV > /tmp/claude-1000/ub_probe.txt 2>&1 ) & printf '\r\n' > $DEV; sleep 2
if ! grep -qi 'snic10e#' /tmp/claude-1000/ub_probe.txt; then
  echo "    not at prompt -> SBR"
  setpci -s $BRIDGE BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s $BRIDGE BRIDGE_CONTROL=00:40
  t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
  sleep 1.5
fi
cmd "setenv bootdelay -1"

echo "[2] program PEM BAR1 window: DRAM $ADDR (32MB) -> host BAR2 0xF4000000"
cmd "write64 0x00011800C0000080 0x00000000F8000000"   # P2N_BAR0
cmd "write64 0x00011800C0000088 0x00000000F4000000"   # P2N_BAR1 (host BAR2 base)
# BAR1_INDEX0..7 -> DRAM regions (ADDR>>22 .. +7) (*4MB), CA|ES=1|valid=0xB.
# region base derives from ADDR so we can dodge a bad DRAM bank (0x20000000 failed;
# 0x30000000 tested good via u-boot mw/md).
RBASE=$(( ADDR >> 22 ))
i=0
while [ $i -lt 8 ]; do
  reg=$(printf '0x%011X' $((0x11800C00000A8 + i*8)))
  region=$(( RBASE + i ))
  val=$(printf '0x%X' $(( (region<<4) | 0xB )))
  cmd "write64 $reg $val" 0.4
  i=$((i+1))
done

echo "[3] restore host BAR2 + enable memory"
setpci -s $CARD BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s $CARD COMMAND=0x02
echo "    BAR2=$(setpci -s $CARD BASE_ADDRESS_2) CMD=$(setpci -s $CARD COMMAND)"

# Evict any stale/dirty L2 lines for the load region BEFORE the DMA push. The BAR2 push
# writes DDR directly (bypasses the card CPU cache); leftover dirty L2 lines from a prior
# boot would otherwise shadow the fresh image on CPU reads (crc/bootoctlinux) -> corrupt boot.
# (This is the non-thermal root cause: card is only ~48C; it's cache incoherence.)
cmd "flush_l2c" 0.6
cmd "flush_dcache" 0.6

echo "[4] host: push $SZ bytes into card DRAM via BAR2 (resource2)"
python3 - "$IMG" <<'PY'
import mmap, os, sys, time
img = sys.argv[1]
data = open(img,'rb').read()
fd = os.open("/sys/bus/pci/devices/0000:02:00.0/resource2", os.O_RDWR)
m = mmap.mmap(fd, 64*1024*1024, mmap.MAP_SHARED)
t0=time.time()
m[0:len(data)] = data
m.flush()
print("    wrote %d bytes in %.2fs" % (len(data), time.time()-t0))
m.close(); os.close(fd)
PY

echo "[5] u-boot crc32 GATE (expect $HOSTCRC)"
: > /tmp/claude-1000/crc.txt
( timeout 8 cat $DEV > /tmp/claude-1000/crc.txt 2>&1 ) &
sleep 0.3
printf 'crc32 %s 0x%s\r\n' "$ADDR" "$SZHEX" > $DEV
sleep 6
CRC=$(grep -aoiE 'crc32 for [0-9a-fx. ]*==> [0-9a-f]+' /tmp/claude-1000/crc.txt | grep -aoE '[0-9a-f]+$' | tail -1)
echo "    u-boot crc=$CRC host=$HOSTCRC   raw: $(tail -2 /tmp/claude-1000/crc.txt | tr -d '\r')"
if [ "$CRC" = "$HOSTCRC" ]; then
  echo "[6] CRC MATCH -> bootoctlinux"
  : > /tmp/claude-1000/fastboot.log
  ( timeout 60 cat $DEV > /tmp/claude-1000/fastboot.log 2>&1 ) &
  sleep 0.3
  # RGO: octeon-ethernet.receive_group_order=N -> 2^N RX groups (multi-core NAPI). Default off.
  RGO_ARG=""
  [ -n "$RGO" ] && RGO_ARG=" octeon-ethernet.receive_group_order=$RGO"
  printf 'bootoctlinux %s numcores=8 endbootargs console=ttyS0,115200%s\r\n' "$ADDR" "$RGO_ARG" > $DEV
  sleep 55
  echo "==== BOOT TAIL ===="
  tail -25 /tmp/claude-1000/fastboot.log | tr -d '\r'
else
  echo "[6] CRC MISMATCH -> not booting. (endianness/window issue; will adjust)"
fi
echo "==================== END ===================="
