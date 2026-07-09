#!/bin/bash
# Dopo reboot host: card sana? PCI-console u-boot viva su PCIe? (go/no-go iniettore)
cd /home/nico/Desktop/cavium/hostmod || exit 1
setpci -s 02:00.0 BASE_ADDRESS_0=0xf800000c COMMAND=0x06 2>/dev/null
dmesg -C
echo "=== [1] salute SLI (STATUS deve essere ~0x82002, NON 0xffff) ==="
insmod octwin.ko bar0=0xf8000000 2>/dev/null; rmmod octwin 2>/dev/null
dmesg | grep -a "SLI_CTL_STATUS"
dmesg -C
echo "=== [2] DRAM leggibile? console desc @0x6c000 (rawrd) ==="
insmod octwin.ko bar0=0xf8000000 rawrd=1 rd_addr=0x6c000 2>/dev/null; rmmod octwin 2>/dev/null
dmesg | grep -a "octwin: \[0x6c"
echo
echo ">>> se STATUS != 0xffff  E  0x6c000 non e' tutto 0xffff  => console viva, iniettore OK"
echo ">>> se ancora 0xffff => card non ha rifatto u-boot (bootdelay non -1, o serve seriale)"
