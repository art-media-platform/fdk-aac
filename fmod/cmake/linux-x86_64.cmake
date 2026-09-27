# Cross-compiles the Linux shared library with zig's clang against a pinned glibc floor. The zig
# executable comes from ZIG (the Makefile passes it). CMake needs one executable per tool, so thin
# driver scripts wrap `zig cc|c++|ar|ranlib` with the target.
#
# glibc 2.28 sits below Unity 6.3's player floor (UnityPlayer.so needs GLIBC_2.35).

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_zig "$ENV{ZIG}")
if(NOT _zig OR NOT EXISTS "${_zig}")
    message(FATAL_ERROR "ZIG must name the zig executable (got '${_zig}')")
endif()
set(_target x86_64-linux-gnu.2.28)
set(_drivers "${CMAKE_BINARY_DIR}/zig-drivers")
file(MAKE_DIRECTORY "${_drivers}")
foreach(_tool cc c++ ar ranlib)
    if(_tool MATCHES "^(cc|c\\+\\+)$")
        set(_line "exec \"${_zig}\" ${_tool} -target ${_target} \"$@\"")
    else()
        set(_line "exec \"${_zig}\" ${_tool} \"$@\"")
    endif()
    file(WRITE "${_drivers}/zig-${_tool}" "#!/bin/sh\n${_line}\n")
    file(CHMOD "${_drivers}/zig-${_tool}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()

set(CMAKE_C_COMPILER   "${_drivers}/zig-cc")
set(CMAKE_CXX_COMPILER "${_drivers}/zig-c++")
set(CMAKE_AR           "${_drivers}/zig-ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${_drivers}/zig-ranlib" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
