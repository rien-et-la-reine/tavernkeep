// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The whole-book flow (flow.h). */

#include "flow.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bartap/byte_source.h"

#define WINDOW_SIZE 32768U
#define SPINE_CAPACITY 2048U
#define NAME_SIZE 1024U

static void fail(flow_report_t *report, const char *format, ...)
{
    if (report->failure[0] != '\0') {
        return;                     //the first failure is the one worth reading
    }
    va_list args;
    va_start(args, format);
    vsnprintf(report->failure, sizeof report->failure, format, args);
    va_end(args);
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state != 0U ? *state : 0x9E3779B9U;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

const char *flow_result_name(bartap_epub_result_t result)
{
    switch (result) {
    case BARTAP_EPUB_RESULT_OK: return "OK";
    case BARTAP_EPUB_RESULT_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case BARTAP_EPUB_RESULT_IO_ERROR: return "IO_ERROR";
    case BARTAP_EPUB_RESULT_CORRUPT: return "CORRUPT";
    case BARTAP_EPUB_RESULT_UNSUPPORTED: return "UNSUPPORTED";
    case BARTAP_EPUB_RESULT_NOT_FOUND: return "NOT_FOUND";
    case BARTAP_EPUB_RESULT_LIMIT_EXCEEDED: return "LIMIT_EXCEEDED";
    case BARTAP_EPUB_RESULT_NOT_AN_EPUB: return "NOT_AN_EPUB";
    case BARTAP_EPUB_RESULT_NOT_A_ZIP: return "NOT_A_ZIP";
    default: return "?";
    }
}

void flow_options_default(flow_options_t *options)
{
    memset(options, 0, sizeof *options);
    options->feed = 512;
    options->input_size = 512;
    options->scratch_size = 512;
    options->max_document_size = 4U << 20;
    options->max_content_size = 64U << 20;
    options->resolve_every = 1;
    options->snapshot_check = true;
    options->seed = 1;
}

/* ------------------------------------------------------------ progress */

//a call made progress if it says so, or if it changed its state at all; a call that changes nothing and
//says nothing would do the same again for ever
typedef struct {
    unsigned char *before;
    size_t size;
    unsigned stalls;
} watch_t;

static void watch_begin(watch_t *w, const void *state, size_t size)
{
    w->before = malloc(size);
    w->size = size;
    w->stalls = 0;
    memcpy(w->before, state, size);
}

static void watch_end(watch_t *w)
{
    free(w->before);
    w->before = NULL;
}

//after a call: false once it has stalled too often in a row
static bool watch(watch_t *w, const void *state, bool progressed)
{
    if (!progressed && memcmp(w->before, state, w->size) == 0) {
        if (++w->stalls > FLOW_STALL_MAX) {
            return false;
        }
    } else {
        w->stalls = 0;
    }
    memcpy(w->before, state, w->size);
    return true;
}

/* ------------------------------------------------------------ recorded items */

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
    size_t count;
} record_t;

static void record_bytes(record_t *r, const void *data, size_t size)
{
    if (r->length + size > r->capacity) {
        r->capacity = (r->length + size) * 2U + 256U;
        r->data = realloc(r->data, r->capacity);
    }
    memcpy(r->data + r->length, data, size);
    r->length += size;
}

//an item, flattened, so that two streams of items compare with memcmp
static void record_item(record_t *r, const bartap_text_item_t *item)
{
    const uint32_t fields[10] = { item->kind, item->before, item->style, item->heading, item->indent, item->user,
        item->depth, (uint32_t)item->length, (uint32_t)item->value_length, item->value_truncated };
    record_bytes(r, fields, sizeof fields);
    if (item->length > 0U) {
        record_bytes(r, item->text, item->length);
    }
    if (item->value_length > 0U) {
        record_bytes(r, item->value, item->value_length);
    }
    r->count++;
}

/* ------------------------------------------------------------ one spine item */

typedef struct {
    const flow_options_t *options;
    flow_report_t *report;
    uint32_t random;
} run_t;

static size_t feed_size(run_t *run, size_t fixed)
{
    if (fixed != 0U) {
        return fixed;
    }
    const uint32_t r = next_random(&run->random);
    switch (r % 4U) {
    case 0: return 1;
    case 1: return 1U + (r >> 8) % 16U;
    case 2: return 1U + (r >> 8) % 512U;
    default: return 4096;
    }
}

