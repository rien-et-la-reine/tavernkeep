// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Exercise text directly, so a mutation does not first need to survive ZIP and OPF parsing. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bartap.h"

/* Memory-mode opening reads the chapter's name from its local header for the image base directory: a
 * 30-byte header whose name length (bytes 26-27) covers the 13 bytes after it. */
static const uint8_t local_header[] = "PK\3\4" "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
    "\15\0" "\0\0" "OEBPS/c.xhtml";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 16384U) return 0;
    bartap_source_t source;
    bartap_source_memory(&source, local_header, (uint32_t)(sizeof local_header - 1U));
    bartap_spine_item_t spine = {0};
    bartap_book_t book = {0};
    book.source = &source;
    book.spine = &spine;
    book.spine_count = 1U;
    bartap_text_t text;
    if (bartap_text_open_memory(&text, &book, 0U, data, size) != BARTAP_OK) return 0;
    for (size_t step = 0U; step < size * 2U + 64U; step++) {
        bartap_loc_t loc;
        if (bartap_text_loc(&text, &loc) != BARTAP_OK) break;
        if (loc.byte_offset > size || loc.anchor_len > size - loc.byte_offset
                || memcmp(loc.anchor, data + loc.byte_offset, loc.anchor_len) != 0) abort();
        bartap_token_t first, second;
        const bartap_err_t err = bartap_text_next(&text, &first);
        if (step < 16U) {
            bartap_seek_t how;
            if (bartap_text_seek(&text, &loc, &how) != BARTAP_OK) abort();
            if (bartap_text_next(&text, &second) != err) abort();
            if (err == BARTAP_OK && (first.type != second.type || first.style != second.style
                    || first.block != second.block || first.level != second.level
                    || first.len != second.len || first.offset != second.offset
                    || memcmp(first.text, second.text, first.len) != 0)) abort();
        }
        if (err != BARTAP_OK) break;
    }
    return 0;
}
