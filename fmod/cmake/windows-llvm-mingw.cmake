# Cross-compiles Windows DLLs with llvm-mingw (clang + mingw-w64, UCRT). Included by windows-<arch>.cmake,
# which sets AMP_WIN_ARCH; the toolchain root comes from LLVM_MINGW_ROOT (the Makefile passes it).
#
# llvm-mingw links compiler-rt statically; `-static` (see CMakeLists.txt) also pulls in libunwind, so the
# DLL depends only on KERNEL32 and the UCRT api-ms-win-crt-* sets that ship with Windows 10.

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR ${AMP_WIN_ARCH})

set(_llvm_mingw "$ENV{LLVM_MINGW_ROOT}")
if(NOT _llvm_mingw OR NOT EXISTS "${_llvm_mingw}/bin/${AMP_WIN_ARCH}-w64-mingw32-clang")
    message(FATAL_ERROR "LLVM_MINGW_ROOT must name an llvm-mingw toolchain (got '${_llvm_mingw}')")
endif()
set(_triple ${AMP_WIN_ARCH}-w64-mingw32)

set(CMAKE_C_COMPILER   "${_llvm_mingw}/bin/${_triple}-clang")
set(CMAKE_CXX_COMPILER "${_llvm_mingw}/bin/${_triple}-clang++")
set(CMAKE_RC_COMPILER  "${_llvm_mingw}/bin/${_triple}-windres")
set(CMAKE_AR           "${_llvm_mingw}/bin/llvm-ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${_llvm_mingw}/bin/llvm-ranlib" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH "${_llvm_mingw}/${_triple}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
