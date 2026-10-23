# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
#
# Checks which headers the library's files include.
#
#   cmake "-DDIRS=dir;dir..." -DEXPECT_VIOLATIONS=N -P check_includes.cmake
#
# Every .c and .h file under DIRS may include only <stdint.h>, <stddef.h>, <stdbool.h> and <string.h>,
# and its own "bartap/NAME.h" headers. Anything else - another standard header, a platform header, a
# quoted header outside bartap/, or an include through a macro - is a violation. Passes only if exactly
# EXPECT_VIOLATIONS were found: 0 for the library, and the planted count for the selftest, which proves
# each kind is caught. Finding no files at all fails, so a wrong path cannot pass.
#
# A directive is any line whose first non-blank character is '#' followed by "include" (so #include_next
# too). A line like that inside a block comment would be reported as well; reword the comment.

if(NOT DEFINED DIRS OR NOT DEFINED EXPECT_VIOLATIONS)
    message(FATAL_ERROR "usage: cmake \"-DDIRS=dir;...\" -DEXPECT_VIOLATIONS=N -P check_includes.cmake")
endif()

set(allowed_system stdint.h stddef.h stdbool.h string.h)
set(files)
foreach(dir IN LISTS DIRS)
    file(GLOB_RECURSE found "${dir}/*.c" "${dir}/*.h")
    list(APPEND files ${found})
endforeach()
list(LENGTH files file_count)
if(file_count EQUAL 0)
    message(FATAL_ERROR "no .c or .h files under ${DIRS}")
endif()

set(violations 0)
foreach(path IN LISTS files)
    file(STRINGS "${path}" directives REGEX "^[ \t]*#[ \t]*include")
    foreach(line IN LISTS directives)
        set(ok FALSE)
        if(line MATCHES "^[ \t]*#[ \t]*include[_a-z]*[ \t]*<([^>]+)>")
            list(FIND allowed_system "${CMAKE_MATCH_1}" index)
            if(NOT index EQUAL -1)
                set(ok TRUE)
            endif()
        elseif(line MATCHES "^[ \t]*#[ \t]*include[_a-z]*[ \t]*\"([^\"]+)\"")
            if(CMAKE_MATCH_1 MATCHES "^bartap/[A-Za-z0-9_]+\\.h$")
                set(ok TRUE)
            endif()
        endif()
        if(NOT ok)
            math(EXPR violations "${violations} + 1")
            string(STRIP "${line}" line)
            message("${path}: not allowed: ${line}")
        endif()
    endforeach()
endforeach()

message("${file_count} files checked, ${violations} disallowed includes (expected ${EXPECT_VIOLATIONS})")
if(NOT violations EQUAL EXPECT_VIOLATIONS)
    message(FATAL_ERROR "include check failed")
endif()
