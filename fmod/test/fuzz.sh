#!/bin/bash
# Builds and runs the libFuzzer target inside a Linux container (Xcode's clang ships no libFuzzer).
#   test/fuzz.sh <seconds> <FMOD_API_INC> [corpus-dir]
# Runs from the host: mounts the fork at /src and the FMOD headers at /fmod-inc (read-only).
set -euo pipefail

SECONDS_TO_RUN=${1:?seconds}
FMOD_INC=${2:?FMOD_API_INC}
FMOD_DIR=$(cd "$(dirname "$0")/.." && pwd)
FORK_DIR=$(cd "$FMOD_DIR/.." && pwd)
CORPUS=${3:-$FMOD_DIR/build/fuzz-corpus}
ARTIFACTS=${FUZZ_ARTIFACTS:-$FMOD_DIR/build/fuzz-artifacts}
IMAGE=${FUZZ_IMAGE:-debian:bookworm-slim}

# Seeds: each fixture behind the harness's four control bytes (all zero), plus longer streams (a 36 s ADTS,
# a mid-stream rate change) that reach the seek index, the walk's reach and the limits within -max_len.
mkdir -p "$CORPUS" "$ARTIFACTS"
F="$FMOD_DIR/test/fixtures"
for f in "$F"/*.aac "$F"/*.m4a; do
    { printf '\0\0\0\0'; cat "$f"; } > "$CORPUS/seed-$(basename "$f")"
done
{ printf '\0\0\0\0'; for n in 1 2 3 4 5 6 7 8 9 10 11 12; do cat "$F/adts_lc_44k_stereo.aac"; done; } > "$CORPUS/seed-long.aac"
{ printf '\0\0\0\0'; cat "$F/adts_he_48k_stereo.aac" "$F/adts_lc_44k_stereo.aac"; } > "$CORPUS/seed-rate-change.aac"

# -len_control=0 lets inputs grow to -max_len at once (libFuzzer otherwise keeps them near the largest seed).
# The resync limit is lowered so inputs of that size reach it. ampaac's C also traps unsigned wraps; fdk's C++
# shifts negative values left by design, so shift-base is off there.
docker run --rm \
    -v "$FORK_DIR":/src:ro \
    -v "$FMOD_INC":/fmod-inc:ro \
    -v "$CORPUS":/corpus \
    -v "$ARTIFACTS":/artifacts \
    "$IMAGE" bash -c "
set -euo pipefail
if ! command -v clang >/dev/null; then
    apt-get update -qq >/dev/null && apt-get install -y -qq clang >/dev/null
fi
mkdir -p /build && cd /build
SAN='-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -g -O2'
SAN_C=\"\$SAN -fsanitize=unsigned-integer-overflow\"
SAN_CXX=\"\$SAN -fno-sanitize=shift-base\"
INC='-I/src/fmod/src -I/src/fmod/test -I/fmod-inc -I/src/libAACdec/include -I/src/libSYS/include'
FDK_INC=\$(for m in libAACdec libArithCoding libDRCdec libFDK libMpegTPDec libPCMutils libSACdec libSBRdec libSYS; do printf -- '-I/src/%s/include ' \$m; done)
for f in /src/libAACdec/src/*.cpp /src/libArithCoding/src/*.cpp /src/libDRCdec/src/*.cpp /src/libFDK/src/*.cpp \
         /src/libMpegTPDec/src/*.cpp /src/libPCMutils/src/*.cpp /src/libSACdec/src/*.cpp /src/libSBRdec/src/*.cpp /src/libSYS/src/*.cpp; do
    clang++ \$SAN_CXX -fno-exceptions -fno-rtti -w \$FDK_INC -c \"\$f\" -o \"\$(basename \"\$f\" .cpp).o\" &
    while [ \$(jobs -r | wc -l) -ge \$(nproc) ]; do sleep 0.1; done
done
wait
for f in /src/fmod/src/ampaac_codec.c /src/fmod/src/ampaac_io.c /src/fmod/src/ampaac_adts.c /src/fmod/src/ampaac_mp4.c /src/fmod/test/fake_fmod.c /src/fmod/test/ampaac_fuzz.c; do
    clang \$SAN_C -DAMPAAC_RESYNC_LIMIT=32768u -std=c11 \$INC -c \"\$f\" -o \"\$(basename \"\$f\" .c).o\"
done
clang++ \$SAN *.o -o ampaac_fuzz
echo \"fuzz: \$(clang --version | head -1); corpus \$(ls /corpus | wc -l) files; \${SECONDS_TO_RUN:-$SECONDS_TO_RUN} s\"
./ampaac_fuzz -max_total_time=$SECONDS_TO_RUN -max_len=524288 -len_control=0 -rss_limit_mb=4096 -timeout=10 \\
    -print_final_stats=1 -jobs=\$(nproc) -workers=\$(nproc) -artifact_prefix=/artifacts/ /corpus
"
