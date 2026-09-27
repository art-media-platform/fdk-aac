#
# ampaac — FMOD codec plugin: AAC via the Fraunhofer FDK AAC decoder
#
# Usage:
#   make osx | ios | android | windows | linux   build one platform
#   make all                                     every platform
#   make test                                    host tests (macOS); SAN=1 adds ASan + UBSan
#   make install                                 copy every built library into the Unity libs tree
#   make clean | help
#
# Required:
#   FMOD_API_INC=<FMOD Engine SDK>/api/core/inc  (fmod.h, fmod_codec.h; never committed here)
#
# Overrides:
#   BUILD_TYPE=Release|Debug   BUILD_ROOT=<dir>   UNITY_LIBS=<dir>   CMAKE=<cmake>   GENERATOR=<gen>
#   LLVM_MINGW_ROOT=<llvm-mingw>   ZIG=<zig>   VERBOSE=1   SPIKE=1 (callback traces, AMPAAC_LENGTH_SCALE)
#
# A build never writes into the Unity tree. `make install` does, temp + rename, so an editor holding the
# old library keeps its mapped inode; Unity loads a new native plugin only after an editor restart.
#

MAKEFLAGS    += --warn-undefined-variables
SHELL         = /bin/bash -o nounset -o errexit -o pipefail
.DEFAULT_GOAL = help

# --- Paths ------------------------------------------------------------------

FMOD_DIR     := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
FORK_DIR     := $(patsubst %/,%,$(dir $(FMOD_DIR)))
WORKSPACE    := $(patsubst %/,%,$(dir $(FORK_DIR)))
UNITY_PROJ   ?= $(WORKSPACE)/amp.3D.unity
UNITY_LIBS   ?= $(UNITY_PROJ)/Assets/art.media.platform/libs
BUILD_ROOT   ?= $(FMOD_DIR)/build
FMOD_API_INC ?=

unity-engine-path = $(shell python3 "$(UNITY_PROJ)/cmd/tools.py" --project-path "$(UNITY_PROJ)" --get-engine-path)
ANDROID_NDK       = $(unity-engine-path)/PlaybackEngines/AndroidPlayer/NDK

# Pinned cross toolchains (release + SHA-256 in README.md), unpacked outside the repository.
LLVM_MINGW_ROOT ?= $(HOME)/Applications/llvm-mingw-20260922-ucrt-macos-universal
ZIG             ?= $(HOME)/Applications/zig-aarch64-macos-0.16.0/zig

# --- Build settings ---------------------------------------------------------

BUILD_TYPE   ?= Release
BUILD_TYPE_LC = $(shell echo $(BUILD_TYPE) | tr A-Z a-z)
CMAKE        ?= cmake
GENERATOR    ?= Unix Makefiles
JOBS         ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
SPIKE        ?=
SAN          ?=

# Not exported: `cmake --build` treats any VERBOSE in the environment, even empty, as verbose.
VERBOSE      ?=
ifeq ($(VERBOSE),)
unexport VERBOSE
.SILENT:
endif

define require-fmod
	if [ -z "$(FMOD_API_INC)" ] || [ ! -f "$(FMOD_API_INC)/fmod_codec.h" ]; then \
		echo "ampaac: set FMOD_API_INC=<FMOD Engine SDK>/api/core/inc (fmod_codec.h not found)"; \
		exit 1; \
	fi
endef

# $(1) = build tag (also the build subdir), $(2) = platform -D flags
define cmake-build
	$(require-fmod)
	$(eval BUILD_DIR := $(BUILD_ROOT)/$(1)-$(BUILD_TYPE_LC))
	mkdir -p "$(BUILD_DIR)"
	$(CMAKE) -S "$(FMOD_DIR)" -B "$(BUILD_DIR)"          \
		-G "$(GENERATOR)"                                 \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE)                  \
		-DFMOD_API_INC:PATH="$(FMOD_API_INC)"             \
		-DAMPAAC_SPIKE=$(if $(SPIKE),ON,OFF)              \
		$(2) > "$(BUILD_DIR)/configure.log"
	MAKEFLAGS= $(CMAKE) --build "$(BUILD_DIR)" --config $(BUILD_TYPE) --parallel $(JOBS)
	echo "ampaac: $(1) built -> $$(sed -n 2p "$(BUILD_DIR)/artifact.txt")"
endef

.PHONY: help all osx ios android windows windows-build linux linux-build test install clean

