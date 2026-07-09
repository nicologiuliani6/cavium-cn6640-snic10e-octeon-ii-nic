#!/bin/bash
# 10G bring-up: fast-boot OpenWrt with 8-way multi-core RX, spread the RX IRQs across the
# card's 8 cores, enable RPS, then bring up the DPI-offload NIC. Run AFTER a fresh host
# reboot (the card needs a real power-cycle/PERST so its DRAM retrains cold — a long
# load-test session leaves the DRAM marginal and the fast BAR2 reload can't land the image).
#
# The RX bottleneck was a single NAPI on cpu0 (all xaui0 RX + our tap on one core, ~3.4G
# loopback). octeon-ethernet.receive_group_order=3 -> 8 POW receive groups, PIP flow-hash
# spreads packets across them, each group its own NAPI+IRQ. Affinity pins one group per core.
# Multi-flow traffic (iperf3 -P N) then uses all cores. Single-flow stays on one group (no gain).
set -e
DIR=/home/nico/Desktop/cavium
MTU=${MTU:-9000}
RGO=${RGO:-3}
[ "$(id -u)" = 0 ] || exec sudo MTU="$MTU" RGO="$RGO" "$0" "$@"
BDF=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
[ -n "$BDF" ] || { echo "Cavium not found"; exit 1; }
echo "[*] Cavium $BDF  MTU=$MTU  receive_group_order=$RGO ($((1<<RGO)) RX groups)"

# 1) fast-boot OpenWrt with multi-core RX (retry: BAR2 push is flaky if card DRAM marginal)
booted=0
for t in 1 2 3 4 5; do
  fuser -k /dev/ttyUSB0 2>/dev/null || true; sleep 0.3
  out=$(ADDR=${ADDR:-0x20000000} RGO=$RGO bash "$DIR/fast-boot-bar2.sh" 2>&1 | tr -d '\r')
  if echo "$out" | grep -q 'CRC MATCH'; then echo "[*] boot attempt $t: image OK, booting"; booted=1; break; fi
  echo "[*] boot attempt $t: crc mismatch (marginal DRAM) - retry"
done
[ "$booted" = 1 ] || { echo "[!] fast-boot failed 5x - card DRAM marginal, power-cycle the host and retry"; exit 1; }
sleep 8
BAUD=115200 bash "$DIR/cexec.sh" 'echo BOOT_$((7*7))' 2>/dev/null | grep -q BOOT_49 || { echo "[!] card shell not up"; exit 1; }

# 2) card: firewall off, link up, spread the 8 WORKQ RX IRQs one-per-core, insmod DPI NIC
BAUD=115200 bash "$DIR/ko-xfer.sh" "$DIR/cardmod/octshm_card.ko" /tmp/octshm.ko >/dev/null 2>&1
BAUD=115200 bash "$DIR/cexec.sh" \
  '/etc/init.d/firewall stop 2>/dev/null; iptables -F 2>/dev/null; iptables -P INPUT ACCEPT 2>/dev/null' \
  "ip link set xaui0 nomaster; ip link set xaui0 up mtu $MTU" \
  "ip link set xaui1 nomaster; ip addr flush dev xaui1; ip addr add 10.9.9.2/24 dev xaui1; ip link set xaui1 up mtu $MTU" \
  'c=0; for i in $(grep -l . /proc/irq/*/../*/name 2>/dev/null); do :; done; for irq in $(awk -F: "/Ethernet/{gsub(/ /,\"\",\$1);print \$1}" /proc/interrupts); do printf "%x" $((1<<(c%8))) > /proc/irq/$irq/smp_affinity 2>/dev/null; c=$((c+1)); done; echo IRQ_SPREAD_$c' \
  'for q in /sys/class/net/xaui0/queues/rx-*/rps_cpus; do echo ff > $q 2>/dev/null; done' \
  'insmod /tmp/octshm.ko uplink=xaui0 dma=2 dpiwait=0; echo CARD_RC=$?' \
  'killall iperf3 2>/dev/null; iperf3 -s -B 10.9.9.2 >/tmp/ip.log 2>&1 &' 2>&1 | grep -aE 'IRQ_SPREAD|CARD_RC' | tr -d '\r'

# 3) host side NIC
setpci -s $BDF BASE_ADDRESS_0=0xf8000000 BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0 COMMAND=0x06
rmmod octshm_host 2>/dev/null || true
insmod "$DIR/hostmod/octshm_host.ko" base=0xf4000000 dma=1 poll_us=200
ip addr flush dev oct0 2>/dev/null || true
ip addr add 10.9.9.1/24 dev oct0
ip link set oct0 mtu $MTU up
sleep 1.5
echo "[*] ping:"; ping -c3 -W1 -i0.3 10.9.9.2 | grep -E 'loss|rtt' || true
echo "[*] up. MULTI-FLOW test (exercises all RX cores):"
echo "      sudo iperf3 -c 10.9.9.2 -t8 -R -P 8"
