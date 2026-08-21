#!/usr/bin/env python3
"""Reset the board and report emulated frames/sec over a fixed window.

From reset with no input the Ruby attract mode is deterministic, so the same
wall-clock window covers the same workload on every build. That makes results
comparable across optimisation attempts -- which a free-running average is not,
because scene complexity varies wildly (a fade-to-black is far cheaper than the
scrolling forest).

  bench.py [label] [warmup_s] [window_s]
"""
import re
import statistics
import sys
import time

import serial

label = sys.argv[1] if len(sys.argv) > 1 else "run"
fskip = int(sys.argv[5]) if len(sys.argv) > 5 else -1
warmup = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
window = float(sys.argv[3]) if len(sys.argv) > 3 else 25.0
port = sys.argv[4] if len(sys.argv) > 4 else "/dev/ttyACM0"

ser = serial.Serial(port, 1500000, timeout=0.2)
ser.dtr = False
ser.rts = True
time.sleep(0.2)
ser.reset_input_buffer()
ser.rts = False

if fskip >= 0:
    # Let the board finish booting, then set frameskip over the control
    # channel so the sweep needs no reflash between points.
    time.sleep(3.0)
    ser.write(bytes([0xA5, 0x04, fskip]))
    ser.flush()
    label = f"{label} fskip={fskip}"

t0 = time.time()
text = ""
samples = []
while time.time() - t0 < warmup + window:
    chunk = ser.read(ser.in_waiting or 1)
    if not chunk:
        continue
    text += chunk.decode("utf-8", "replace")
    while "\n" in text:
        line, _, text = text.partition("\n")
        m = re.search(r"BENCH t=(\d+)s emu=([\d.]+) draw=(\d+) DISPCNT=(\w+)", line)
        if m and time.time() - t0 >= warmup:
            samples.append((float(m.group(2)), int(m.group(3)), m.group(4)))
ser.close()

if not samples:
    raise SystemExit("no BENCH lines captured")

emu = [s[0] for s in samples]
draw = [s[1] for s in samples]
print(f"[{label}]  n={len(samples)}")
print(f"  emulated fps : mean {statistics.mean(emu):5.1f}  "
      f"median {statistics.median(emu):5.1f}  min {min(emu)}  max {max(emu)}")
print(f"  drawn fps    : mean {statistics.mean(draw):5.1f}")
print(f"  % of full speed (59.72): {statistics.mean(emu) / 59.7275 * 100:.1f}%")
