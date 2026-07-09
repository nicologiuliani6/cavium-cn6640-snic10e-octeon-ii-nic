#!/bin/bash
# diag-write.sh — isola bug: scrive pattern noto piccolo, rilegge con mmap E con dd, confronta.
set -u
DEV=${DEV:-/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A5069RR4-if00-port0}
ADDR=${ADDR:-0x48000000}
OFF=${OFF:-0x10000}
TMPD=/run/cavium; mkdir -p "$TMPD" 2>/dev/null || TMPD=/tmp

CARD=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
BRIDGE=$(basename "$(dirname "$(readlink -f /sys/bus/pci/devices/0000:$CARD)")" | sed 's/^0000://')
RES=/sys/bus/pci/devices/0000:$CARD/resource
BAR0=$(awk 'NR==1{print $1}' "$RES"); BAR2=$(awk 'NR==3{print $1}' "$RES")
BAR2LO=$(printf '0x%08x' $(( (BAR2 & 0xffffffff) | 0xc )))
BAR2HI=$(printf '0x%08x' $(( BAR2 >> 32 )))
P2N_BAR0=$(printf '0x%016X' $(( BAR0 ))); P2N_BAR1=$(printf '0x%016X' $(( BAR2 )))
echo "[*] card=$CARD bridge=$BRIDGE"

cmd(){ printf '%s\r\n' "$1" > "$DEV"; sleep "${2:-0.3}"; }
fuser -k -9 "$DEV" 2>/dev/null; sleep 0.4
timeout 3 stty -F "$DEV" 115200 cs8 -cstopb -parenb clocal -crtscts raw -echo

echo "[trace] full reset..."
( timeout 20 cat "$DEV" > "$TMPD/diag_rst.txt" 2>&1 ) &
sleep 0.3
setpci -s "$BRIDGE" BRIDGE_CONTROL=40:40; sleep 0.2; setpci -s "$BRIDGE" BRIDGE_CONTROL=00:40
echo "[trace] waiting link retrain (polling vendor id)..."
up=0
for t in $(seq 1 40); do
  sleep 0.1
  [ "$(lspci -n -s "$CARD" 2>/dev/null | awk '{print $3}')" = "177d:0092" ] && { up=1; echo "[trace] link back after ${t}00ms"; break; }
done
[ "$up" = 1 ] || echo "[trace] WARN: vendor id never came back after 4s"
for i in $(seq 1 50); do printf '\r' > "$DEV"; sleep 0.03; done
cmd "reset" 9
for i in $(seq 1 120); do printf '\r' > "$DEV"; sleep 0.03; done
sleep 1
grep -aq 'Clearing DRAM' "$TMPD/diag_rst.txt" && echo "[0] reset OK" || echo "[0] WARN: no banner"

echo "[trace] window setup..."
( timeout 15 cat "$DEV" > "$TMPD/diag_win.txt" 2>&1 ) &
cmd "setenv bootdelay -1"
cmd "write64 0x00011800C0000080 $P2N_BAR0"
cmd "write64 0x00011800C0000088 $P2N_BAR1"
RBASE=$(( ADDR >> 22 )); i=0
while [ $i -lt 8 ]; do
  reg=$(printf '0x%011X' $((0x11800C00000A8 + i*8)))
  val=$(printf '0x%X' $(( ((RBASE + i)<<4) | 0xB )))
  cmd "write64 $reg $val" 0.35
  i=$((i+1))
done
cmd "flush_l2c" 0.6; cmd "flush_dcache" 0.6
sleep 0.5
echo "[1] window setup console output was:"
tr -d '\r' < "$TMPD/diag_win.txt"
echo "[1] --- end console output ---"

setpci -s "$CARD" BASE_ADDRESS_2="$BAR2LO" BASE_ADDRESS_3="$BAR2HI" COMMAND=0x02 >/dev/null

echo "[2] test A: python mmap write 32 bytes pattern, readback via mmap"
OFF=$OFF CARD=$CARD python3 - <<'PY'
import mmap,os
off=int(os.environ["OFF"],0); card=os.environ["CARD"]
pat=bytes(range(32))
fd=os.open("/sys/bus/pci/devices/0000:%s/resource2"%card,os.O_RDWR)
m=mmap.mmap(fd,64*1024*1024,mmap.MAP_SHARED)
m[off:off+32]=pat
m.flush()
rb=bytes(m[off:off+32])
print("wrote:", pat.hex())
print("mmap-read:", rb.hex())
print("MATCH" if rb==pat else "MISMATCH")
m.close(); os.close(fd)
PY

echo "[2] test B: same region, readback via dd (bypass python mmap)"
sudo dd if=/sys/bus/pci/devices/0000:$CARD/resource2 bs=1 skip=$((OFF)) count=32 2>/dev/null | xxd

echo "[3] test C: re-read via mmap AGAIN (no rewrite) - stability check"
OFF=$OFF CARD=$CARD python3 - <<'PY'
import mmap,os
off=int(os.environ["OFF"],0); card=os.environ["CARD"]
fd=os.open("/sys/bus/pci/devices/0000:%s/resource2"%card,os.O_RDWR)
m=mmap.mmap(fd,64*1024*1024,mmap.MAP_SHARED)
print("2nd mmap-read:", bytes(m[off:off+32]).hex())
m.close(); os.close(fd)
PY
