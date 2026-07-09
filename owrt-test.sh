#!/bin/sh
# Drive the LIVE OpenWrt serial console (after openwrt-boot.sh booted the card)
# to bring up the two SFP+ ports and test the port0<->port1 DAC loopback.
#
# DAC loopback on one host: both ports in the SAME kernel would short-circuit
# via the loopback path and never touch the wire. So we push one port into a
# netns -> traffic must traverse the physical DAC. iperf3 then measures 10G.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.owrt-test.log
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV 115200 raw -echo 2>/dev/null
: > $LOG
# background reader for the whole session
( cat $DEV >> $LOG ) & RP=$!
send(){ printf '%s\n' "$1" > $DEV; sleep "${2:-2}"; }

send ""        1          # wake console
send ""        1          # get root prompt (initramfs: no password)
send "echo MARK_IFACES"   1
send "ip -o link | grep -E 'xaui'" 3
# data ports are xaui0 / xaui1 (10G SFP+). xaui0 sits in br-lan -> free it.
send "ip link set xaui0 nomaster" 2
send "ip netns add ns1" 1
send "ip link set xaui1 netns ns1" 2
send "ip addr add 10.10.0.1/24 dev xaui0" 1
send "ip link set xaui0 up" 2
send "ip -n ns1 addr add 10.10.0.2/24 dev xaui1" 1
send "ip -n ns1 link set xaui1 up" 3
send "sleep 4" 5
send "echo MARK_LINK" 1
send "ip -o link show xaui0; ip -n ns1 -o link show xaui1" 3
send "ethtool xaui0 2>/dev/null | grep -iE 'speed|link'" 2
send "echo MARK_PING" 1
send "ping -c4 10.10.0.2" 8
send "echo MARK_IPERF" 1
send "ip netns exec ns1 iperf3 -s -D" 2
send "iperf3 -c 10.10.0.2 -t 10 -i 1" 16
send "echo MARK_DONE" 2
kill $RP 2>/dev/null
echo "==================== OPENWRT TEST ===================="
cat -v $LOG | tail -120
echo "==================== END ===================="
