#!/usr/bin/env python3
# Read the first N bytes of the card's 64MB PCI window (BAR2 / resource2) from
# the host. Used to check whether card-side BAR1-index programming made the
# window readable (magic value) instead of all-0xFF.
import mmap, os, sys, struct
DEV = "/sys/bus/pci/devices/0000:04:00.0/resource2"
N = 64
fd = os.open(DEV, os.O_RDWR)
try:
    m = mmap.mmap(fd, N, mmap.MAP_SHARED, mmap.PROT_READ)
except Exception as e:
    print("mmap failed:", e); sys.exit(2)
data = m[:N]
print("first %d bytes of resource2:" % N)
for off in range(0, N, 8):
    q_le = struct.unpack_from("<Q", data, off)[0]
    q_be = struct.unpack_from(">Q", data, off)[0]
    print("  +0x%02x: LE=0x%016x  BE=0x%016x" % (off, q_le, q_be))
allff = all(b == 0xFF for b in data)
print("ALL-0xFF" if allff else "*** NON-FF DATA PRESENT ***")
m.close(); os.close(fd)
