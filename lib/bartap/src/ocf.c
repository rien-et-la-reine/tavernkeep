// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The ZIP container (PKWARE APPNOTE): the end of central directory record, found by scanning back over
 * the last min(size, 65535 + 22) bytes so trailing padding is tolerated (BARTAP_WARN_TRAILING_BYTES);
 * then the central directory, walked to find entries by name or by FNV-1a hash. An entry's sizes and
 * CRC-32 come from its central directory record, never the local header or a data descriptor. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

bartap_err_t bartap_zip_open(bartap_zip_t *zip, const bartap_source_t *source)
{
    // TODO(rewrite)
    (void)zip;
    (void)source;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_zip_find(const bartap_zip_t *zip, const bartap_source_t *source, const char *path,
    size_t len, bartap_entry_t *entry)
{
    // TODO(rewrite)
    (void)zip;
    (void)source;
    (void)path;
    (void)len;
    (void)entry;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_zip_find_hash(const bartap_zip_t *zip, const bartap_source_t *source, uint32_t hash,
    size_t len, bartap_entry_t *entry)
{
    // TODO(rewrite)
    (void)zip;
    (void)source;
    (void)hash;
    (void)len;
    (void)entry;
    return BARTAP_ERR_UNSUPPORTED;
}
