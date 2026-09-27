#!/usr/bin/env python3
"""Regenerates the ampaac test fixtures: 3 s tones encoded by macOS afconvert (no third-party audio).

Left channel 440 Hz, right 660 Hz (mono: 440 Hz) at -12 dBFS. Run on macOS:
    python3 make_fixtures.py
"""
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "fixtures")
SECONDS = 3.0
AMPLITUDE = 0.25

# name, container, data format, bitrate, sample rate, channels
FIXTURES = [
    ("adts_lc_44k_stereo.aac",   "adts", "aac ", 128000, 44100, 2),
    ("adts_lc_22k_mono.aac",     "adts", "aac ",  48000, 22050, 1),
    ("adts_he_48k_stereo.aac",   "adts", "aach",  64000, 48000, 2),
    ("adts_hev2_48k_stereo.aac", "adts", "aacp",  32000, 48000, 2),
]


def write_tone(path, rate, channels):
    frames = int(SECONDS * rate)
    with wave.open(path, "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        data = bytearray()
        for n in range(frames):
            left = int(AMPLITUDE * 32767 * math.sin(2 * math.pi * 440 * n / rate))
            data += struct.pack("<h", left)
            if channels == 2:
                right = int(AMPLITUDE * 32767 * math.sin(2 * math.pi * 660 * n / rate))
                data += struct.pack("<h", right)
        out.writeframes(bytes(data))


def main():
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name, container, fmt, bitrate, rate, channels in FIXTURES:
            wav = os.path.join(tmp, f"tone_{rate}_{channels}.wav")
            if not os.path.exists(wav):
                write_tone(wav, rate, channels)
            dest = os.path.join(OUT, name)
            subprocess.run(["afconvert", "-f", container, "-d", fmt, "-b", str(bitrate), wav, dest], check=True)
            print(f"{name}: {os.path.getsize(dest)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