//resolve cfi over the document: it must succeed
static void check_resolve(run_t *run, const uint8_t *doc, size_t size, const bartap_epub_cfi_t *cfi, const char *text)
{
    bartap_epub_resolver_t resolver;
    bartap_epub_result_t r = bartap_epub_cfi_resolver_init(&resolver, cfi,
        run->options->text_flags & BARTAP_EPUB_RESOLVE_STRICT);
    if (r != BARTAP_EPUB_RESULT_OK) {
        fail(run->report, "resolver_init(%s): %s", text, flow_result_name(r));
        return;
    }
    size_t offset = 0;
    bool done = false;
    uint32_t position = 0;
    watch_t w;
    watch_begin(&w, &resolver, sizeof resolver);
    const uint64_t cap = 64U + 4U * (uint64_t)size;
    for (uint64_t calls = 0; !done; calls++) {
        if (calls > cap) {
            fail(run->report, "resolve(%s): more than %llu calls", text, (unsigned long long)cap);
            break;
        }
        const size_t n = size - offset < 4096U ? size - offset : 4096U;
        size_t used;
        r = bartap_epub_cfi_resolve_next(&resolver, doc + offset, n, &used, offset + n == size, &done, &position);
        if (used > n) {
            fail(run->report, "resolve(%s): took %zu of %zu bytes", text, used, n);
            break;
        }
        offset += used;
        if (r != BARTAP_EPUB_RESULT_OK) {
            fail(run->report, "resolve(%s): %s at byte %zu", text, flow_result_name(r), offset);
            break;
        }
        if (!watch(&w, &resolver, used > 0U || done)) {
            fail(run->report, "resolve(%s): no progress", text);
            break;
        }
    }
    watch_end(&w);
    if (done && position > size) {
        fail(run->report, "resolve(%s): position %u past the end (%zu)", text, position, size);
    }
    run->report->resolves++;
}

//an item's position: formatted, parsed back, and resolved
static void check_position(run_t *run, const bartap_text_t *text, const uint8_t *doc, size_t size, uint32_t index,
    uint32_t spine_index, char *cfi_text, size_t cfi_text_size, uint32_t *resolved)
{
    cfi_text[0] = '\0';
    bartap_epub_cfi_t cfi;
    bartap_epub_result_t r = bartap_text_position(text, &cfi);
    if (r != BARTAP_EPUB_RESULT_OK) {
        if (r != BARTAP_EPUB_RESULT_LIMIT_EXCEEDED) {
            fail(run->report, "position of item %u: %s", index, flow_result_name(r));
        } else if (run->report->cfi == BARTAP_EPUB_RESULT_OK) {
            run->report->cfi = r;
        }
        return;
    }
    if (cfi.spine_index != spine_index) {
        fail(run->report, "position of item %u: spine index %u, not %u", index, cfi.spine_index, spine_index);
    }
    r = bartap_epub_cfi_format(&cfi, cfi_text, cfi_text_size);
    if (r != BARTAP_EPUB_RESULT_OK) {
        fail(run->report, "format of item %u: %s", index, flow_result_name(r));
        return;
    }
    bartap_epub_cfi_t back;
    r = bartap_epub_cfi_parse(cfi_text, &back);
    if (r != BARTAP_EPUB_RESULT_OK || bartap_epub_cfi_compare(&cfi, &back) != 0
            || back.step_count != cfi.step_count || back.has_offset != cfi.has_offset) {
        fail(run->report, "%s does not round-trip: %s", cfi_text, flow_result_name(r));
        return;
    }
    const uint32_t every = run->options->resolve_every;
    const uint32_t max = run->options->resolve_max;
    if (every != 0U && index % every == 0U && (max == 0U || *resolved < max)) {
        (*resolved)++;
        check_resolve(run, doc, size, &cfi, cfi_text);
    }
}

typedef struct {
    bartap_text_t text;
    bartap_epub_result_t result;
    uint32_t warnings;
    //the copy restored from a snapshot, run to its end
    bool copied;
    bartap_epub_result_t copy_result;
    uint32_t copy_warnings;
    record_t copy_items;
} text_run_t;

