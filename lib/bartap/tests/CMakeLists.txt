# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
#
# Host tests for Bartap, over inputs that make_fixtures.py generates into the build directory and the
# committed fixtures in fixtures/. Built from the library's own CMakeLists.txt when BARTAP_BUILD_TESTS
# is on (the default when Bartap is the top-level project); see README.md.
#
# Also checks the library's portability rules mechanically: which headers it includes
# (check_includes.cmake), and that it has no writable static data and calls nothing outside the
# allowed C library functions (check_symbols.cmake), headers included.

option(BARTAP_TESTS_SANITIZE "Build the library and its tests with AddressSanitizer and UndefinedBehaviorSanitizer" ON)
option(BARTAP_TESTS_M32 "Build the library and its tests as 32-bit code (-m32)" OFF)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The generated inputs live in the build directory, never in the source tree, and are remade whenever
# make_fixtures.py changes. The script clears only the directories it made itself.
set(BARTAP_TESTS_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(BARTAP_TESTS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT "${BARTAP_TESTS_GENERATED_DIR}/manifest.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py"
        --out "${BARTAP_TESTS_GENERATED_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py"
    COMMENT "Generating Bartap test inputs"
    VERBATIM
)
# The books for the book-level cases and the fuzz seeds, beside (not inside) the generated
# inputs, since make_fixtures.py refuses a directory holding anything it did not make
set(BARTAP_TESTS_BOOKS_DIR "${CMAKE_CURRENT_BINARY_DIR}/books")
add_custom_command(
    OUTPUT "${BARTAP_TESTS_BOOKS_DIR}/books.json"
    COMMAND "${Python3_EXECUTABLE}" -W ignore "${CMAKE_CURRENT_SOURCE_DIR}/make_books.py"
        --out "${BARTAP_TESTS_BOOKS_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_books.py"
    COMMENT "Generating Bartap test books"
    VERBATIM
)
add_custom_target(bartap_fixtures DEPENDS "${BARTAP_TESTS_GENERATED_DIR}/manifest.json"
    "${BARTAP_TESTS_BOOKS_DIR}/books.json")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fixture_tables.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture_tables.py"
        "${BARTAP_TESTS_GENERATED_DIR}/manifest.json" "${CMAKE_CURRENT_BINARY_DIR}/fixture_tables.h"
        "${BARTAP_TESTS_BOOKS_DIR}/books.json"
    DEPENDS "${BARTAP_TESTS_GENERATED_DIR}/manifest.json" "${BARTAP_TESTS_BOOKS_DIR}/books.json" fixture_tables.py
    VERBATIM
)

set(BARTAP_TESTS_SOURCES
    test_main.c
    test_headers.c
    test_text.c
    test_xml.c
    test_fixtures.c
    "${CMAKE_CURRENT_BINARY_DIR}/fixture_tables.h"
)

