# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
#
# Checks a built library's symbols for the portability rules that headers alone cannot show.
#
#   cmake -DNM=nm -DLIBRARY=libbartap.a "-DOBJECTS=a.c;b.c" -DEXPECT_VIOLATIONS=N -P check_symbols.cmake
#
# Violations:
# - writable data of any kind (nm types B, C, D, G, S, V in either case): global or static mutable state.
#   Read-only data (R), such as a CRC table, is fine.
# - an undefined symbol other than memcpy, memset, memcmp and memmove (the <string.h> functions; memmove
#   because compilers emit calls to it for some copies), __stack_chk_fail and __stack_chk_guard, which a
#   compiler's default stack protector adds, and on Arm the __aeabi_* helpers of the run-time ABI (RTABI:
#   division, 64-bit shifts, the aeabi memcpy/memset family) that the compiler emits for plain C, and
#   _GLOBAL_OFFSET_TABLE_, which 32-bit x86 position-independent code references whenever it touches a
#   global; the linker resolves it, and it is not a call. Anything else is a call into the C library or
#   the platform (malloc, abort, __assert_fail, printf...).
# Every undefined symbol is listed, allowed or not, so the output shows what the library pulls in.
# Passes only if exactly EXPECT_VIOLATIONS were found: 0 for the library, and the planted count for the
# selftest. Each name in OBJECTS must appear in nm's output as an object (NAME.o or NAME.obj), so a wrong
# library cannot pass by having nothing in it.

foreach(var NM LIBRARY OBJECTS EXPECT_VIOLATIONS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "usage: cmake -DNM=nm -DLIBRARY=lib.a \"-DOBJECTS=a.c;...\" "
            "-DEXPECT_VIOLATIONS=N -P check_symbols.cmake")
    endif()
endforeach()

execute_process(COMMAND "${NM}" "${LIBRARY}" OUTPUT_VARIABLE output ERROR_VARIABLE errors
    RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${NM} ${LIBRARY} failed (${status}): ${errors}")
endif()

foreach(object IN LISTS OBJECTS)
    string(REPLACE "." "\\." pattern "${object}")
    if(NOT "\n${output}" MATCHES "\n${pattern}\\.(o|obj):")
        message(FATAL_ERROR "${object} is not in ${LIBRARY}")
    endif()
endforeach()

set(allowed_undefined
    "^(memcpy|memset|memcmp|memmove|__stack_chk_fail|__stack_chk_guard|__aeabi_[A-Za-z0-9_]+|_GLOBAL_OFFSET_TABLE_)$")
set(violations 0)
set(undefined "")
string(REPLACE "\n" ";" lines "${output}")
foreach(line IN LISTS lines)
    if(NOT line MATCHES "^[0-9A-Fa-f ]* ([A-Za-z]) (.+)$")
        continue()
    endif()
    set(type "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    if(type MATCHES "^[BbCDdGgSsVv]$")
        math(EXPR violations "${violations} + 1")
        message("writable data: ${name} (${type})")
    elseif(type STREQUAL "U")
        list(APPEND undefined "${name}")
        if(NOT name MATCHES "${allowed_undefined}")
            math(EXPR violations "${violations} + 1")
            message("call outside the allowed functions: ${name}")
        endif()
    endif()
endforeach()

list(REMOVE_DUPLICATES undefined)
list(SORT undefined)
if(undefined)
    string(REPLACE ";" " " undefined "${undefined}")
else()
    set(undefined "(none)")
endif()
message("undefined symbols in ${LIBRARY}: ${undefined}")

message("${violations} violations in ${LIBRARY} (expected ${EXPECT_VIOLATIONS})")
if(NOT violations EQUAL EXPECT_VIOLATIONS)
    message(FATAL_ERROR "symbol check failed")
endif()
