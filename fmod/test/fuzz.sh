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

mkdir -p "$CORPUS" "$ARTIFACTS"
cp "$FMOD_DIR"/test/fixtures/*.aac "$FMOD_DIR"/test/fixtures/*.m4a "$CORPUS"/ 2>/dev/null || true

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
SAN='-fsanitize=fuzzer,address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=all -g -O2'
INC='-I/src/fmod/src -I/src/fmod/test -I/fmod-inc -I/src/libAACdec/include -I/src/libSYS/include'
FDK_INC=\$(for m in libAACdec libArithCoding libDRCdec libFDK libMpegTPDec libPCMutils libSACdec libSBRdec libSYS; do printf -- '-I/src/%s/include ' \$m; done)
for f in /src/libAACdec/src/*.cpp /src/libArithCoding/src/*.cpp /src/libDRCdec/src/*.cpp /src/libFDK/src/*.cpp \
         /src/libMpegTPDec/src/*.cpp /src/libPCMutils/src/*.cpp /src/libSACdec/src/*.cpp /src/libSBRdec/src/*.cpp /src/libSYS/src/*.cpp; do
    clang++ \$SAN -fno-exceptions -fno-rtti -w \$FDK_INC -c \"\$f\" -o \"\$(basename \"\$f\" .cpp).o\" &
    while [ \$(jobs -r | wc -l) -ge \$(nproc) ]; do sleep 0.1; done
done
wait
for f in /src/fmod/src/ampaac_codec.c /src/fmod/src/ampaac_io.c /src/fmod/src/ampaac_adts.c /src/fmod/src/ampaac_mp4.c /src/fmod/test/fake_fmod.c /src/fmod/test/ampaac_fuzz.c; do
    clang \$SAN -std=c11 \$INC -c \"\$f\" -o \"\$(basename \"\$f\" .c).o\"
done
clang++ \$SAN *.o -o ampaac_fuzz
echo \"fuzz: \$(clang --version | head -1); corpus \$(ls /corpus | wc -l) files; \${SECONDS_TO_RUN:-$SECONDS_TO_RUN} s\"
./ampaac_fuzz -max_total_time=$SECONDS_TO_RUN -rss_limit_mb=4096 -timeout=10 -print_final_stats=1 \\
    -jobs=\$(nproc) -workers=\$(nproc) -artifact_prefix=/artifacts/ /corpus
"
