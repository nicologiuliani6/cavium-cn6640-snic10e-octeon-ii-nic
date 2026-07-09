#!/bin/bash
# boot-clean.sh — reload OpenWrt onto the card over BAR2 WITHOUT the crc gate.
#
# Why no crc: the card CPU crc32 reads the freshly-pushed image into L2 BEFORE we fix the
# WC-push corruption. The host-side wordfix then corrects DRAM, but a host write can't
# invalidate the card's L2 -> bootoctlinux reads the stale, still-corrupt e_entry from L2 and
# jumps to garbage ("Starting cores 0xff" then dead). Root-caused 2026-07-03.
#
# Fix: full chip `reset` (fresh L2) -> program window -> push -> wordfix -> boot, with NOTHING
# making the card CPU touch the load region between the push and the boot.
#
# All PCI locations + BARs are AUTO-DETECTED (survives adding/removing other cards). Override
# any of CARD/BRIDGE/BAR0/BAR2/DEV/IMG via env if detection is wrong.
set -u
DEV=${DEV:-/dev/ttyUSB0}
IMG=${IMG:-/home/nico/openwrt/bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin}
ADDR=${ADDR:-0x20000000}          # proven working addr (cavium-fast-bar2-boot memory, verified 2026-07-02)
OFF=${OFF:-0x10000}               # push image OFF bytes into the window: dodges the bad 0x1c cell
RGO=${RGO:-3}
TMPD=${TMPD:-/run/cavium}; mkdir -p "$TMPD" 2>/dev/null || TMPD=/tmp
BADDR=$(printf '0x%X' $(( ADDR + OFF )))

# --- auto-detect PCI location + BIOS-assigned BARs ---
CARD=${CARD:-$(lspci -d 177d:0092 | awk '{print $1}' | head -1)}
[ -n "$CARD" ] || { echo "boot-clean: Cavium 177d:0092 not found on PCI bus"; exit 1; }
BRIDGE=${BRIDGE:-$(basename "$(dirname "$(readlink -f /sys/bus/pci/devices/0000:$CARD)")" | sed 's/^0000://')}
RES=/sys/bus/pci/devices/0000:$CARD/resource
BAR0=${BAR0:-$(awk 'NR==1{print $1}' "$RES")}     # BAR0 base (e.g. 0x...f8000000)
BAR2=${BAR2:-$(awk 'NR==3{print $1}' "$RES")}     # BAR2 base (e.g. 0x...f4000000)
BAR0LO=$(printf '0x%08x' $(( BAR0 & 0xffffffff )))
BAR2LO=$(printf '0x%08x' $(( (BAR2 & 0xffffffff) | 0xc )))          # BAR2 flags: prefetch+64bit
BAR2HI=$(printf '0x%08x' $(( BAR2 >> 32 )))                         # BAR2 upper (if BIOS puts it >4G)
P2N_BAR0=$(printf '0x%016X' $(( BAR0 )))
P2N_BAR1=$(printf '0x%016X' $(( BAR2 )))
echo "[*] card=$CARD bridge=$BRIDGE BAR0=$BAR0LO BAR2=$BAR2HI:$BAR2LO"

cmd(){ printf '%s\r\n' "$1" > "$DEV"; sleep "${2:-0.3}"; }

