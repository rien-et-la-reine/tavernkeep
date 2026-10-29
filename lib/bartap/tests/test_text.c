// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "bartap_internal.h"

/* A stored chapter in a tiny in-memory archive. A complete ZIP is unnecessary for testing an
 * already-open book; the stream still validates its local header, size and CRC. */
typedef struct {
    uint8_t bytes[8192];
    uint8_t window[BARTAP_WINDOW_SIZE];
    bartap_source_t source;
    bartap_book_t book;
    bartap_spine_item_t spine;
    bartap_text_t text;
    size_t data_offset;
} text_fixture_t;

static void setup(text_fixture_t *f, const char *xhtml, bool memory)
{
    memset(f, 0, sizeof *f);
    static const char path[] = "OEBPS/text/chapter.xhtml";
    const size_t n = strlen(xhtml);
    f->data_offset = 30U + sizeof path - 1U;
    f->bytes[0] = 'P'; f->bytes[1] = 'K'; f->bytes[2] = 3; f->bytes[3] = 4;
    f->bytes[26] = sizeof path - 1U;
    memcpy(f->bytes + 30, path, sizeof path - 1U);
    memcpy(f->bytes + f->data_offset, xhtml, n);
    bartap_source_memory(&f->source, f->bytes, (uint32_t)(f->data_offset + n));
    f->book.source = &f->source;
    f->book.spine = &f->spine;
    f->book.spine_count = 1U;
    f->book.zip.cd_offset = f->source.size;
    f->spine.path_hash = bartap_hash(path, sizeof path - 1U);
    f->spine.path_len = sizeof path - 1U;
    f->spine.size = (uint32_t)n;
    f->spine.compressed_size = (uint32_t)n;
    f->spine.crc32 = bartap_crc32(0U, xhtml, n);
    EXPECT_EQ_U(memory ? bartap_text_open_memory(&f->text, &f->book, 0U,
        f->bytes + f->data_offset, n) : bartap_text_open(&f->text, &f->book, 0U, f->window), BARTAP_OK);
}

static bool same_token(const bartap_token_t *a, const bartap_token_t *b)
{
    return a->type == b->type && a->block == b->block && a->level == b->level
        && a->style == b->style && a->len == b->len && a->offset == b->offset
        && memcmp(a->text, b->text, a->len) == 0;
}

static void text_nested_styles_and_whitespace(void)
{
    text_fixture_t f;
    setup(&f, "<blockquote><p> \n<i>one<i>two</i>three</i> four<br/>\n\t five</p></blockquote>", true);
    static const bartap_tok_type_t types[] = {
        BARTAP_TOK_BLOCK_START, BARTAP_TOK_BLOCK_START, BARTAP_TOK_WORD, BARTAP_TOK_WORD,
        BARTAP_TOK_WORD, BARTAP_TOK_SPACE, BARTAP_TOK_WORD, BARTAP_TOK_LINE_BREAK,
        BARTAP_TOK_WORD, BARTAP_TOK_BLOCK_END, BARTAP_TOK_BLOCK_END
    };
    static const char words[][6] = { "one", "two", "three", "four", "five" };
    unsigned word = 0U;
    bartap_token_t t;
    for (size_t i = 0U; i < sizeof types / sizeof types[0]; i++) {
        REQUIRE(bartap_text_next(&f.text, &t) == BARTAP_OK);
        EXPECT_EQ_U(t.type, types[i]);
        if (t.type == BARTAP_TOK_WORD) {
            REQUIRE(word < 5U);
            EXPECT(strcmp(t.text, words[word]) == 0);
            EXPECT_EQ_U(t.style, word < 3U ? BARTAP_STYLE_ITALIC : 0U);
            word++;
        }
    }
    EXPECT_EQ_U(word, 5U);
    EXPECT_EQ_U(bartap_text_next(&f.text, &t), BARTAP_END);
    EXPECT_EQ_U(bartap_text_warnings(&f.text), 0U);
}

