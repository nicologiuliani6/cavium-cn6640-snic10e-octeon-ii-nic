#!/bin/bash
# Restore the working 1.09 Gbit/s RX-DMA hybrid NIC.
# Preconditions: Cavium card running OpenWrt at shell prompt on /dev/ttyUSB0 @115200,
# host sees the EP (lspci -d 177d:), IOMMU on. Run with sudo.
set -e
DIR=/home/nico/Desktop/cavium
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

# find the Cavium BDF (moves across reseats/reboots)
BDF=$(lspci -d 177d:0092 | awk '{print $1}' | head -1)
[ -n "$BDF" ] || { echo "Cavium not found; try: echo 1 > /sys/bus/pci/rescan"; exit 1; }
echo "[*] Cavium at $BDF"

# transfer + load card module, config xaui, quiet console, firewall off
python3 - "$DIR" <<'PY'
import serial,time,sys,subprocess,hashlib
DIR=sys.argv[1]
# transfer ko via existing base64 xfer helper (reuse ko-xfer-115.sh)
subprocess.run(["bash",f"{DIR}/ko-xfer-115.sh",f"{DIR}/cardmod/octshm_card.ko","/tmp/octshm.ko"],
               check=False)
s=serial.Serial('/dev/ttyUSB0',115200,timeout=0)
def cmd(c,w=2.5):
    s.reset_input_buffer(); s.write(c.encode()+b'\r\n'); s.flush(); time.sleep(w)
    return bytes(x for x in s.read(40000) if 32<=x<127 or x in(10,13)).decode('ascii','replace')
cmd("dmesg -n 1",1)
cmd("/etc/init.d/firewall stop 2>/dev/null; iptables -P INPUT ACCEPT 2>/dev/null",2)
cmd("rmmod octshm_card 2>/dev/null",2)
cmd("ip link set xaui0 nomaster; ip link set xaui0 up mtu 9000",2)
cmd("ip link set xaui1 nomaster; ip addr flush dev xaui1; ip addr add 10.9.9.2/24 dev xaui1; ip link set xaui1 up mtu 9000",2)
print("card insmod:",cmd("insmod /tmp/octshm.ko uplink=xaui0 dma=2 dpiwait=0; echo RC=$?",3)[-40:])
cmd("killall iperf3 2>/dev/null; iperf3 -s -B 10.9.9.2 >/tmp/ip.log 2>&1 &",2)
s.close()
PY

echo "[*] host: BAR2 + module + oct0"
setpci -s $BDF BASE_ADDRESS_0=0xf8000000
setpci -s $BDF BASE_ADDRESS_2=0xf400000c
setpci -s $BDF BASE_ADDRESS_3=0x00000000
setpci -s $BDF COMMAND=0x06
rmmod octshm_host 2>/dev/null || true
insmod "$DIR/hostmod/octshm_host.ko" base=0xf4000000 dma=1
ip addr flush dev oct0 2>/dev/null || true
ip addr add 10.9.9.1/24 dev oct0
ip link set oct0 mtu 9000 up
sleep 1.5
echo "[*] ping test:"
ping -c3 -W1 -i0.3 10.9.9.2 | grep -E 'packet loss|rtt'
echo "[*] restored. iperf: sudo iperf3 -c 10.9.9.2 -t5"
