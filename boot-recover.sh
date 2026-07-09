#!/bin/bash
# Runs at boot (via cavium-recover.service). After a power-cycle the card is at u-boot;
# this fast-boots OpenWrt into it and brings up the stable DPI NIC, so the machine comes
# back with the card working without any manual step. Logs to boot-recover.log.
DIR=/home/nico/Desktop/cavium
exec >> "$DIR/boot-recover.log" 2>&1
echo "==================== boot-recover $(date) ===================="

# let PCI, USB-serial and the desktop settle
sleep 45

cd "$DIR" || exit 1

# card present?
if ! lspci -d 177d:0092 >/dev/null 2>&1; then
	echo "card not on PCI bus - abort"
	exit 1
fi

# fast-boot OpenWrt into the card (handles SBR-if-needed + crc-gated boot)
for try in 1 2 3; do
	echo "--- fast-boot attempt $try ---"
	bash fast-boot-bar2.sh
	sleep 3
	# did OpenWrt come up? probe the shell
	if BAUD=115200 bash cexec.sh 'echo BOOTOK_$((7*7))' 2>/dev/null | grep -q 'BOOTOK_49'; then
		echo "OpenWrt up"
		break
	fi
	echo "boot probe failed, retrying"
	# reset the FT232 in case its USB endpoint hung
	pkill -9 -f ttyUSB0 2>/dev/null
	U=$(basename $(readlink -f /sys/class/tty/ttyUSB0/device 2>/dev/null | sed 's:/ttyUSB0::'))
	echo -n "$U" > /sys/bus/usb/drivers/ftdi_sio/unbind 2>/dev/null; sleep 1
	echo -n "$U" > /sys/bus/usb/drivers/ftdi_sio/bind 2>/dev/null; sleep 2
done

# bring up the stable DPI NIC (strict per-device IOMMU + firewall-off + IPs + modules)
sleep 3
bash dpi-up-clean.sh
echo "==================== boot-recover done $(date) ===================="