static void text_roundtrip_every_boundary(void)
{
    static const char doc[] = "bare &amp; text<p>\n<i>italic <i>twice</i> still</i> plain "
        "<img src='../images/pic.png'/> after</p><div><p>caf\xc3\xa9 &#x1f642;</p><hr/></div>"
        "<pre> x\r\ny\r z\t&amp;<![CDATA[more\nwords]]></pre>"
        "<p>abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz</p>"
        "<table><tr><td>cell</td><td>next</td></tr></table><section>loose</section>";
    for (unsigned mode = 0U; mode < 2U; mode++) {
        text_fixture_t f;
        setup(&f, doc, mode != 0U);
        bartap_token_t expected, actual;
        unsigned count = 0U, images = 0U;
        for (;;) {
            bartap_loc_t loc;
            REQUIRE(bartap_text_loc(&f.text, &loc) == BARTAP_OK);
            REQUIRE(loc.byte_offset <= strlen(doc));
            REQUIRE(loc.anchor_len <= strlen(doc) - loc.byte_offset);
            EXPECT(memcmp(loc.anchor, doc + loc.byte_offset, loc.anchor_len) == 0);
            bartap_block_context_t before[BARTAP_TEXT_DEPTH_MAX], after[BARTAP_TEXT_DEPTH_MAX];
            size_t nb, na;
            REQUIRE(bartap_text_context(&f.text, before, BARTAP_TEXT_DEPTH_MAX, &nb) == BARTAP_OK);
            const bartap_err_t end = bartap_text_next(&f.text, &expected);
            REQUIRE(end == BARTAP_OK || end == BARTAP_END);
            bartap_seek_t how;
            REQUIRE(bartap_text_seek(&f.text, &loc, &how) == BARTAP_OK);
            EXPECT_EQ_U(how, loc.anchor_len != 0U ? BARTAP_SEEK_EXACT : BARTAP_SEEK_OFFSET);
            REQUIRE(bartap_text_context(&f.text, after, BARTAP_TEXT_DEPTH_MAX, &na) == BARTAP_OK);
            REQUIRE(na == nb);
            for (size_t i = 0U; i < nb; i++) {
                EXPECT_EQ_U(before[i].block, after[i].block);
                EXPECT_EQ_U(before[i].level, after[i].level);
            }
            REQUIRE(bartap_text_next(&f.text, &actual) == end);
            if (end == BARTAP_END) {
                break;
            }
            EXPECT(same_token(&expected, &actual));
            if (actual.type == BARTAP_TOK_IMAGE) {
                EXPECT(strcmp(actual.text, "OEBPS/images/pic.png") == 0);
                images++;
            }
            REQUIRE(++count < 200U);
        }
        EXPECT(count > 30U);
        EXPECT_EQ_U(images, 1U);
    }
}

static void text_recovery_limits_and_fragments(void)
{
    text_fixture_t f;
    setup(&f, "<blockquote><p><i>a</p>b</blockquote></p><p id='target'>c</p>", true);
    bartap_token_t t;
    int blocks = 0;
    bartap_err_t err;
    while ((err = bartap_text_next(&f.text, &t)) == BARTAP_OK) {
        if (t.type == BARTAP_TOK_BLOCK_START) blocks++;
        if (t.type == BARTAP_TOK_BLOCK_END) blocks--;
        EXPECT(blocks >= 0);
    }
    EXPECT_EQ_U(err, BARTAP_END);
    EXPECT_EQ_U(blocks, 0U);
    EXPECT((bartap_text_warnings(&f.text) & BARTAP_WARN_MARKUP) != 0U);
    REQUIRE(bartap_text_seek_fragment(&f.text, "target") == BARTAP_OK);
    REQUIRE(bartap_text_next(&f.text, &t) == BARTAP_OK);
    EXPECT_EQ_U(t.type, BARTAP_TOK_BLOCK_START);
    REQUIRE(bartap_text_next(&f.text, &t) == BARTAP_OK);
    EXPECT(strcmp(t.text, "c") == 0);
    EXPECT((bartap_text_warnings(&f.text) & BARTAP_WARN_MARKUP) != 0U); // sticky over seek
    EXPECT_EQ_U(bartap_text_seek_fragment(&f.text, "absent"), BARTAP_ERR_NOT_FOUND);
    EXPECT_EQ_U(bartap_text_seek_fragment(&f.text, ""), BARTAP_OK);
}

/* Reads the whole document, checking that blocks balance; returns the tokens' words joined by '|'
 * ('_' for SPACE, '^' for an italic word, '[' / ']' for blocks, '@' for an image). */
