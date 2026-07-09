#!/bin/sh
# Follow-up perf: parallel TCP + UDP to find the real ceiling of the DAC link.
# xaui0 (root ns) <-> xaui1 (ns1). Server already may be running; restart clean.
DEV=/dev/ttyUSB0
LOG=/home/nico/Desktop/cavium/.owrt-perf2.log
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
stty -F $DEV 115200 raw -echo 2>/dev/null
: > $LOG
( cat $DEV >> $LOG ) & RP=$!
send(){ printf '%s\n' "$1" > $DEV; sleep "${2:-2}"; }
send "" 1
send "killall iperf3 2>/dev/null; sleep 1; ip netns exec ns1 iperf3 -s -D" 2
send "echo MARK_CPUINFO" 1
send "grep -c ^processor /proc/cpuinfo; grep -m1 'cpu model' /proc/cpuinfo" 2
send "echo MARK_TCP_P8" 1
send "iperf3 -c 10.10.0.2 -t 10 -P 8 -i 0" 14
send "echo MARK_UDP" 1
send "iperf3 -c 10.10.0.2 -u -b 10G -t 10 -i 1" 14
send "echo MARK_DONE" 2
kill $RP 2>/dev/null
echo "==================== PERF2 ===================="
cat -v $LOG | tail -120
echo "==================== END ===================="
