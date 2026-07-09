#!/bin/sh
# "Correct ordering" preloaded attempt. Assumes prep-ram-fw.sh already put the
# card at the u-boot prompt with PEM windows set and the NIC firmware in RAM at
# 0x21000000. Steps: restore host config BARs; load liquidio (preloaded=1) in the
# background so it programs the host DMA queues and enters its 30s "started" wait;
# THEN bootoct the firmware so it comes up into ready queues and sends
# CORE_DRV_ACTIVE. Watch for CORE_OK / a new netdev.
DEV=/dev/ttyUSB0
LB=/home/nico/Desktop/cavium/lio-build/drivers/net/ethernet/cavium/liquidio
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV 115200 raw -echo 2>/dev/null

# serial capture
( cat $DEV > /home/nico/Desktop/cavium/.cav-ordered.log 2>&1 ) & CAP=$!

echo "[*] restore host config BARs (mem only)"
setpci -s 04:00.0 BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s 04:00.0 BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s 04:00.0 COMMAND=0x02

dmesg -C
rmmod liquidio 2>/dev/null; rmmod liquidio_core 2>/dev/null
insmod "$LB/liquidio-core.ko"
echo "[*] insmod liquidio preloaded=1 (background; sets up queues, waits 30s)"
insmod "$LB/liquidio.ko" preloaded=1 fw_type=nic &
INS=$!

echo "[*] wait 6s for probe to program queues..."
sleep 6
echo "[*] bootoct firmware NOW (queues are ready)"
printf 'bootoct 0x21000000 coremask=0xff\r\n' > $DEV

echo "[*] waiting for handshake (up to 35s)..."
wait $INS 2>/dev/null
sleep 2
kill $CAP 2>/dev/null

echo "==================== DMESG ===================="
dmesg | grep -iE 'liquidio|octeon|core|nic|firmware' | tail -40
echo "==================== NETDEVS ===================="
ls /sys/class/net/ | grep -vE 'veth|docker|br-|^lo$|wlx|enp7' || echo "(no new eth)"
echo "==================== FW CONSOLE ===================="
cat -v /home/nico/Desktop/cavium/.cav-ordered.log | tail -25
echo "==================== END ===================="
