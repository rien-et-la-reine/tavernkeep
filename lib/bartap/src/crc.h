// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_CRC_H
#define BARTAP_CRC_H

/* CRC-32 (internal): zip's checksum (APPNOTE.TXT 4.4.7), also used to check text-layer snapshots.
 * Reflected, polynomial 0xEDB88320, from a 1 KiB read-only table (Q12). Start from BARTAP_CRC32_INIT
 * and finish with bartap_crc32_final. */

#include <stddef.h>
#include <stdint.h>

#define BARTAP_CRC32_INIT 0xFFFFFFFFUL

uint32_t bartap_crc32_update(uint32_t crc, const uint8_t *data, size_t size);

static inline uint32_t bartap_crc32_final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFUL;
}

#endif
