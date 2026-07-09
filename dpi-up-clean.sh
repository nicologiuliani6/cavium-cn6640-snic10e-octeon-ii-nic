#!/bin/bash
# Bring up the DPI-offload NIC (card->host RX via the DPI hardware DMA engine).
# Reverse (card->host) ~2.15 Gbit/s jumbo with the card CPU ~96% IDLE (the copy is
# offloaded to the DPI engine, unlike the old CPU-copy path that saturated the cores).
# Card must be running OpenWrt at the /dev/ttyUSB0 @115200 shell. Uses cexec.sh (reliable)
# instead of an inline python serial block. Run with sudo. MTU arg (default 9000).
set -e
DIR=/home/nico/Desktop/cavium
MTU=${1:-9000}
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
BDF=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
[ -n "$BDF" ] || { echo "Cavium not found"; exit 1; }
echo "[*] Cavium at $BDF, MTU $MTU"

# per-device strict IOMMU so a stray DPI DMA faults instead of corrupting host RAM (no freeze)
echo DMA > /sys/bus/pci/devices/0000:$BDF/iommu_group/type 2>/dev/null || true

# push current card module + bring up (firewall OFF: it silently blocks TCP while ping still works)
BAUD=115200 bash "$DIR/ko-xfer.sh" "$DIR/cardmod/octshm_card.ko" /tmp/octshm.ko >/dev/null 2>&1
BAUD=115200 bash "$DIR/cexec.sh" \
  'rmmod octshm_card 2>/dev/null; /etc/init.d/firewall stop 2>/dev/null; iptables -F 2>/dev/null; iptables -P INPUT ACCEPT 2>/dev/null' \
  "ip link set xaui0 nomaster; ip link set xaui0 up mtu $MTU" \
  "ip link set xaui1 nomaster; ip addr flush dev xaui1; ip addr add 10.9.9.2/24 dev xaui1; ip link set xaui1 up mtu $MTU" \
  'insmod /tmp/octshm.ko uplink=xaui0 dma=2 dpiwait=0; echo CARD_RC=$?' \
  'killall iperf3 2>/dev/null; iperf3 -s -B 10.9.9.2 >/tmp/ip.log 2>&1 &' 2>&1 | grep -aE 'CARD_RC' | tr -d '\r'

# host side
setpci -s $BDF BASE_ADDRESS_0=0xf8000000 BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0 COMMAND=0x06
rmmod octshm_host 2>/dev/null || true
insmod "$DIR/hostmod/octshm_host.ko" base=0xf4000000 dma=1 poll_us=200
ip addr flush dev oct0 2>/dev/null || true
ip addr add 10.9.9.1/24 dev oct0
ip link set oct0 mtu $MTU up
sleep 1.5
echo "[*] ping:"; ping -c3 -W1 -i0.3 10.9.9.2 | grep -E 'loss|rtt'
echo "[*] up. test: sudo iperf3 -c 10.9.9.2 -t6 -R   (reverse = card->host DPI path)"
