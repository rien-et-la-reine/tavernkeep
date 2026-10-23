# Host tests for the EPUB container layer (src/epub/: byte_source.c, zip.c, inflate.c), over inputs that
# make_fixtures.py generates into the build directory and the committed fixtures in fixtures/.
# Standalone, like tests/filesystem; see README.md.
#
#   cmake -S tests/epub -B build-epub-tests && cmake --build build-epub-tests
#   ./build-epub-tests/epub_tests
cmake_minimum_required(VERSION 3.13)
project(tavernkeep_epub_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(TAVERNKEEP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

option(EPUB_TESTS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The generated inputs live in the build directory, never in the source tree, and are remade whenever
# make_fixtures.py changes. The script clears only the directories it made itself.
set(EPUB_TESTS_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(EPUB_TESTS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT "${EPUB_TESTS_GENERATED_DIR}/manifest.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py"
        --out "${EPUB_TESTS_GENERATED_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py"
    COMMENT "Generating EPUB test inputs"
    VERBATIM
)
add_custom_target(epub_fixtures DEPENDS "${EPUB_TESTS_GENERATED_DIR}/manifest.json")

set(EPUB_TESTS_OWN_SOURCES
    test_main.c
    test_scaffold.c
    ${TAVERNKEEP_ROOT}/src/epub/byte_source.c
    ${TAVERNKEEP_ROOT}/src/epub/zip.c
    ${TAVERNKEEP_ROOT}/src/epub/inflate.c
)

add_executable(epub_tests ${EPUB_TESTS_OWN_SOURCES})
add_dependencies(epub_tests epub_fixtures)

target_include_directories(epub_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TAVERNKEEP_ROOT}/src
)
target_compile_definitions(epub_tests PRIVATE
    EPUB_TESTS_FIXTURE_DIR="${EPUB_TESTS_FIXTURE_DIR}"
    EPUB_TESTS_GENERATED_DIR="${EPUB_TESTS_GENERATED_DIR}"
)

# Warnings are errors for the code under test and the tests, as a test that compiles with a warning is
# one nobody has read closely.
set_source_files_properties(${EPUB_TESTS_OWN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic;-Werror")

if(EPUB_TESTS_SANITIZE)
    target_compile_options(epub_tests PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer)
    target_link_options(epub_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME epub COMMAND epub_tests)
add_test(NAME epub_shuffled COMMAND epub_tests --shuffle 20261023)
add_test(NAME harness_selftest COMMAND epub_tests --selftest)
