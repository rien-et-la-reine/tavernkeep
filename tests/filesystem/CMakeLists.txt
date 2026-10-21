# Host tests for the storage layer above the SD driver (src/storage/filesystem.c, diskio.c, storage.c, save_point.c), running the real
# FatFs over a RAM-backed fake block device. Standalone for now; see README.md.
#
#   cmake -S tests/filesystem -B build-fs-tests && cmake --build build-fs-tests
#   ./build-fs-tests/fs_tests
cmake_minimum_required(VERSION 3.13)
project(tavernkeep_filesystem_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(TAVERNKEEP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

option(FS_TESTS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

# The code under test. mutate.py points these at mutated copies; nothing else should need to.
set(FS_TESTS_FILESYSTEM_SOURCE "${TAVERNKEEP_ROOT}/src/storage/filesystem.c" CACHE FILEPATH
    "filesystem.c to test")
set(FS_TESTS_DISKIO_SOURCE "${TAVERNKEEP_ROOT}/src/storage/diskio.c" CACHE FILEPATH
    "diskio.c to test")
set(FS_TESTS_STORAGE_SOURCE "${TAVERNKEEP_ROOT}/src/storage/storage.c" CACHE FILEPATH
    "storage.c to test")
set(FS_TESTS_SAVE_POINT_SOURCE "${TAVERNKEEP_ROOT}/src/storage/save_point.c" CACHE FILEPATH
    "save_point.c to test")

set(FS_TESTS_OWN_SOURCES
    test_main.c
    test_diskio.c
    test_filesystem.c
    test_storage.c
    test_save_point.c
    fake_block_device.c
    fake_platform.c
    ${FS_TESTS_FILESYSTEM_SOURCE}
    ${FS_TESTS_DISKIO_SOURCE}
    ${FS_TESTS_STORAGE_SOURCE}
    ${FS_TESTS_SAVE_POINT_SOURCE}
    ${TAVERNKEEP_ROOT}/src/storage/sd_crc.c
)
set(FS_TESTS_FATFS_SOURCES
    ${TAVERNKEEP_ROOT}/third_party/fatfs/source/ff.c
    ${TAVERNKEEP_ROOT}/third_party/fatfs/source/ffunicode.c
)

add_executable(fs_tests ${FS_TESTS_OWN_SOURCES} ${FS_TESTS_FATFS_SOURCES})

target_include_directories(fs_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TAVERNKEEP_ROOT}/src
    ${TAVERNKEEP_ROOT}/third_party/fatfs/source
)
# FF_USE_MKFS: the firmware has no format (ffconf.h leaves it 0); the tests build f_mkfs only to create
# the volumes they run on (fixture_format)
target_compile_definitions(fs_tests PRIVATE TAVERNKEEP_LOGGING_ENABLED=1 FF_USE_MKFS=1)

# Warnings are errors for the code under test and the tests, as a test that compiles with a warning is
# one nobody has read closely. FatFs is third-party and left alone.
set_source_files_properties(${FS_TESTS_OWN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic;-Werror")

if(FS_TESTS_SANITIZE)
    target_compile_options(fs_tests PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer)
    target_link_options(fs_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME filesystem COMMAND fs_tests)
add_test(NAME filesystem_shuffled COMMAND fs_tests --shuffle 20261016)
add_test(NAME harness_selftest COMMAND fs_tests --selftest)
