# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13...3.31)

# These cache defaults may be overridden at configure time for future boards.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2 CACHE STRING "Pico SDK board target")
endif()

if(NOT DEFINED PICO_PLATFORM)
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico SDK processor platform")
endif()

# The Pico SDK must be imported before project().
include(cmake/pico_sdk_import.cmake)

project(tavernkeep VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.0.0")
    message(FATAL_ERROR
        "Tavernkeep requires Pico SDK 2.0.0 or newer for RP2350 support; "
        "found ${PICO_SDK_VERSION_STRING}")
endif()

option(TAVERNKEEP_ENABLE_LOGGING "Enable firmware log output" ON)
option(TAVERNKEEP_STDIO_UART "Enable Pico SDK UART stdio" OFF)
option(TAVERNKEEP_STDIO_USB "Enable Pico SDK USB CDC stdio" OFF)
option(TAVERNKEEP_STDIO_RTT "Enable Pico SDK RTT stdio" ON)

set(TAVERNKEEP_SOURCES
    src/main.c
    src/platform/board.c
    src/platform/debug.c
    src/platform/gpio_irq.c
    src/storage/filesystem.c
    src/storage/sd_spi.c
    src/storage/sd_crc.c
)

add_executable(tavernkeep ${TAVERNKEEP_SOURCES})

pico_set_program_name(tavernkeep "Tavernkeep")
pico_set_program_version(tavernkeep "${PROJECT_VERSION}")

target_include_directories(tavernkeep PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(tavernkeep PRIVATE
    pico_stdlib
    hardware_gpio
    hardware_spi
)

if(TAVERNKEEP_ENABLE_LOGGING)
    target_compile_definitions(tavernkeep PRIVATE TAVERNKEEP_LOGGING_ENABLED=1)
else()
    target_compile_definitions(tavernkeep PRIVATE TAVERNKEEP_LOGGING_ENABLED=0)
endif()

if(TAVERNKEEP_STDIO_UART)
    pico_enable_stdio_uart(tavernkeep 1)
else()
    pico_enable_stdio_uart(tavernkeep 0)
endif()

if(TAVERNKEEP_STDIO_USB)
    pico_enable_stdio_usb(tavernkeep 1)
else()
    pico_enable_stdio_usb(tavernkeep 0)
endif()

if(TAVERNKEEP_STDIO_RTT)
    pico_enable_stdio_rtt(tavernkeep 1)
else()
    pico_enable_stdio_rtt(tavernkeep 0)
endif()

# Pico SDK sources are added to this target internally, so target-wide warning
# flags would also affect the SDK. Apply them only to Tavernkeep-owned sources.
set_source_files_properties(${TAVERNKEEP_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic"
)

pico_add_extra_outputs(tavernkeep)
