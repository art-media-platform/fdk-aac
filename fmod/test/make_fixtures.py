#!/usr/bin/env python3
"""Regenerates the ampaac test fixtures: 3 s tones encoded by macOS afconvert (no third-party audio).

Left channel 440 Hz, right 660 Hz (mono: 440 Hz) at -12 dBFS. Run on macOS:
    python3 make_fixtures.py [fixture name ...]    (all fixtures when none are named)
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
    ("m4a_lc_44k_stereo.m4a",    "m4af", "aac ", 128000, 44100, 2),
    ("m4a_he_48k_stereo.m4a",    "m4af", "aach",  64000, 48000, 2),
    ("m4a_hev2_48k_stereo.m4a",  "m4af", "aacp",  32000, 48000, 2),
]

# Alignment fixtures: 1.5 s linear chirps (left 200 Hz up to 4 kHz, right 4 kHz down to 200 Hz, inside
# HE-AAC's waveform-coded band) whose decodes the tests correlate against the analytic signal.
CHIRPS = [
    ("chirp_lc_44k.m4a", "m4af", "aac ", 128000, 44100),
    ("chirp_he_48k.m4a", "m4af", "aach",  64000, 48000),
    ("chirp_lc_44k.aac", "adts", "aac ", 128000, 44100),
]
CHIRP_SECONDS = 1.5

# Channel-layout fixtures: 1 s tones per part (channel c at 300 + 200c Hz), parts concatenated as ADTS.
# fdk pins its output to 1, 2, 6 or 8 channels, so 3 and 5 channels extend to 6; a layout that changes
# mid-stream must keep FMOD's format and still reach the end.
MULTICHANNEL = [
    ("adts_lc_44k_3ch.aac",             [3]),
    ("adts_lc_44k_5ch_then_stereo.aac", [5, 2]),
]
MULTI_SECONDS = 1.0
CHIRP_LOW, CHIRP_HIGH = 200.0, 4000.0

# 2 s of digital silence, LC 44.1 kHz stereo like adts_lc_44k_stereo.aac: its frames are 13 bytes, so a stream that
# opens with it is the VBR case for the ADTS length estimate (a quiet opening).
SILENCE = ("adts_lc_44k_silence.aac", 128000, 44100, 2, 2.0)

# Derived from m4a_lc_44k_stereo.m4a (afconvert writes moov first and gapless info only as iTunSMPB).
MOOV_AT_END = "m4a_lc_44k_moovend.m4a"
ELST_ONLY   = "m4a_lc_44k_elst.m4a"
CONTAINERS  = {"moov", "trak", "mdia", "minf", "stbl", "edts", "udta"}


def parse_boxes(buf):
    boxes, off = [], 0
    while off + 8 <= len(buf):
        size, kind = struct.unpack(">I4s", buf[off:off + 8])
        kind = kind.decode("latin1")
        body = buf[off + 8:off + size]
        boxes.append([kind, parse_boxes(body) if kind in CONTAINERS else body])
        off += size
    return boxes


def serialize(boxes):
    out = bytearray()
    for kind, body in boxes:
        payload = serialize(body) if isinstance(body, list) else body
        out += struct.pack(">I4s", 8 + len(payload), kind.encode("latin1")) + payload
    return bytes(out)


def find(boxes, kind):
    return next(box for box in boxes if box[0] == kind)


def shift_chunk_offsets(moov, delta):
    stbl = find(find(find(find(moov[1], "trak")[1], "mdia")[1], "minf")[1], "stbl")
    stco = find(stbl[1], "stco")
    count = struct.unpack(">I", stco[1][4:8])[0]
    offsets = struct.unpack(f">{count}I", stco[1][8:8 + 4 * count])
    stco[1] = stco[1][:8] + struct.pack(f">{count}I", *(o + delta for o in offsets))


def mdat_payload_offset(buf):
    off = 0
    while off + 8 <= len(buf):
        size, kind = struct.unpack(">I4s", buf[off:off + 8])
        if kind == b"mdat":
            return off + 8
        off += size
    raise ValueError("no mdat")


def relayout(boxes, moov_last, old_mdat):
    """Serializes ftyp, moov, mdat (or ftyp, mdat, moov) with stco rebased onto mdat's new position."""
    ftyp, moov, mdat = find(boxes, "ftyp"), find(boxes, "moov"), find(boxes, "mdat")
    order = [ftyp, mdat, moov] if moov_last else [ftyp, moov, mdat]
    new_mdat = len(serialize(order[:order.index(mdat)])) + 8
    shift_chunk_offsets(moov, new_mdat - old_mdat)
    return serialize(order)


