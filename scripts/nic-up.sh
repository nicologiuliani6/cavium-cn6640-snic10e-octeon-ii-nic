#!/bin/bash
# nic-up.sh — host side of the bring-up: wait for the card's shared-memory NIC, load
# octnic, and bring oct0/oct1 up. The card configures itself from its baked /etc/rc.local
# (octcarrier + octshm_card + the temp feed), so nothing here needs a serial console.
#
# THE UNLOCK behind the card side: the Vitesse VSC8488 PHY reports link-down on a foreign
# QLogic DAC, so octeon-ethernet never calls cvmx_helper_link_set(up) and the GMX/PKO TX
# path stays disabled (tx_packets stuck at 0) even if you force netif_carrier.
# cardmod/octcarrier.ko calls cvmx_helper_link_set(ipd_port, {up,10G,full}) itself
# (+ re-asserts on a timer) -> xaui TX un-gates -> full 10G both ways.
#
# Optional single-host test rig: if PEER0_DEV/PEER0_MAC (and PEER1_*) name a local 10 GbE
# NIC cabled to the card's SFP+ ports, each peer port is moved into its own netns so the
# traffic actually crosses the wire instead of being short-circuited in RAM. With a real
# external peer (switch, another machine) leave these unset and just use oct0/oct1.
#
# Run: sudo bash nic-up.sh
set -u
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
REPO="$(dirname "$DIR")"
HRX=${HRX:-1}		# host-RAM RX descriptors (matches the card image's rc.local hrx=1)
RXTH=${RXTH:-8}		# parallel host RX drain threads (REV ~2.75G->8.9G). Needs HRX.
NTXQ=${NTXQ:-8}		# multi-queue TX (FWD): N host txqs -> N cores PIO in parallel
ZTX=${ZTX:-}		# zero-copy TX (empty=off). WORKS now (pipelined inbound DPI, xtype fix),
			# but ~6.6G < PIO 7.75G: card-reads-host is read-latency-bound vs posted
			# PIO writes, so PIO ships. Kept for reference / future tuning.
			# NB keep EMPTY not 0 -- ${ZTX:+..} triggers on any non-empty incl "0".
PORTS=${PORTS:-2}	# host netdevs: 1=oct0 only, 2=oct0+oct1
PEER0_DEV=${PEER0_DEV:-}	# optional same-host peer NIC cabled to card xaui0
PEER0_MAC=${PEER0_MAC:-}
PEER1_DEV=${PEER1_DEV:-}	# optional same-host peer NIC cabled to card xaui1
PEER1_MAC=${PEER1_MAC:-}
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

BDF=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
[ -n "$BDF" ] || { echo "FATAL: Cavium 177d:0092 not on PCI bus"; exit 1; }

# peer NIC present? (still in the root ns, or already moved into its netns). Absent is fine.
have_peer() { # dev ns
  [ -n "$1" ] || return 1
  ip link show "$1" >/dev/null 2>&1 || ip netns exec "$2" ip link show "$1" >/dev/null 2>&1
}

# wait for the card-side octshm to be ready (ctrl magic 0x4f435348 over BAR2)
setpci -s $BDF COMMAND=0x06
for i in $(seq 1 30); do
  v=$(python3 -c "import mmap,os;fd=os.open('/sys/bus/pci/devices/0000:$BDF/resource2',os.O_RDONLY);m=mmap.mmap(fd,4096,mmap.PROT_READ);print(int.from_bytes(bytes(m[0:4]),'little'));m.close();os.close(fd)" 2>/dev/null)
  [ "$v" = "1329808200" ] && { echo "card octshm ready (try $i)"; break; }
  sleep 1
done
setpci -s $BDF COMMAND=0x06
# match card PCIe MaxPayload to the bridge (256B); SBR leaves it at 128B = more TLPs/DMA
DC=$(setpci -s $BDF CAP_EXP+8.w 2>/dev/null)
[ -n "$DC" ] && setpci -s $BDF CAP_EXP+8.w=$(printf '%04x' $(( 0x$DC & ~0x00e0 | 0x0020 ))) 2>/dev/null

rmmod octnic 2>/dev/null || true
# octnic auto-discovers BAR2 (base=0) -> `modprobe octnic` if installed, else insmod by path.
OPTS="ports=$PORTS dma=1 poll_us=${POLLUS:-20} ${HRX:+hrx=1} ${RXTH:+rxthreads=$RXTH} ${ZTX:+ztx=1} ${NTXQ:+ntxq=$NTXQ}"
modprobe octnic $OPTS 2>/dev/null || insmod "$REPO/hostmod/octnic.ko" $OPTS

# bring oct$i up; if a same-host peer NIC is configured, also wire it into a private
# netns + subnet (static ARP both sides). Returns 1 when there is no peer to wire.
setup_port() { # oct ns dev peermac cip nip
  local OCT=$1 NS=$2 DEV=$3 PMAC=$4 CIP=$5 NIP=$6
  # octnic registers oct1 a hair after oct0 -> wait for the netdev so its IP actually lands
  local n=0; while ! ip link show $OCT >/dev/null 2>&1 && [ $n -lt 40 ]; do sleep 0.25; n=$((n+1)); done
  nmcli device set $OCT managed no 2>/dev/null || true	# else NM flushes the IP (oct1 loss)
  ip addr flush dev $OCT; ip addr add $CIP/24 dev $OCT; ip link set $OCT mtu 9000 up
  have_peer "$DEV" $NS || { echo "[$OCT] up ($CIP/24, mtu 9000) -- no local peer, test rig skipped"; return 1; }
  ip netns add $NS 2>/dev/null || true
  ip link set $DEV netns $NS 2>/dev/null || true
  ip netns exec $NS ip link set lo up
  local OMAC=$(cat /sys/class/net/$OCT/address)
  ip neigh replace $NIP lladdr $PMAC dev $OCT nud permanent
  ip netns exec $NS ip addr flush dev $DEV
  ip netns exec $NS ip addr add $NIP/24 dev $DEV
  ip netns exec $NS ip link set $DEV up mtu 9000
  ip netns exec $NS ip neigh replace $CIP lladdr $OMAC dev $DEV nud permanent
}
P0=0; P1=0
setup_port oct0 peer0 "$PEER0_DEV" "$PEER0_MAC" 10.9.9.1 10.9.9.2 && P0=1
if [ "$PORTS" -ge 2 ]; then
  setup_port oct1 peer1 "$PEER1_DEV" "$PEER1_MAC" 10.9.10.1 10.9.10.2 && P1=1
fi
if [ "$P0$P1" = 00 ]; then
  echo "oct0${PORTS:+/oct1} up, no same-host peer NIC -- nothing to ping. Card is live."
else
  sleep 2
  [ "$P0" = 1 ] && {
    echo "[ping oct0] $(ping -c3 -W1 10.9.9.2 2>&1 | grep -oE '[0-9]+% packet loss')"
    echo ">>> oct0 iperf: sudo ip netns exec peer0 iperf3 -s -B 10.9.9.2  |  sudo iperf3 -c 10.9.9.2 -B 10.9.9.1 -P8 -t10 [-R]"
  }
  [ "$P1" = 1 ] && {
    echo "[ping oct1] $(ping -c3 -W1 10.9.10.2 2>&1 | grep -oE '[0-9]+% packet loss')"
    echo ">>> oct1 iperf: sudo ip netns exec peer1 iperf3 -s -B 10.9.10.2  |  sudo iperf3 -c 10.9.10.2 -B 10.9.10.1 -P8 -t10 [-R]"
  }
fi
