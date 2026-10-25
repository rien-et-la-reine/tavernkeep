// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//the book layer (bartap/epub.h) over the books make_books.py writes, whose contents and expectations it
//defines: the open modes, the cover (EPUB 3's cover-image property, EPUB 2's meta), finding any entry, the
//table of contents' targets and fragments, href resolution (RFC 3986's dot segments, percent-decoding), and
//locations that are stale or forged

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bartap/epub.h"
#include "bartap/text.h"

#include "books.h"
#include "harness.h"

static void expect_field(const test_book_t *b, bartap_epub_field_t field, const char *value)
{
    char buffer[BARTAP_EPUB_METADATA_MAX + 1U];
    size_t length;
    const bartap_epub_result_t r = bartap_epub_metadata(&b->book, field, buffer, sizeof buffer, &length);
    if (value == NULL) {
        EXPECT_EQ_U(r, BARTAP_EPUB_RESULT_NOT_FOUND);
        return;
    }
    EXPECT_EQ_U(r, BARTAP_EPUB_RESULT_OK);
    EXPECT(r == BARTAP_EPUB_RESULT_OK && strcmp(buffer, value) == 0 && length == strlen(value));
}

//the cover's entry, read whole, against its path's entry found by the finder
static void expect_cover(test_book_t *b, const char *path, uint8_t type)
{
    bartap_epub_location_t cover;
    uint8_t found_type;
    const bartap_epub_result_t r = bartap_epub_cover(&b->book, &cover, &found_type);
    if (path == NULL) {
        EXPECT_EQ_U(r, BARTAP_EPUB_RESULT_NOT_FOUND);
        return;
    }
    REQUIRE(r == BARTAP_EPUB_RESULT_OK);
    EXPECT_EQ_U(found_type, type);
    bartap_epub_finder_t finder;
    REQUIRE(bartap_epub_find_begin(&finder, &b->book, (const uint8_t *)path, strlen(path)) == BARTAP_EPUB_RESULT_OK);
    bartap_epub_location_t at;
    bool done = false;
    bartap_epub_result_t f = BARTAP_EPUB_RESULT_OK;
    while (!done && f == BARTAP_EPUB_RESULT_OK) {
        f = bartap_epub_find_step(&finder, &done, &at);
    }
    EXPECT_EQ_U(f, BARTAP_EPUB_RESULT_OK);
    EXPECT(at.entry_offset == cover.entry_offset && at.entry_index == cover.entry_index);
}

static void epub_epub3_full(void)
{
    bartap_epub_result_t r;
    test_book_t *b = test_book_new("epub3.epub", BARTAP_EPUB_OPEN_FULL, 0, &r);
    REQUIRE(b != NULL && r == BARTAP_EPUB_RESULT_OK);
    EXPECT_EQ_U(bartap_epub_warnings(&b->book), 0U);
    expect_field(b, BARTAP_EPUB_FIELD_TITLE, "A Test Book");     //whitespace collapsed and trimmed
    expect_field(b, BARTAP_EPUB_FIELD_CREATOR, "Test Author");
    expect_field(b, BARTAP_EPUB_FIELD_LANGUAGE, "en");
    expect_field(b, BARTAP_EPUB_FIELD_IDENTIFIER, "urn:uuid:00000000-0000-4000-8000-000000000000");
    EXPECT_EQ_U(bartap_epub_spine_count(&b->book), 3U);
    uint8_t name[BARTAP_EPUB_PATH_MAX + 1U];
    EXPECT_EQ_U(bartap_epub_spine_name(&b->book, 2, name, sizeof name), BARTAP_EPUB_RESULT_OK);
    EXPECT(strcmp((const char *)name, "epub/text/caf\xC3\xA9.xhtml") == 0);   //the manifest's href, percent-decoded
    uint32_t index;
    EXPECT_EQ_U(bartap_epub_spine_index(&b->book, "epub/text/end.xhtml", &index), BARTAP_EPUB_RESULT_OK);
    EXPECT_EQ_U(index, 1U);
    expect_cover(b, "epub/images/cover.png", BARTAP_EPUB_IMAGE_PNG);
    bartap_epub_location_t cover;
    REQUIRE(bartap_epub_cover(&b->book, &cover, NULL) == BARTAP_EPUB_RESULT_OK);
    uint8_t *png;
    size_t size;
    EXPECT_EQ_U(test_book_read(b, &cover, &png, &size), BARTAP_EPUB_RESULT_OK);
    EXPECT(size > 8U && memcmp(png, "\x89PNG", 4) == 0);
    free(png);
    test_book_free(b);
}

