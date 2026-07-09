#!/usr/bin/env python3
# Live serial wiring monitor. Prints status every 0.5s so you can wiggle J1
# wires and see instantly when the card console comes alive.
import serial,time,sys
port='/dev/ttyUSB0'; baud=115200
try:
    s=serial.Serial(port,baud,timeout=0)
except Exception as e:
    print("CANNOT OPEN",port,e); sys.exit(1)
print("monitoring %s @%d -- wiggle J1 wires now (Ctrl-C to stop)"%(port,baud))
while True:
    s.write(b'\r\n'); s.flush()
    buf=bytearray(); t0=time.time()
    while time.time()-t0<0.5:
        d=s.read(4096)
        if d: buf+=d
        time.sleep(0.01)
    printable=sum(1 for c in buf if 32<=c<127 or c in (10,13))
    nul=buf.count(0)
    if printable>3:
        txt=bytes(c for c in buf if 32<=c<127 or c in(10,13)).decode('ascii','replace')
        print("GOOD  text! printable=%d  <<%s>>"%(printable,txt[-80:].replace('\n','|')))
    elif len(buf)==0:
        print("SILENT (line idle/high) -- no data, RX maybe disconnected")
    elif nul>len(buf)*0.8:
        print("NUL flood (line held LOW) -- TX wire bad/GND issue  bytes=%d"%len(buf))
    else:
        print("GARBAGE (baud/noise) bytes=%d printable=%d"%(len(buf),printable))
