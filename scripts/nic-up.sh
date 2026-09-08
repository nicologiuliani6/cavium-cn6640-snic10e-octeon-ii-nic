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
# Run: sudo bash nic-up.sh     (IP0/IP1 override the addresses put on oct0/oct1)
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
IP0=${IP0:-10.9.9.1/24}		# address for oct0 (card xaui0)
IP1=${IP1:-10.9.10.1/24}	# address for oct1 (card xaui1)
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

BDF=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
[ -n "$BDF" ] || { echo "FATAL: Cavium 177d:0092 not on PCI bus"; exit 1; }

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

setup_port() { # oct cidr
  local OCT=$1 CIDR=$2
  # octnic registers oct1 a hair after oct0 -> wait for the netdev so its IP actually lands
  local n=0; while ! ip link show $OCT >/dev/null 2>&1 && [ $n -lt 40 ]; do sleep 0.25; n=$((n+1)); done
  ip link show $OCT >/dev/null 2>&1 || { echo "FATAL: $OCT never appeared (octnic not loaded?)"; return 1; }
  nmcli device set $OCT managed no 2>/dev/null || true	# else NM flushes the IP (oct1 loss)
  ip addr flush dev $OCT; ip addr add $CIDR dev $OCT; ip link set $OCT mtu 9000 up
  echo "[$OCT] up ($CIDR, mtu 9000)"
}
setup_port oct0 "$IP0"
[ "$PORTS" -ge 2 ] && setup_port oct1 "$IP1"
exit 0
