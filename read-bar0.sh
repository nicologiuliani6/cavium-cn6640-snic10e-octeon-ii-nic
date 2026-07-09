#!/bin/sh
# Read card BAR0 (SLI CSRs) directly. Run AFTER load-fw.sh, BEFORE any driver.
# Valid SLI regs (not all-FF) = host can reach the Octeon -> driver path viable.
if [ "$(id -u)" != "0" ]; then echo "run with sudo"; exit 1; fi
echo "=== enable mem+master decode ==="
setpci -s 04:00.0 COMMAND=0x06
setpci -s 04:00.0 COMMAND
echo "=== BAR0 reads at SLI offsets ==="
python3 - <<'PY'
import mmap, struct, os
try:
    fd=os.open("/sys/bus/pci/devices/0000:04:00.0/resource0", os.O_RDWR|os.O_SYNC)
    m=mmap.mmap(fd,0x4000,offset=0)
    names={0x3E00:"SLI_MAC_NUMBER",0x3D80:"SLI_S2M_PORT0_CTL",0x0:"base",0x2000:"mid",0x3F00:"hi"}
    allff=True
    for off,nm in names.items():
        m.seek(off); v=struct.unpack("<Q",m.read(8))[0]
        if v!=0xffffffffffffffff: allff=False
        print(f"  0x{off:04x} {nm:18s} = 0x{v:016x}")
    print("VERDICT:", "ALL-FF (host cannot read SLI -> blocked)" if allff else "VALID DATA (host can read SLI -> proceed!)")
except Exception as e:
    print("read failed:",e)
PY
