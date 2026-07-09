#!/bin/bash
# Direct-wire iperf: NC523 host <-> Cavium OpenWrt xaui (no octshm/oct0).
# Usage: sudo bash wire-iperf.sh
set -u
DIR=/home/nico/Desktop/cavium
DEV=${DEV:-$(ls /dev/serial/by-id/*FT232* 2>/dev/null | head -1)}
DEV=${DEV:-/dev/ttyUSB0}
BAUD=115200
HOST_IP=10.10.10.1
CARD_IP=10.10.10.2
MTU=${MTU:-1500}

[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

mkdir -p /tmp/claude-1000

# wake serial
for _ in 1 2 3 4 5; do printf '\003' > "$DEV" 2>/dev/null || true; sleep 0.12; done
printf '\r\n' > "$DEV" 2>/dev/null || true; sleep 0.4

pick_host() {
  for d in enp1s0f0 enp1s0f1 enp2s0f0 enp2s0f1; do
    [ -d "/sys/class/net/$d" ] || continue
    ip link set "$d" up 2>/dev/null || true
    ethtool "$d" 2>/dev/null | grep -q 'Link detected: yes' && { echo "$d"; return; }
  done
  echo enp1s0f1
}

HOST_DEV=$(pick_host)
echo "[*] host=$HOST_DEV  serial=$DEV"

# stop NM touching NC ports
for d in enp1s0f0 enp1s0f1 enp2s0f0 enp2s0f1; do
  [ -d "/sys/class/net/$d" ] || continue
  nmcli dev disconnect "$d" 2>/dev/null || true
  nmcli dev set "$d" managed no 2>/dev/null || true
  ip addr flush dev "$d" 2>/dev/null || true
done
rmmod octshm_host 2>/dev/null || true
ip link set oct0 down 2>/dev/null || true

# qlcnic: no MSI + pause off
ethtool -A "$HOST_DEV" autoneg off rx off tx off 2>/dev/null || true
ethtool -K "$HOST_DEV" gro off lro off gso off tso off 2>/dev/null || true
ip addr add "$HOST_IP/24" dev "$HOST_DEV" 2>/dev/null || ip addr replace "$HOST_IP/24" dev "$HOST_DEV"
ip link set "$HOST_DEV" mtu "$MTU" up promisc on
sysctl -w net.ipv4.conf."$HOST_DEV".rp_filter=0 >/dev/null
sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null

CARD_MAC=00:0f:b7:96:8f:4d
HOST_MAC=$(cat "/sys/class/net/$HOST_DEV/address")
ip neigh replace "$CARD_IP" lladdr "$CARD_MAC" dev "$HOST_DEV" nud permanent

echo "[*] host: $(ip -br addr show "$HOST_DEV")"
echo "[*] link: $(ethtool "$HOST_DEV" 2>/dev/null | grep -i 'Link detected')"

# card: MUST rmmod octshm (rx_handler steals xaui1 RX)
export BAUD DEV
CARD_LOG=/tmp/claude-1000/wire-card.log
bash "$DIR/cexec.sh" \
  'rmmod octshm_card 2>/dev/null; echo RM_OK' \
  '/etc/init.d/firewall stop 2>/dev/null' \
  'ip link set xaui0 nomaster; ip link set xaui0 down' \
  'ip link set xaui1 nomaster' \
  'ip addr flush dev xaui1' \
  "ip addr add $CARD_IP/24 dev xaui1" \
  "ip link set xaui1 mtu $MTU up promisc on" \
  "ip neigh replace $HOST_IP lladdr $HOST_MAC dev xaui1 nud permanent" \
  'ethtool -A xaui1 autoneg off rx off tx off 2>/dev/null' \
  'ip -br addr show xaui1' \
  'killall iperf3 2>/dev/null; iperf3 -s -B '"$CARD_IP"' >/tmp/ip.log 2>&1 & sleep 0.5; echo SRV_OK' \
  > "$CARD_LOG" 2>&1

echo "=== card ==="
grep -aE 'RM_OK|SRV_OK|xaui1|10\.10\.10' "$CARD_LOG" | tail -6
grep -q SRV_OK "$CARD_LOG" || echo "WARN: serial SRV_OK missing"

# verify host still has correct src IP
ip addr show "$HOST_DEV" | grep -q "$HOST_IP" || ip addr replace "$HOST_IP/24" dev "$HOST_DEV"

sleep 1
echo "[*] ping $CARD_IP (src must be $HOST_IP)"
ping -c5 -W1 -I "$HOST_DEV" "$CARD_IP" || true
RX=$(ethtool -S "$HOST_DEV" 2>/dev/null | awk '/rx_pkts:/{print $2}')
TX=$(cat "/sys/class/net/$HOST_DEV/statistics/tx_packets")
echo "[*] qlcnic rx_pkts=$RX host_tx=$TX"

if ping -c1 -W1 -I "$HOST_DEV" "$CARD_IP" >/dev/null 2>&1; then
  echo "=== iperf host->card 15s ==="
  iperf3 -c "$CARD_IP" -B "$HOST_IP" -t15 -i1
  echo "=== iperf card->host 15s ==="
  killall iperf3 2>/dev/null; sleep 0.3
  iperf3 -s -B "$HOST_IP" -D
  sleep 0.5
  bash "$DIR/cexec.sh" "iperf3 -c $HOST_IP -B $CARD_IP -t15 -i1"
  killall iperf3 2>/dev/null
  exit 0
fi

# fallback: octshm PCIe path (needs DAC loopback xaui0<->xaui1 on Cavium)
echo "[*] wire fail -> try octshm PCIe path (need loopback DAC xaui0<->xaui1)"
BDF=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
[ -n "$BDF" ] || { echo "no cavium pci"; exit 1; }
bash "$DIR/nic-up.sh" 2>&1 | tail -3
sleep 2
if ping -c3 -W1 10.9.9.2 >/dev/null 2>&1; then
  echo "=== iperf octshm reverse 15s ==="
  iperf3 -c 10.9.9.2 -t15 -R -P4 -i1 | tail -8
  echo "=== iperf octshm forward 15s ==="
  iperf3 -c 10.9.9.2 -t15 -P4 -i1 | tail -8
  exit 0
fi
echo "[FAIL] wire + octshm both down"
exit 1
