#
# ampaac — FMOD codec plugin decoding AAC with the Fraunhofer FDK AAC decoder.
#
# Builds the decoder half of fdk-aac (the encoder is never compiled) plus the FMOD codec glue into one
# library per platform. See ./Makefile for the entry points.
#
# Inputs (set by the Makefile, or pass with -D...):
#   AMPAAC_PLATFORM   OSX | IOS | ANDROID | WINDOWS | LINUX
#   FMOD_API_INC      directory holding fmod.h and fmod_codec.h (FMOD Engine SDK: api/core/inc)
#   AMPAAC_SPIKE      ON: trace FMOD callbacks to stderr (measurement builds)
#   AMPAAC_TESTS      ON: also build the host test runner (OSX, LINUX)
#
# Platform-specific:
#   ANDROID_NDK / ANDROID_ABI      AMPAAC_PLATFORM=ANDROID
#   CMAKE_TOOLCHAIN_FILE           WINDOWS and LINUX when cross-building (see ./cmake)
#

cmake_minimum_required(VERSION 3.20)

# Platform selection precedes project(): CMake reads the system name, sysroot and deployment target while
# initializing the compiler, and ignores later changes.

if(NOT DEFINED AMPAAC_PLATFORM)
    message(FATAL_ERROR "AMPAAC_PLATFORM is not set (expected one of: OSX, IOS, ANDROID, WINDOWS, LINUX)")
endif()
if(NOT DEFINED FMOD_API_INC OR NOT EXISTS "${FMOD_API_INC}/fmod_codec.h")
    message(FATAL_ERROR "FMOD_API_INC must name the FMOD Engine SDK's api/core/inc (fmod_codec.h not found in '${FMOD_API_INC}')")
endif()
if(NOT DEFINED APPLE_PLATFORMS_PATH)
    set(APPLE_PLATFORMS_PATH "/Applications/Xcode.app/Contents/Developer/Platforms")
endif()

