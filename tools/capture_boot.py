#!/usr/bin/env python3
"""Reset the board and capture its boot log from a single open handle.

esptool holds the port exclusively, so resetting with it and then reading with
`cat` loses the first ~300ms -- which is exactly where app_main's diagnostics
(rom mmap address, heap allocations) are printed. Toggling DTR/RTS ourselves on
an already-open handle avoids the race.

Usage: capture_boot.py [seconds] [port] [baud]
"""
import sys
import time

import serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"

baud = int(sys.argv[3]) if len(sys.argv) > 3 else 1500000
ser = serial.Serial(port, baud, timeout=0.2)

# Standard Espressif auto-reset: RTS drives EN (reset), DTR drives GPIO0.
# Hold GPIO0 high so the chip boots the app rather than the ROM loader.
ser.dtr = False
ser.rts = True   # EN low -> held in reset
time.sleep(0.15)
ser.reset_input_buffer()
ser.rts = False  # release EN -> boot

deadline = time.time() + secs
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
ser.close()
