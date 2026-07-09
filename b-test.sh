#!/bin/sh
# B test: enable CN66xx DPI packet-output engine on card, TX out npi0 (PKO port32),
# see if it reaches host octoq_host OQ0. octoq_host (pinned, hardened) is armed FIRST
# so any card->host DMA targets the pinned ring (crash-mitigation).
DEV=/dev/ttyUSB0
CARD=02:00.0
HM=/home/nico/Desktop/cavium/hostmod
KO=/home/nico/Desktop/cavium/octdpi/octpktdpi_card.ko
L=/home/nico/Desktop/cavium/.b-test.log
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
exec > >(tee "$L") 2>&1
cmd(){ printf '%s\r\n' "$1" > $DEV; sleep "${2:-1.2}"; }
rdserial(){ python3 -c "
import serial,time
s=serial.Serial('$DEV',115200,timeout=0);time.sleep(0.2)
print(bytes(b for b in s.read(9000) if 32<=b<127 or b in(10,13)).decode('ascii','replace'));s.close()"; }

echo "==== [1] host: BARs+busmaster, arm hardened octoq_host OQ0 (pinned) ===="
rmmod octoq_host 2>/dev/null
setpci -s $CARD BASE_ADDRESS_0=0xf800000c BASE_ADDRESS_1=0 BASE_ADDRESS_2=0xf400000c BASE_ADDRESS_3=0 COMMAND=0x06
dmesg -C
insmod "$HM/octoq_host.ko" q=0 ndesc=256 bufsz=2048
sleep 1
dmesg | grep -i octoq | tail -3

echo "==== [2] transfer DPI packet module to card ===="
pkill -9 -f 'cat /dev/ttyUSB0' 2>/dev/null
stty -F $DEV 115200 raw -echo 2>/dev/null
bash /home/nico/Desktop/cavium/ko-xfer-115.sh "$KO" /tmp/pkt.ko 2>&1 | tail -3

echo "==== [3] card: enable DPI packet engine, TX out npi0 ===="
printf '\r\n' > $DEV; sleep 0.4
cmd "insmod /tmp/pkt.ko" 2.5
cmd "dmesg | tail -4" 1.5
rdserial
cmd "ip link set npi0 up; ip addr add 10.9.0.1/24 dev npi0 2>/dev/null; echo NPIUP" 1.5
cmd "for i in \$(seq 1 40); do arping -c1 -w1 -I npi0 10.9.0.88 >/dev/null 2>&1; done; echo TXDONE; cat /sys/class/net/npi0/statistics/tx_packets" 8
rdserial

echo "==== [4] host: did OQ0 receive? ===="
sleep 1
dmesg | grep -i octoq | tail -8
python3 - <<'PY'
import mmap,os,struct
sz=os.path.getsize('/sys/bus/pci/devices/0000:02:00.0/resource0')
fd=os.open('/sys/bus/pci/devices/0000:02:00.0/resource0',os.O_RDWR)
m=mmap.mmap(fd,sz,mmap.MAP_SHARED)
rd=lambda o: struct.unpack('<I',m[o:o+4])[0]
print("OQ_PKTS_SENT q0..3:", [hex(rd(0x2400+q*0x10)) for q in range(4)])
print("OQ_CREDITS   q0..3:", [hex(rd(0x1800+q*0x10)) for q in range(4)])
m.close();os.close(fd)
PY

echo "==== [5] safe teardown ===="
cmd "rmmod octpktdpi_card 2>/dev/null; echo CARDCLEAN" 1.5
rmmod octoq_host 2>/dev/null && echo "octoq_host unloaded (hardened)"
echo "==================== END ===================="