//the METADATA modes give the same metadata and cover as FULL, and have no spine
static void epub_open_modes_agree(void)
{
    static const char *const books[] = { "epub3.epub", "epub2.epub", "plain.epub", "lenient.epub" };
    for (size_t i = 0; i < sizeof books / sizeof books[0]; i++) {
        bartap_epub_result_t r;
        test_book_t *full = test_book_new(books[i], BARTAP_EPUB_OPEN_FULL, 0, &r);
        REQUIRE(full != NULL && r == BARTAP_EPUB_RESULT_OK);
        for (uint8_t mode = BARTAP_EPUB_OPEN_METADATA; mode <= BARTAP_EPUB_OPEN_METADATA_COVER; mode++) {
            test_book_t *light = test_book_new(books[i], mode, 0, &r);
            REQUIRE(light != NULL && r == BARTAP_EPUB_RESULT_OK);
            for (int f = 0; f < 4; f++) {
                char a[256];
                char c[256];
                const bartap_epub_result_t ra = bartap_epub_metadata(&full->book, (bartap_epub_field_t)f, a, sizeof a, NULL);
                const bartap_epub_result_t rc = bartap_epub_metadata(&light->book, (bartap_epub_field_t)f, c, sizeof c, NULL);
                EXPECT_EQ_U(rc, ra);
                EXPECT(ra != BARTAP_EPUB_RESULT_OK || strcmp(a, c) == 0);
            }
            bartap_epub_location_t a;
            bartap_epub_location_t c;
            uint8_t at;
            uint8_t ct;
            const bartap_epub_result_t ra = bartap_epub_cover(&full->book, &a, &at);
            const bartap_epub_result_t rc = bartap_epub_cover(&light->book, &c, &ct);
            if (mode == BARTAP_EPUB_OPEN_METADATA_COVER) {
                EXPECT_EQ_U(rc, ra);
                EXPECT(ra != BARTAP_EPUB_RESULT_OK || (a.entry_offset == c.entry_offset && at == ct));
            } else {
                EXPECT_EQ_U(rc, BARTAP_EPUB_RESULT_NOT_FOUND);
            }
            EXPECT_EQ_U(bartap_epub_spine_count(&light->book), 0U);
            bartap_epub_stream_t stream;
            EXPECT_EQ_U(bartap_epub_stream_open(&stream, &light->book, 0, 1000, light->window, sizeof light->window,
                light->input, sizeof light->input), BARTAP_EPUB_RESULT_INVALID_ARGUMENT);
            bartap_epub_toc_t toc;
            EXPECT_EQ_U(bartap_epub_toc_open(&toc, &light->book, light->window, sizeof light->window, light->input,
                sizeof light->input), BARTAP_EPUB_RESULT_INVALID_ARGUMENT);
            test_book_free(light);
        }
        test_book_free(full);
    }
}

static void epub_epub2_ncx_and_meta_cover(void)
{
    bartap_epub_result_t r;
    test_book_t *b = test_book_new("epub2.epub", BARTAP_EPUB_OPEN_FULL, 0, &r);
    REQUIRE(b != NULL && r == BARTAP_EPUB_RESULT_OK);
    expect_field(b, BARTAP_EPUB_FIELD_TITLE, "Second Edition");
    expect_field(b, BARTAP_EPUB_FIELD_CREATOR, NULL);
    expect_cover(b, "OEBPS/images/front.jpg", BARTAP_EPUB_IMAGE_JPEG);
    bartap_epub_toc_t toc;
    REQUIRE(bartap_epub_toc_open(&toc, &b->book, b->window, sizeof b->window, b->input, sizeof b->input)
        == BARTAP_EPUB_RESULT_OK);
    bartap_epub_toc_entry_t entries[4];
    size_t count = 0;
    bool end = false;
    while (!end) {
        bool ready;
        REQUIRE(bartap_epub_toc_next(&toc, &entries[count < 3U ? count : 3U], &ready, &end) == BARTAP_EPUB_RESULT_OK);
        count += ready;
    }
    REQUIRE(count == 2U);
    EXPECT(strcmp(entries[0].label, "First & Foremost") == 0 && entries[0].depth == 1U);
    EXPECT(entries[0].target.spine_index == 0U && strcmp((const char *)entries[0].target.fragment, "start") == 0);
    EXPECT(strcmp(entries[1].label, "Nested") == 0 && entries[1].depth == 2U && entries[1].target.spine_index == 1U);
    test_book_free(b);
}

