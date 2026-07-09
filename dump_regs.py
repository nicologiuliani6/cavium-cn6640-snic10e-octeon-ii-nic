#!/usr/bin/env python3
# Dump the card's packet-path CSRs from the host:
#  - all of BAR0 (SLI_PKT_* register file, 0x0000-0x4000) via direct mmio
#  - DPI / PKO / selected NCB CSRs via the BAR0 window (liquidio protocol)
# Usage: sudo python3 dump_regs.py <label>   -> writes /tmp/claude-1000/regs-<label>.txt
import mmap, os, struct, sys, time

label = sys.argv[1] if len(sys.argv) > 1 else "dump"
out = open(f"/tmp/claude-1000/regs-{label}.txt", "w")

RES0 = "/sys/bus/pci/devices/0000:02:00.0/resource0"
sz = os.path.getsize(RES0)
fd = os.open(RES0, os.O_RDWR)
m = mmap.mmap(fd, sz, mmap.MAP_SHARED)

def r32(o): return struct.unpack('<I', m[o:o+4])[0]
def w32(o, v): m[o:o+4] = struct.pack('<I', v)
def r64(o): return struct.unpack('<Q', m[o:o+8])[0]
def w64(o, v): m[o:o+8] = struct.pack('<Q', v)

# ---- BAR0 direct dump (SLI_PKT register file) ----
out.write("== BAR0 (u64 non-zero) ==\n")
for o in range(0, sz, 8):
    v = r64(o)
    if v not in (0, 0xffffffffffffffff):
        out.write(f"BAR0+0x{o:04x} = 0x{v:016x}\n")

# ---- window reads (liquidio protocol) ----
WIN_WR_ADDR = 0x00
WIN_RD_ADDR_LO = 0x10
WIN_RD_ADDR_HI = 0x14
WIN_RD_DATA = 0x40

def win_rd(addr):
    hi = ((addr >> 32) & 0xffffffff) | 0x00060000
    w32(WIN_RD_ADDR_HI, hi); r32(WIN_RD_ADDR_HI)
    w32(WIN_RD_ADDR_LO, addr & 0xffffffff); r32(WIN_RD_ADDR_LO)
    return r64(WIN_RD_DATA)

def dump(name, addr):
    v = win_rd(addr)
    out.write(f"{name} @0x{addr:016x} = 0x{v:016x}\n")

out.write("== DPI ==\n")
dump("DPI_CTL",          0x0001DF0000000000)
dump("DPI_DMA_CONTROL",  0x0001DF0000000048)
dump("DPI_REQ_GBL_EN",   0x0001DF0000000050)
dump("DPI_PKT_ERR_RSP",  0x0001DF0000000078)
for i in range(6):
    dump(f"DPI_DMA_ENG{i}_EN",  0x0001DF0000000080 + i*8)
for i in range(6):
    dump(f"DPI_ENG{i}_BUF",     0x0001DF0000000880 + i*8)
for i in range(4):
    dump(f"DPI_SLI_PRT{i}_CFG", 0x0001DF0000000900 + i*8)
for i in range(8):
    dump(f"DPI_DMA_PP{i}_CNT",  0x0001DF0000000B00 + i*8)

out.write("== PKO ==\n")
dump("PKO_REG_FLAGS",       0x0001180050000000)
dump("PKO_REG_READ_IDX",    0x0001180050000008)
dump("PKO_REG_CMD_BUF",     0x0001180050000010)
dump("PKO_REG_GMX_PORT_MODE",0x0001180050000018)
dump("PKO_REG_QUEUE_MODE",  0x0001180050000048)
dump("PKO_REG_BIST_RESULT", 0x0001180050000080)
dump("PKO_REG_ERROR",       0x0001180050000088)
dump("PKO_REG_DEBUG0",      0x0001180050000098)
dump("PKO_REG_ENGINE_INHIBIT",0x0001180050000750)

out.write("== PKO queue config for queues 120..156 (READ_IDX + MEM_QUEUE_PTRS) ==\n")
# write index via window write: use WIN_WR (0x00 addr, 0x20/0x24 data, 0x30 mask)
WIN_WR_DATA_LO = 0x20
WIN_WR_DATA_HI = 0x24
WIN_WR_MASK = 0x30
def win_wr(addr, val):
    w64(WIN_WR_MASK, 0xff)
    w64(WIN_WR_ADDR, addr)
    w32(WIN_WR_DATA_HI, (val >> 32) & 0xffffffff); r32(WIN_WR_DATA_HI)
    w32(WIN_WR_DATA_LO, val & 0xffffffff)

for q in list(range(0, 40)) + list(range(120, 156)):
    win_wr(0x0001180050000008, q)          # PKO_REG_READ_IDX = q (inc=0)
    v = win_rd(0x0001180050001000)         # PKO_MEM_QUEUE_PTRS readback
    if v != 0:
        out.write(f"PKO_QUEUE_PTRS[{q}] = 0x{v:016x}\n")

out.write("== SLI (PEXP window) ==\n")
# SLI_PKT_* also visible at PEXP 0x00011F0000010000 base; dump a few control regs
for name, a in [
    ("SLI_PKT_CTL",        0x00011F0000012010),
    ("SLI_PKT_OUT_ENB",    0x00011F0000012110),
    ("SLI_PKT_INPUT_CONTROL",0x00011F0000012120),
    ("SLI_PKT_SLIST_ES",   0x00011F0000012050),
    ("SLI_TX_PIPE",        0x00011F0000012230),
]:
    dump(name, a)

out.close()
m.close(); os.close(fd)
print(f"dumped -> /tmp/claude-1000/regs-{label}.txt")
