# ampaac — AAC Decoding for FMOD

`ampaac` is an FMOD codec plugin that decodes AAC with the Fraunhofer FDK AAC decoder in this repository.
It builds for macOS, iOS, Android, Windows and Linux and lets FMOD play AAC files and HTTP netstreams, which
FMOD's built-in codecs do not (FMOD's platform AAC codecs on iOS and Android decode only files on disk). It
decodes only: the encoder is never compiled. Run-time behavior is verified on macOS and Linux x86_64 with
FMOD 2.03.14; the iOS, Android and Windows libraries are verified by `make check` only.

This directory is the art.media.platform (AMP) addition to the fork. No file of the FDK AAC Codec is
modified (see [Changes to the FDK AAC Codec](#changes-to-the-fdk-aac-codec)).

## What It Decodes

- **Profiles:** AAC-LC, HE-AAC (SBR) and HE-AACv2 (PS), covered by the fixtures. xHE-AAC (USAC) in MP4
  goes through the same decoder path; no fixture covers it yet.
- **Containers:** ADTS (`.aac`), including ID3v2 prefixes and streams that start mid-frame (ICY captures);
  MP4/M4A with `moov` before or after `mdat`. Fragmented MP4 is rejected.
- **Output:** 16-bit PCM, 1–8 channels in WAV channel order. The channel count is pinned at open, so a
  mid-stream configuration change cannot change FMOD's format.
- **Level:** fdk's loudness normalization is off (`AAC_DRC_REFERENCE_LEVEL` = -1): output keeps the
  encoded level.
- **Gapless:** M4A trims the encoder's priming and padding (edit list or `iTunSMPB`) and compensates
  fdk's output delay, so output is sample-aligned with the encoder's input. ADTS carries no gapless data:
  its priming plays, as it does with Apple's decoder.

## Behavior in FMOD

FMOD 2.03 ends a stream at the length its codec declares at open: it plays on to that length when the
codec's data ends sooner, cuts the tail when the data runs longer, and never asks for the length again. So:

- **Length:** M4A declares its exact length (for a truncated file, the length of the access units it
  holds). ADTS declares an unknown length, so FMOD ends the stream at
  the decoder's end of stream. The running estimate is published as the tag `AMPAAC_LENGTH_MS`
  (`FMOD_TAGTYPE_USER`, 32-bit integer) and becomes exact at the end. With `FMOD_ACCURATETIME`, ADTS
  reads the whole stream at open and declares its exact length.
- **Seeking:** M4A seeks are sample-exact: decoding restarts 8 access units ahead of the target (from the
  sync sample at or before that point when the track marks sync samples) and drops the pre-roll output.
  ADTS seeks walk frame headers forward from the nearest known position, for up to 100 ms per seek; they
  are exact wherever the walk reaches and estimated beyond it.
- **Sample-rate changes** (implicit SBR found after open) arrive as FMOD's `Sample Rate Change` FLOAT tag.
- **Netstreams:** every FMOD netstream seek is a new HTTP request, so forward jumps of up to 256 KiB are
  read through instead of seeked.
- **A body cut short:** when an HTTP body ends before its Content-Length, FMOD neither fails nor
  reconnects, and reads keep succeeding up to that length, so the codec cannot see the cut. Once FMOD's
  receive ring (twice the stream buffer size) has wrapped, the missing bytes are its stale contents,
  cycling; before that, zeros or an early end of file. ADTS frames in stale bytes are valid, so the missing
  span replays audio from one ring earlier, as it does with FMOD's own MP3 codec. M4A access units read
  from stale bytes fail to decode, and fdk's concealment holds the output near silence (under −66 dBFS) to
  the declared end. A server must never cut a body.
- **Errors:** open returns `FMOD_ERR_FORMAT` only for data that is not AAC; file and network errors pass
  through unchanged.
- **Memory:** the codec's state and tables come from FMOD's allocator; fdk's decoder calls `calloc`.
- **Stack:** a decode peaks near 50 KB of stack (`make test` measures it: 49,736 bytes on arm64, 49,800 on
  x86_64; fdk's frame decoder alone takes 35.9 KB). FMOD's default STREAM (96 KiB) and NONBLOCKING
  (112 KiB) thread stacks hold that with under 2× headroom on STREAM; raise both with
  `FMOD_Thread_SetAttributes` before the first System is created (192 KiB leaves 3.9×).

FMOD reports a sound opened by a plugin codec as `FMOD_SOUND_TYPE_UNKNOWN`.

## Registering

Every build exports `AMPAAC_GetCodecDescription`; the shared libraries also export FMOD's standard
`FMODGetCodecDescription`, so `System::loadPlugin` can load them.

```c
FMOD_CODEC_DESCRIPTION* AMPAAC_GetCodecDescription(void);

unsigned int handle = 0;
FMOD_System_RegisterCodec(system, AMPAAC_GetCodecDescription(), &handle, 1000);
```

Registering before or after `System::init` both work. FMOD's manual lists its built-in priorities
(`System::registerCodec`): WAV 600, Ogg Vorbis 800, AIFF 1000, FLAC 1100, AudioQueue 2200, MediaCodec 2250,
MPEG 2400. Priority 1000 places ampaac after WAV (measured: between 500 and 700) and Ogg, level with AIFF
(each rejects the other's data), and before FLAC, MPEG (measured: between 2150 and 2500) and the platform
codecs, which cannot netstream AAC. Formats tried after ampaac pay its probe: an MP3 open over HTTP made 6
requests instead of 4. FMOD for Unity's C# wrapper has no
`registerCodec`: P/Invoke `FMOD5_System_RegisterCodec(IntPtr system, IntPtr description, out uint handle,
uint priority)` from FMOD's library, and `AMPAAC_GetCodecDescription` from `ampaac` (`__Internal` on iOS,
where `libampaac.a` links into the app).

## Building

**Prerequisite:** the FMOD Engine SDK's core headers (`api/core/inc`: `fmod.h`, `fmod_codec.h`, …). They
come with the SDK under the FMOD EULA and are never committed here. The Makefile looks for the 2.03.14
SDK unpacked at `~/Applications/fmodstudioapi20314mac`; `FMOD_API_INC=<dir>` points it elsewhere. Every leg
builds against these headers: the plugin uses only FMOD's codec API.

```sh
make osx                                                 # one leg: osx | ios | android | windows | linux
make all                                                 # every leg
make check                                               # check every built library (below)
make install                                             # copy into UNITY_LIBS/<subdir>/ (temp + rename)
```

A build writes only under `build/`; `make install` copies into the Unity project's libs tree
(`UNITY_LIBS`). `make help` lists the targets and overrides.

| Leg | Output (install subdir) | Toolchain | Floor |
|---|---|---|---|
| macOS | `OSX/libampaac.dylib` (x86_64 + arm64) | Xcode clang | macOS 10.15 (arm64: 11.0) |
| iOS | `iOS/libampaac.a` (arm64, one prelinked object, one global symbol) | Xcode clang | iOS 12.4 |
| Android | `Android/arm64-v8a/libampaac.so` (16 KB page aligned) | NDK r27c | API 27 |
| Windows | `Windows/x86_64/ampaac.dll` (`ampaac.pdb` beside it, not shipped) | llvm-mingw 20260922 (UCRT) | Windows 10 |
| Linux | `Linux/x86_64/libampaac.so` | zig 0.16.0 | glibc 2.28 |

The Android leg takes the NDK from the Unity editor of the AMP Unity project (`UNITY_PROJ`), or from
`ANDROID_NDK=<ndk>`. The FMOD SDK and the cross toolchains are pinned by release and download hash and
unpacked outside the repository; the Makefile looks for them under `~/Applications/<download name>`, or at
`FMOD_API_INC`, `LLVM_MINGW_ROOT` and `ZIG`:

| Toolchain | Download | SHA-256 |
|---|---|---|
| FMOD Engine 2.03.14 (build 164239) | `fmodstudioapi20314mac-installer.dmg` (fmod.com, account required; its `FMOD Programmers API` folder) | `75b7cf57861567b9e1a4a36034b6fbeb92a7c90da082574c6ca93777cfed46bb` |
| llvm-mingw 20260922 (LLVM 23.1.2) | [`llvm-mingw-20260922-ucrt-macos-universal.tar.xz`](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260922) | `52e5f5a7b131021d0c39a37a38fa380a1da7885cd04bd61afd0cd4ecfb8bc1f3` |
| zig 0.16.0 | [`zig-aarch64-macos-0.16.0.tar.xz`](https://ziglang.org/download/0.16.0/zig-aarch64-macos-0.16.0.tar.xz) | `b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489` |

Every leg builds with stack protection, keeps the builder's paths out of the binary, and stamps
`ampaac fdk-aac <revision>` into it (the fork's short commit hash, `-dirty` for uncommitted changes).
`make check` verifies each built library against its row above: architecture, OS floor (the Windows floor
follows from its UCRT imports), dependencies, no C++ runtime (imports, and the PDB of the statically linked
Windows DLL), exports, stack protection, embedded paths, and a revision stamp equal to `HEAD`.

## Testing

- `make test` — host tests on macOS (both slices when Rosetta is present; `SAN=1` adds ASan + UBSan):
  per fixture the rate, channels, tone, length, seeks and trickled reads; gapless alignment against chirp
  fixtures; unknown size, truncation and corruption; peak stack depth.
- `make linux-test` — the host tests on x86_64 Linux, then FMOD's own Linux library playing every fixture
  as an HTTP netstream (`test/serve_range.py`), with seeks and a WAV control, in an amd64 container.
- `make fuzz` (`test/fuzz.sh`, `FUZZ_SECONDS`) — libFuzzer + ASan + UBSan in a Linux container. Inputs are
  capped at the largest seed (about 24 KB), so large-file paths need their own tests.
- `test/fmod_harness.c` — drives a real FMOD library with the codec registered (open, play, seek, length).
- `test/make_fixtures.py` — regenerates `test/fixtures/` (macOS `afconvert`).

## Licensing

The decoder is the Fraunhofer FDK AAC Codec Library for Android, under the license in
[`../NOTICE`](../NOTICE). A product that ships a binary built here takes on its conditions:

- Ship the complete text of `NOTICE` in the product's documentation or other materials.
- Offer the complete source free of charge to everyone who receives the binary: this repository at the
  revision stamped in the binary.
- Do not use Fraunhofer's name to endorse or promote the product, and charge no copyright license fee for
  the library.
- The license grants no patent rights (`NOTICE` §3). Patent licenses for AAC decoding are available through
  Via Licensing Alliance or the patent owners; obtain the ones that apply before distributing a product that
  decodes AAC.

## Changes to the FDK AAC Codec

`NOTICE` §2 requires a modified version to carry prominent notices of its changes and their dates.

- **2026-09-27** — `fmod/` added: the FMOD codec plugin, its build, tests and fixtures. No file of the FDK
  AAC Codec is modified. The plugin compiles the decoder modules (`libAACdec`, `libArithCoding`,
  `libDRCdec`, `libFDK`, `libMpegTPDec`, `libPCMutils`, `libSACdec`, `libSBRdec`, `libSYS`) as they are,
  with the library's own `SUPPRESS_BUILD_DATE_INFO` switch defined.

A change to any file of the FDK AAC Codec gets a dated entry here, and the library's name becomes "Third-Party
Modified Version of the Fraunhofer FDK AAC Codec Library for Android", as §2 requires.
