#!/usr/bin/env python3

import serial
import sys

ser = serial.Serial(
    "/dev/ttyUSB0",
    115200,
    timeout=0,
)

with open("/tmp/serial.log", "wb", buffering=0) as log:
    while True:
        b = ser.read(1)      # legge appena arriva un byte
        if b:
            sys.stdout.buffer.write(b)
            sys.stdout.buffer.flush()   # immediato
            log.write(b)