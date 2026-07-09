#!/bin/bash
# nic-up.sh — bring up the octshm NIC on an already-booted card (no reboot).
# Card module reload leaves stale DPI/tap state, so ALWAYS boot-clean first, then run this ONCE.
# Flags: CARDF / HOSTF env (e.g. CARDF="lockfree=1" HOSTF="lockfree=1").
set -u
DIR=/home/nico/Desktop/cavium
MTU=${MTU:-9000}
CARDF=${CARDF:-}
HOSTF=${HOSTF:-}
# auto-detect PCI location + BIOS-assigned BARs (survives topology changes)
BDF=${BDF:-$(lspci -d 177d:0092 | awk '{print $1}' | head -1)}
[ -n "$BDF" ] || { echo "nic-up: Cavium 177d:0092 not found"; exit 1; }
RES=/sys/bus/pci/devices/0000:$BDF/resource
BAR0=${BAR0:-$(awk 'NR==1{print $1}' "$RES")}
BAR2=${BAR2:-$(awk 'NR==3{print $1}' "$RES")}
BAR0LO=$(printf '0x%08x' $(( BAR0 & 0xffffffff )))
BAR2LO=$(printf '0x%08x' $(( (BAR2 & 0xffffffff) | 0xc )))
BAR2HI=$(printf '0x%08x' $(( BAR2 >> 32 )))
BAR2BASE=$(printf '0x%08x' $(( BAR2 & 0xffffffff )))

# transfer module (idempotent) + card side, console silenced to stop the printk flood
BAUD=115200 bash "$DIR/ko-xfer.sh" "$DIR/cardmod/octshm_card.ko" /tmp/octshm.ko >/dev/null 2>&1
BAUD=115200 bash "$DIR/cexec.sh" \
  'echo 0 > /proc/sys/kernel/printk' \
  '/etc/init.d/firewall stop 2>/dev/null; iptables -F; iptables -P INPUT ACCEPT' \
  "ip link set xaui0 nomaster; ip link set xaui0 down mtu $MTU" \
  "ip link set xaui1 nomaster; ip addr flush dev xaui1; ip addr add 10.9.9.2/24 dev xaui1; ip link set xaui1 up mtu $MTU" \
  'c=0; for irq in $(awk -F: "/Ethernet/{gsub(/ /,\"\",\$1);print \$1}" /proc/interrupts); do printf "%x" $((1<<(c%8))) > /proc/irq/$irq/smp_affinity 2>/dev/null; c=$((c+1)); done' \
  'for q in /sys/class/net/xaui1/queues/rx-*/rps_cpus; do echo ff > $q 2>/dev/null; done' \
  "insmod /tmp/octshm.ko uplink=xaui1 dma=2 dpiwait=0 $CARDF; echo CARD_RC=\$?" \
  'killall iperf3 2>/dev/null; sleep 0.3; iperf3 -s -B 10.9.9.2 >/tmp/ip.log 2>&1 & echo SRV_UP' 2>&1 | tr -d '\r' | grep -aE 'CARD_RC|SRV_UP'

# host side — bus master (0x04) is mandatory: card DPI DMAs into host RAM
setpci -s $BDF BASE_ADDRESS_0=$BAR0LO BASE_ADDRESS_2=$BAR2LO BASE_ADDRESS_3=$BAR2HI COMMAND=0x06
rmmod octshm_host 2>/dev/null || true
insmod "$DIR/hostmod/octshm_host.ko" base=$BAR2BASE dma=1 poll_us=200 $HOSTF
ip addr flush dev oct0 2>/dev/null || true
ip addr add 10.9.9.1/24 dev oct0
ip link set oct0 mtu $MTU up
sleep 1.5
echo "[ping] $(ping -c3 -W1 -i0.3 10.9.9.2 2>&1 | grep -oE '[0-9]+% packet loss')"
