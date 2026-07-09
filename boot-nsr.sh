#!/bin/bash
# boot-nsr.sh — boot the card with NO SERIAL. Needs the one-time persisted env:
#   bootdelay=1
#   wa/wb/wc/wd  = program the PEM BAR1 window (+ flush_l2c/flush_dcache to activate it)
#   bootcmd      = run wa wb wc wd ; sleep 25 ; flush_l2c ; flush_dcache ; bootoctlinux 0x20010000 ...
#
# Host-only flow (no console): SBR reboots the card -> u-boot runs bootcmd -> programs its
# own window -> sleeps 25s. The host restores its BARs, waits for BAR2 to become writable
# (window live), pushes the OpenWrt image into card DRAM via BAR2 + wordfix, then u-boot
# finishes the sleep, flushes and boots the freshly-pushed image.
set -u
IMG=${IMG:-/home/nico/openwrt/bin/targets/octeon/generic/openwrt-octeon-generic-snic10e-initramfs-kernel.bin}
OFF=${OFF:-0x10000}
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

CARD=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
[ -n "$CARD" ] || { echo "boot-nsr: card not found"; exit 1; }
BRIDGE=$(basename "$(dirname "$(readlink -f /sys/bus/pci/devices/0000:$CARD)")" | sed 's/^0000://')
echo "[*] card=$CARD bridge=$BRIDGE img=$(basename "$IMG")"

echo "[0] SBR reset (card reboots, u-boot autoruns bootcmd, programs window, sleeps)..."
setpci -s "$BRIDGE" BRIDGE_CONTROL=40:40; sleep 0.2; setpci -s "$BRIDGE" BRIDGE_CONTROL=00:40
sleep 6						# u-boot boot + bootdelay + window program

setpci -s "$CARD" BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0x00000000 COMMAND=0x06

OFF=$OFF CARD=$CARD python3 - "$IMG" <<'PY'
import mmap,os,sys,time
off=int(os.environ["OFF"],0); card=os.environ["CARD"]
d=open(sys.argv[1],'rb').read()
fd=os.open("/sys/bus/pci/devices/0000:%s/resource2"%card,os.O_RDWR)
m=mmap.mmap(fd,64*1024*1024,mmap.MAP_SHARED)
# wait until BAR2 is writable (u-boot has programmed the window) via a marker round-trip
mark=b"NSRLIVE0"; live=False
for _ in range(40):
    m[off:off+8]=mark; m.flush()
    if bytes(m[off:off+8])==mark: live=True; print("[2] window live after %s tries"%(_+1)); break
    time.sleep(0.5)
if not live:
    print("[2] FAIL: BAR2 never writable; card not at bootcmd/window? "); sys.exit(1)
# push image + wordfix (WC-write corruption correction)
m[off:off+len(d)]=d; m.flush()
for it in range(10):
    rb=bytes(m[off:off+len(d)]); bad=[i for i in range(0,len(d),4) if d[i:i+4]!=rb[i:i+4]]
    if not bad: print("[3] DRAM CLEAN (%d fix passes, %d bytes)"%(it,len(d))); break
    for i in bad: m[off+i:off+i+4]=d[i:i+4]
    m.flush()
else:
    print("[3] WARN still",len(bad),"bad"); sys.exit(1)
m.close(); os.close(fd)
PY
[ $? -eq 0 ] || { echo "abort"; exit 1; }
echo "[4] image pushed; u-boot finishes sleep -> flush -> bootoctlinux (~20s). Then run twocard-up.sh."
