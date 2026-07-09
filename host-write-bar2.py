#!/usr/bin/env python3
# Host writes a pattern into the card's DRAM through BAR2, to prove host->card
# direction of the shared-memory channel (card verifies via u-boot read64).
import mmap, os, struct
fd = os.open("/sys/bus/pci/devices/0000:04:00.0/resource2", os.O_RDWR)
m = mmap.mmap(fd, 4096, mmap.MAP_SHARED)
m[0x100:0x108] = struct.pack(">Q", 0x1122334455667788)
m.flush()
print("host readback @0x100 BE=0x%016x" % struct.unpack(">Q", m[0x100:0x108])[0])
m.close(); os.close(fd)
print("host wrote 0x1122334455667788 -> card DRAM 0x02000100")
