#!/bin/bash
# twocard-up.sh — bring up the Cavium <-> NC523 10G link and both iperf paths.
#
# THE UNLOCK: the DAC connects NC523 enp1s0f1 <-> card xaui0. xaui0 RX works but
# its Vitesse VSC8488 PHY reports link-down on the foreign QLogic DAC, so the
# octeon-ethernet poll never calls cvmx_helper_link_set(up) and the GMX/PKO TX
# path stays DISABLED (tx_packets stuck at 0) even if you force netif_carrier.
# cardmod/octcarrier.ko calls cvmx_helper_link_set(ipd_port0, {up,10G,full})
# itself (+ re-asserts on a timer) -> xaui0 TX un-gates -> full 10G both ways.
#
# Assumes the card is already booted (boot-clean.sh) with /tmp/octshm.ko present.
# Run: sudo bash twocard-up.sh   [MODE=wire|host]  (default host)
set -u
DIR=/home/nico/Desktop/cavium
MODE=${MODE:-host}
HRX=${HRX:-1}		# host-RAM RX descriptors (matches card image rc.local hrx=1)
RXTH=${RXTH:-8}		# parallel host RX drain threads (REV ~2.75G->7G). Needs HRX.
ZTX=${ZTX:-}		# zero-copy TX (empty=off). BROKEN: inbound DPI corrupts host->card DMA.
			# NB keep EMPTY not 0 -- ${ZTX:+..} triggers on any non-empty incl "0".
NCNS=nc
NCDEV=${NCDEV:-enp1s0f1}
NCMAC=${NCMAC:-c4:34:6b:cc:38:fc}
CARD_XAUI0_MAC=00:0f:b7:96:8f:4c
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

BDF=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
BAR2=$(printf '0x%08x' $(( $(awk 'NR==3{print $1}' /sys/bus/pci/devices/0000:$BDF/resource) & 0xffffffff )))

# NC523 into a netns so both ends can live on one host without route confusion
ip netns add $NCNS 2>/dev/null || true
ip link set $NCDEV netns $NCNS 2>/dev/null || true
ip netns exec $NCNS ip link set lo up

# transfer card modules (idempotent): octcarrier (xaui0 link force) + octshm (host NIC)
BAUD=115200 bash "$DIR/ko-xfer.sh" "$DIR/cardmod/octcarrier.ko" /tmp/octcarrier.ko >/dev/null 2>&1
[ "$MODE" = wire ] || BAUD=115200 bash "$DIR/ko-xfer.sh" "$DIR/cardmod/octshm_card.ko" /tmp/octshm.ko >/dev/null 2>&1

if [ "$MODE" = wire ]; then
  # direct card xaui0 <-> NC523, no octshm (measures the raw 10G SFP+ link)
  C=192.168.50.1; N=192.168.50.2
  BAUD=115200 bash "$DIR/cexec.sh" \
    'rmmod octshm_card 2>/dev/null; /etc/init.d/firewall stop 2>/dev/null; iptables -F; iptables -P INPUT ACCEPT' \
    "ip addr flush dev xaui0; ip addr add $C/24 dev xaui0; ip link set xaui0 up mtu 9000 promisc on" \
    "ip neigh replace $N lladdr $NCMAC dev xaui0 nud permanent" \
    'ethtool -A xaui0 rx off tx off 2>/dev/null' \
    'rmmod octcarrier 2>/dev/null; insmod /tmp/octcarrier.ko dev=xaui0 ipd_port=0; sleep 1; echo "carrier=$(cat /sys/class/net/xaui0/carrier)"' \
    'killall iperf3 2>/dev/null; iperf3 -s -B '"$C"' >/tmp/ip.log 2>&1 & echo SRV_UP' 2>&1 | tr -d '\r' | grep -aE 'carrier=|SRV_UP'
  ip netns exec $NCNS ip addr flush dev $NCDEV
  ip netns exec $NCNS ip addr add $N/24 dev $NCDEV
  ip netns exec $NCNS ip link set $NCDEV up mtu 9000
  ip netns exec $NCNS ip neigh replace $C lladdr $CARD_XAUI0_MAC dev $NCDEV nud permanent
  sleep 1
  echo "[ping] $(ip netns exec $NCNS ping -c3 -W1 $C 2>&1 | grep -oE '[0-9]+% packet loss')"
  echo ">>> iperf: sudo ip netns exec $NCNS iperf3 -c $C -P4 -t10   (add -R for reverse)"