static void epub_nav_targets_and_fragments(void)
{
    bartap_epub_result_t r;
    test_book_t *b = test_book_new("epub3.epub", BARTAP_EPUB_OPEN_FULL, 0, &r);
    REQUIRE(b != NULL && r == BARTAP_EPUB_RESULT_OK);
    bartap_epub_toc_t toc;
    REQUIRE(bartap_epub_toc_open(&toc, &b->book, b->window, sizeof b->window, b->input, sizeof b->input)
        == BARTAP_EPUB_RESULT_OK);
    static const struct {
        const char *label;
        uint32_t depth;
        uint32_t spine;
        const char *fragment;
    } expected[] = {
        { "Chapter One", 1, 0, "one" }, { "The %20 note", 2, 0, "n1" }, { "Chapter Two", 1, 0, "two" },
        { "Legacy anchor", 1, 0, "legacy" }, { "The End", 1, 1, "" }, { "Encoded", 1, 2, "xAy" },
    };
    size_t count = 0;
    bool end = false;
    while (!end) {
        bartap_epub_toc_entry_t e;
        bool ready;
        REQUIRE(bartap_epub_toc_next(&toc, &e, &ready, &end) == BARTAP_EPUB_RESULT_OK);
        if (!ready) {
            continue;
        }
        REQUIRE(count < sizeof expected / sizeof expected[0]);  //the landmarks nav is not the table of contents
        EXPECT(strcmp(e.label, expected[count].label) == 0);
        EXPECT_EQ_U(e.depth, expected[count].depth);
        EXPECT_EQ_U(e.target.spine_index, expected[count].spine);
        EXPECT(strcmp((const char *)e.target.fragment, expected[count].fragment) == 0);
        count++;
    }
    EXPECT_EQ_U(count, sizeof expected / sizeof expected[0]);
    EXPECT_EQ_U(bartap_epub_toc_warnings(&toc), 0U);

    //every fragment leads to its element in the content document, through seek_id
    uint8_t *doc;
    size_t size;
    bartap_epub_location_t at;
    REQUIRE(bartap_epub_spine_location(&b->book, 0, &at) == BARTAP_EPUB_RESULT_OK);
    REQUIRE(test_book_read(b, &at, &doc, &size) == BARTAP_EPUB_RESULT_OK);
    static const char *const ids[] = { "one", "n1", "two", "legacy" };
    static const char *const words[] = { "One", "The", "Two", "A" };
    for (size_t i = 0; i < 4; i++) {
        bartap_text_t text;
        (void)bartap_text_init(&text, 0, 0);
        EXPECT_EQ_U(bartap_text_seek_id(&text, (const uint8_t *)ids[i], strlen(ids[i])), BARTAP_EPUB_RESULT_OK);
        size_t offset = 0;
        bool ready = false;
        bool done = false;
        bartap_text_item_t item;
        while (!ready && !done) {
            size_t used;
            REQUIRE(bartap_text_next(&text, doc + offset, size - offset, &used, true, &item, &ready, &done)
                == BARTAP_EPUB_RESULT_OK);
            offset += used;
        }
        EXPECT(ready && item.length == strlen(words[i]) && memcmp(item.text, words[i], item.length) == 0);
    }
    free(doc);
    test_book_free(b);
}

