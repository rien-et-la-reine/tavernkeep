# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
#
# Bartap: an allocation-free, streaming, platform-agnostic EPUB container and package library for small
# devices. See README.md.
#
# Standalone, from this directory:
#   cmake -S . -B build && cmake --build build && (cd build && ctest)
# In another project:
#   add_subdirectory(path/to/bartap) and target_link_libraries(... bartap)
cmake_minimum_required(VERSION 3.13)
project(bartap VERSION 0.0.0 LANGUAGES C)

# PROJECT_IS_TOP_LEVEL needs CMake 3.21
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(BARTAP_TOP_LEVEL ON)
else()
    set(BARTAP_TOP_LEVEL OFF)
endif()

# Off by default inside another project: the tests need a hosted C compiler and Python 3, which a
# cross-compiled firmware build has neither of
option(BARTAP_BUILD_TESTS "Build Bartap's host tests" ${BARTAP_TOP_LEVEL})
option(BARTAP_WARNINGS_AS_ERRORS "Treat warnings in Bartap's own sources as errors" ${BARTAP_TOP_LEVEL})

set(BARTAP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/byte_source.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/zip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/inflate.c
)
set(BARTAP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(BARTAP_WARNING_OPTIONS -Wall -Wextra -Wpedantic)
    if(BARTAP_WARNINGS_AS_ERRORS)
        list(APPEND BARTAP_WARNING_OPTIONS -Werror)
    endif()
endif()

add_library(bartap STATIC ${BARTAP_SOURCES})
target_include_directories(bartap PUBLIC ${BARTAP_INCLUDE_DIR})
# Plain ISO C, whatever the including project uses: no compiler extensions to come to depend on
set_target_properties(bartap PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(bartap PRIVATE ${BARTAP_WARNING_OPTIONS})

if(BARTAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
