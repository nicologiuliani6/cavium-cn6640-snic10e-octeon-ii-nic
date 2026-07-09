#!/bin/sh
# Load NIC firmware ELF into card RAM via u-boot ymodem (loady) and bootoct it.
# Non-destructive: RAM only, no flash write. Card reboots back to normal on reset.
DEV=/dev/ttyUSB0
ELF=/srv/tftp/lio_210sv.elf
BAUD=115200
ADDR=0x21000000
LOG=/home/nico/Desktop/cavium/.cav-fw.log
if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
stty -F $DEV $BAUD raw -echo 2>/dev/null

echo "[*] reset card to u-boot prompt..."
setpci -s 00:03.0 BRIDGE_CONTROL=40:40; sleep 0.12; setpci -s 00:03.0 BRIDGE_CONTROL=00:40
# hammer Enter ~12s to stop autoboot
t=0; while [ $t -lt 240 ]; do printf '\r' > $DEV; sleep 0.05; t=$((t+1)); done
sleep 1

echo "[*] starting loady at $ADDR ..."
printf 'loady %s\r\n' "$ADDR" > $DEV
sleep 2

echo "[*] sending $ELF via ymodem (~100s)..."
sb -q "$ELF" < $DEV > $DEV
echo "[*] ymodem done, exit $?"
sleep 2

echo "[*] booting firmware: bootoct $ADDR coremask=0xff"
: > $LOG
( timeout 20 cat $DEV > $LOG ) &
LP=$!
sleep 0.3
printf 'bootoct %s coremask=0xff\r\n' "$ADDR" > $DEV
sleep 18
kill $LP 2>/dev/null; wait $LP 2>/dev/null
echo "==================== BOOT OUTPUT ===================="
cat -v $LOG | grep -v "^Octeon snic10e# $" | tail -50
echo "==================== END ===================="
