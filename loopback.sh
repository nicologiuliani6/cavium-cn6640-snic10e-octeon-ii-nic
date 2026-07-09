#!/bin/sh
# Test loopback FT232: unisci TX<->RX dell'adattatore (niente card). Manda marker, lo rilegge.
DEV=/dev/ttyUSB0
[ "$(id -u)" = 0 ] || { echo "usa: sudo sh loopback.sh"; exit 1; }
MARK="LOOP12345"

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
i=0
echo "### LOOPBACK $DEV - unisci TX<->RX, Ctrl-C stop ###"
while true; do
  i=$((i+1))
  printf '%s\r' "$MARK" >&3
  got=$(timeout 1 dd bs=100 count=1 <&3 2>/dev/null)
  if printf '%s' "$got" | grep -q "$MARK"; then
    echo "[$i]  OK   FT232 funziona (TX->RX torna: $MARK)"
  elif [ -n "$got" ]; then
    echo "[$i]  ?    torna qualcosa ma non il marker: '$(printf '%s' "$got" | tr -d '\r\n' | cut -c1-20)'"
  else
    echo "[$i]  --   niente -> TX<->RX non uniti, o FT232 rotta"
  fi
  sleep 0.4
done
