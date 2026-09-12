cmake_minimum_required(VERSION 3.16)

project(tavernkeep_host_tests LANGUAGES C)

enable_testing()

# ---------------------------------------------------------------- options

option(TAVERNKEEP_TEST_COVERAGE "Instrument host tests with GCC coverage" OFF)
option(TAVERNKEEP_TEST_KNOWN_GAPS "Run regressions for documented production gaps" OFF)
option(TAVERNKEEP_TEST_SANITIZE "Build host tests with ASan and UBSan" OFF)
option(TAVERNKEEP_TEST_STRICT_WARNINGS "Treat warnings as errors" OFF)
set(TAVERNKEEP_TEST_SEED "" CACHE STRING
    "Seed for the randomised property suite; empty uses its built-in default")

# The harness: SDK peripheral fakes, the simulated clock and the SD card model.
set(TAVERNKEEP_HARNESS_SOURCES
    support/pico_mock.c
    support/sd_card_model.c
    support/sim_clock.c
)
# Fixture and assertion helpers used by the adversarial suites.
set(TAVERNKEEP_FIXTURE_SOURCES
    support/sd_fixture.c
    support/test_harness.c
)

# ----------------------------------------------------------- target setup

function(tavernkeep_configure_target name)
    target_include_directories(${name} PRIVATE fakes support ../src)
    set_target_properties(${name} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
        if(TAVERNKEEP_TEST_STRICT_WARNINGS)
            target_compile_options(${name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${name} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wconversion -Wsign-conversion
            -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes
            -Wpointer-arith -Wwrite-strings -Wundef
            -Wdouble-promotion -Wformat=2)
        if(TAVERNKEEP_TEST_STRICT_WARNINGS)
            target_compile_options(${name} PRIVATE -Werror)
        endif()
        if(TAVERNKEEP_TEST_SANITIZE)
            target_compile_options(${name} PRIVATE
                -fsanitize=address,undefined
                -fno-sanitize-recover=all
                -fno-omit-frame-pointer -g)
            target_link_options(${name} PRIVATE
                -fsanitize=address,undefined)
        endif()
    endif()
    if(TAVERNKEEP_TEST_COVERAGE)
        if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
            message(FATAL_ERROR "TAVERNKEEP_TEST_COVERAGE requires GCC/gcov")
        endif()
        target_compile_options(${name} PRIVATE --coverage -O0 -g)
        target_link_options(${name} PRIVATE --coverage)
    endif()
endfunction()

function(add_host_test name)
    add_executable(${name} ${ARGN})
    tavernkeep_configure_target(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# ------------------------------------------------------------- SD suites

# The original SD suite, now running against the card model. It is registered
# twice, once under each clock model: a case that only passes with poll-tick
# time is depending on the legacy artifact rather than on elapsed time.
add_executable(sd_spi_host_tests
    test_sd_spi.c
    support/gpio_irq_mock.c
    ${TAVERNKEEP_HARNESS_SOURCES}
    ../src/storage/sd_spi.c
    ../src/storage/sd_crc.c
)
tavernkeep_configure_target(sd_spi_host_tests)
add_test(NAME sd_spi_host_tests
    COMMAND sd_spi_host_tests --clock=bus-time)
add_test(NAME sd_spi_polltick_host_tests
    COMMAND sd_spi_host_tests --clock=poll-tick)

# Regressions for the two production defects fixed during the test overhaul.
# They were registered as disabled known-gap cases before the fixes; they are
# part of the enabled suite now so a reintroduction fails the default run.
foreach(regression IN ITEMS single-release multi-release repeated-teardown)
    add_test(NAME sd_removal_${regression}_host_tests
        COMMAND sd_spi_host_tests --gap-${regression} --clock=bus-time)
endforeach()

add_host_test(sd_protocol_host_tests test_sd_protocol.c
    support/gpio_irq_mock.c
    ${TAVERNKEEP_HARNESS_SOURCES} ${TAVERNKEEP_FIXTURE_SOURCES}
    ../src/storage/sd_spi.c
    ../src/storage/sd_crc.c)

add_host_test(sd_faults_host_tests test_sd_faults.c
    support/gpio_irq_mock.c
    ${TAVERNKEEP_HARNESS_SOURCES} ${TAVERNKEEP_FIXTURE_SOURCES}
    ../src/storage/sd_spi.c
    ../src/storage/sd_crc.c)

# The write path: CMD24/CMD25 against the card model, which validates the
# data CRC16, the start-block token each command requires, the N_WR idle byte
# and keeps a CMD25 transfer open across programming busy. Includes the
# former SD-007 acceptance cases, promoted out of the known-gap label.
add_host_test(sd_writes_host_tests test_sd_writes.c
    support/gpio_irq_mock.c
    ${TAVERNKEEP_HARNESS_SOURCES} ${TAVERNKEEP_FIXTURE_SOURCES}
    ../src/storage/sd_spi.c
    ../src/storage/sd_crc.c)

add_executable(sd_property_host_tests test_sd_property.c
    support/gpio_irq_mock.c
    ${TAVERNKEEP_HARNESS_SOURCES} ${TAVERNKEEP_FIXTURE_SOURCES}
    ../src/storage/sd_spi.c
    ../src/storage/sd_crc.c)
tavernkeep_configure_target(sd_property_host_tests)
if(TAVERNKEEP_TEST_SEED STREQUAL "")
    add_test(NAME sd_property_host_tests COMMAND sd_property_host_tests)
else()
    add_test(NAME sd_property_host_tests
        COMMAND sd_property_host_tests --seed ${TAVERNKEEP_TEST_SEED})
endif()

# The CRC7 helper on its own: specification vectors plus a differential
# cross-check against the card model's independent implementation. It needs no
# bus, so it links only the model (for sd_crc7 and the trace stubs the harness
# references), the simulated clock and the assertion helpers.
add_executable(sd_crc_host_tests test_sd_crc.c
    support/sd_card_model.c
    support/sim_clock.c
    support/test_harness.c
    ../src/storage/sd_crc.c)
tavernkeep_configure_target(sd_crc_host_tests)
if(TAVERNKEEP_TEST_SEED STREQUAL "")
    add_test(NAME sd_crc_host_tests COMMAND sd_crc_host_tests)
else()
    add_test(NAME sd_crc_host_tests
        COMMAND sd_crc_host_tests --seed ${TAVERNKEEP_TEST_SEED})
endif()

# The CRC16 helper: the CRC-16/XMODEM parameterisation the SD data CRC uses,
# checked against the published catalogue value, against the card model's
# independent implementation, and for the residue property a receiver relies on.
add_executable(sd_crc16_host_tests test_sd_crc16.c
    support/sd_card_model.c
    support/sim_clock.c
    support/test_harness.c
    ../src/storage/sd_crc.c)
tavernkeep_configure_target(sd_crc16_host_tests)
if(TAVERNKEEP_TEST_SEED STREQUAL "")
    add_test(NAME sd_crc16_host_tests COMMAND sd_crc16_host_tests)
else()
    add_test(NAME sd_crc16_host_tests
        COMMAND sd_crc16_host_tests --seed ${TAVERNKEEP_TEST_SEED})
endif()

# ------------------------------------------------------- platform suites

add_host_test(gpio_irq_host_tests test_gpio_irq.c
    support/gpio_irq_hardware_mock.c
    ../src/platform/gpio_irq.c)

add_host_test(block_device_host_tests test_block_device.c)
add_host_test(filesystem_host_tests test_filesystem.c ../src/storage/filesystem.c)

add_host_test(sd_irq_integration_host_tests test_sd_irq_integration.c
    ${TAVERNKEEP_HARNESS_SOURCES} support/gpio_irq_hardware_mock.c
    support/gpio_irq_integration_bridge.c
    ../src/storage/sd_spi.c ../src/storage/sd_crc.c ../src/platform/gpio_irq.c)

add_host_test(main_host_tests test_main.c support/main_under_test.c)
foreach(mode IN ITEMS no-led debug-failure irq-failure)
    add_test(NAME main_${mode}_host_tests COMMAND main_host_tests ${mode})
endforeach()

foreach(variant IN ITEMS absent normal inverted explicit_noninverted)
    set(target board_${variant}_host_tests)
    add_host_test(${target} test_board.c ../src/platform/board.c)
    if(NOT variant STREQUAL "absent")
        target_compile_definitions(${target} PRIVATE PICO_DEFAULT_LED_PIN=25)
    endif()
    if(variant STREQUAL "inverted")
        target_compile_definitions(${target} PRIVATE PICO_DEFAULT_LED_PIN_INVERTED=1)
    elseif(variant STREQUAL "explicit_noninverted")
        target_compile_definitions(${target} PRIVATE PICO_DEFAULT_LED_PIN_INVERTED=0)
    endif()
endforeach()

foreach(enabled IN ITEMS 0 1)
    set(target debug_${enabled}_host_tests)
    add_host_test(${target} test_debug.c support/debug_under_test.c)
    target_compile_definitions(${target} PRIVATE TAVERNKEEP_LOGGING_ENABLED=${enabled})
endforeach()

# -------------------------------------------------------- known-gap cases

# These assert the behaviour the driver should have and fail against the
# current source. They are visible in CTest but disabled by default so a green
# default run is never mistaken for "no known gaps". See KNOWN_GAPS.md.
add_executable(sd_gaps_host_tests test_sd_gaps.c
    support/gpio_irq_mock.c
    ${TAVERNKEEP_HARNESS_SOURCES} ${TAVERNKEEP_FIXTURE_SOURCES}
    ../src/storage/sd_spi.c
    ../src/storage/sd_crc.c)
tavernkeep_configure_target(sd_gaps_host_tests)

foreach(gap IN ITEMS stop-residual r1-tolerance command-crc)
    add_test(NAME sd_gap_${gap} COMMAND sd_gaps_host_tests --gap-${gap})
    set_tests_properties(sd_gap_${gap} PROPERTIES TIMEOUT 60 LABELS "known-gap")
    if(NOT TAVERNKEEP_TEST_KNOWN_GAPS)
        set_tests_properties(sd_gap_${gap} PROPERTIES DISABLED TRUE)
    endif()
endforeach()

# ------------------------------------------------------------ test labels

# Sanitizer builds are slower; give every case room without hiding a hang.
if(TAVERNKEEP_TEST_SANITIZE)
    set(tavernkeep_test_timeout 300)
else()
    set(tavernkeep_test_timeout 120)
endif()

get_property(all_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS all_tests)
    get_test_property(${test} LABELS existing_labels)
    if(NOT existing_labels)
        set_tests_properties(${test} PROPERTIES
            TIMEOUT ${tavernkeep_test_timeout} LABELS "host")
    endif()
endforeach()