static void epub_missing_and_damaged_parts(void)
{
    bartap_epub_result_t r;
    test_book_t *b = test_book_new("dangling.epub", BARTAP_EPUB_OPEN_FULL, 0, &r);
    REQUIRE(b != NULL && r == BARTAP_EPUB_RESULT_OK);
    EXPECT_EQ_U(bartap_epub_warnings(&b->book), BARTAP_EPUB_WARNING_SPINE_ITEM_MISSING | BARTAP_EPUB_WARNING_NO_TOC);
    expect_cover(b, NULL, 0);
    bartap_epub_stream_t stream;
    EXPECT_EQ_U(bartap_epub_stream_open(&stream, &b->book, 1, 1000, b->window, sizeof b->window, b->input,
        sizeof b->input), BARTAP_EPUB_RESULT_NOT_FOUND);
    EXPECT_EQ_U(bartap_epub_stream_open(&stream, &b->book, 2, 1000, b->window, sizeof b->window, b->input,
        sizeof b->input), BARTAP_EPUB_RESULT_NOT_FOUND);
    bartap_epub_toc_t toc;
    EXPECT_EQ_U(bartap_epub_toc_open(&toc, &b->book, b->window, sizeof b->window, b->input, sizeof b->input),
        BARTAP_EPUB_RESULT_NOT_FOUND);

    //a location that is not an entry's: refused at the first read, never read out of bounds
    bartap_epub_location_t at;
    REQUIRE(bartap_epub_spine_location(&b->book, 0, &at) == BARTAP_EPUB_RESULT_OK);
    const bartap_epub_location_t forged[] = {
        { at.entry_offset + 1U, at.entry_index }, { at.entry_offset, 1000 }, { 0, 0 }, { UINT32_MAX - 3U, 0 },
    };
    for (size_t i = 0; i < sizeof forged / sizeof forged[0]; i++) {
        uint8_t *data;
        size_t size;
        EXPECT_EQ_U(test_book_read(b, &forged[i], &data, &size), BARTAP_EPUB_RESULT_CORRUPT);
        free(data);
    }
    //the stream's declared size is known once its first read has returned, and is what it delivers
    REQUIRE(bartap_epub_stream_open_at(&stream, &b->book, &at, 1U << 20, b->window, sizeof b->window, b->input,
        sizeof b->input) == BARTAP_EPUB_RESULT_OK);
    bartap_offset_t declared;
    EXPECT_EQ_U(bartap_epub_stream_size(&stream, &declared), BARTAP_EPUB_RESULT_INVALID_ARGUMENT);
    uint8_t buffer[4096];
    size_t got;
    bool end;
    EXPECT_EQ_U(bartap_epub_stream_read(&stream, buffer, sizeof buffer, &got, &end), BARTAP_EPUB_RESULT_OK);
    EXPECT_EQ_U(bartap_epub_stream_size(&stream, &declared), BARTAP_EPUB_RESULT_OK);
    size_t total = got;
    while (!end) {
        REQUIRE(bartap_epub_stream_read(&stream, buffer, sizeof buffer, &got, &end) == BARTAP_EPUB_RESULT_OK);
        total += got;
    }
    EXPECT_EQ_U(total, declared);
    //a cap below the declared size is refused before anything is decoded
    REQUIRE(bartap_epub_stream_open_at(&stream, &b->book, &at, declared - 1U, b->window, sizeof b->window, b->input,
        sizeof b->input) == BARTAP_EPUB_RESULT_OK);
    r = BARTAP_EPUB_RESULT_OK;
    for (int i = 0; i < 4 && r == BARTAP_EPUB_RESULT_OK; i++) {
        r = bartap_epub_stream_read(&stream, buffer, sizeof buffer, &got, &end);
    }
    EXPECT_EQ_U(r, BARTAP_EPUB_RESULT_LIMIT_EXCEEDED);
    test_book_free(b);
}

//the first of two entries with one name wins (D21)
static void epub_finder_takes_the_first(void)
{
    bartap_epub_result_t r;
    test_book_t *b = test_book_new("duplicate.epub", BARTAP_EPUB_OPEN_FULL, 0, &r);
    REQUIRE(b != NULL && r == BARTAP_EPUB_RESULT_OK);
    static const char css[] = "epub/style.css";
    bartap_epub_finder_t finder;
    REQUIRE(bartap_epub_find_begin(&finder, &b->book, (const uint8_t *)css, strlen(css)) == BARTAP_EPUB_RESULT_OK);
    bartap_epub_location_t at;
    bool done = false;
    while (!done) {
        REQUIRE(bartap_epub_find_step(&finder, &done, &at) == BARTAP_EPUB_RESULT_OK);
    }
    uint8_t *data;
    size_t size;
    EXPECT_EQ_U(test_book_read(b, &at, &data, &size), BARTAP_EPUB_RESULT_OK);
    EXPECT(strcmp((const char *)data, "p { text-indent: 1em; }\n") == 0);
    free(data);
    static const char missing[] = "epub/nothing.css";
    REQUIRE(bartap_epub_find_begin(&finder, &b->book, (const uint8_t *)missing, strlen(missing))
        == BARTAP_EPUB_RESULT_OK);
    r = BARTAP_EPUB_RESULT_OK;
    done = false;
    for (int steps = 0; r == BARTAP_EPUB_RESULT_OK && !done && steps < 1000; steps++) {
        r = bartap_epub_find_step(&finder, &done, &at);
    }
    EXPECT_EQ_U(r, BARTAP_EPUB_RESULT_NOT_FOUND);
    uint8_t long_path[BARTAP_EPUB_PATH_MAX + 1U];
    memset(long_path, 'a', sizeof long_path);
    EXPECT_EQ_U(bartap_epub_find_begin(&finder, &b->book, long_path, sizeof long_path),
        BARTAP_EPUB_RESULT_LIMIT_EXCEEDED);
    test_book_free(b);
}

