// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The book: the whole-book flow (../common/flow.h) over the input as an EPUB. Open; every metadata field;
 * the table of contents to its end; every spine item through the text layer to its end, fed in mixed piece
 * sizes. For every item: bartap_text_position, format, parse (must round-trip equal), then resolve (must
 * succeed). Each spine item is also saved, restored into a fresh text layer and run on, which must deliver
 * the same items. The input's hash chooses strict or lenient markup, and whether element items are on, so
 * that a corpus entry always takes the same path. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "flow.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > UINT32_MAX) {
        return 0;
    }
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ data[i]) * 16777619U;
    }
    flow_options_t options;
    flow_options_default(&options);
    options.feed = 0;
    options.seed = hash | 1U;
    options.input_size = (hash & 0x10U) ? 1U : 512U;
    options.scratch_size = (hash & 0x20U) ? BARTAP_EPUB_SCRATCH_MIN : 512U;
    options.max_document_size = 256U << 10;
    options.max_content_size = 1U << 20;
    options.resolve_every = 1;
    options.resolve_max = 64;
    if (hash & 0x1U) {
        options.text_flags |= BARTAP_TEXT_STRICT;
        options.open_flags |= BARTAP_EPUB_OPEN_STRICT_MARKUP;
    }
    if (hash & 0x2U) {
        options.text_flags |= BARTAP_TEXT_DELIVER_ELEMENTS | BARTAP_TEXT_DELIVER_ATTRIBUTES;
    }
    flow_report_t report;
    flow_run(data, size, &options, &report);
    if (report.failure[0] != '\0') {
        fprintf(stderr, "%s\n", report.failure);
        abort();
    }
    return 0;
}