static bartap_err_t drain(text_fixture_t *f, char *out, size_t size)
{
    bartap_token_t t;
    bartap_err_t err;
    int depth = 0;
    out[0] = '\0';
    while ((err = bartap_text_next(&f->text, &t)) == BARTAP_OK) {
        const char *piece = t.type == BARTAP_TOK_SPACE ? "_" : t.type == BARTAP_TOK_BLOCK_START ? "["
            : t.type == BARTAP_TOK_BLOCK_END ? "]" : t.type == BARTAP_TOK_IMAGE ? "@" : t.text;
        depth += t.type == BARTAP_TOK_BLOCK_START ? 1 : t.type == BARTAP_TOK_BLOCK_END ? -1 : 0;
        EXPECT(depth >= 0);
        if (strlen(out) + strlen(piece) + 3U < size) {
            if ((t.style & BARTAP_STYLE_ITALIC) != 0U) strcat(out, "^");
            strcat(out, piece);
            strcat(out, "|");
        }
    }
    EXPECT_EQ_U(depth, 0U);
    return err;
}

/* Lenient (D19): nesting past the frame stack is ignored with a warning, never fatal. */
static void text_depth_overflow_degrades(void)
{
    text_fixture_t f;
    char doc[1024] = "", out[512];
    for (unsigned i = 0U; i < BARTAP_TEXT_DEPTH_MAX + 8U; i++) strcat(doc, "<div>");
    strcat(doc, "deep");
    for (unsigned i = 0U; i < BARTAP_TEXT_DEPTH_MAX + 8U; i++) strcat(doc, "</div>");
    strcat(doc, "<p>after</p>");
    setup(&f, doc, true);
    EXPECT_EQ_U(drain(&f, out, sizeof out), BARTAP_END);
    EXPECT(strstr(out, "deep|") != NULL);
    // every ignored </div> was absorbed: "after" opens one paragraph at the top level
    EXPECT(strstr(out, "]|[|after|]|") != NULL && strstr(out, "after|]|]") == NULL);
    EXPECT((bartap_text_warnings(&f.text) & BARTAP_WARN_MARKUP) != 0U);

    // Unclosed inline elements: the paragraph's end closes the ignored ones too, so a later real
    // </i> is not absorbed by a stale count.
    strcpy(doc, "<p>");
    for (unsigned i = 0U; i < BARTAP_TEXT_DEPTH_MAX + 8U; i++) strcat(doc, "<i>");
    strcat(doc, "x</p><p><i>z</i> w</p>");
    setup(&f, doc, true);
    EXPECT_EQ_U(drain(&f, out, sizeof out), BARTAP_END);
    EXPECT(strcmp(out, "[|^x|]|[|^z|_|w|]|") == 0);
    EXPECT((bartap_text_warnings(&f.text) & BARTAP_WARN_MARKUP) != 0U);

    // Overflow starting outside any block: the implicit paragraph opens first, and blocks still balance.
    doc[0] = '\0';
    for (unsigned i = 0U; i < BARTAP_TEXT_DEPTH_MAX + 8U; i++) strcat(doc, "<b>");
    strcat(doc, "y");
    setup(&f, doc, true);
    EXPECT_EQ_U(drain(&f, out, sizeof out), BARTAP_END);
    EXPECT(strcmp(out, "[|y|]|") == 0);
}

static void text_structural_boundaries(void)
{
    text_fixture_t f;
    char out[256];
    setup(&f, "<table><tr><td>alpha</td><td>beta</td></tr><tr><th>x</th><td>y</td></tr></table>", true);
    EXPECT_EQ_U(drain(&f, out, sizeof out), BARTAP_END);
    EXPECT(strcmp(out, "[|alpha|_|beta|]|[|x|_|y|]|") == 0);
    setup(&f, "<section>one</section><section>two</section>", true);
    EXPECT_EQ_U(drain(&f, out, sizeof out), BARTAP_END);
    EXPECT(strcmp(out, "[|one|]|[|two|]|") == 0);
    // an image outside any block gets the implicit paragraph text would get
    setup(&f, "<section><h1>T</h1><img src='../images/t.png'/></section>", false);
    EXPECT_EQ_U(drain(&f, out, sizeof out), BARTAP_END);
    EXPECT(strcmp(out, "[|T|]|[|@|]|") == 0);
    EXPECT_EQ_U(bartap_text_warnings(&f.text), 0U);
}

