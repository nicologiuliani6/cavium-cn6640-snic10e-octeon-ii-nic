#!/bin/sh
# Loop infinito. Ogni ~2s stampa stato RX/TX mentre muovi i fili. Ctrl-C stop.
DEV=/dev/ttyUSB0
[ "$(id -u)" = 0 ] || { echo "usa: sudo sh check-links.sh"; exit 1; }
CARD='Octeon\|snic10e\|U-Boot\|CN6\|DRAM\|Cavium\|autoboot\|=>'

set_tty() { timeout 3 stty -F "$DEV" 115200 cs8 -cstopb -parenb clocal -crtscts raw -echo 2>/dev/null; }
if ! set_tty; then
  echo "reset FT232 (USB incastrata)..."
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
  set_tty || { echo "porta $DEV assente/occupata"; exit 1; }
fi

exec 3<>"$DEV"
echo "### LOOP RX/TX  $DEV @115200 - muovi i fili, Ctrl-C stop ###"
i=0
while true; do
  i=$((i+1))
  printf '\r\r\rversion\r' >&3   # solletica card
  txt=$(timeout 1 dd bs=400 count=1 <&3 2>/dev/null)
  b=$(printf '%s' "$txt" | wc -c | tr -d ' ')
  if printf '%s' "$txt" | grep -q "$CARD"; then
    echo "[$i]  RX=OK  TX=OK   ${b}B  $(printf '%s' "$txt" | tr -d '\r\n' | cut -c1-30)"
  elif [ "$b" -gt 0 ]; then
    echo "[$i]  RX=OK  TX=?    ${b}B garbage (baud o pin4<->pin5 scambiati)"
  else
    echo "[$i]  RX=--  TX=--   0B (filo pin5/GND staccato o card muta)"
  fi
  sleep 0.4
done
