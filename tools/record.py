#!/usr/bin/env python3
"""Record real video + audio off the device and mux it with ffmpeg.

The emulator advances exactly one frame per request and idles in between, so
capture runs far slower than real time but the result has no gaps and the audio
lines up with the video frame-for-frame.

Audio is returned every step; video only every --video-every steps, since a
frame is 76800 bytes and the UART is the bottleneck.

  record.py out.mp4 --frames 600 --video-every 2 --script "START:8,wait 40,A:8"
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

import serial

W, H = 240, 160
FB_LEN = W * H * 2
GBA_FPS = 59.7275  # true GBA refresh

MAGIC = 0xA5
CMD_KEYS = 0x01
CMD_STEP = 0x03

KEYS = {
    "A": 0, "B": 1, "SELECT": 2, "START": 3,
    "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7, "R": 8, "L": 9,
}


class Device:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.5)
        self.buf = bytearray()
        self.keys = 0

    def set_keys(self, mask):
        self.keys = mask
        self.ser.write(bytes([MAGIC, CMD_KEYS, mask & 0xFF, (mask >> 8) & 0xFF]))
        self.ser.flush()

    def _block(self, tag, deadline):
        """Read one <TAG n> ... payload block, tolerating interleaved log text."""
        open_tag = f"<{tag} ".encode()
        while True:
            i = self.buf.find(open_tag)
            if i >= 0:
                j = self.buf.find(b">", i)
                if j >= 0:
                    n = int(self.buf[i + len(open_tag) : j])
                    k = j + 1
                    while k < len(self.buf) and self.buf[k] in (13, 10):
                        k += 1
                    if k > j + 1 and len(self.buf) - k >= n:
                        payload = bytes(self.buf[k : k + n])
                        del self.buf[: k + n]
                        return payload
            if time.time() > deadline:
                raise TimeoutError(f"timed out reading <{tag}>")
            # read(n) blocks for the full timeout whenever fewer than n bytes are
            # pending, which throttled capture to ~1.3fps regardless of baud.
            # Ask for what is actually buffered, falling back to a 1-byte wait.
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                self.buf += chunk

    def step(self, with_video):
        self.ser.write(bytes([MAGIC, CMD_STEP, 1 if with_video else 0]))
        self.ser.flush()
        deadline = time.time() + 30
        video = self._block("FB", deadline) if with_video else None
        audio = self._block("AU", deadline)
        if video is not None and len(video) != FB_LEN:
            raise RuntimeError(f"short frame: {len(video)}")
        return video, audio


def parse_script(script, total_frames):
    """Expand a script into a per-frame list of key bitmasks."""
    timeline = []
    for raw in script.split(","):
        step = raw.strip()
        if not step:
            continue
        head, _, arg = step.partition(" ")
        head = head.upper()
        if head == "WAIT":
            timeline += [0] * int(arg)
        else:
            key, _, hold = head.partition(":")
            frames = int(hold) if hold else 6
            if key not in KEYS:
                raise SystemExit(f"unknown key {key!r}")
            timeline += [1 << KEYS[key]] * frames
            timeline += [0] * 2
    timeline += [0] * max(0, total_frames - len(timeline))
    return timeline[:total_frames]


def rgb565_be_to_rgb888(buf):
    out = bytearray(W * H * 3)
    for i in range(W * H):
        v = (buf[2 * i] << 8) | buf[2 * i + 1]
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        out[3 * i + 0] = (r << 3) | (r >> 2)
        out[3 * i + 1] = (g << 2) | (g >> 4)
        out[3 * i + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--video-every", type=int, default=2)
    ap.add_argument("--script", default="")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=1500000)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--slowdown", type=float, default=1.0,
                    help="1.0 = full speed; 4.0 mimics the board's real 15fps")
    args = ap.parse_args()

    dev = Device(args.port, args.baud)
    dev.set_keys(0)
    time.sleep(0.2)
    dev.ser.reset_input_buffer()

    timeline = parse_script(args.script, args.frames)

    tmp = tempfile.mkdtemp(prefix="esp-gba-rec-")
    vpath, apath = os.path.join(tmp, "v.raw"), os.path.join(tmp, "a.raw")
    nvid = 0
    nsamp = 0
    t0 = time.time()

    with open(vpath, "wb") as vf, open(apath, "wb") as af:
        for i in range(args.frames):
            if timeline[i] != dev.keys:
                dev.set_keys(timeline[i])
            want_video = (i % args.video_every) == 0
            video, audio = dev.step(want_video)
            if video:
                # Written as raw rgb565be; ffmpeg converts natively, which is far
                # faster than doing the per-pixel unpack in Python (that was the
                # capture bottleneck, not the UART).
                vf.write(video)
                nvid += 1
            af.write(audio)
            nsamp += len(audio) // 4  # stereo s16
            if i % 25 == 0:
                el = time.time() - t0
                rate = (i + 1) / el if el else 0
                eta = (args.frames - i) / rate if rate else 0
                print(f"  frame {i}/{args.frames}  {rate:.1f} fps cap  ETA {eta:.0f}s",
                      flush=True)

    # Derive the true audio rate from what the device actually produced, rather
    # than trusting soundSetSampleRate(). emulated_seconds = frames / GBA_FPS.
    emulated_s = args.frames / GBA_FPS
    rate = round(nsamp / emulated_s)
    vfps = GBA_FPS / args.video_every
    print(f"captured {nvid} video frames, {nsamp} stereo samples "
          f"({emulated_s:.2f}s emulated) -> audio {rate} Hz, video {vfps:.2f} fps")

    out_fps = vfps / args.slowdown
    atempo = 1.0 / args.slowdown
    vf_chain = f"scale=iw*{args.scale}:ih*{args.scale}:flags=neighbor"

    cmd = [
        "ffmpeg", "-y",
        "-f", "rawvideo", "-pixel_format", "rgb565be",
        "-video_size", f"{W}x{H}", "-framerate", f"{out_fps}", "-i", vpath,
        "-f", "s16le", "-ar", str(rate), "-ac", "2", "-i", apath,
        "-filter:v", vf_chain,
    ]
    if abs(atempo - 1.0) > 1e-6:
        cmd += ["-filter:a", f"atempo={atempo}"]
    cmd += [
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
        "-c:a", "aac", "-b:a", "128k", "-shortest", args.output,
    ]
    print("  " + " ".join(cmd))
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    dev.ser.close()
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
