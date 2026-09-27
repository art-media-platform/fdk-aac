#!/usr/bin/env python3
"""
linux_check — the Linux leg's runtime check; test/linux.sh runs it inside an amd64 container.

    linux_check.py <build-dir> <libfmodstudio.so> <libampaac.so> <fixtures-dir>

1. ampaac_test (the host tests) on x86_64.
2. fmod_harness with FMOD's own Linux library and ampaac registered at the client's priority: every fixture
   opened as an HTTP netstream from serve_range.py and played to the end, one mid-stream seek per container,
   and a WAV control that FMOD's built-in codec must still open.
One line per check; exits 1 if any check fails.
"""
import hashlib
import math
import os
import platform
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import wave
from pathlib import Path

PRIORITY = "1000"          # the client's registration priority (after WAV and Ogg, before FLAC and MPEG)
PORT = 8791
TYPE_PLUGIN = 0            # FMOD reports FMOD_SOUND_TYPE_UNKNOWN for a sound a plugin codec opened
TYPE_WAV = 15
UNKNOWN_LENGTH = 0xFFFFFFFF
TONE_MS = 3000             # make_fixtures.py SECONDS
CHIRP_MS = 1500            # make_fixtures.py CHIRP_SECONDS
# make_fixtures.py MULTICHANNEL: 1 s parts; fdk's mixer pins 3 and 5 channels to 6 (3/0/2.1).
LAYOUTS = {"adts_lc_44k_3ch.aac": (1000, 6), "adts_lc_44k_5ch_then_stereo.aac": (2000, 6)}
CONTROL_MS = 2000
ADTS_TAIL_MS = 200         # ADTS carries no gapless data: encoder priming + last-frame padding play
END_SLACK_MS = 150         # last polled position vs. the end (5 ms polls, 1024-sample mixer blocks)
SEEK_TO_MS = 2000          # fmod_harness seeks 1 s into playback


class Report:
    def __init__(self):
        self.checks = 0
        self.failed = 0

    def check(self, subject, name, ok, detail):
        self.checks += 1
        self.failed += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'}  {subject:<28} {name:<14} {detail}", flush=True)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()[:16]


def write_control(path):
    """A 1 kHz stereo 16-bit WAV, CONTROL_MS long."""
    rate = 48000
    frames = bytearray()
    for n in range(rate * CONTROL_MS // 1000):
        sample = int(12000 * math.sin(2 * math.pi * 1000 * n / rate))
        frames += struct.pack("<hh", sample, sample)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))


def harness(args, scenario, name, seek_ms=None):
    cmd = [str(args.build / "fmod_harness"), args.fmod, args.codec, scenario, f"http://127.0.0.1:{PORT}/{name}"]
    if seek_ms is not None:
        cmd.append(str(seek_ms))
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                          env=dict(os.environ, AMPAAC_PRIORITY=PRIORITY))
    fields, errors = {}, []
    for line in done.stdout.splitlines():
        if line.startswith("error="):
            errors.append(line)
            continue
        for pair in line.split():
            key, _, value = pair.partition("=")
            fields.setdefault(key, value)   # the first: the play loop repeats openstate and result
    if done.returncode != 0:
        errors.append(f"exit {done.returncode}: {done.stderr.strip()}")
    return fields, errors


def number(fields, key, cast=int):
    try:
        return cast(fields[key])
    except (KeyError, ValueError):
        return None


def opened(report, subject, fields, errors, want_type, want_channels, ampaac=True):
    report.check(subject, "harness", not errors, "; ".join(errors) or "no FMOD errors")
    report.check(subject, "registered", fields.get("register") == "0", f"register={fields.get('register')} "
                 f"priority={fields.get('priority')}")
    ready = fields.get("openstate") == "0"
    report.check(subject, "open", ready, f"openstate={fields.get('openstate')} in {fields.get('open_s')} s")
    ok = number(fields, "type") == want_type and number(fields, "channels") == want_channels
    detail = f"type={fields.get('type')} channels={fields.get('channels')} bits={fields.get('bits')}"
    if ampaac:
        # Only ampaac sets this tag: the plugin that opened the stream is ours.
        tag = number(fields, "tag_open_ms")
        ok = ok and bool(tag)
        detail += f"; AMPAAC_LENGTH_MS at open {tag}"
    report.check(subject, "codec", ok, detail)
    return ready


def expected(name):
    """Content length (ms) and FMOD channel count of a fixture."""
    if name in LAYOUTS:
        return LAYOUTS[name]
    return (CHIRP_MS if name.startswith("chirp_") else TONE_MS), (1 if "mono" in name else 2)