static void text_seek_moved_and_extreme_offsets(void)
{
    text_fixture_t f;
    const char *original = "<p>one two three</p>";
    setup(&f, original, true);
    bartap_token_t t;
    REQUIRE(bartap_text_next(&f.text, &t) == BARTAP_OK);
    REQUIRE(bartap_text_next(&f.text, &t) == BARTAP_OK);
    bartap_loc_t loc;
    REQUIRE(bartap_text_loc(&f.text, &loc) == BARTAP_OK);
    REQUIRE(bartap_text_next(&f.text, &t) == BARTAP_OK);
    const bartap_token_t expected = t;
    setup(&f, "<!--x--><p>one two three</p>", true);
    bartap_seek_t how;
    REQUIRE(bartap_text_seek(&f.text, &loc, &how) == BARTAP_OK);
    EXPECT_EQ_U(how, BARTAP_SEEK_MOVED);
    REQUIRE(bartap_text_next(&f.text, &t) == BARTAP_OK);
    EXPECT_EQ_U(t.type, expected.type);
    EXPECT(strcmp(t.text, expected.text) == 0);
    loc.byte_offset = UINT32_MAX;
    REQUIRE(bartap_text_seek(&f.text, &loc, &how) == BARTAP_OK);
    EXPECT_EQ_U(how, BARTAP_SEEK_OFFSET);
    EXPECT_EQ_U(bartap_text_next(&f.text, &t), BARTAP_END);
}

static int failed_read(void *user, uint32_t offset, void *buf, size_t len)
{
    (void)user; (void)offset; (void)buf; (void)len;
    return -1;
}

static void text_errors_and_detached_memory(void)
{
    text_fixture_t f;
    bartap_token_t token;
    setup(&f, "<p>one two</p>", false);
    f.text.stream.entry.crc32 ^= 1U;
    bartap_err_t err;
    do {
        err = bartap_text_next(&f.text, &token);
    } while (err == BARTAP_OK);
    EXPECT_EQ_U(err, BARTAP_ERR_CORRUPT);
    setup(&f, "<p>one two</p>", false);
    f.source.read = failed_read;
    EXPECT_EQ_U(bartap_text_next(&f.text, &token), BARTAP_ERR_IO);

    setup(&f, "<p>a <img src='../../../../bad'/> b</p>", true);
    f.source.read = failed_read; // PSRAM iteration/seek must not consult the archive
    unsigned spaces = 0U;
    while ((err = bartap_text_next(&f.text, &token)) == BARTAP_OK) {
        if (token.type == BARTAP_TOK_SPACE) spaces++;
    }
    EXPECT_EQ_U(err, BARTAP_END);
    EXPECT_EQ_U(spaces, 1U);
    EXPECT((bartap_text_warnings(&f.text) & BARTAP_WARN_MARKUP) != 0U);
    EXPECT_EQ_U(bartap_text_seek_fragment(&f.text, ""), BARTAP_OK);
    EXPECT_EQ_U(bartap_text_next(&f.text, &token), BARTAP_OK);

    // APPNOTE 4.3.7: a local header starts with PK\3\4. Without it, the name read for the image
    // directory is not a name, in either mode.
    setup(&f, "<p>one</p>", true);
    f.bytes[3] = 5;
    EXPECT_EQ_U(bartap_text_open_memory(&f.text, &f.book, 0U, f.bytes + f.data_offset, 10U),
        BARTAP_ERR_CORRUPT);
    EXPECT_EQ_U(bartap_text_open(&f.text, &f.book, 0U, f.window), BARTAP_ERR_CORRUPT);
}

const test_case_t text_tests[] = {
    TEST_CASE(text_depth_overflow_degrades),
    TEST_CASE(text_structural_boundaries),
    TEST_CASE(text_errors_and_detached_memory),
    TEST_CASE(text_nested_styles_and_whitespace),
    TEST_CASE(text_roundtrip_every_boundary),
    TEST_CASE(text_recovery_limits_and_fragments),
    TEST_CASE(text_seek_moved_and_extreme_offsets),
};
const size_t text_test_count = sizeof text_tests / sizeof text_tests[0];