if(AMPAAC_PLATFORM STREQUAL "ANDROID")
    set(CMAKE_SYSTEM_NAME                   "Android")
    set(CMAKE_TOOLCHAIN_FILE                "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    set(ANDROID_PLATFORM                    android-27)
    set(ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES ON)          # 16 KB LOAD alignment (Android 15+)
    set(ANDROID_STL                         none)        # no C++ runtime: fdk-aac uses none
    set(AMPAAC_INSTALL_SUBDIR               "Android/${ANDROID_ABI}")

elseif(AMPAAC_PLATFORM STREQUAL "IOS")
    set(CMAKE_SYSTEM_NAME           "iOS")
    set(CMAKE_SYSTEM_PROCESSOR      "arm64")
    set(CMAKE_OSX_ARCHITECTURES     "arm64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.4" CACHE STRING "" FORCE)
    set(CMAKE_OSX_SYSROOT           "${APPLE_PLATFORMS_PATH}/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk" CACHE PATH "" FORCE)
    set(AMPAAC_INSTALL_SUBDIR       "iOS")

elseif(AMPAAC_PLATFORM STREQUAL "OSX")
    # No CMAKE_SYSTEM_NAME: naming the host system switches CMake into cross-compiling mode. The FORCE keeps
    # Darwin-Initialize from caching the host OS version as the deployment target.
    set(CMAKE_OSX_ARCHITECTURES     "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE)
    set(AMPAAC_INSTALL_SUBDIR       "OSX")

elseif(AMPAAC_PLATFORM STREQUAL "WINDOWS")
    if(NOT CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        message(FATAL_ERROR "AMPAAC_PLATFORM=WINDOWS off a Windows host needs -DCMAKE_TOOLCHAIN_FILE=cmake/windows-<arch>.cmake")
    endif()

elseif(AMPAAC_PLATFORM STREQUAL "LINUX")
    if(NOT CMAKE_HOST_LINUX AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        message(FATAL_ERROR "AMPAAC_PLATFORM=LINUX off a Linux host needs -DCMAKE_TOOLCHAIN_FILE=cmake/linux-x86_64.cmake")
    endif()

else()
    message(FATAL_ERROR "Unsupported AMPAAC_PLATFORM: ${AMPAAC_PLATFORM}")
endif()

project(ampaac
    VERSION     1.0
    DESCRIPTION "FMOD codec plugin: AAC via the Fraunhofer FDK AAC decoder"
    LANGUAGES   C CXX
)

if(AMPAAC_PLATFORM STREQUAL "WINDOWS")
    string(REPLACE "aarch64" "arm64" _arch "${CMAKE_SYSTEM_PROCESSOR}")
    set(AMPAAC_INSTALL_SUBDIR "Windows/${_arch}")
elseif(AMPAAC_PLATFORM STREQUAL "LINUX")
    set(AMPAAC_INSTALL_SUBDIR "Linux/${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(CMAKE_C_STANDARD                 11)
set(CMAKE_C_STANDARD_REQUIRED        ON)
set(CMAKE_POSITION_INDEPENDENT_CODE  ON)
set(CMAKE_C_VISIBILITY_PRESET        hidden)
set(CMAKE_CXX_VISIBILITY_PRESET      hidden)


# --- fdk-aac decoder --------------------------------------------------------
#
# The decoder modules, globbed per directory (non-recursive: libFDK/src/arm etc. are #included, not
# compiled). Upstream's lists name the same files; a new decoder directory would show up as link errors.

set(FDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
get_filename_component(FDK_ROOT_ABS "${FDK_ROOT}" REALPATH)

# Sources are named relative to the repository in the binaries (__FILE__, debug info), never by the
# builder's absolute path. Stack protection on every leg (Apple clang and the NDK default to it; zig and
# llvm-mingw do not): the decoder parses untrusted network input.
if(NOT MSVC)
    add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/../=" "-ffile-prefix-map=${FDK_ROOT_ABS}/="
                        -fstack-protector-strong)
endif()
set(FDK_DECODER_MODULES
    libAACdec libArithCoding libDRCdec libFDK libMpegTPDec libPCMutils libSACdec libSBRdec libSYS)

set(FDK_DECODER_SOURCES)
set(FDK_DECODER_INCLUDES)
foreach(module ${FDK_DECODER_MODULES})
    file(GLOB module_sources CONFIGURE_DEPENDS "${FDK_ROOT}/${module}/src/*.cpp")
    list(APPEND FDK_DECODER_SOURCES  ${module_sources})
    list(APPEND FDK_DECODER_INCLUDES "${FDK_ROOT}/${module}/include")
endforeach()

add_library(fdkdec OBJECT ${FDK_DECODER_SOURCES})
target_include_directories(fdkdec PRIVATE ${FDK_DECODER_INCLUDES})
# Reproducible objects: every __DATE__/__TIME__ the decoder modules expand sits behind this upstream switch
# (zig's clang rejects the expansion outright: -Werror=date-time).
target_compile_definitions(fdkdec PRIVATE SUPPRESS_BUILD_DATE_INFO)
if(MSVC)
    target_compile_options(fdkdec PRIVATE /EHs-c- /GR-)
else()
    target_compile_options(fdkdec PRIVATE -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
endif()


# --- ampaac -----------------------------------------------------------------

find_package(Git QUIET)
set(AMPAAC_SOURCE_REV "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FDK_ROOT}" rev-parse --short HEAD
                    OUTPUT_VARIABLE _rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rev_rc)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FDK_ROOT}" status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_rev_rc EQUAL 0)
        set(AMPAAC_SOURCE_REV "${_rev}")
        if(_dirty)
            string(APPEND AMPAAC_SOURCE_REV "-dirty")
        endif()
    endif()
endif()

set(AMPAAC_SOURCES
    src/ampaac_codec.c
    src/ampaac_io.c
    src/ampaac_adts.c
    src/ampaac_mp4.c)

function(ampaac_configure target)
    target_include_directories(${target} PRIVATE
        src
        "${FMOD_API_INC}"
        "${FDK_ROOT}/libAACdec/include"
        "${FDK_ROOT}/libSYS/include")
    target_compile_definitions(${target} PRIVATE AMPAAC_SOURCE_REV="${AMPAAC_SOURCE_REV}"
        $<$<BOOL:${AMPAAC_SPIKE}>:AMPAAC_SPIKE>)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffunction-sections -fdata-sections)
    endif()
endfunction()

if(AMPAAC_PLATFORM STREQUAL "IOS")
    # One relocatable object (ld -r) whose only global is AMPAAC_GetCodecDescription, archived: the app link
    # sees no fdk symbols, so another copy of fdk-aac in the same binary cannot collide.
    add_library(ampaac_objects OBJECT ${AMPAAC_SOURCES})
    ampaac_configure(ampaac_objects)
    target_compile_definitions(ampaac_objects PRIVATE AMPAAC_STATIC)

    set(_prelinked "${CMAKE_CURRENT_BINARY_DIR}/ampaac_prelinked.o")
    set(_archive   "${CMAKE_CURRENT_BINARY_DIR}/libampaac.a")
    add_custom_command(
        OUTPUT  "${_archive}"
        COMMAND "${CMAKE_C_COMPILER}" -arch arm64 -isysroot "${CMAKE_OSX_SYSROOT}"
                -mios-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET} -nostdlib -r
                -Wl,-exported_symbol,_AMPAAC_GetCodecDescription
                -o "${_prelinked}"
                $<TARGET_OBJECTS:ampaac_objects> $<TARGET_OBJECTS:fdkdec>
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${_archive}"
        COMMAND xcrun libtool -static -no_warning_for_no_symbols -o "${_archive}" "${_prelinked}"
        DEPENDS ampaac_objects fdkdec $<TARGET_OBJECTS:ampaac_objects> $<TARGET_OBJECTS:fdkdec>
        COMMAND_EXPAND_LISTS
        VERBATIM)
    add_custom_target(ampaac ALL DEPENDS "${_archive}")
    set(AMPAAC_ARTIFACT "${_archive}")
else()
    add_library(ampaac SHARED ${AMPAAC_SOURCES} $<TARGET_OBJECTS:fdkdec>)
    ampaac_configure(ampaac)
    # fdk-aac is C++ without exceptions, RTTI or the C++ library: link as C, no C++ runtime dependency.
    set_target_properties(ampaac PROPERTIES LINKER_LANGUAGE C)

    # Release libraries ship with local symbols stripped (they are committed to the Unity assets repo).
    if(APPLE)
        target_link_options(ampaac PRIVATE -Wl,-dead_strip $<$<CONFIG:Release>:-Wl,-x>)
    elseif(AMPAAC_PLATFORM STREQUAL "WINDOWS")
        # PDB beside the DLL (kept out of the app); compiler-rt, libunwind and the CRT startup linked in.
        # --gc-sections is explicit: lld leaves unreferenced sections in when it writes a PDB. --strip-all
        # drops the DWARF that llvm-mingw's prebuilt runtime objects carry (the PDB keeps the symbols).
        set_target_properties(ampaac PROPERTIES PREFIX "")
        target_compile_options(ampaac PRIVATE -g -gcodeview)
        target_compile_options(fdkdec PRIVATE -g -gcodeview)
        target_link_options(ampaac PRIVATE -static -Wl,--gc-sections -Wl,--exclude-all-symbols -Wl,--no-insert-timestamp
                            -Wl,--pdb= -Wl,-Xlink=-pdbaltpath:%_PDB% $<$<CONFIG:Release>:-Wl,--strip-all>)
    else()
        target_link_options(ampaac PRIVATE -Wl,--gc-sections -Wl,--no-undefined $<$<CONFIG:Release>:-s>)
        target_link_libraries(ampaac PRIVATE m)
    endif()
    set(AMPAAC_ARTIFACT "$<TARGET_FILE:ampaac>")
endif()

# The Makefile installs from here (temp + rename), so an editor holding the old library keeps its inode.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/artifact.txt" CONTENT "${AMPAAC_INSTALL_SUBDIR}\n${AMPAAC_ARTIFACT}\n")


# --- Host tests -------------------------------------------------------------

if(AMPAAC_TESTS AND AMPAAC_PLATFORM MATCHES "^(OSX|LINUX)$")
    add_executable(ampaac_test test/ampaac_test.c test/fake_fmod.c ${AMPAAC_SOURCES} $<TARGET_OBJECTS:fdkdec>)
    ampaac_configure(ampaac_test)
    target_include_directories(ampaac_test PRIVATE test)
    set_target_properties(ampaac_test PROPERTIES LINKER_LANGUAGE C)
    if(NOT APPLE)
        target_link_libraries(ampaac_test PRIVATE m)
    endif()

    # Drives a real FMOD library (loaded at run time) with the codec registered: test/fmod_harness.c.
    add_executable(fmod_harness test/fmod_harness.c)
    target_include_directories(fmod_harness PRIVATE "${FMOD_API_INC}")
    target_link_libraries(fmod_harness PRIVATE ${CMAKE_DL_LIBS})

    # test/linux.sh runs these in amd64 containers, where Rosetta's loader rejects an executable whose last LOAD
    # segment holds no file bytes ("bss_size overflow"); without RELRO, .data and .bss share one segment.
    if(AMPAAC_PLATFORM STREQUAL "LINUX")
        target_link_options(ampaac_test PRIVATE -Wl,-z,norelro)
        target_link_options(fmod_harness PRIVATE -Wl,-z,norelro)
    endif()
endif()
