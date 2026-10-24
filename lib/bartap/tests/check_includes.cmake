# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
#
# Checks which headers the library's files include.
#
#   cmake "-DPUBLIC_DIRS=dir;..." "-DPRIVATE_DIRS=dir;..." -DEXPECT_VIOLATIONS=N -P check_includes.cmake
#
# PUBLIC_DIRS hold the public headers (include/), PRIVATE_DIRS the sources and internal headers (src/).
# Every .c and .h file under either may include <stdint.h>, <stddef.h>, <stdbool.h> and <string.h>,
# and the public "bartap/NAME.h" headers. A file under PRIVATE_DIRS may also include an internal header
# as a bare "NAME.h", but only one that exists directly in one of PRIVATE_DIRS; a public header may not,
# since it would not compile for a user of the library. Anything else - another standard header, a
# platform header, a quoted header outside bartap/, an internal header from a public one, or an include
# through a macro - is a violation. Passes only if exactly EXPECT_VIOLATIONS were found: 0 for the
# library, and the planted count for the selftest, which proves each kind is caught. Finding no files
# at all fails, so a wrong path cannot pass.
#
# A directive is any line whose first non-blank character is '#' followed by "include" (so #include_next
# too). A line like that inside a block comment would be reported as well; reword the comment.

if(NOT DEFINED PUBLIC_DIRS OR NOT DEFINED PRIVATE_DIRS OR NOT DEFINED EXPECT_VIOLATIONS)
    message(FATAL_ERROR "usage: cmake \"-DPUBLIC_DIRS=dir;...\" \"-DPRIVATE_DIRS=dir;...\" "
        "-DEXPECT_VIOLATIONS=N -P check_includes.cmake")
endif()

set(allowed_system stdint.h stddef.h stdbool.h string.h)
set(files)
set(private_files)
set(internal_headers)
foreach(dir IN LISTS PUBLIC_DIRS PRIVATE_DIRS)
    file(GLOB_RECURSE found "${dir}/*.c" "${dir}/*.h")
    list(APPEND files ${found})
endforeach()
foreach(dir IN LISTS PRIVATE_DIRS)
    file(GLOB_RECURSE found "${dir}/*.c" "${dir}/*.h")
    list(APPEND private_files ${found})
    file(GLOB headers RELATIVE "${dir}" "${dir}/*.h")
    list(APPEND internal_headers ${headers})
endforeach()
list(REMOVE_DUPLICATES files)
list(LENGTH files file_count)
if(file_count EQUAL 0)
    message(FATAL_ERROR "no .c or .h files under ${PUBLIC_DIRS} or ${PRIVATE_DIRS}")
endif()

set(violations 0)
foreach(path IN LISTS files)
    list(FIND private_files "${path}" private_index)
    file(STRINGS "${path}" directives REGEX "^[ \t]*#[ \t]*include")
    foreach(line IN LISTS directives)
        set(ok FALSE)
        if(line MATCHES "^[ \t]*#[ \t]*include[_a-z]*[ \t]*<([^>]+)>")
            list(FIND allowed_system "${CMAKE_MATCH_1}" index)
            if(NOT index EQUAL -1)
                set(ok TRUE)
            endif()
        elseif(line MATCHES "^[ \t]*#[ \t]*include[_a-z]*[ \t]*\"([^\"]+)\"")
            set(quoted "${CMAKE_MATCH_1}")
            if(quoted MATCHES "^bartap/[A-Za-z0-9_]+\\.h$")
                set(ok TRUE)
            elseif(NOT private_index EQUAL -1 AND quoted MATCHES "^[A-Za-z0-9_]+\\.h$")
                list(FIND internal_headers "${quoted}" internal_index)
                if(NOT internal_index EQUAL -1)
                    set(ok TRUE)
                endif()
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
