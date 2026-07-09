#!/bin/sh
# Host side of the ordered-preloaded attempt. Waits for the serial orchestrator
# to signal CARD_READY (firmware in RAM, card at prompt), restores the host config
# BARs, loads the liquidio driver (preloaded=1) so it programs the host DMA queues
# and enters its 30s "started" wait, then signals GO_BOOTOCT so the orchestrator
# boots the firmware into the ready queues.
LB=/home/nico/Desktop/cavium/lio-build/drivers/net/ethernet/cavium/liquidio
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

echo "[host] waiting for CARD_READY..."
w=0; while [ ! -f /tmp/claude-1000/CARD_READY ] && [ $w -lt 600 ]; do sleep 0.5; w=$((w+1)); done
[ -f /tmp/claude-1000/CARD_READY ] || { echo "[host] CARD_READY timeout"; exit 1; }

echo "[host] restore config BARs (mem only)"
setpci -s 04:00.0 BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0
setpci -s 04:00.0 BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0
setpci -s 04:00.0 COMMAND=0x02

dmesg -C
rmmod liquidio 2>/dev/null; rmmod liquidio_core 2>/dev/null
insmod "$LB/liquidio-core.ko"
echo "[host] insmod liquidio preloaded=1 (bg)"
insmod "$LB/liquidio.ko" preloaded=1 fw_type=nic &
INS=$!
sleep 6
echo "[host] GO_BOOTOCT (probe done, queues programmed)"
touch /tmp/claude-1000/GO_BOOTOCT
wait $INS 2>/dev/null
sleep 3
echo "==================== HOST DMESG ===================="
dmesg | grep -iE 'liquidio|octeon|core|nic|firmware|drv' | tail -40
echo "==================== NETDEVS ===================="
ls /sys/class/net/ | grep -vE 'veth|docker|br-|^lo$|wlx|enp7' || echo "(no new eth)"
echo "==================== END ===================="