else
  # host oct0 <-> NC523 through the card L2 bridge over the 10G wire.
  # NOSER=1: the card self-configures at boot (baked /etc/rc.local: octcarrier +
  # octshm_card + temp daemon), so skip the serial cexec entirely = fully no-serial.
  C=10.9.9.1; N=10.9.9.2
  if [ "${NOSER:-0}" != 1 ]; then
    BAUD=115200 bash "$DIR/cexec.sh" \
      'echo 0 > /proc/sys/kernel/printk; /etc/init.d/firewall stop 2>/dev/null; iptables -F; iptables -P INPUT ACCEPT' \
      'ip addr flush dev xaui0; ip link set xaui0 up mtu 9000 promisc on' \
      'rmmod octcarrier 2>/dev/null; insmod /tmp/octcarrier.ko dev=xaui0 ipd_port=0' \
      'rmmod octshm_card 2>/dev/null; insmod /tmp/octshm.ko uplink=xaui0 dma=2 dpiwait=0; echo CARD_RC=$?' \
      'kill $(cat /tmp/tempd.pid 2>/dev/null) 2>/dev/null; (while :; do t1=$(cat /sys/class/hwmon/hwmon0/temp1_input 2>/dev/null); t2=$(cat /sys/class/hwmon/hwmon0/temp2_input 2>/dev/null); [ -n "$t1" ] && echo "$t1 $t2" > /proc/octshm/temp; sleep 5; done) & echo $! > /tmp/tempd.pid' 2>&1 | tr -d '\r' | grep -aE 'CARD_RC='
  else
    # wait for the card-side octshm to be ready (ctrl magic 0x4f435348 over BAR2)
    setpci -s $BDF COMMAND=0x06
    for i in $(seq 1 30); do
      v=$(python3 -c "import mmap,os;fd=os.open('/sys/bus/pci/devices/0000:$BDF/resource2',os.O_RDONLY);m=mmap.mmap(fd,4096,mmap.PROT_READ);print(int.from_bytes(bytes(m[0:4]),'little'));m.close();os.close(fd)" 2>/dev/null)
      [ "$v" = "1329808200" ] && { echo "card octshm ready (try $i)"; break; }
      sleep 1
    done
  fi
  setpci -s $BDF COMMAND=0x06
  rmmod octshm_host 2>/dev/null || true
  insmod "$DIR/hostmod/octshm_host.ko" base=$BAR2 dma=1 poll_us=${POLLUS:-20} ${HRX:+hrx=1} ${RXTH:+rxthreads=$RXTH} ${ZTX:+ztx=1}
  ip addr flush dev oct0; ip addr add $C/24 dev oct0; ip link set oct0 mtu 9000 up
  OCT0MAC=$(cat /sys/class/net/oct0/address)
  ip neigh replace $N lladdr $NCMAC dev oct0 nud permanent
  ip netns exec $NCNS ip addr flush dev $NCDEV
  ip netns exec $NCNS ip addr add $N/24 dev $NCDEV
  ip netns exec $NCNS ip link set $NCDEV up mtu 9000
  ip netns exec $NCNS ip neigh replace $C lladdr $OCT0MAC dev $NCDEV nud permanent
  sleep 2
  echo "[ping] $(ping -c3 -W1 $N 2>&1 | grep -oE '[0-9]+% packet loss')"
  echo ">>> iperf server: sudo ip netns exec $NCNS iperf3 -s -B $N"
  echo ">>> iperf client: sudo iperf3 -c $N -B $C -P4 -t10   (add -R for reverse)"
fi