//markup errors in the OPF are recovered from and reported; strict mode refuses them
static void epub_lenient_and_strict_markup(void)
{
    bartap_epub_result_t r;
    test_book_t *b = test_book_new("lenient.epub", BARTAP_EPUB_OPEN_FULL, 0, &r);
    REQUIRE(b != NULL && r == BARTAP_EPUB_RESULT_OK);
    EXPECT(bartap_epub_warnings(&b->book) & BARTAP_EPUB_WARNING_MARKUP_RECOVERED);
    expect_field(b, BARTAP_EPUB_FIELD_TITLE, "Lenient & Sloppy");
    test_book_free(b);
    b = test_book_new("lenient.epub", BARTAP_EPUB_OPEN_FULL, BARTAP_EPUB_OPEN_STRICT_MARKUP, &r);
    REQUIRE(b != NULL);
    EXPECT_EQ_U(r, BARTAP_EPUB_RESULT_CORRUPT);
    test_book_free(b);
    //a long attribute value that nothing needs whole opens, and so does a book with one in its content
    b = test_book_new("long-attributes.epub", BARTAP_EPUB_OPEN_FULL, BARTAP_EPUB_OPEN_STRICT_MARKUP, &r);
    REQUIRE(b != NULL);
    EXPECT_EQ_U(r, BARTAP_EPUB_RESULT_OK);
    test_book_free(b);
}

//the scratch buffer may be the window's memory, and the opener may be discarded once open has finished
static void epub_opening_memory(void)
{
    test_book_t *b = calloc(1, sizeof *b);
    REQUIRE(test_book_load(b, "epub3.epub"));
    const bartap_epub_memory_t memory = { b->window, sizeof b->window, b->window, sizeof b->window, b->input,
        sizeof b->input, b->spine, 64, 1U << 20, BARTAP_EPUB_OPEN_FULL, 0 };
    EXPECT_EQ_U(bartap_epub_open(&b->opener, &b->book, &b->source, &memory), BARTAP_EPUB_RESULT_OK);
    memset(&b->opener, 0xA5, sizeof b->opener);
    memset(b->scratch, 0xA5, sizeof b->scratch);
    expect_field(b, BARTAP_EPUB_FIELD_TITLE, "A Test Book");
    expect_cover(b, "epub/images/cover.png", BARTAP_EPUB_IMAGE_PNG);
    bartap_epub_toc_t toc;
    EXPECT_EQ_U(bartap_epub_toc_open(&toc, &b->book, b->window, sizeof b->window, b->input, sizeof b->input),
        BARTAP_EPUB_RESULT_OK);
    //too little memory: refused before any read
    bartap_epub_memory_t small = memory;
    small.scratch_size = BARTAP_EPUB_SCRATCH_MIN - 1U;
    EXPECT_EQ_U(bartap_epub_open_begin(&b->opener, &b->book, &b->source, &small), BARTAP_EPUB_RESULT_INVALID_ARGUMENT);
    small = memory;
    small.spine_capacity = 2;
    EXPECT_EQ_U(bartap_epub_open(&b->opener, &b->book, &b->source, &small), BARTAP_EPUB_RESULT_LIMIT_EXCEEDED);
    small = memory;
    small.max_document_size = 0;
    EXPECT_EQ_U(bartap_epub_open(&b->opener, &b->book, &b->source, &small), BARTAP_EPUB_RESULT_LIMIT_EXCEEDED);
    test_book_free(b);
}

