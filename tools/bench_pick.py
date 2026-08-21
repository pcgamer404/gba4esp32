#!/usr/bin/env python3
"""Reset, pick a ROM by index over serial, then report emulated/drawn fps.

bench.py assumes the game is already running from reset. Since the firmware
boots into the picker, a benchmark first has to choose something, and the
picker is touch/BOOT only. OS_CMD_PICK exists for exactly this.

Packing a fresh cart into flash takes minutes, so the wait for the game to
start is generous and driven by the BENCH lines actually appearing rather than
by a fixed sleep.

  bench_pick.py LABEL ROM_INDEX [warmup_s] [window_s] [port]
"""
import re
import statistics
import sys
import time

import serial

label = sys.argv[1] if len(sys.argv) > 1 else "run"
pick = int(sys.argv[2]) if len(sys.argv) > 2 else 0
warmup = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0
window = float(sys.argv[4]) if len(sys.argv) > 4 else 25.0
port = sys.argv[5] if len(sys.argv) > 5 else "/dev/ttyACM0"

BOOT_TIMEOUT = 420.0  # a 16MB cart can take minutes to pack and flash

ser = serial.Serial(port, 1500000, timeout=0.2)
ser.dtr = False
ser.rts = True
time.sleep(0.2)
ser.reset_input_buffer()
ser.rts = False

text = ""
samples = []
first_bench = None
picked = False
t0 = time.time()
last_pick_send = 0.0

while True:
    now = time.time()
    if first_bench is None and now - t0 > BOOT_TIMEOUT:
        raise SystemExit("game never started (no BENCH lines)")
    if first_bench is not None and now - first_bench > warmup + window:
        break

    # The menu only polls serial while it is up, and boot chatter means there is
    # almost always a pending chunk -- so resend on a timer, not just when idle.
    if not picked and now - t0 > 6.0 and now - last_pick_send > 0.5:
        ser.write(bytes([0xA5, 0x05, pick]))
        ser.flush()
        last_pick_send = now

    try:
        chunk = ser.read(ser.in_waiting or 1)
    except serial.SerialException as e:
        raise SystemExit(f"serial dropped: {e}")
    if not chunk:
        continue

    decoded = chunk.decode("utf-8", "replace")
    sys.stdout.write(decoded)
    sys.stdout.flush()
    text += decoded

    while "\n" in text:
        line, _, text = text.partition("\n")
        if "serial pick" in line:
            picked = True
        m = re.search(r"BENCH t=(\d+)s emu=([\d.]+) draw=(\d+) DISPCNT=(\w+)", line)
        if not m:
            continue
        if first_bench is None:
            first_bench = time.time()
            continue
        if time.time() - first_bench >= warmup:
            samples.append((float(m.group(2)), int(m.group(3))))

ser.close()

if not samples:
    raise SystemExit("no BENCH lines captured after warmup")

emu = [s[0] for s in samples]
draw = [s[1] for s in samples]
print()
print(f"[{label}]  n={len(samples)}")
print(f"  emulated fps : mean {statistics.mean(emu):5.1f}  "
      f"median {statistics.median(emu):5.1f}  min {min(emu)}  max {max(emu)}")
print(f"  drawn fps    : mean {statistics.mean(draw):5.1f}  "
      f"median {statistics.median(draw):5.1f}")
print(f"  % of full speed (59.72): {statistics.mean(emu) / 59.7275 * 100:.1f}%")