//run text over doc from offset to the end, recording its items; with check, also positions and the snapshot
static void run_text(run_t *run, text_run_t *t, const uint8_t *doc, size_t size, size_t offset, uint32_t spine_index,
    size_t feed, record_t *items, bool check, uint64_t snapshot_call)
{
    const flow_options_t *o = run->options;
    char cfi_text[BARTAP_EPUB_CFI_TEXT_MAX];
    uint32_t resolved = 0;
    bool end = false;
    watch_t w;
    watch_begin(&w, &t->text, sizeof t->text);
    const uint64_t cap = 256U + 8U * (uint64_t)size;
    t->result = BARTAP_EPUB_RESULT_OK;
    for (uint64_t calls = 0; !end; calls++) {
        if (calls > cap) {
            fail(run->report, "text of spine item %u: more than %llu calls", spine_index, (unsigned long long)cap);
            break;
        }
        if (check && snapshot_call == calls && o->snapshot_check) {
            //save here, restore into a fresh text, and run that to the end fed differently: it must deliver
            //exactly what this one goes on to deliver
            uint8_t snapshot[BARTAP_TEXT_SNAPSHOT_MAX];
            size_t length;
            bartap_epub_result_t r = bartap_text_save(&t->text, snapshot, sizeof snapshot, &length);
            if (r != BARTAP_EPUB_RESULT_OK) {
                fail(run->report, "save in spine item %u: %s", spine_index, flow_result_name(r));
            } else {
                if (length > run->report->snapshot_largest) {
                    run->report->snapshot_largest = length;
                }
                text_run_t *copy = calloc(1, sizeof *copy);
                (void)bartap_text_init(&copy->text, spine_index, o->text_flags);
                bartap_offset_t resume_at;
                r = bartap_text_restore(&copy->text, snapshot, length, &resume_at);
                if (r != BARTAP_EPUB_RESULT_OK) {
                    fail(run->report, "restore in spine item %u: %s", spine_index, flow_result_name(r));
                } else if (resume_at > offset) {
                    fail(run->report, "restore in spine item %u: resumes at %u, past %zu", spine_index, resume_at,
                        offset);
                } else {
                    run_text(run, copy, doc, size, resume_at, spine_index, feed == 1U ? 7U : 1U, &t->copy_items,
                        false, 0);
                    t->copied = true;
                    t->copy_result = copy->result;
                    t->copy_warnings = copy->warnings;
                    items->count = 0;   //from here on, items holds what the copy must have delivered
                    items->length = 0;
                }
                free(copy);
            }
        }
        const size_t n0 = feed_size(run, feed);
        const size_t n = size - offset < n0 ? size - offset : n0;
        size_t used = 0;
        bartap_text_item_t item;
        bool ready;
        const bartap_epub_result_t r = bartap_text_next(&t->text, doc + offset, n, &used, offset + n == size, &item,
            &ready, &end);
        if (used > n) {
            fail(run->report, "text of spine item %u: took %zu of %zu bytes", spine_index, used, n);
            break;
        }
        offset += used;
        if (ready) {
            record_item(items, &item);
            if (check) {
                check_position(run, &t->text, doc, size, (uint32_t)run->report->items, spine_index, cfi_text,
                    sizeof cfi_text, &resolved);
                run->report->items++;
                if (o->on_item != NULL) {
                    o->on_item(o->context, spine_index, &item, cfi_text);
                }
            }
        }
        if (r != BARTAP_EPUB_RESULT_OK) {
            t->result = r;
            break;
        }
        if (!watch(&w, &t->text, used > 0U || ready || end)) {
            fail(run->report, "text of spine item %u: no progress at byte %zu", spine_index, offset);
            break;
        }
    }
    watch_end(&w);
    t->warnings = bartap_text_warnings(&t->text);
}

