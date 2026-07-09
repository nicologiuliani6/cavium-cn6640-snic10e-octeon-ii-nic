#!/usr/bin/env python3
# SAFE card CSR dump: reads ONLY specific documented offsets (never scans a range, so
# it can't hit a reserved BAR0 offset -> MAbort -> host freeze). SLI PKT regs via BAR0
# direct (known-valid offsets); DPI regs via the BAR0 window (win_rd touches only the
# valid window regs 0x10/0x14/0x40). Run with vendor firmware booted vs OpenWrt; diff.
import mmap, os, struct, sys

BAR0 = "/sys/bus/pci/devices/0000:02:00.0/resource0"
fd = os.open(BAR0, os.O_RDWR)
m = mmap.mmap(fd, 0x4000, mmap.MAP_SHARED)
def r32(o): return struct.unpack('<I', m[o:o+4])[0]
def w32(o, v): m[o:o+4] = struct.pack('<I', v & 0xffffffff)
def r64(o): return struct.unpack('<Q', m[o:o+8])[0]
def win_rd(addr):
    w32(0x14, (addr >> 32) | 0x00060000); r32(0x14)
    w32(0x10, addr & 0xffffffff);          r32(0x10)
    return r64(0x40)

tag = sys.argv[1] if len(sys.argv) > 1 else "dump"
out = open(f"/tmp/claude-1000/regs-{tag}.txt", "w")
def p(s): out.write(s + "\n"); print(s)
p(f"===== {tag} =====")

# SLI global packet regs (BAR0 direct, documented offsets only)
sli_glob = {
 0x1000:"PKT_INSTR_ENB",0x1010:"PKT_OUT_ENB",0x1020:"PKT_INSTR_SIZE",0x1030:"SLIST_ROR",
 0x1040:"SLIST_NS",0x1050:"SLIST_ES64",0x1070:"PKT_IPTR",0x1080:"PKT_DPADDR",
 0x1090:"DATA_OUT_ROR",0x10A0:"DATA_OUT_NS",0x10B0:"DATA_OUT_ES64",0x10D0:"PKT_OUT_BMODE",
 0x10E0:"PKT_PCIE_PORT64",0x1170:"PKT_INPUT_CONTROL",0x1180:"OQ_WMARK",0x11A0:"INSTR_RD_SIZE",
 0x11B0:"IN_PCIE_PORT",0x11F0:"PORT_IN_RST_OQ",0x11F4:"PORT_IN_RST_IQ",0x1220:"PKT_CTL",
 0x3D80:"S2M_PORT0_CTL",
}
p("--- SLI global (BAR0 direct) ---")
for o in sorted(sli_glob): p(f"  SLI_{sli_glob[o]:<18}(+{o:#06x}) = {r64(o):#018x}")

p("--- SLI per-queue OQ0..3 / IQ0..3 (BAR0 direct) ---")
for q in range(4):
    p(f"  OQ{q}: BUFF={r64(0x0C00+q*0x10):#010x} BASE={r64(0x1400+q*0x10):#014x} "
      f"CRED={r64(0x1800+q*0x10):#08x} SIZE={r64(0x1C00+q*0x10):#08x} SENT={r64(0x2400+q*0x10):#08x}")
for q in range(4):
    p(f"  IQ{q}: CNT={r64(0x2000+q*0x10):#08x} BASE={r64(0x2800+q*0x10):#014x} "
      f"DBELL={r64(0x2C00+q*0x10):#08x} SIZE={r64(0x3000+q*0x10):#08x} HDR={r64(0x3400+q*0x10):#018x}")

# DPI regs (via window)
p("--- DPI (via window, documented regs only) ---")
dpi = {0x40:"CTL",0x48:"DMA_CONTROL",0x50:"REQ_GBL_EN",0x58:"REQ_ERR_RSP",0x78:"PKT_ERR_RSP",
       0x800:"NCBX_CFG",0x830:"PINT_INFO",0x838:"REQ_ERR_SKIP",0x980:"INFO_REG",0x08:"INT_REG"}
for off in sorted(dpi): p(f"  DPI_{dpi[off]:<14}(+{off:#05x}) = {win_rd(0x0001DF0000000000|off):#018x}")
for e in range(8):
    p(f"  DPI_ENG{e}: EN={win_rd(0x0001DF0000000080|e*8):#06x} BUF={win_rd(0x0001DF0000000880|e*8):#010x} "
      f"COUNTS={win_rd(0x0001DF0000000300|e*8):#010x} IBUFF={win_rd(0x0001DF0000000280|e*8):#014x}")
for pt in range(4):
    p(f"  DPI_SLI_PRTX_CFG[{pt}] = {win_rd(0x0001DF0000000900|pt*8):#010x}")
out.close(); m.close(); os.close(fd)
print(f"saved /tmp/claude-1000/regs-{tag}.txt")
