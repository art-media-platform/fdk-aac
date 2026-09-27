#!/bin/bash
# The Linux leg's runtime check: builds ampaac_test and fmod_harness with the leg's toolchain (zig), then runs
# test/linux_check.py in an amd64 container (Debian bookworm, glibc 2.36) against FMOD's own Linux library.
#   test/linux.sh <libfmodstudio.so> [libampaac.so]
# Needs ZIG and FMOD_API_INC in the environment (the Makefile passes both) and docker with amd64 emulation.
set -euo pipefail

FMOD_LIB=${1:?libfmodstudio.so (FMOD for Unity: platforms/linux/lib/x86_64)}
FMOD_DIR=$(cd "$(dirname "$0")/.." && pwd)
CODEC=${2:-$FMOD_DIR/build/linux-x86_64-release/libampaac.so}
BUILD=${LINUX_TEST_BUILD:-$FMOD_DIR/build/linux-x86_64-test}
IMAGE=${LINUX_IMAGE:-python:3.12-slim-bookworm}
: "${ZIG:?ZIG must name the zig executable}" "${FMOD_API_INC:?FMOD_API_INC must name the FMOD SDK api/core/inc}"

abs() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
[ -f "$CODEC" ] || { echo "linux.sh: $CODEC missing (make linux first)"; exit 1; }

mkdir -p "$BUILD"
ZIG="$ZIG" cmake -S "$FMOD_DIR" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAMPAAC_PLATFORM=LINUX \
    -DAMPAAC_TESTS=ON -DFMOD_API_INC:PATH="$FMOD_API_INC" \
    -DCMAKE_TOOLCHAIN_FILE="$FMOD_DIR/cmake/linux-x86_64.cmake" > "$BUILD/configure.log"
cmake --build "$BUILD" --target ampaac_test fmod_harness --parallel > "$BUILD/build.log"

docker run --rm --platform linux/amd64 \
    -v "$BUILD":/build:ro \
    -v "$FMOD_DIR/test":/test:ro \
    -v "$(abs "$FMOD_LIB")":/fmod/libfmodstudio.so:ro \
    -v "$(abs "$CODEC")":/codec/libampaac.so:ro \
    "$IMAGE" python3 /test/linux_check.py /build /fmod/libfmodstudio.so /codec/libampaac.so /test/fixtures