//one spine item: decompressed whole into memory, then read as text
static void run_spine_item(run_t *run, const bartap_epub_t *book, uint32_t index, uint8_t *window, uint8_t *input)
{
    const flow_options_t *o = run->options;
    flow_report_t *report = run->report;
    if (o->on_spine != NULL) {
        uint8_t name[NAME_SIZE];
        if (bartap_epub_spine_name(book, index, name, sizeof name) != BARTAP_EPUB_RESULT_OK) {
            name[0] = 0;
        }
        o->on_spine(o->context, index, (const char *)name);
    }
    bartap_epub_stream_t stream;
    bartap_epub_result_t r = bartap_epub_stream_open(&stream, book, index, o->max_content_size, window, WINDOW_SIZE,
        input, o->input_size);
    if (r != BARTAP_EPUB_RESULT_OK) {
        if (r != BARTAP_EPUB_RESULT_NOT_FOUND && report->streams == BARTAP_EPUB_RESULT_OK) {
            report->streams = r;
        }
        return;
    }
    uint8_t *doc = NULL;
    size_t size = 0;
    size_t declared = 0;
    bool known = false;
    bool end = false;
    watch_t w;
    watch_begin(&w, &stream, sizeof stream);
    for (uint64_t calls = 0; !end; calls++) {
        if (calls > 64U + 4U * (uint64_t)o->max_content_size) {
            fail(report, "stream of spine item %u: too many calls", index);
            break;
        }
        uint8_t chunk[4096];
        size_t got;
        r = bartap_epub_stream_read(&stream, chunk, sizeof chunk, &got, &end);
        if (r != BARTAP_EPUB_RESULT_OK) {
            if (report->streams == BARTAP_EPUB_RESULT_OK) {
                report->streams = r;
            }
            break;
        }
        bartap_offset_t s;
        if (!known && bartap_epub_stream_size(&stream, &s) == BARTAP_EPUB_RESULT_OK) {
            known = true;
            declared = s;
            doc = malloc(declared + 1U);
        }
        if (got > 0U && (!known || size + got > declared)) {
            fail(report, "stream of spine item %u: delivered past its declared size %zu", index, declared);
            break;
        }
        if (got > 0U) {
            memcpy(doc + size, chunk, got);
            size += got;
        }
        if (!watch(&w, &stream, got > 0U || end)) {
            fail(report, "stream of spine item %u: no progress", index);
            break;
        }
    }
    watch_end(&w);
    if (end && size != declared) {
        fail(report, "stream of spine item %u: %zu bytes at the end, declared %zu", index, size, declared);
    }
    if (!end || report->failure[0] != '\0') {
        free(doc);
        return;
    }
    report->content_bytes += size;

    text_run_t *t = calloc(1, sizeof *t);
    (void)bartap_text_init(&t->text, index, o->text_flags);
    record_t items = { NULL, 0, 0, 0 };
    const uint64_t snapshot_call = next_random(&run->random) % (size / 2U + 2U);
    run_text(run, t, doc, size, 0, index, o->feed, &items, true, snapshot_call);
    if (t->result != BARTAP_EPUB_RESULT_OK && report->text == BARTAP_EPUB_RESULT_OK) {
        report->text = t->result;
    }
    report->text_warnings |= t->warnings;
    const record_t *after = &t->copy_items;
    if (t->copied && (items.count != after->count || items.length != after->length
            || (items.length > 0U && memcmp(items.data, after->data, items.length) != 0)
            || t->copy_result != t->result || t->copy_warnings != t->warnings)) {
        fail(report, "spine item %u: after a snapshot, %zu items (%s, warnings %x) where the original went on with "
            "%zu (%s, warnings %x)", index, after->count, flow_result_name(t->copy_result), t->copy_warnings,
            items.count, flow_result_name(t->result), t->warnings);
    }
    free(items.data);
    free(t->copy_items.data);
    free(t);
    free(doc);
}

/* ------------------------------------------------------------ the book */

