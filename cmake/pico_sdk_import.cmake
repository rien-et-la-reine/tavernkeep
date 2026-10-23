# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT

# Locate an existing Pico SDK without downloading or hard-coding a host path.
# The SDK's supported importer performs the actual CMake integration.

if((NOT DEFINED PICO_SDK_PATH OR PICO_SDK_PATH STREQUAL "")
        AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()

if(NOT DEFINED PICO_SDK_PATH OR PICO_SDK_PATH STREQUAL "")
    message(FATAL_ERROR
        "PICO_SDK_PATH is not set. Point it at a local Raspberry Pi Pico SDK "
        "checkout, either in the environment or with -DPICO_SDK_PATH=<path>.")
endif()

get_filename_component(
    PICO_SDK_PATH
    "${PICO_SDK_PATH}"
    REALPATH
    BASE_DIR "${CMAKE_BINARY_DIR}"
)
set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH
    "Path to the Raspberry Pi Pico SDK" FORCE)

set(_tavernkeep_pico_import
    "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

if(NOT EXISTS "${_tavernkeep_pico_import}")
    message(FATAL_ERROR
        "${PICO_SDK_PATH} does not look like a Pico SDK checkout: "
        "external/pico_sdk_import.cmake was not found.")
endif()

include("${_tavernkeep_pico_import}")