echo "[trace] fuser -k on $DEV..."
fuser -k -9 "$DEV" 2>/dev/null; sleep 0.4
echo "[trace] stty attempt 1..."
if ! timeout 3 stty -F "$DEV" 115200 cs8 -cstopb -parenb clocal -crtscts raw -echo; then
  echo "[trace] stty stuck -> USB-reset FT232..."
  for p in $(fuser "$DEV" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
  python3 - <<'PY' 2>/dev/null
import fcntl,os,glob
for b in glob.glob("/sys/bus/usb/devices/*"):
    try:
        if open(b+"/idVendor").read().strip()=="0403" and open(b+"/idProduct").read().strip()=="6001":
            n="/dev/bus/usb/%03d/%03d"%(int(open(b+"/busnum").read()),int(open(b+"/devnum").read()))
            fd=os.open(n,os.O_WRONLY); fcntl.ioctl(fd,ord("U")<<8|20,0); os.close(fd)
    except Exception: pass
PY
  sleep 2
  echo "[trace] stty attempt 2..."
  timeout 3 stty -F "$DEV" 115200 cs8 -cstopb -parenb clocal -crtscts raw -echo || { echo "[trace] stty still stuck, DEV dead"; exit 1; }
fi
echo "[trace] stty OK"

# [0] full chip reset -> fresh cores + fresh L2 (soft SBR alone leaves L2 dirty)
echo "[trace] starting reset cat bg reader..."
( timeout 20 cat "$DEV" > "$TMPD/bc_rst.txt" 2>&1 ) &
sleep 0.3
echo "[trace] toggling bridge SBR..."
setpci -s "$BRIDGE" BRIDGE_CONTROL=40:40; sleep 0.2; setpci -s "$BRIDGE" BRIDGE_CONTROL=00:40; sleep 1
echo "[trace] continuous CR flood covering full boot banner + 10s autoboot countdown..."
for i in $(seq 1 700); do printf '\r' > "$DEV"; sleep 0.03; done
sleep 1
echo "[trace] checking reset banner..."
grep -aq 'Clearing DRAM' "$TMPD/bc_rst.txt" && echo "[0] full reset OK (fresh L2)" || echo "[0] WARN: no DRAM banner"

# [1] program PEM BAR1 window: DRAM ADDR (32MB) -> host BAR2 physical base
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
cmd "flush_l2c" 0.6
cmd "flush_dcache" 0.6
echo "[1] window @ $ADDR -> BAR2 $BAR2LO programmed"

# [2] host push + wordfix (fixes WC-corrupted words in DRAM; NO card CPU involved)
setpci -s "$CARD" BASE_ADDRESS_2="$BAR2LO" BASE_ADDRESS_3="$BAR2HI" COMMAND=0x02 >/dev/null
OFF=$OFF CARD=$CARD python3 - "$IMG" <<'PY'
import mmap,os,sys
off=int(os.environ["OFF"],0); card=os.environ["CARD"]
d=open(sys.argv[1],'rb').read()
fd=os.open("/sys/bus/pci/devices/0000:%s/resource2"%card,os.O_RDWR)
m=mmap.mmap(fd,64*1024*1024,mmap.MAP_SHARED); m[off:off+len(d)]=d; m.flush()
for it in range(10):
    rb=bytes(m[off:off+len(d)]); bad=[i for i in range(0,len(d),4) if d[i:i+4]!=rb[i:i+4]]
    if not bad: print("[2] DRAM CLEAN (%d fix passes)"%it); break
    for i in bad: m[off+i:off+i+4]=d[i:i+4]
    m.flush()
else:
    print("[2] WARN: still",len(bad),"bad words; first offsets:",bad[:8],"stride:",[bad[i+1]-bad[i] for i in range(min(7,len(bad)-1))])
    sys.exit(1)
m.close(); os.close(fd)
PY
[ $? -ne 0 ] && { echo "abort: DRAM not clean"; exit 1; }

# [3] boot — FIRST card-CPU read of the region; clean cache-miss from corrected DRAM
RGO_ARG=""; [ -n "$RGO" ] && RGO_ARG=" octeon-ethernet.receive_group_order=$RGO"
( timeout 70 cat "$DEV" > "$TMPD/bc_boot.txt" 2>&1 ) &
sleep 0.3
printf 'bootoctlinux %s numcores=8 endbootargs console=ttyS0,115200%s\r\n' "$BADDR" "$RGO_ARG" > "$DEV"
echo "[3] booting from $BADDR (RGO=$RGO, image OFF=$OFF into window)..."
sleep 60
echo "=== boot result ==="
tr -d '\r' < "$TMPD/bc_boot.txt" | grep -aE 'entry point|Starting cores|Linux version|Freeing|procd|link becomes|xaui|login:|EXCEPTION|panic' | tail -12
grep -aq 'link becomes ready\|procd' "$TMPD/bc_boot.txt" && exit 0 || exit 2