help:
	@echo "ampaac — make targets"
	@echo "  FMOD_API_INC:  $(if $(FMOD_API_INC),$(FMOD_API_INC),<unset: required>)"
	@echo "  UNITY_LIBS:    $(UNITY_LIBS)"
	@echo "  BUILD_ROOT:    $(BUILD_ROOT)"
	@echo "  make osx | ios | android | windows | linux | all"
	@echo "  make test      host tests (SAN=1 for ASan + UBSan)"
	@echo "  make install   copy built libraries into UNITY_LIBS (temp + rename)"
	@echo "  make clean     remove BUILD_ROOT (also after a Unity editor upgrade)"

osx:
	$(call cmake-build,osx,-DAMPAAC_PLATFORM=OSX)

ios:
	$(call cmake-build,ios,-DAMPAAC_PLATFORM=IOS)

android:
	$(eval NDK := $(ANDROID_NDK))
	if [ ! -d "$(NDK)" ]; then \
		echo "ampaac: Android NDK not found at '$(NDK)' (resolved via $(UNITY_PROJ)/cmd/tools.py; after an editor upgrade run make clean)"; \
		exit 1; \
	fi
	$(call cmake-build,android-arm64-v8a,-DAMPAAC_PLATFORM=ANDROID -DANDROID_NDK:PATH="$(NDK)" -DANDROID_ABI=arm64-v8a)

windows:
	if [ -z "$(LLVM_MINGW_ROOT)" ] || [ ! -x "$(LLVM_MINGW_ROOT)/bin/x86_64-w64-mingw32-clang" ]; then \
		echo "ampaac: set LLVM_MINGW_ROOT to the llvm-mingw toolchain (bin/x86_64-w64-mingw32-clang)"; \
		exit 1; \
	fi
	LLVM_MINGW_ROOT="$(LLVM_MINGW_ROOT)" $(MAKE) --no-print-directory windows-build

windows-build:
	$(call cmake-build,windows-x86_64,-DAMPAAC_PLATFORM=WINDOWS -DCMAKE_TOOLCHAIN_FILE="$(FMOD_DIR)/cmake/windows-x86_64.cmake")

linux:
	if [ ! -x "$(ZIG)" ]; then \
		echo "ampaac: set ZIG to the zig executable (zig 0.16.0)"; \
		exit 1; \
	fi
	ZIG="$(ZIG)" $(MAKE) --no-print-directory linux-build

linux-build:
	$(call cmake-build,linux-x86_64,-DAMPAAC_PLATFORM=LINUX -DCMAKE_TOOLCHAIN_FILE="$(FMOD_DIR)/cmake/linux-x86_64.cmake")

all: osx ios android windows linux

test:
	$(require-fmod)
	$(eval TEST_DIR := $(BUILD_ROOT)/osx-test$(if $(SAN),-san))
	mkdir -p "$(TEST_DIR)"
	$(CMAKE) -S "$(FMOD_DIR)" -B "$(TEST_DIR)" -G "$(GENERATOR)"                          \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo -DAMPAAC_PLATFORM=OSX -DAMPAAC_TESTS=ON                    \
		-DFMOD_API_INC:PATH="$(FMOD_API_INC)"                                               \
		$(if $(SAN),"-DCMAKE_C_FLAGS=-fsanitize=address$(,)undefined -fno-omit-frame-pointer" \
		            "-DCMAKE_CXX_FLAGS=-fsanitize=address$(,)undefined -fno-omit-frame-pointer" \
		            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address$(,)undefined")              \
		> "$(TEST_DIR)/configure.log"
	MAKEFLAGS= $(CMAKE) --build "$(TEST_DIR)" --target ampaac_test --parallel $(JOBS)
	"$(TEST_DIR)/ampaac_test" "$(FMOD_DIR)/test/fixtures"

# Copies each built library to UNITY_LIBS/<subdir>/ through a temp name + rename.
install:
	found=0; \
	for manifest in "$(BUILD_ROOT)"/*-$(BUILD_TYPE_LC)/artifact.txt; do \
		[ -f "$$manifest" ] || continue; \
		subdir=$$(sed -n 1p "$$manifest"); artifact=$$(sed -n 2p "$$manifest"); \
		[ -f "$$artifact" ] || { echo "ampaac: $$artifact missing (build first)"; exit 1; }; \
		dest="$(UNITY_LIBS)/$$subdir"; name=$$(basename "$$artifact"); \
		mkdir -p "$$dest"; \
		cp "$$artifact" "$$dest/.$$name.tmp"; \
		mv -f "$$dest/.$$name.tmp" "$$dest/$$name"; \
		echo "ampaac: installed $$dest/$$name"; found=1; \
	done; \
	[ $$found = 1 ] || { echo "ampaac: nothing built under $(BUILD_ROOT)"; exit 1; }

clean:
	rm -rf "$(BUILD_ROOT)"

, := ,
