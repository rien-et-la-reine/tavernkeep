# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
#
# libFuzzer harnesses, one per entry point (README.md). Built only with BARTAP_BUILD_FUZZERS=ON, under clang.
# The library is built again for them, instrumented for coverage and with AddressSanitizer and
# UndefinedBehaviorSanitizer, so that what the fuzzer explores is the library's own code.

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "BARTAP_BUILD_FUZZERS needs clang (libFuzzer): configure with -DCMAKE_C_COMPILER=clang")
endif()
find_package(ZLIB REQUIRED)

set(BARTAP_FUZZ_OPTIONS -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
    -fno-omit-frame-pointer -g -O1)
set(BARTAP_FUZZ_LINK_OPTIONS -fsanitize=fuzzer,address,undefined)

add_library(bartap_fuzz_lib STATIC ${BARTAP_SOURCES})
target_include_directories(bartap_fuzz_lib PUBLIC ${BARTAP_INCLUDE_DIR} ${BARTAP_PRIVATE_INCLUDE_DIR})
target_compile_definitions(bartap_fuzz_lib PUBLIC ${BARTAP_PUBLIC_DEFINITIONS})
set_target_properties(bartap_fuzz_lib PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(bartap_fuzz_lib PRIVATE ${BARTAP_FUZZ_OPTIONS} ${BARTAP_WARNING_OPTIONS})

set(BARTAP_FUZZERS inflate xml ocf epub)
foreach(name IN LISTS BARTAP_FUZZERS)
    add_executable(fuzz_${name} fuzz_${name}.c)
    target_link_libraries(fuzz_${name} PRIVATE bartap_fuzz_lib)
    set_target_properties(fuzz_${name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(fuzz_${name} PRIVATE ${BARTAP_FUZZ_OPTIONS} ${BARTAP_WARNING_OPTIONS})
    target_link_options(fuzz_${name} PRIVATE ${BARTAP_FUZZ_LINK_OPTIONS})
    # every committed seed and crash, run once: the regression suite
    add_test(NAME fuzz_${name}_corpus
        COMMAND fuzz_${name} -runs=0 ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name})
endforeach()
target_link_libraries(fuzz_inflate PRIVATE ZLIB::ZLIB)
