// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* CRC-32 as ZIP uses it (PKWARE APPNOTE 4.4.7): the reflected polynomial 0xEDB88320, initial value and
 * final XOR 0xFFFFFFFF. Every entry's uncompressed bytes are checked against the CRC-32 its central
 * directory record gives. */

#include <stddef.h>
#include <stdint.h>

#include "bartap_internal.h"

uint32_t bartap_crc32(uint32_t crc, const void *data, size_t len)
{
    // TODO(rewrite)
    (void)data;
    (void)len;
    return crc;
}