add_executable(bartap_tests ${BARTAP_TESTS_SOURCES})
add_dependencies(bartap_tests bartap_fixtures)
target_link_libraries(bartap_tests PRIVATE bartap)
# The tests may include the library's internal header as well as its public one
target_include_directories(bartap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${BARTAP_PRIVATE_INCLUDE_DIR})
set_target_properties(bartap_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(bartap_tests PRIVATE
    BARTAP_TESTS_FIXTURE_DIR="${BARTAP_TESTS_FIXTURE_DIR}"
    BARTAP_TESTS_GENERATED_DIR="${BARTAP_TESTS_GENERATED_DIR}"
    BARTAP_TESTS_BOOKS_DIR="${BARTAP_TESTS_BOOKS_DIR}"
)

# A test that compiles with a warning is one nobody has read closely
target_compile_options(bartap_tests PRIVATE ${BARTAP_WARNING_OPTIONS})

# Sanitizers and -m32 go on the library as well as the tests, so the library's own code is what they
# check. The library's symbol check builds its own uninstrumented copy (below).
set(BARTAP_TEST_TARGETS bartap bartap_tests)
set(BARTAP_TEST_EXECUTABLES bartap_tests)
if(BARTAP_TESTS_SANITIZE)
    foreach(target IN LISTS BARTAP_TEST_TARGETS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all
            -fno-omit-frame-pointer)
    endforeach()
    foreach(target IN LISTS BARTAP_TEST_EXECUTABLES)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
if(BARTAP_TESTS_M32)
    foreach(target IN LISTS BARTAP_TEST_TARGETS)
        target_compile_options(${target} PRIVATE -m32)
    endforeach()
    foreach(target IN LISTS BARTAP_TEST_EXECUTABLES)
        target_link_options(${target} PRIVATE -m32)
    endforeach()
endif()

add_test(NAME bartap COMMAND bartap_tests)
add_test(NAME bartap_shuffled COMMAND bartap_tests --shuffle 20261023)
add_test(NAME harness_selftest COMMAND bartap_tests --selftest)


# ------------------------------------------------------------------ portability checks

# Every header the library's own files include must be one of the four it may use, or one of its own:
# bartap.h anywhere, bartap_internal.h only from src/
add_test(NAME include_check
    COMMAND ${CMAKE_COMMAND}
        -DPUBLIC_DIRS=${BARTAP_INCLUDE_DIR}
        -DPRIVATE_DIRS=${BARTAP_PRIVATE_INCLUDE_DIR}
        -DEXPECT_VIOLATIONS=0
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_includes.cmake)
# ...and the check must catch what it is meant to: planted files holding each kind of violation
add_test(NAME include_check_selftest
    COMMAND ${CMAKE_COMMAND}
        -DPUBLIC_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/check_selftest/includes/public
        -DPRIVATE_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/check_selftest/includes/private
        -DEXPECT_VIOLATIONS=7
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_includes.cmake)

# One translation unit per header, public and internal, holding its #include and a typedef, which
# defines no symbol but keeps the unit from being empty (ISO C forbids an empty translation unit, and
# -Wpedantic says so) for a header that declares nothing yet, such as epub.h. Compiled into the
# symbol check's library, so that anything a header defines (a function, writable data) is checked
# like the sources, and each header is shown to compile on its own. An unused static function or
# variable in a header is an error here, since the optimiser would otherwise remove it unseen; an unused
# static inline function is allowed, as GCC and Clang never warn about one.
set(BARTAP_HEADER_UNIT_DIR "${CMAKE_CURRENT_BINARY_DIR}/header_units")
set(BARTAP_HEADER_UNITS)
set(BARTAP_HEADER_UNIT_OBJECTS)
file(GLOB bartap_public_headers CONFIGURE_DEPENDS "${BARTAP_INCLUDE_DIR}/*.h")
file(GLOB bartap_private_headers CONFIGURE_DEPENDS "${BARTAP_PRIVATE_INCLUDE_DIR}/*.h")
foreach(header IN LISTS bartap_public_headers bartap_private_headers)
    get_filename_component(stem "${header}" NAME_WE)
    set(include "${stem}.h")
    set(unit "${BARTAP_HEADER_UNIT_DIR}/header_${stem}.c")
    # written through configure_file so an unchanged unit keeps its timestamp
    file(WRITE "${unit}.in" "#include \"${include}\"\ntypedef int bartap_header_unit_${stem};\n")
    configure_file("${unit}.in" "${unit}" COPYONLY)
    list(APPEND BARTAP_HEADER_UNITS "${unit}")
    list(APPEND BARTAP_HEADER_UNIT_OBJECTS "header_${stem}.c")
endforeach()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(BARTAP_HEADER_UNIT_OPTIONS -Werror=unused-function -Werror=unused-variable)
    set_source_files_properties(${BARTAP_HEADER_UNITS} PROPERTIES
        COMPILE_OPTIONS "${BARTAP_HEADER_UNIT_OPTIONS}")
endif()

# An uninstrumented copy of the library, as a firmware would build it, so that sanitizer runtime calls
# and data do not show up in its symbols. Same sources, flags and word size as the library under test,
# plus the header units.
add_library(bartap_symbol_check STATIC EXCLUDE_FROM_ALL ${BARTAP_SOURCES} ${BARTAP_HEADER_UNITS})
target_include_directories(bartap_symbol_check PRIVATE ${BARTAP_INCLUDE_DIR} ${BARTAP_PRIVATE_INCLUDE_DIR})
set_target_properties(bartap_symbol_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(bartap_symbol_check PRIVATE ${BARTAP_WARNING_OPTIONS})
target_compile_definitions(bartap_symbol_check PRIVATE ${BARTAP_PUBLIC_DEFINITIONS})
# A library that breaks both rules, to prove the check notices: one writable static and one call to
# malloc
add_library(bartap_symbol_check_selftest STATIC EXCLUDE_FROM_ALL check_selftest/symbols/violations.c)
set_target_properties(bartap_symbol_check_selftest PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
if(BARTAP_TESTS_M32)
    target_compile_options(bartap_symbol_check PRIVATE -m32)
    target_compile_options(bartap_symbol_check_selftest PRIVATE -m32)
endif()
# Built with the tests, though nothing links them
add_dependencies(bartap_tests bartap_symbol_check bartap_symbol_check_selftest)

# ...and the header units must fail on what they are meant to: one planted header with an unused static
# function, one with an unused static variable, each compiled as a header unit would be. Built only by
# these tests, which pass when the build fails. Clang never reports an unused static variable declared
# in a header (as of clang 21, not even with -Weverything), so that one is checked under GCC only; the
# compiler matrix builds with both
if(BARTAP_HEADER_UNIT_OPTIONS)
    set(planted_headers unused_function)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        list(APPEND planted_headers unused_variable)
    endif()
    foreach(planted IN LISTS planted_headers)
        set(unit "${BARTAP_HEADER_UNIT_DIR}/selftest_${planted}.c")
        file(WRITE "${unit}.in" "#include \"${planted}.h\"\n")
        configure_file("${unit}.in" "${unit}" COPYONLY)
        add_library(bartap_header_unit_selftest_${planted} OBJECT EXCLUDE_FROM_ALL "${unit}")
        target_include_directories(bartap_header_unit_selftest_${planted} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/check_selftest/headers)
        set_target_properties(bartap_header_unit_selftest_${planted} PROPERTIES C_STANDARD 11
            C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        target_compile_options(bartap_header_unit_selftest_${planted} PRIVATE ${BARTAP_HEADER_UNIT_OPTIONS})
        add_test(NAME header_unit_selftest_${planted}
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                --target bartap_header_unit_selftest_${planted})
        set_tests_properties(header_unit_selftest_${planted} PROPERTIES WILL_FAIL TRUE)
    endforeach()
endif()

if(CMAKE_NM)
    add_test(NAME symbol_check
        COMMAND ${CMAKE_COMMAND}
            -DNM=${CMAKE_NM}
            -DLIBRARY=$<TARGET_FILE:bartap_symbol_check>
            "-DOBJECTS=${BARTAP_OBJECTS};${BARTAP_HEADER_UNIT_OBJECTS}"
            -DEXPECT_VIOLATIONS=0
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.cmake)
    add_test(NAME symbol_check_selftest
        COMMAND ${CMAKE_COMMAND}
            -DNM=${CMAKE_NM}
            -DLIBRARY=$<TARGET_FILE:bartap_symbol_check_selftest>
            -DOBJECTS=violations.c
            -DEXPECT_VIOLATIONS=2
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.cmake)
else()
    message(WARNING "No nm found (CMAKE_NM): the symbol check will not run")
endif()
