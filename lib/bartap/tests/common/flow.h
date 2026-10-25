// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_TESTS_FLOW_H
#define BARTAP_TESTS_FLOW_H

/* The whole-book flow, shared by the epub fuzzer, the corpus runner and the CFI dumper: open a book from
 * memory, read every metadata field, the table of contents to its end, and every spine item through the
 * text layer to its end. For every item: its position, formatted, parsed back (it must round-trip), and
 * resolved (it must succeed). Each spine item is also saved and restored once, part way, into a fresh text
 * layer fed in different pieces, which must deliver exactly the same items from there.
 *
 * Every step loop has an iteration cap derived from the input's size, and fails if a call returns OK
 * without making progress (no input taken, nothing delivered, and its state unchanged byte for byte) more
 * than FLOW_STALL_MAX times in a row: D5 forbids livelocks. A broken invariant is reported in
 * report->failure; result codes from damaged input are not failures. Test code: uses the C library freely. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bartap/epub.h"
#include "bartap/text.h"

#define FLOW_STALL_MAX 4U

typedef struct {
    uint32_t text_flags;            //BARTAP_TEXT_* for the text layer and the resolver
    uint32_t open_flags;            //BARTAP_EPUB_OPEN_* bits
    size_t feed;                    //bytes fed to the text layer per call; 0 for a mix of sizes
    size_t input_size;              //the streams' compressed-input buffers
    size_t scratch_size;
    bartap_offset_t max_document_size;
    bartap_offset_t max_content_size;
    uint32_t resolve_every;         //resolve every Nth item's position (1: all; 0: none)
    uint32_t resolve_max;           //and at most this many per spine item (0: no cap)
    bool snapshot_check;
    uint32_t seed;                  //for the mixed feed sizes and where a snapshot is taken
    //called for every item delivered, with its position as text ("" if it has none)
    void (*on_item)(void *context, uint32_t spine_index, const bartap_text_item_t *item, const char *cfi);
    //called for every spine item, before its items, with its archive path
    void (*on_spine)(void *context, uint32_t spine_index, const char *path);
    void *context;
} flow_options_t;

typedef struct {
    bartap_epub_result_t open;
    bartap_epub_result_t metadata;  //the first result other than OK or NOT_FOUND, else OK
    bartap_epub_result_t toc;
    bartap_epub_result_t streams;   //the first stream error, else OK
    bartap_epub_result_t text;      //the first text-layer error, else OK
    bartap_epub_result_t cfi;       //the first position, parse or resolve error, else OK
    uint32_t warnings;
    uint32_t text_warnings;         //OR of every spine item's
    uint32_t toc_warnings;
    uint32_t spine_count;
    uint64_t content_bytes;         //decompressed bytes of the spine items
    uint64_t items;
    uint64_t toc_entries;
    uint64_t resolves;
    size_t snapshot_largest;
    char failure[512];              //empty unless an invariant broke
} flow_report_t;

void flow_options_default(flow_options_t *options);

void flow_run(const uint8_t *data, size_t size, const flow_options_t *options, flow_report_t *report);

const char *flow_result_name(bartap_epub_result_t result);

#endif