void flow_run(const uint8_t *data, size_t size, const flow_options_t *options, flow_report_t *report)
{
    memset(report, 0, sizeof *report);
    run_t run = { options, report, options->seed };
    bartap_byte_source_memory_t memory_source;
    bartap_byte_source_t source;
    bartap_byte_source_memory_init(&memory_source, data, (bartap_offset_t)size, &source);

    uint8_t *window = malloc(WINDOW_SIZE);
    uint8_t *input = malloc(options->input_size);
    uint8_t *scratch = malloc(options->scratch_size);
    bartap_epub_spine_item_t *spine = calloc(SPINE_CAPACITY, sizeof *spine);
    bartap_epub_t *book = malloc(sizeof *book);
    bartap_epub_opener_t *opener = malloc(sizeof *opener);
    const bartap_epub_memory_t memory = { scratch, options->scratch_size, window, WINDOW_SIZE, input,
        options->input_size, spine, SPINE_CAPACITY, options->max_document_size, BARTAP_EPUB_OPEN_FULL,
        options->open_flags };

    bartap_epub_result_t r = bartap_epub_open_begin(opener, book, &source, &memory);
    bool done = false;
    watch_t w;
    watch_begin(&w, opener, sizeof *opener);
    const uint64_t cap = 100000U + 64U * (uint64_t)size + 32U * (uint64_t)options->max_document_size;
    for (uint64_t steps = 0; r == BARTAP_EPUB_RESULT_OK && !done; steps++) {
        if (steps > cap) {
            fail(report, "open: more than %llu steps", (unsigned long long)cap);
            break;
        }
        r = bartap_epub_open_step(opener, &done);
        if (r == BARTAP_EPUB_RESULT_OK && !watch(&w, opener, done)) {
            fail(report, "open: no progress");
            break;
        }
    }
    watch_end(&w);
    report->open = r;
    if (r != BARTAP_EPUB_RESULT_OK || !done) {
        goto out;
    }
    report->warnings = bartap_epub_warnings(book);
    report->spine_count = bartap_epub_spine_count(book);

    for (int f = 0; f < 4; f++) {
        char field[BARTAP_EPUB_METADATA_MAX + 1U];
        size_t length;
        r = bartap_epub_metadata(book, (bartap_epub_field_t)f, field, sizeof field, &length);
        if (r == BARTAP_EPUB_RESULT_OK && length != strlen(field)) {
            fail(report, "metadata field %d: length %zu for %zu bytes", f, length, strlen(field));
        }
        if (r != BARTAP_EPUB_RESULT_OK && r != BARTAP_EPUB_RESULT_NOT_FOUND
                && report->metadata == BARTAP_EPUB_RESULT_OK) {
            report->metadata = r;
        }
    }

    bartap_epub_toc_t *toc = malloc(sizeof *toc);
    r = bartap_epub_toc_open(toc, book, window, WINDOW_SIZE, input, options->input_size);
    if (r == BARTAP_EPUB_RESULT_OK) {
        bool end = false;
        watch_begin(&w, toc, sizeof *toc);
        const uint64_t toc_cap = 100000U + 32U * (uint64_t)options->max_document_size;
        for (uint64_t steps = 0; r == BARTAP_EPUB_RESULT_OK && !end; steps++) {
            if (steps > toc_cap) {
                fail(report, "toc: more than %llu steps", (unsigned long long)toc_cap);
                break;
            }
            bartap_epub_toc_entry_t entry;
            bool ready;
            r = bartap_epub_toc_next(toc, &entry, &ready, &end);
            if (ready) {
                report->toc_entries++;
                if (entry.label_length != strlen(entry.label) || entry.label_length > BARTAP_EPUB_LABEL_MAX
                        || entry.target.fragment_length != strlen((const char *)entry.target.fragment)
                        || (entry.target.spine_index != UINT32_MAX && entry.target.spine_index >= report->spine_count)) {
                    fail(report, "toc entry %llu is inconsistent", (unsigned long long)report->toc_entries);
                }
            }
            if (r == BARTAP_EPUB_RESULT_OK && !watch(&w, toc, ready || end)) {
                fail(report, "toc: no progress");
                break;
            }
        }
        watch_end(&w);
        report->toc_warnings = bartap_epub_toc_warnings(toc);
    }
    report->toc = r == BARTAP_EPUB_RESULT_NOT_FOUND ? BARTAP_EPUB_RESULT_OK : r;
    free(toc);

    for (uint32_t i = 0; i < report->spine_count && report->failure[0] == '\0'; i++) {
        run_spine_item(&run, book, i, window, input);
    }
out:
    free(opener);
    free(book);
    free(spine);
    free(scratch);
    free(input);
    free(window);
}
