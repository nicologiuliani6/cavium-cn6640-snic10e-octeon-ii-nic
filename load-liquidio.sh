#!/bin/sh
# Load the patched liquidio driver (preloaded=1) against the LiquidIO NIC firmware
# already booted on the card. Restores the host config BARs (mem decode only; the
# driver enables bus-master itself once it has programmed valid DMA ring addresses
# into the card CSRs). Captures dmesg + resulting netdevs.
LB=/home/nico/Desktop/cavium/lio-build/drivers/net/ethernet/cavium/liquidio
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

echo "[*] restore host config BARs (mem-enable only, NO bus-master)"
setpci -s 04:00.0 BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s 04:00.0 BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s 04:00.0 COMMAND=0x02
echo "    BAR0=$(setpci -s 04:00.0 BASE_ADDRESS_0) BAR2=$(setpci -s 04:00.0 BASE_ADDRESS_2) CMD=$(setpci -s 04:00.0 COMMAND)"

dmesg -C
echo "[*] insmod liquidio-core.ko"
insmod "$LB/liquidio-core.ko" 2>&1
echo "[*] insmod liquidio.ko preloaded=1 fw_type=nic"
insmod "$LB/liquidio.ko" preloaded=1 fw_type=nic 2>&1
echo "[*] rc=$?"
sleep 4
echo "==================== DMESG ===================="
dmesg | tail -60
echo "==================== NETDEVS ===================="
ls /sys/class/net/
echo "==================== LSPCI ===================="
lspci -s 04:00.0 -vvv 2>/dev/null | grep -iE 'Control:|Kernel driver|Region'
echo "==================== END ===================="