def check_play(report, args, name):
    want_ms, channels = expected(name)
    fields, errors = harness(args, "play", name)
    if not opened(report, name, fields, errors, TYPE_PLUGIN, channels):
        return
    declared = number(fields, "length_open_ms")
    tag_end = number(fields, "tag_end_ms")
    if name.endswith(".m4a"):
        report.check(name, "length", declared == want_ms, f"declared {declared} ms (exact {want_ms})")
        end_ms = want_ms
    else:
        # These short files end inside the open's 128 KiB walk: their exact length is declared at open.
        ok = tag_end is not None and declared == tag_end and want_ms <= tag_end <= want_ms + ADTS_TAIL_MS
        report.check(name, "length", ok, f"declared {'unknown' if declared == UNKNOWN_LENGTH else declared}; "
                     f"AMPAAC_LENGTH_MS at end {tag_end} (content {want_ms} + priming/padding)")
        end_ms = tag_end or want_ms
    last = number(fields, "last_position_ms")
    stopped = number(fields, "stopped_after_s", float)
    ok = last is not None and end_ms - END_SLACK_MS <= last <= end_ms and stopped is not None and \
        want_ms / 1000 - 0.2 <= stopped <= end_ms / 1000 + 1.5
    report.check(name, "played", ok, f"last position {last} ms; channel stopped after {stopped} s")


def check_seek(report, args, name):
    fields, errors = harness(args, "seek", name, SEEK_TO_MS)
    subject = f"{name} (seek)"
    if not opened(report, subject, fields, errors, TYPE_PLUGIN, 2):
        return
    last = number(fields, "last_position_ms")
    stopped = number(fields, "stopped_after_s", float)
    # Unseeked, the channel would stop after ~3 s; seeking 1 s in to 2 s leaves ~1 s to play. Where the
    # codec lands is the host tests' job (bit-exact against a continuous decode); this checks FMOD's side.
    remaining = 1.0 + (TONE_MS - SEEK_TO_MS) / 1000
    ok = last is not None and last >= TONE_MS - END_SLACK_MS and stopped is not None and \
        remaining - 0.3 <= stopped <= remaining + ADTS_TAIL_MS / 1000 + 0.3
    report.check(subject, "seek", ok, f"seek {fields.get('seek_to_ms')} ms at {fields.get('seek_at_s')} s; "
                 f"stopped after {stopped} s at {last} ms")


def check_control(report, args, name):
    fields, errors = harness(args, "play", name)
    if not opened(report, name, fields, errors, TYPE_WAV, 2, ampaac=False):
        return
    last = number(fields, "last_position_ms")
    report.check(name, "played", last is not None and last >= CONTROL_MS - END_SLACK_MS,
                 f"FMOD's WAV codec (ampaac registered); last position {last} ms")


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    args = type("Args", (), {})()
    args.build, args.fmod, args.codec, fixtures = Path(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4])
    report = Report()

    print(f"linux_check: {platform.machine()} {os.confstr('CS_GNU_LIBC_VERSION')}; "
          f"libampaac.so {sha256(args.codec)}; libfmodstudio.so {sha256(args.fmod)} (sha256/64)", flush=True)

    done = subprocess.run([str(args.build / "ampaac_test"), str(fixtures)], capture_output=True, text=True, timeout=300)
    summary = (done.stdout.strip().splitlines() or ["no output"])[-1]
    report.check("ampaac_test", "host tests", done.returncode == 0 and summary.endswith(" 0 failures"), summary)
    if done.returncode != 0:
        print(done.stdout + done.stderr)

    media = Path("/tmp/media")
    shutil.rmtree(media, ignore_errors=True)
    shutil.copytree(fixtures, media)
    write_control(media / "control_1k_48k.wav")
    server = subprocess.Popen([sys.executable, str(Path(__file__).with_name("serve_range.py")), str(media), str(PORT),
                               "/tmp/requests.jsonl"])
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{PORT}/control_1k_48k.wav", timeout=1).read(1)
                break
            except OSError:
                time.sleep(0.1)
        for name in sorted(p.name for p in media.iterdir() if p.suffix in (".aac", ".m4a")):
            check_play(report, args, name)
        check_seek(report, args, "adts_lc_44k_stereo.aac")
        check_seek(report, args, "m4a_he_48k_stereo.m4a")
        check_control(report, args, "control_1k_48k.wav")
    finally:
        server.terminate()
        server.wait()

    print(f"linux_check: {report.checks} checks, {report.failed} failed")
    sys.exit(1 if report.failed else 0)


if __name__ == "__main__":
    main()
