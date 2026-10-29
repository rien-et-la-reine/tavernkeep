// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* CRC-32 as ZIP uses it (PKWARE APPNOTE 4.4.7): the reflected polynomial 0xEDB88320, initial value and
 * final XOR 0xFFFFFFFF. Every entry's uncompressed bytes are checked against the CRC-32 its central
 * directory record gives. */

#include <stddef.h>
#include <stdint.h>

#include "bartap_internal.h"

static const uint32_t bartap_crc32_table[16] = {
    0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
    0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
    0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
    0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL,
};

uint32_t bartap_crc32(uint32_t crc, const void *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return crc;
    }
    const uint8_t *const bytes = data;
    uint32_t state = ~crc;
    for (size_t i = 0; i < len; i++) {
        state = (state >> 4U) ^ bartap_crc32_table[(state ^ bytes[i]) & 0x0FU];
        state = (state >> 4U) ^ bartap_crc32_table[(state ^ (bytes[i] >> 4U)) & 0x0FU];
    }
    return ~state;
}
