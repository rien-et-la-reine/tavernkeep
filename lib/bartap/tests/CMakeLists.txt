# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
#
# Host tests for Bartap, over inputs that make_fixtures.py generates into the build directory and the
# committed fixtures in fixtures/. Built from the library's own CMakeLists.txt when BARTAP_BUILD_TESTS
# is on (the default when Bartap is the top-level project); see README.md.
#
# Also checks the library's portability rules mechanically: which headers it includes
# (check_includes.cmake), and that it has no writable static data and calls nothing outside the
# allowed C library functions (check_symbols.cmake).

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
add_custom_target(bartap_fixtures DEPENDS "${BARTAP_TESTS_GENERATED_DIR}/manifest.json")

set(BARTAP_TESTS_SOURCES
    test_main.c
    test_scaffold.c
)

add_executable(bartap_tests ${BARTAP_TESTS_SOURCES})
add_dependencies(bartap_tests bartap_fixtures)
target_link_libraries(bartap_tests PRIVATE bartap)
target_include_directories(bartap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(bartap_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(bartap_tests PRIVATE
    BARTAP_TESTS_FIXTURE_DIR="${BARTAP_TESTS_FIXTURE_DIR}"
    BARTAP_TESTS_GENERATED_DIR="${BARTAP_TESTS_GENERATED_DIR}"
)
# A test that compiles with a warning is one nobody has read closely
target_compile_options(bartap_tests PRIVATE ${BARTAP_WARNING_OPTIONS})

# Sanitizers and -m32 go on the library as well as the tests, so the library's own code is what they
# check. The library's symbol check builds its own uninstrumented copy (below).
if(BARTAP_TESTS_SANITIZE)
    foreach(target bartap bartap_tests)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all
            -fno-omit-frame-pointer)
    endforeach()
    target_link_options(bartap_tests PRIVATE -fsanitize=address,undefined)
endif()
if(BARTAP_TESTS_M32)
    foreach(target bartap bartap_tests)
        target_compile_options(${target} PRIVATE -m32)
    endforeach()
    target_link_options(bartap_tests PRIVATE -m32)
endif()

add_test(NAME bartap COMMAND bartap_tests)
add_test(NAME bartap_shuffled COMMAND bartap_tests --shuffle 20261023)
add_test(NAME harness_selftest COMMAND bartap_tests --selftest)

# ------------------------------------------------------------------ portability checks

# Every header the library's own files include must be one of the four it may use, or one of its own
add_test(NAME include_check
    COMMAND ${CMAKE_COMMAND}
        "-DDIRS=${BARTAP_INCLUDE_DIR};${PROJECT_SOURCE_DIR}/src"
        -DEXPECT_VIOLATIONS=0
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_includes.cmake)
# ...and the check must catch what it is meant to: one file holding each kind of violation
add_test(NAME include_check_selftest
    COMMAND ${CMAKE_COMMAND}
        "-DDIRS=${CMAKE_CURRENT_SOURCE_DIR}/check_selftest/includes"
        -DEXPECT_VIOLATIONS=5
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_includes.cmake)

# An uninstrumented copy of the library, as a firmware would build it, so that sanitizer runtime calls
# and data do not show up in its symbols. Same sources, flags and word size as the library under test.
add_library(bartap_symbol_check STATIC EXCLUDE_FROM_ALL ${BARTAP_SOURCES})
target_include_directories(bartap_symbol_check PRIVATE ${BARTAP_INCLUDE_DIR})
set_target_properties(bartap_symbol_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(bartap_symbol_check PRIVATE ${BARTAP_WARNING_OPTIONS})
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

if(CMAKE_NM)
    add_test(NAME symbol_check
        COMMAND ${CMAKE_COMMAND}
            -DNM=${CMAKE_NM}
            -DLIBRARY=$<TARGET_FILE:bartap_symbol_check>
            "-DOBJECTS=byte_source.c;zip.c;inflate.c"
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