static void expect_href(const char *base, const char *href, bartap_epub_result_t result, const char *path,
    const char *fragment)
{
    uint8_t out[64];
    size_t length;
    bartap_epub_target_t target;
    const bartap_epub_result_t r = bartap_epub_href_resolve(NULL, (const uint8_t *)base, strlen(base),
        (const uint8_t *)href, strlen(href), out, sizeof out, &length, &target);
    EXPECT_EQ_U(r, result);
    if (r == BARTAP_EPUB_RESULT_OK) {
        EXPECT(strcmp((const char *)out, path) == 0 && length == strlen(path));
        EXPECT(strcmp((const char *)target.fragment, fragment) == 0);
        EXPECT_EQ_U(target.spine_index, UINT32_MAX);
        if (strcmp((const char *)out, path) != 0) {
            printf("    %s + %s: got %s, expected %s\n", base, href, out, path);
        }
    }
}

static void epub_href_resolution(void)
{
    expect_href("a/b/c.xhtml", "d.xhtml", BARTAP_EPUB_RESULT_OK, "a/b/d.xhtml", "");
    expect_href("a/b/c.xhtml", "../d.xhtml#x", BARTAP_EPUB_RESULT_OK, "a/d.xhtml", "x");
    expect_href("a/b/c.xhtml", "./e/../f/./g.xhtml", BARTAP_EPUB_RESULT_OK, "a/b/f/g.xhtml", "");
    expect_href("a/b/c.xhtml", "#only", BARTAP_EPUB_RESULT_OK, "a/b/c.xhtml", "only");
    expect_href("a/b/c.xhtml", "", BARTAP_EPUB_RESULT_OK, "a/b/c.xhtml", "");
    expect_href("c.xhtml", "caf%C3%A9.xhtml#x%41y", BARTAP_EPUB_RESULT_OK, "caf\xC3\xA9.xhtml", "xAy");
    expect_href("c.xhtml", "d.xhtml?q=1#f", BARTAP_EPUB_RESULT_OK, "d.xhtml", "f");
    expect_href("c.xhtml", "d.xhtml#epubcfi(/4/2%20)", BARTAP_EPUB_RESULT_OK, "d.xhtml", "epubcfi(/4/2%20)");
    expect_href("a/c.xhtml", "../../x.xhtml", BARTAP_EPUB_RESULT_UNSUPPORTED, "", "");
    expect_href("a/c.xhtml", "http://example.com/x", BARTAP_EPUB_RESULT_UNSUPPORTED, "", "");
    expect_href("a/c.xhtml", "/x.xhtml", BARTAP_EPUB_RESULT_UNSUPPORTED, "", "");
    expect_href("a/c.xhtml", "x%2.xhtml", BARTAP_EPUB_RESULT_CORRUPT, "", "");
    expect_href("a/c.xhtml", "0123456789012345678901234567890123456789012345678901234567890123456789",
        BARTAP_EPUB_RESULT_LIMIT_EXCEEDED, "", "");
    //a fragment longer than BARTAP_EPUB_FRAGMENT_MAX is kept in part, and says so
    char href[160];
    snprintf(href, sizeof href, "x.xhtml#%080d", 0);
    uint8_t out[64];
    size_t length;
    bartap_epub_target_t target;
    EXPECT_EQ_U(bartap_epub_href_resolve(NULL, (const uint8_t *)"x.xhtml", 7, (const uint8_t *)href, strlen(href), out,
        sizeof out, &length, &target), BARTAP_EPUB_RESULT_OK);
    EXPECT(target.fragment_truncated && target.fragment_length == BARTAP_EPUB_FRAGMENT_MAX);
}

const test_case_t epub_tests[] = {
    TEST_CASE(epub_epub3_full),
    TEST_CASE(epub_open_modes_agree),
    TEST_CASE(epub_epub2_ncx_and_meta_cover),
    TEST_CASE(epub_nav_targets_and_fragments),
    TEST_CASE(epub_missing_and_damaged_parts),
    TEST_CASE(epub_finder_takes_the_first),
    TEST_CASE(epub_lenient_and_strict_markup),
    TEST_CASE(epub_opening_memory),
    TEST_CASE(epub_href_resolution),
};
const size_t epub_test_count = sizeof epub_tests / sizeof epub_tests[0];
