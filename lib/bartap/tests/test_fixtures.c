// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "bartap_internal.h"

typedef struct {
    const char *input;
    const char *expected;
    bartap_err_t result;
} inflate_case_t;
typedef struct {
    const char *input;
    const char *entry;
    const char *expected;
    bartap_err_t result;
    bool book;
} zip_case_t;
typedef struct {
    const char *name;
    const char *title;
    const char *author;
    const char *language;
    const char *identifier;
    const char *first_label;
    int spine, toc, warnings, toc_warnings;
} book_case_t;
#include "fixture_tables.h"

static uint8_t *read_file(const char *root, const char *name, size_t *len)
{
    char path[1024];
    *len = 0U;
    if (name == NULL) {
        return calloc(1U, 1U);
    }
    (void)snprintf(path, sizeof path, "%s/%s", root, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    const long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *data = malloc((size_t)n + 1U);
    if (data == NULL) { fclose(f); return NULL; }
    if (fread(data, 1U, (size_t)n, f) != (size_t)n) { free(data); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return data;
}

static void fixtures_inflate_differential(void)
{
    static const size_t input_chunks[] = { 1U, 7U, 512U };
    static const size_t output_chunks[] = { 13U, 1U, 511U };
    uint8_t window[BARTAP_WINDOW_SIZE];
    uint8_t *output = malloc(1024U * 1024U);
    REQUIRE(output != NULL);
    for (size_t c = 0U; c < sizeof inflate_cases / sizeof inflate_cases[0]; c++) {
        const inflate_case_t *tc = &inflate_cases[c];
        size_t input_len, expected_len;
        uint8_t *input = read_file(BARTAP_TESTS_GENERATED_DIR, tc->input, &input_len);
        uint8_t *expected = read_file(BARTAP_TESTS_GENERATED_DIR, tc->expected, &expected_len);
        REQUIRE(input != NULL && expected != NULL);
        for (size_t variant = 0U; variant < 3U; variant++) {
            bartap_inflate_t inf;
            bartap_inflate_init(&inf, window);
            size_t consumed = 0U, produced = 0U;
            bartap_err_t err = BARTAP_OK;
            while (err == BARTAP_OK) {
                const size_t rem = input_len - consumed;
                const size_t in_n = rem < input_chunks[variant] ? rem : input_chunks[variant];
                const size_t out_n = output_chunks[variant];
                REQUIRE(produced + out_n < 1024U * 1024U);
                size_t used = 0U, made = 0U;
                err = bartap_inflate(&inf, input + consumed, in_n, &used,
                    output + produced, out_n, &made);
                REQUIRE(used <= in_n && made <= out_n);
                consumed += used;
                produced += made;
                if (used == 0U && made == 0U) break;
            }
            if (err != tc->result) printf("    input %s, chunk variant %zu\n", tc->input, variant);
            EXPECT_EQ_U(err, tc->result);
            if (tc->expected != NULL) {
                EXPECT_EQ_U(produced, expected_len);
                EXPECT(produced == expected_len && memcmp(output, expected, expected_len) == 0);
            }
        }
        free(input);
        free(expected);
    }
    free(output);
}

static void fixtures_zip_bytes_and_errors(void)
{
    bartap_work_t work;
    bartap_spine_item_t spine[16];
    for (size_t c = 0U; c < sizeof zip_cases / sizeof zip_cases[0]; c++) {
        const zip_case_t *tc = &zip_cases[c];
        size_t input_len, expected_len;
        uint8_t *input = read_file(BARTAP_TESTS_GENERATED_DIR, tc->input, &input_len);
        uint8_t *expected = read_file(BARTAP_TESTS_GENERATED_DIR, tc->expected, &expected_len);
        REQUIRE(input != NULL && expected != NULL);
        bartap_source_t source;
        bartap_source_memory(&source, input, (uint32_t)input_len);
        bartap_book_t book = {0};
        book.source = &source;
        bartap_err_t err = tc->book ? bartap_open(&book, &source, spine, 16U, &work)
            : bartap_zip_open(&book.zip, &source);
        size_t total = 0U;
        bool same = true;
        if (err == BARTAP_OK && tc->entry != NULL) {
            bartap_stream_t stream;
            err = bartap_stream_open_path(&stream, &book, tc->entry, work.window);
            uint8_t buffer[113];
            while (err == BARTAP_OK) {
                size_t got;
                err = bartap_stream_read(&stream, buffer, sizeof buffer, &got);
                if (err == BARTAP_OK) {
                    REQUIRE(got != 0U);
                    if (tc->result == BARTAP_END && (total + got > expected_len
                            || memcmp(buffer, expected + total, got) != 0)) same = false;
                    total += got;
                }
            }
        }
        if (err != tc->result) printf("    archive %s entry %s\n", tc->input, tc->entry != NULL ? tc->entry : "(open)");
        EXPECT_EQ_U(err, tc->result);
        if (tc->result == BARTAP_END) {
            EXPECT_EQ_U(total, expected_len);
            EXPECT(same);
        }
        free(input);
        free(expected);
    }
}

typedef struct {
    bartap_source_t memory;
    unsigned reads;
} counted_source_t;

static int counted_read(void *user, uint32_t offset, void *buf, size_t len)
{
    counted_source_t *source = user;
    source->reads++;
    return source->memory.read(source->memory.user, offset, buf, len);
}

static void fixture_book_stream_memory_equivalence(void)
{
    size_t size;
    uint8_t *bytes = read_file(BARTAP_TESTS_FIXTURE_DIR, "pride-and-prejudice.epub", &size);
    REQUIRE(bytes != NULL);
    counted_source_t counted = {0};
    bartap_source_memory(&counted.memory, bytes, (uint32_t)size);
    bartap_source_t source = { counted_read, &counted, (uint32_t)size };
    bartap_book_t book;
    bartap_work_t work;
    bartap_spine_item_t spine[128];
    REQUIRE(bartap_open(&book, &source, spine, 128U, &work) == BARTAP_OK);
    EXPECT_EQ_U(bartap_spine_count(&book), 65U);
    printf("    fixture book open: %u source reads\n", counted.reads);
    EXPECT(counted.reads < 2000U); // guards against one directory walk per chapter

    unsigned total_tokens = 0U;
    for (uint16_t i = 0U; i < bartap_spine_count(&book); i++) {
        bartap_stream_t stream;
        REQUIRE(bartap_stream_open(&stream, &book, i, work.window) == BARTAP_OK);
        const size_t n = bartap_stream_size(&stream);
        uint8_t *chapter = malloc(n + 1U);
        REQUIRE(chapter != NULL);
        size_t read = 0U, got;
        bartap_err_t err;
        while ((err = bartap_stream_read(&stream, chapter + read, n + 1U - read, &got)) == BARTAP_OK) {
            REQUIRE(got > 0U && got <= n - read);
            read += got;
        }
        REQUIRE(err == BARTAP_END && read == n);
        bartap_text_t memory, streaming;
        REQUIRE(bartap_text_open_memory(&memory, &book, i, chapter, n) == BARTAP_OK);
        REQUIRE(bartap_text_open(&streaming, &book, i, work.window) == BARTAP_OK);
        bartap_block_context_t blocks[BARTAP_TEXT_DEPTH_MAX];
        size_t depth = 0U;
        bartap_token_t a, b;
        while ((err = bartap_text_next(&memory, &a)) == BARTAP_OK) {
            REQUIRE(bartap_text_next(&streaming, &b) == BARTAP_OK);
            EXPECT(a.type == b.type && a.block == b.block && a.level == b.level
                && a.style == b.style && a.len == b.len && a.offset == b.offset
                && memcmp(a.text, b.text, a.len) == 0);
            if (a.type == BARTAP_TOK_BLOCK_START) {
                REQUIRE(depth < BARTAP_TEXT_DEPTH_MAX);
                blocks[depth].block = a.block;
                blocks[depth++].level = a.level;
            } else if (a.type == BARTAP_TOK_BLOCK_END) {
                REQUIRE(depth > 0U);
                depth--;
                EXPECT_EQ_U(a.block, blocks[depth].block);
                EXPECT_EQ_U(a.level, blocks[depth].level);
            }
            total_tokens++;
        }
        EXPECT_EQ_U(err, BARTAP_END);
        EXPECT_EQ_U(bartap_text_next(&streaming, &b), BARTAP_END);
        EXPECT_EQ_U(depth, 0U);
        free(chapter);
    }
    EXPECT(total_tokens > 10000U);
    bartap_toc_t toc;
    REQUIRE(bartap_toc_open(&toc, &book, work.window) == BARTAP_OK);
    bartap_toc_entry_t entry;
    unsigned entries = 0U;
    bartap_err_t err;
    while ((err = bartap_toc_next(&toc, &entry)) == BARTAP_OK) {
        EXPECT(entry.spine_index < book.spine_count);
        entries++;
    }
    EXPECT_EQ_U(err, BARTAP_END);
    EXPECT(entries > 60U);
    free(bytes);
}

static void fixtures_generated_books(void)
{
    bartap_work_t work;
    bartap_spine_item_t spine[16];
    for (size_t c = 0U; c < sizeof book_cases / sizeof book_cases[0]; c++) {
        const book_case_t *tc = &book_cases[c];
        printf("    book %s\n", tc->name);
        size_t size;
        uint8_t *bytes = read_file(BARTAP_TESTS_BOOKS_DIR, tc->name, &size);
        REQUIRE(bytes != NULL);
        bartap_source_t source;
        bartap_source_memory(&source, bytes, (uint32_t)size);
        bartap_book_t book;
        REQUIRE(bartap_open(&book, &source, spine, 16U, &work) == BARTAP_OK);
        EXPECT_EQ_U(bartap_spine_count(&book), tc->spine);
        EXPECT_EQ_U(bartap_warnings(&book), tc->warnings);
        const char *meta[] = {tc->title, tc->author, tc->language, tc->identifier};
        for (unsigned i = 0U; i < BARTAP_META_COUNT_; i++) {
            if (meta[i] != NULL) {
                char buf[BARTAP_META_MAX + 1];
                REQUIRE(bartap_metadata(&book, (bartap_meta_t)i, buf, sizeof buf) == BARTAP_OK);
                EXPECT(strcmp(buf, meta[i]) == 0);
            }
        }
        bartap_toc_t toc;
        bartap_err_t err = bartap_toc_open(&toc, &book, work.window);
        unsigned count = 0U;
        if (tc->toc == 0) {
            EXPECT_EQ_U(err, BARTAP_ERR_NOT_FOUND);
        } else {
            REQUIRE(err == BARTAP_OK);
            bartap_toc_entry_t entry;
            while ((err = bartap_toc_next(&toc, &entry)) == BARTAP_OK) {
                if (count == 0U && tc->first_label != NULL) EXPECT(strcmp(entry.label, tc->first_label) == 0);
                if (entry.fragment[0] != '\0') {
                    // The TOC and text streams need distinct inflate windows while both are live.
                    uint8_t text_window[BARTAP_WINDOW_SIZE];
                    bartap_text_t text;
                    REQUIRE(bartap_text_open(&text, &book, entry.spine_index, text_window) == BARTAP_OK);
                    EXPECT_EQ_U(bartap_text_seek_fragment(&text, entry.fragment),
                        strcmp(entry.fragment, "legacy") == 0 ? BARTAP_ERR_NOT_FOUND : BARTAP_OK);
                }
                count++;
            }
            EXPECT_EQ_U(err, BARTAP_END);
            if (tc->toc >= 0) EXPECT_EQ_U(count, tc->toc);
            if (tc->toc_warnings >= 0) EXPECT_EQ_U(bartap_toc_warnings(&toc), tc->toc_warnings);
        }
        free(bytes);
    }
}

const test_case_t fixtures_tests[] = {
    TEST_CASE(fixtures_generated_books),
    TEST_CASE(fixtures_inflate_differential),
    TEST_CASE(fixtures_zip_bytes_and_errors),
    TEST_CASE(fixture_book_stream_memory_equivalence),
};
const size_t fixtures_test_count = sizeof fixtures_tests / sizeof fixtures_tests[0];
