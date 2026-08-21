#!/usr/bin/env python3
"""Drive the emulator over UART: inject keypresses, pull framebuffers as PNGs.

The board has no panel and no buttons yet, so this stands in for both. Keys are
OR'd into osReadKey() on the device, and a screenshot is the raw RGB565
framebuffer streamed back between <FB n> / </FB> markers.

  play.py shots.d --script "wait 120, START, wait 60, shot intro"

Script steps are comma-separated:
  wait N        advance ~N frames (no input)
  KEY[:N]       hold KEY for N frames (default 6), then release
  shot NAME     capture a screenshot to NAME.png
"""
import os
import sys
import time

import serial
from PIL import Image

W, H = 240, 160
FB_LEN = W * H * 2

MAGIC = 0xA5
CMD_KEYS = 0x01
CMD_SHOT = 0x02

# Bit order comes from osKeyMap[] in os.c.
KEYS = {
    "A": 0, "B": 1, "SELECT": 2, "START": 3,
    "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7, "R": 8, "L": 9,
}

# Measured throughput while actually rendering. (The 35fps seen before the
# per-game overrides landed was the cost of drawing a forced-blank screen.)
FRAME_S = 1.0 / 16.0


def set_keys(ser, mask):
    ser.write(bytes([MAGIC, CMD_KEYS, mask & 0xFF, (mask >> 8) & 0xFF]))
    ser.flush()


def screenshot(ser, path):
    """Request one frame and decode it.

    Everything accumulates into a single buffer. Reading the header and the
    payload with separate helpers loses bytes, because a chunked read routinely
    pulls part of the payload in while still looking for the header.
    """
    ser.reset_input_buffer()
    ser.write(bytes([MAGIC, CMD_SHOT]))
    ser.flush()

    deadline = time.time() + 25
    buf = bytearray()
    start = None  # index of first payload byte, once the header is parsed
    length = None

    while time.time() < deadline:
        chunk = ser.read(8192)
        if chunk:
            buf += chunk

        if length is None:
            marker = buf.find(b"<FB ")
            if marker >= 0:
                # The console VFS translates \n to \r\n, so the header arrives as
                # "<FB 76800>\r\n". Find '>' and step over whatever EOL follows
                # rather than matching a fixed terminator.
                end = buf.find(b">", marker)
                if end >= 0:
                    payload_start = end + 1
                    while payload_start < len(buf) and buf[payload_start] in (13, 10):
                        payload_start += 1
                    # Only commit once the EOL is fully present.
                    if payload_start > end + 1:
                        length = int(buf[marker + 4 : end])
                        if length != FB_LEN:
                            raise RuntimeError(
                                f"device reported {length} bytes, expected {FB_LEN}"
                            )
                        start = payload_start
            elif len(buf) > (1 << 20):
                raise RuntimeError("no <FB> marker in 1MB of output")

        if length is not None and len(buf) - start >= length:
            break
    else:
        have = 0 if start is None else len(buf) - start
        raise TimeoutError(f"got {have}/{FB_LEN} bytes of framebuffer")

    payload = buf[start : start + length]

    # FB is byteswapped for the SPI panel, so it is big-endian RGB565.
    img = Image.frombytes("RGB", (W, H), rgb565_be_to_rgb888(payload[:length]))
    img.save(path)
    return img


def rgb565_be_to_rgb888(buf):
    out = bytearray(W * H * 3)
    for i in range(W * H):
        v = (buf[2 * i] << 8) | buf[2 * i + 1]
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        # Replicate high bits into the low ones so white stays white.
        out[3 * i + 0] = (r << 3) | (r >> 2)
        out[3 * i + 1] = (g << 2) | (g >> 4)
        out[3 * i + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def run_script(ser, outdir, script):
    shots = []
    for raw in script.split(","):
        step = raw.strip()
        if not step:
            continue
        head, _, arg = step.partition(" ")
        head = head.upper()

        if head == "WAIT":
            time.sleep(int(arg) * FRAME_S)
        elif head == "SHOT":
            name = (arg.strip() or f"shot{len(shots)}") + ".png"
            path = os.path.join(outdir, name)
            screenshot(ser, path)
            shots.append(path)
            print(f"  captured {path}")
        else:
            key, _, hold = head.partition(":")
            frames = int(hold) if hold else 6
            if key not in KEYS:
                raise SystemExit(f"unknown key {key!r}")
            set_keys(ser, 1 << KEYS[key])
            time.sleep(frames * FRAME_S)
            set_keys(ser, 0)
            time.sleep(2 * FRAME_S)
            print(f"  pressed {key} for {frames} frames")
    return shots


def main():
    outdir = sys.argv[1]
    script = sys.argv[2] if len(sys.argv) > 2 else "wait 240, shot boot"
    port = sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyACM0"
    os.makedirs(outdir, exist_ok=True)

    ser = serial.Serial(port, 1500000, timeout=0.3)
    set_keys(ser, 0)
    time.sleep(0.2)
    ser.reset_input_buffer()

    shots = run_script(ser, outdir, script)
    ser.close()
    print(f"{len(shots)} screenshot(s) -> {outdir}")


if __name__ == "__main__":
    main()
