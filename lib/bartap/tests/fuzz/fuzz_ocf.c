// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//libFuzzer harness for ocf.c: open a zip archive and look up the entries an EPUB needs.
//TODO(rewrite): read every entry to its end and check the bytes agree across buffer sizes

#include <stddef.h>
#include <stdint.h>

#include "bartap_internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > UINT32_MAX) {
        return 0;
    }
    bartap_source_t source;
    bartap_source_memory(&source, data, (uint32_t)size);
    bartap_zip_t zip;
    if (bartap_zip_open(&zip, &source) != BARTAP_OK) {
        return 0;
    }
    bartap_entry_t entry;
    (void)bartap_zip_find(&zip, &source, "mimetype", 8, &entry);
    (void)bartap_zip_find(&zip, &source, "META-INF/container.xml", 22, &entry);
    return 0;
}
