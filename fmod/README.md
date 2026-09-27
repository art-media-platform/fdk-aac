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
  MP4/M4A with `moov` before or after `mdat`, within the first 64 top-level boxes. Fragmented MP4 is
  rejected.
- **Output:** 16-bit PCM in WAV channel order: 1, 2 or 6 channels. A 3–5 channel stream opens as 5.1 with
  the missing channels silent; fdk's mixer downmixes 7 and 8 channels to 5.1 (its default output limit is 6).
  The count is pinned at open, so a mid-stream layout change cannot change FMOD's format.
- **Level:** fdk's loudness normalization is off (`AAC_DRC_REFERENCE_LEVEL` = -1): output keeps the
  encoded level.
- **Gapless:** M4A trims the encoder's priming and padding (edit list or `iTunSMPB`) and compensates
  fdk's output delay, so output is sample-aligned with the encoder's input. ADTS carries no gapless data:
  its priming plays, as it does with Apple's decoder.

## Behavior in FMOD

FMOD 2.03 ends a stream at the length its codec declares at open: it plays on to that length when the
codec's data ends sooner, cuts the tail when the data runs longer, and never asks for the length again. So:

- **Length:** M4A declares its exact length (for a truncated file, the length of the access units it
  holds). An ADTS open walks the frame headers of the stream's first 128 KiB (the bytes FMOD buffers before
  READY at a 128 KiB stream buffer). Its 100 ms budget is checked every 16 frames, between reads that can
  each wait up to FMOD's network timeout on a still-arriving stream, and a transport fault in the walk ends
  the open. A stream that ends inside them declares its exact length, unless its sampling rate or profile
  changes on the way (the walk stops there); a longer one declares an unknown length, so FMOD ends it at the decoder's end of stream. Its
  running estimate is published as the tag `AMPAAC_LENGTH_MS` (`FMOD_TAGTYPE_USER`, 32-bit integer), from
  the mean frame size of the frames known so far, and becomes exact at the end: a 90 s VBR speech file with
  a quiet opening estimates 97 s at open (the probe's 8 KB alone gave 126 s). A stream without a size gets
  no walk and no estimate. With `FMOD_ACCURATETIME`, ADTS reads a sized stream whole at open and declares its
  exact length; a stream without a size is not walked.
- **Seeking:** M4A seeks are sample-exact: decoding restarts 8 access units ahead of the target (xHE-AAC:
  from the sync sample at or before that point) and drops the pre-roll output. An ADTS seek to within
  256 KiB of the nearest known frame walks frame headers forward, for up to 100 ms and at most 256 KiB, and lands exactly
  wherever the walk reaches. A farther target, or one past the budget, is estimated from the mean frame size
  (close for CBR, approximate for VBR), so a netstream's first request after the seek is for the target's
  bytes: a walk's read on a still-arriving stream would wait for them, and its budget cannot cut a read
  short. `FMOD_ACCURATETIME` indexes the whole stream at open, so every seek lands exactly.
- **Sample-rate changes** (implicit SBR found after open) arrive as FMOD's `Sample Rate Change` FLOAT tag.
- **Netstreams** (measured on FMOD 2.03.14 against a Range server):
  - At open FMOD reads each file's tail: a Range from `floor((size − 128) / 2048) × 2048` to the end
    (128–2,175 bytes; some sizes get a second, shorter one). The open waits on it, up to the network
    timeout, unless the server answers it; a 503 there changes nothing.
  - A seek outside FMOD's receive ring (twice the stream buffer) is a new Range request, from the target
    rounded down to the ring size; within the ring, none. ampaac reads through forward jumps of up to
    256 KiB rather than seeking.
  - A seek whose request fails or outlives the network timeout leaves the channel stranded: it reads as
    playing at the target and never ends, and `Sound::getOpenState` returns the error (24 after a 503, 43
    after the timeout). FMOD's own codecs behave the same. Both FMOD's MP3 codec and ampaac send a failed
    request once more first (ampaac not after a timeout, see Errors), so a single 503 recovers.
  - A body that stalls past the network timeout: `getOpenState` returns 43 from then on. FMOD's MP3 codec
    stops the channel; an ampaac stream plays on from stale bytes (below), and its open state reads ERROR
    for only a few milliseconds, so check the result `getOpenState` returns, not only the state.
- **A body cut short:** when an HTTP body ends before its Content-Length, FMOD neither fails nor
  reconnects, and reads keep succeeding up to that length, so the codec cannot see the cut. Once FMOD's
  receive ring has wrapped, the missing bytes are its stale contents, cycling; before that, zeros or an
  early end of file. FMOD's MP3 and WAV codecs and ampaac's ADTS decode the stale bytes as audio, so the
  missing span loops the ring's audio to the declared end (measured at 16 and 128 KiB stream buffers).
  M4A access units read from stale bytes fail to decode, and fdk's concealment holds the output near
  silence (under −66 dBFS) to the declared end. A server must never cut a body.
- **An M4A's data ending before its declared length** (a failed decoder, a cut before the ring wraps):
  ampaac plays silence to that length. What FMOD itself plays past a codec's early end is unmeasured.
- **Errors:** open returns `FMOD_ERR_FORMAT` for data that is not AAC. Before the data shows `ftyp` or an
  ADTS frame chain, a network failure (`FMOD_ERR_NET_SOCKET_ERROR`, `FMOD_ERR_NET_CONNECT`, `FMOD_ERR_HTTP*`)
  ends the open as it is, and any other file error answers `FMOD_ERR_FORMAT`, which FMOD's codec API treats
  as "not this format": it tries its next codec, which may still read the stream its own way. A fault while
  reading through ID3v2 tags answers `FMOD_ERR_FORMAT` too: FMOD's MPEG codec skips a tag with a hard seek,
  a new request. Measured (FMOD 2.03.14, network timeout 3 s): FMOD's next codec meets the same
  stall, so answering `FMOD_ERR_FORMAT` for a stall inside the first 8 KB failed the open after two
  timeouts instead of one, with the same final result. After that, a failed read is retried once at once
  from the same offset (on a netstream, a new request, as FMOD's MP3 codec does), except after a timeout
  (`FMOD_ERR_NET_SOCKET_ERROR`), which a retry would only repeat; a second failure passes through unchanged.
- **Damage and limits:** a stream with a size or an access-unit table conceals damaged frames and decodes
  what follows. An ADTS stream of unknown size (a live source) ends after 10 s of unbroken concealment
  (logged in FMOD's debug log); any stream ends after 16 decoder failures in a row, and ADTS when a resync
  finds no frame in 1 MiB (over a hundred maximum-size frames; not logged). These ends read as a normal end
  of file, leave the length an estimate, and a later seek past them plays.
- **Memory:** the codec's state and tables come from FMOD's allocator; fdk's decoder calls `calloc`.
- **Stack:** a decode peaks near 50 KB of stack (`make test` measures it: 49,784 bytes on arm64, 49,848 on
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
requests instead of 4. FMOD then rewinds the stream for its next codec. A server without Range support is
rewound only inside the sound's file buffer (`FMOD_CREATESOUNDEXINFO::filebuffersize`, 2 KiB by default), and
ampaac reads through up to 4 ID3v2 tags (up to 256 KiB each), then up to 8 KiB, before it rejects a stream: unhinted, a live MP3 without Range failed to open
(`FMOD_ERR_FILE_COULDNOTSEEK`). Hint the codec for content you know is not AAC
(`FMOD_CREATESOUNDEXINFO::suggestedsoundtype`, e.g. `FMOD_SOUND_TYPE_MPEG`): FMOD tries it first, and an AAC
stream hinted MPEG still opens through ampaac. A larger file buffer also opens the live MP3, but it moves
FMOD's tail read at open to `floor((size - 128) / buffer) * buffer`. FMOD for Unity's C# wrapper has no
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

- `make test` — host tests on macOS (both slices when Rosetta is present; `SAN=1` adds ASan + UBSan, and
  traps unsigned wraps in ampaac's C): per fixture the rate, channels, tone, length, seeks and trickled
  reads; gapless alignment against chirp fixtures; unknown size, truncation and corruption; read faults and
  their retry; damage and the stream-ending limits; rate changes; far seeks; peak stack depth. A watchdog
  fails a run that passes 600 s.
- `make linux-test` — the host tests on x86_64 Linux, then FMOD's own Linux library playing every fixture
  as an HTTP netstream (`test/serve_range.py`), with seeks and a WAV control, in an amd64 container.
- `make fuzz` (`test/fuzz.sh`, `FUZZ_SECONDS`) — libFuzzer + ASan + UBSan in a Linux container, seeded with
  the fixtures and two longer streams. Inputs grow up to 512 KiB at once (`-max_len`, `-len_control=0`), and
  the fuzz build lowers the ADTS resync limit to 32 KiB so they reach it; four leading control bytes steer
  the source (size, trickle, faults once or for good), the seeks and the limits.
- `test/fmod_harness.c` — drives a real FMOD library with the codec registered: open, play, seek, length,
  pause, and a System release during an open or a seek; its header lists the settings.
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