def derive(src_path):
    original = open(src_path, "rb").read()
    old_mdat = mdat_payload_offset(original)
    with open(os.path.join(OUT, MOOV_AT_END), "wb") as out:
        out.write(relayout(parse_boxes(original), True, old_mdat))
    # elst only: drop udta (iTunSMPB); add edts/elst with the same priming and presented length.
    boxes = parse_boxes(original)
    moov = find(boxes, "moov")
    moov[1] = [b for b in moov[1] if b[0] != "udta"]
    trak = find(moov[1], "trak")
    elst = struct.pack(">BBBBI", 0, 0, 0, 0, 1) + struct.pack(">IiHH", 132300, 2112, 1, 0)
    trak[1].insert(1, ["edts", [["elst", elst]]])
    with open(os.path.join(OUT, ELST_ONLY), "wb") as out:
        out.write(relayout(boxes, False, old_mdat))


def chirp(n, rate, rising):
    t = n / rate
    f0, f1 = (CHIRP_LOW, CHIRP_HIGH) if rising else (CHIRP_HIGH, CHIRP_LOW)
    return math.sin(2 * math.pi * (f0 * t + (f1 - f0) / (2 * CHIRP_SECONDS) * t * t))


def write_chirp(path, rate):
    with wave.open(path, "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        data = bytearray()
        for n in range(int(CHIRP_SECONDS * rate)):
            data += struct.pack("<hh", int(AMPLITUDE * 32767 * chirp(n, rate, True)),
                                int(AMPLITUDE * 32767 * chirp(n, rate, False)))
        out.writeframes(bytes(data))


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


def write_multichannel_tone(path, rate, channels):
    with wave.open(path, "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        data = bytearray()
        for n in range(int(MULTI_SECONDS * rate)):
            for c in range(channels):
                data += struct.pack("<h", int(AMPLITUDE * 32767 * math.sin(2 * math.pi * (300 + 200 * c) * n / rate)))
        out.writeframes(bytes(data))


def main():
    wanted = set(sys.argv[1:])
    def build(name):
        return not wanted or name in wanted
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name, parts in MULTICHANNEL:
            if not build(name):
                continue
            data = bytearray()
            for channels in parts:
                wav = os.path.join(tmp, f"multi_{channels}.wav")
                aac = os.path.join(tmp, f"multi_{channels}.aac")
                write_multichannel_tone(wav, 44100, channels)
                subprocess.run(["afconvert", "-f", "adts", "-d", "aac ", "-b", str(48000 * channels), wav, aac], check=True)
                data += open(aac, "rb").read()
            dest = os.path.join(OUT, name)
            open(dest, "wb").write(bytes(data))
            print(f"{name}: {len(data)} bytes")
        for name, container, fmt, bitrate, rate, channels in FIXTURES:
            if not build(name):
                continue
            wav = os.path.join(tmp, f"tone_{rate}_{channels}.wav")
            if not os.path.exists(wav):
                write_tone(wav, rate, channels)
            dest = os.path.join(OUT, name)
            subprocess.run(["afconvert", "-f", container, "-d", fmt, "-b", str(bitrate), wav, dest], check=True)
            print(f"{name}: {os.path.getsize(dest)} bytes")
        name, bitrate, rate, channels, seconds = SILENCE
        if build(name):
            wav = os.path.join(tmp, "silence.wav")
            with wave.open(wav, "wb") as out:
                out.setnchannels(channels)
                out.setsampwidth(2)
                out.setframerate(rate)
                out.writeframes(bytes(int(seconds * rate) * channels * 2))
            dest = os.path.join(OUT, name)
            subprocess.run(["afconvert", "-f", "adts", "-d", "aac ", "-b", str(bitrate), wav, dest], check=True)
            print(f"{name}: {os.path.getsize(dest)} bytes")
        for name, container, fmt, bitrate, rate in CHIRPS:
            if not build(name):
                continue
            wav = os.path.join(tmp, f"chirp_{rate}.wav")
            if not os.path.exists(wav):
                write_chirp(wav, rate)
            dest = os.path.join(OUT, name)
            subprocess.run(["afconvert", "-f", container, "-d", fmt, "-b", str(bitrate), wav, dest], check=True)
            print(f"{name}: {os.path.getsize(dest)} bytes")
    if build(MOOV_AT_END) or build(ELST_ONLY):
        derive(os.path.join(OUT, "m4a_lc_44k_stereo.m4a"))
        for name in (MOOV_AT_END, ELST_ONLY):
            print(f"{name}: {os.path.getsize(os.path.join(OUT, name))} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
