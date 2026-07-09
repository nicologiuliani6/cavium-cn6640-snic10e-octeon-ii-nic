#!/usr/bin/env python3
# Robust BAR2 image push: the write-combined mmap of the prefetchable BAR drops/corrupts
# sparse words under load, so write each chunk then read it back and retry until it matches.
# DRAM itself is good (u-boot native writes verify), the loss is purely in the WC store path.
import mmap, os, sys, hashlib, time

img = sys.argv[1]
off = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0   # BAR2 window offset to write at
CHUNK = 4096
MAXRETRY = 40

data = open(img, 'rb').read()
fd = os.open("/sys/bus/pci/devices/0000:02:00.0/resource2", os.O_RDWR)
m = mmap.mmap(fd, 64 * 1024 * 1024, mmap.MAP_SHARED)

t0 = time.time()
bad_total = 0
for base in range(0, len(data), CHUNK):
    src = data[base:base + CHUNK]
    dst = off + base
    for attempt in range(MAXRETRY):
        m[dst:dst + len(src)] = src
        m.flush()
        rb = m[dst:dst + len(src)]        # readback forces WC flush + verifies
        if rb == src:
            break
        bad_total += 1
    else:
        print("FAIL: chunk at 0x%x never matched after %d tries" % (base, MAXRETRY))
        m.close(); os.close(fd); sys.exit(1)

# final full verify
rb = m[off:off + len(data)]
ok = hashlib.md5(rb).hexdigest() == hashlib.md5(data).hexdigest()
print("pushed %d bytes in %.1fs, chunk-retries=%d, full-verify=%s"
      % (len(data), time.time() - t0, bad_total, "OK" if ok else "MISMATCH"))
m.close(); os.close(fd)
sys.exit(0 if ok else 1)
