// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//libFuzzer harness for the whole book: open, metadata, the TOC, and every spine item's text.
//TODO(rewrite): seek to every token's location and check the iterator resumes at the same token

#include <stddef.h>
#include <stdint.h>

#include "bartap.h"

#define SPINE_MAX 256U

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bartap_work_t work;
    static bartap_spine_item_t spine[SPINE_MAX];
    if (size > UINT32_MAX) {
        return 0;
    }
    bartap_source_t source;
    bartap_source_memory(&source, data, (uint32_t)size);
    bartap_book_t book;
    if (bartap_open(&book, &source, spine, SPINE_MAX, &work) != BARTAP_OK) {
        return 0;
    }

    char meta[BARTAP_META_MAX + 1];
    for (int field = BARTAP_META_TITLE; field <= BARTAP_META_IDENTIFIER; field++) {
        (void)bartap_metadata(&book, (bartap_meta_t)field, meta, sizeof meta);
    }

    bartap_toc_t toc;
    bartap_toc_entry_t entry;
    if (bartap_toc_open(&toc, &book, work.window) == BARTAP_OK) {
        for (size_t steps = 0; steps < size + 64 && bartap_toc_next(&toc, &entry) == BARTAP_OK; steps++) {
        }
    }

    bartap_text_t text;
    bartap_token_t token;
    for (uint16_t i = 0; i < bartap_spine_count(&book); i++) {
        if (bartap_text_open(&text, &book, i, work.window) != BARTAP_OK) {
            continue;
        }
        for (size_t steps = 0; steps < 64 * size + 64 && bartap_text_next(&text, &token) == BARTAP_OK;
            steps++) {
        }
    }
    return 0;
}
