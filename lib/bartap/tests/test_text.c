// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//the text layer (bartap/text.h): items, breaks and styling from element names; element and attribute items
//and the override window; seeks by position and by id; snapshots restored at every call boundary. items are
//flattened into tokens so that a whole document's compare in one check:
//  WORD     before mark (= none, _ space, | line, # paragraph), text, then {style} ^heading >indent @user
//  RULE     ~ with the same marks
//  ELEMENT_START <name, ATTRIBUTE .name=value, ELEMENT_OPEN +name, ELEMENT_END -name

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bartap/text.h"

#include "harness.h"

typedef struct {
    char tokens[16384];
    size_t count;
    bartap_epub_result_t result;
    size_t consumed;                //document bytes taken
    uint64_t calls;
} items_t;

static void token(items_t *out, const bartap_text_item_t *item)
{
    char t[512];
    int n = 0;
    static const char before[] = "=_|#";
    switch (item->kind) {
    case BARTAP_TEXT_ITEM_WORD:
        n = snprintf(t, sizeof t, "%c%.*s", before[item->before & 3U], (int)item->length, (const char *)item->text);
        break;
    case BARTAP_TEXT_ITEM_RULE:
        n = snprintf(t, sizeof t, "~");
        break;
    case BARTAP_TEXT_ITEM_ELEMENT_START:
        n = snprintf(t, sizeof t, "<%.*s", (int)item->length, (const char *)item->text);
        break;
    case BARTAP_TEXT_ITEM_ATTRIBUTE:
        n = snprintf(t, sizeof t, ".%.*s=%.*s", (int)item->length, (const char *)item->text, (int)item->value_length,
            (const char *)item->value);
        break;
    case BARTAP_TEXT_ITEM_ELEMENT_OPEN:
        n = snprintf(t, sizeof t, "+%.*s", (int)item->length, (const char *)item->text);
        break;
    default:
        n = snprintf(t, sizeof t, "-%.*s", (int)item->length, (const char *)item->text);
        break;
    }
    if (item->kind <= BARTAP_TEXT_ITEM_RULE) {
        if (item->style != 0U) {
            n += snprintf(t + n, sizeof t - (size_t)n, "{%x}", item->style);
        }
        if (item->heading != 0U) {
            n += snprintf(t + n, sizeof t - (size_t)n, "^%u", item->heading);
        }
        if (item->indent != 0U) {
            n += snprintf(t + n, sizeof t - (size_t)n, ">%u", item->indent);
        }
        if (item->user != 0U) {
            n += snprintf(t + n, sizeof t - (size_t)n, "@%u", item->user);
        }
    }
    const size_t length = strlen(out->tokens);
    snprintf(out->tokens + length, sizeof out->tokens - length, "%s%s", length > 0U ? " " : "", t);
    out->count++;
}

//feed doc from offset, piece bytes a call, until the end, an error, or stop calls; tokens appended to out.
//on_item, if set, is called for every item, and may override
typedef void (*on_item_t)(bartap_text_t *text, const bartap_text_item_t *item, void *context);

static void drive(bartap_text_t *text, const char *doc, size_t offset, size_t piece, uint64_t stop, items_t *out,
    on_item_t on_item, void *context)
{
    const size_t size = strlen(doc);
    out->result = BARTAP_EPUB_RESULT_OK;
    bool end = false;
    for (uint64_t calls = 0; !end && calls < stop && calls < 1000000U; calls++) {
        const size_t n = size - offset < piece ? size - offset : piece;
        size_t used;
        bartap_text_item_t item;
        bool ready;
        out->result = bartap_text_next(text, (const uint8_t *)doc + offset, n, &used, offset + n == size, &item, &ready,
            &end);
        offset += used;
        out->calls++;
        if (ready) {
            token(out, &item);
            if (on_item != NULL) {
                on_item(text, &item, context);
            }
        }
        if (out->result != BARTAP_EPUB_RESULT_OK) {
            break;
        }
    }
    out->consumed = offset;
}

static void read_all(const char *doc, uint32_t flags, size_t piece, items_t *out)
{
    memset(out, 0, sizeof *out);
    bartap_text_t text;
    (void)bartap_text_init(&text, 4, flags);
    drive(&text, doc, 0, piece, UINT64_MAX, out, NULL, NULL);
}

static void expect_tokens(const char *doc, uint32_t flags, const char *expected)
{
    items_t out;
    read_all(doc, flags, 7, &out);
    EXPECT_EQ_U(out.result, BARTAP_EPUB_RESULT_OK);
    EXPECT(strcmp(out.tokens, expected) == 0);
    if (strcmp(out.tokens, expected) != 0) {
        printf("    got:      %s\n    expected: %s\n", out.tokens, expected);
    }
}

#define XHTML_START "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>T</title><style>p{}</style></head><body>"
#define XHTML_END "</body></html>"

static void text_words_breaks_and_styles(void)
{
    expect_tokens(XHTML_START "<h1>Title</h1><p>The <em>quick</em> brown<br/>fox&#x2019;s</p><hr/>"
        "<blockquote><p>q</p></blockquote><ul><li>a <b>b<i>c</i></b></li></ul>"
        "<p>x<sup>2</sup> <code>c</code> <u>u</u><s>s</s> <script>dropped()</script>end</p>" XHTML_END, 0,
        "#Title^1 #The _quick{1} _brown |fox\xE2\x80\x99s ~ #q>1 #a>1 _b{2}>1 =c{3}>1 #x =2{8} _c{4} _u{20} =s{40} _end");
    //a word longer than BARTAP_TEXT_WORD_MAX is split, the rest joined to it (before none); entities and a
    //CDATA section are text like any other
    char doc[512];
    snprintf(doc, sizeof doc, XHTML_START "<p>%s&amp;<![CDATA[<x>]]> y</p>" XHTML_END,
        "0123456789012345678901234567890123456789012345678901234567890123456789");
    expect_tokens(doc, 0, "#0123456789012345678901234567890123456789012345678901234567890123 =456789&<x> _y");
}

static void text_element_items(void)
{
    const char *doc = XHTML_START "<p class=\"c\" id=\"i\">a <b>b</b><br/></p>" XHTML_END;
    expect_tokens(doc, BARTAP_TEXT_DELIVER_ELEMENTS | BARTAP_TEXT_DELIVER_ATTRIBUTES,
        "<html .xmlns=http://www.w3.org/1999/xhtml +html <head +head <title +title -title <style +style -style -head "
        "<body +body <p .class=c .id=i +p #a <b +b _b{2} -b <br +br -br -p -body -html");
    //without element items, the same words: element items only add calls
    items_t with;
    items_t without;
    read_all(doc, BARTAP_TEXT_DELIVER_ELEMENTS, 1, &with);
    read_all(doc, 0, 1, &without);
    EXPECT(strstr(with.tokens, "#a") != NULL && strstr(with.tokens, "_b{2}") != NULL);
    EXPECT(strcmp(without.tokens, "#a _b{2}") == 0);
    EXPECT(with.calls > without.calls);
}

//overrides keyed on the class attribute, as a caller mapping CSS classes would
typedef struct {
    char class_name[32];
    bartap_epub_result_t misplaced; //an override attempted where it is not allowed
} classes_t;

static void apply_class(bartap_text_t *text, const bartap_text_item_t *item, void *context)
{
    classes_t *c = context;
    if (item->kind == BARTAP_TEXT_ITEM_ATTRIBUTE && item->length == 5U && memcmp(item->text, "class", 5) == 0) {
        snprintf(c->class_name, sizeof c->class_name, "%.*s", (int)item->value_length, (const char *)item->value);
        bartap_text_override_t o;
        memset(&o, 0, sizeof o);
        c->misplaced = bartap_text_override(text, &o);  //not yet: the start tag is not complete
        return;
    }
    if (item->kind != BARTAP_TEXT_ITEM_ELEMENT_OPEN) {
        return;
    }
    bartap_text_override_t o;
    memset(&o, 0, sizeof o);
    const char *k = c->class_name;
    if (strcmp(k, "hidden") == 0) {
        o.display = BARTAP_TEXT_DISPLAY_NONE;
    } else if (strcmp(k, "inline") == 0) {
        o.display = BARTAP_TEXT_DISPLAY_INLINE;
    } else if (strcmp(k, "block") == 0) {
        o.display = BARTAP_TEXT_DISPLAY_BLOCK;
    } else if (strcmp(k, "upright") == 0) {
        o.style_clear = BARTAP_TEXT_STYLE_ITALIC;
        o.style_set = BARTAP_TEXT_STYLE_BOLD;
    } else if (strcmp(k, "tagged") == 0) {
        o.set_user = true;
        o.user = 7;
    } else if (strcmp(k, "flat") == 0) {
        o.indent = -1;
        o.heading = 7;
    }
    c->class_name[0] = '\0';
    EXPECT_EQ_U(bartap_text_override(text, &o), BARTAP_EPUB_RESULT_OK);
}

static void text_overrides(void)
{
    const char *doc = XHTML_START "<p>start</p><p class=\"hidden\">gone <b>too</b></p>"
        "<p>a<span class=\"block\">mid</span>b<div class=\"inline\">c</div>d</p>"
        "<p><em class=\"upright\">e</em></p><div class=\"tagged\"><p>f <i>g</i></p></div>"
        "<blockquote class=\"flat\"><h2 class=\"flat\">h</h2></blockquote><hr class=\"hidden\"/><head class=\"block\"><title>t</title></head>"
        XHTML_END;
    items_t out;
    memset(&out, 0, sizeof out);
    classes_t classes = { "", BARTAP_EPUB_RESULT_OK };
    bartap_text_t text;
    (void)bartap_text_init(&text, 0, BARTAP_TEXT_DELIVER_ELEMENTS | BARTAP_TEXT_DELIVER_ATTRIBUTES);
    drive(&text, doc, 0, 5, UINT64_MAX, &out, apply_class, &classes);
    EXPECT_EQ_U(out.result, BARTAP_EPUB_RESULT_OK);
    EXPECT_EQ_U(classes.misplaced, BARTAP_EPUB_RESULT_INVALID_ARGUMENT);
    //only the words and rules: the element items are as expected by construction
    char words[1024] = "";
    for (char *t = strtok(out.tokens, " "); t != NULL; t = strtok(NULL, " ")) {
        if (strchr("=_|#~", t[0]) != NULL) {
            strcat(words, words[0] != '\0' ? " " : "");
            strcat(words, t);
        }
    }
    const char *expected = "#start #a #mid #b =c =d #e{2} #f@7 _g{1}@7 #h #t";
    EXPECT(strcmp(words, expected) == 0);
    if (strcmp(words, expected) != 0) {
        printf("    got:      %s\n    expected: %s\n", words, expected);
    }
    //an override is refused anywhere but straight after ELEMENT_OPEN
    bartap_text_override_t o;
    memset(&o, 0, sizeof o);
    EXPECT_EQ_U(bartap_text_override(&text, &o), BARTAP_EPUB_RESULT_INVALID_ARGUMENT);
}

static const char rich[] = XHTML_START "<section id=\"one\"><h1>One</h1><p>The <em>quick</em> fox&#x2019;s den &amp; "
    "its <strong>bold <i>nested</i></strong> run.\r\nMore&nbsp;text,\rand <![CDATA[CDATA <here>]]> too.</p>"
    "<!-- a comment --><p>Astral \xF0\x9F\x98\x80 and caf&eacute; then<br/>a line<hr/>and a rule.</p></section>"
    "<section id=\"two\"><p><a name=\"anchor\"></a>Second section, longer: one two three four five six seven "
    "eight nine ten eleven twelve thirteen fourteen.</p><div name=\"divname\"><p>Last.</p></div></section>" XHTML_END;

static void text_pieces_do_not_matter(void)
{
    items_t whole;
    read_all(rich, BARTAP_TEXT_DELIVER_ELEMENTS | BARTAP_TEXT_DELIVER_ATTRIBUTES, sizeof rich, &whole);
    REQUIRE(whole.result == BARTAP_EPUB_RESULT_OK);
    for (size_t piece = 1; piece <= 17; piece++) {
        items_t out;
        read_all(rich, BARTAP_TEXT_DELIVER_ELEMENTS | BARTAP_TEXT_DELIVER_ATTRIBUTES, piece, &out);
        EXPECT(strcmp(out.tokens, whole.tokens) == 0);
        EXPECT_EQ_U(out.consumed, strlen(rich));
    }
}

//every item's position resolves to where it is, and a seek to it from the start lands on it
typedef struct {
    bartap_epub_cfi_t cfis[512];
    char tokens[512][96];
    size_t count;
} positions_t;

static void collect_position(bartap_text_t *text, const bartap_text_item_t *item, void *context)
{
    positions_t *p = context;
    if (p->count < 512U && bartap_text_position(text, &p->cfis[p->count]) == BARTAP_EPUB_RESULT_OK) {
        items_t one;
        memset(&one, 0, sizeof one);
        token(&one, item);
        snprintf(p->tokens[p->count], sizeof p->tokens[0], "%.*s", (int)sizeof p->tokens[0] - 1, one.tokens);
        p->count++;
    }
}

static void first_item(bartap_text_t *text, const bartap_text_item_t *item, void *context)
{
    (void)text;
    items_t *first = context;
    if (first->count == 0U) {
        token(first, item);
    }
}

static void text_positions_resolve_and_seek(void)
{
    const uint32_t flags = BARTAP_TEXT_DELIVER_ELEMENTS | BARTAP_TEXT_DELIVER_ATTRIBUTES;
    positions_t *p = calloc(1, sizeof *p);
    items_t all;
    memset(&all, 0, sizeof all);
    bartap_text_t text;
    (void)bartap_text_init(&text, 4, flags);
    drive(&text, rich, 0, 9, UINT64_MAX, &all, collect_position, p);
    REQUIRE(all.result == BARTAP_EPUB_RESULT_OK && p->count == all.count);
    for (size_t i = 0; i < p->count; i++) {
        //resolving it gives a byte position; for a word in text without entities, CRs or CDATA there, its bytes
        bartap_epub_resolver_t resolver;
        REQUIRE(bartap_epub_cfi_resolver_init(&resolver, &p->cfis[i], 0) == BARTAP_EPUB_RESULT_OK);
        size_t offset = 0;
        bool done = false;
        uint32_t position = 0;
        bartap_epub_result_t r = BARTAP_EPUB_RESULT_OK;
        while (!done && r == BARTAP_EPUB_RESULT_OK) {
            size_t used;
            const size_t n = sizeof rich - 1U - offset < 11U ? sizeof rich - 1U - offset : 11U;
            r = bartap_epub_cfi_resolve_next(&resolver, (const uint8_t *)rich + offset, n, &used,
                offset + n == sizeof rich - 1U, &done, &position);
            offset += used;
        }
        EXPECT_EQ_U(r, BARTAP_EPUB_RESULT_OK);
        EXPECT(done);
        const char *t = p->tokens[i];
        if (strchr("=_|#", t[0]) != NULL && strchr(t, '&') == NULL && strchr(t, '\xE2') == NULL
                && strchr(t, '\xC2') == NULL && strchr(t, '\xC3') == NULL) {
            //the word's own bytes; or, in a run of text holding an entity or a CR LF, the run's start, with the
            //word later in the same run
            char word[96];
            snprintf(word, sizeof word, "%.*s", (int)strcspn(t + 1, "{^>@"), t + 1);
            const char *found = strstr(rich + position, word);
            EXPECT(found != NULL && (found == rich + position || memchr(rich + position, '<',
                (size_t)(found - (rich + position))) == NULL));
        } else if (strchr("<+-~", t[0]) != NULL) {
            EXPECT(rich[position] == '<');
        }
        //a seek to it delivers first an item at the same position: this one, for a word
        items_t first;
        memset(&first, 0, sizeof first);
        bartap_text_t seeking;
        (void)bartap_text_init(&seeking, 4, flags);
        EXPECT_EQ_U(bartap_text_seek(&seeking, &p->cfis[i]), BARTAP_EPUB_RESULT_OK);
        positions_t *landed = calloc(1, sizeof *landed);
        drive(&seeking, rich, 0, 13, UINT64_MAX, &first, collect_position, landed);
        EXPECT(landed->count > 0U && bartap_epub_cfi_compare(&landed->cfis[0], &p->cfis[i]) == 0);
        if (strchr("=_|#", t[0]) != NULL) {
            EXPECT(landed->count > 0U && strcmp(landed->tokens[0], t) == 0);
        }
        free(landed);
    }
    //a position past the end: nothing to land on
    bartap_epub_cfi_t past;
    REQUIRE(bartap_epub_cfi_parse("epubcfi(/6/10!/4/99)", &past) == BARTAP_EPUB_RESULT_OK);
    items_t none;
    memset(&none, 0, sizeof none);
    (void)bartap_text_init(&text, 4, 0);
    EXPECT_EQ_U(bartap_text_seek(&text, &past), BARTAP_EPUB_RESULT_OK);
    drive(&text, rich, 0, 64, UINT64_MAX, &none, NULL, NULL);
    EXPECT_EQ_U(none.result, BARTAP_EPUB_RESULT_NOT_FOUND);
    EXPECT_EQ_U(none.count, 0U);
    free(p);
}

static void expect_seek_id(const char *id, uint32_t flags, bartap_epub_result_t result, const char *first_token)
{
    bartap_text_t text;
    (void)bartap_text_init(&text, 0, flags);
    EXPECT_EQ_U(bartap_text_seek_id(&text, (const uint8_t *)id, strlen(id)), BARTAP_EPUB_RESULT_OK);
    items_t first;
    memset(&first, 0, sizeof first);
    drive(&text, rich, 0, 8, UINT64_MAX, &first, first_item, &first);
    EXPECT_EQ_U(first.result, result);
    EXPECT(strncmp(first.tokens, first_token, strlen(first_token)) == 0);
    if (strncmp(first.tokens, first_token, strlen(first_token)) != 0) {
        printf("    seek_id(%s): first %s, expected %s\n", id, first.tokens, first_token);
    }
}

static void text_seek_id(void)
{
    expect_seek_id("two", 0, BARTAP_EPUB_RESULT_OK, "#Second");
    expect_seek_id("two", BARTAP_TEXT_DELIVER_ELEMENTS, BARTAP_EPUB_RESULT_OK, "+section");
    expect_seek_id("anchor", 0, BARTAP_EPUB_RESULT_OK, "#Second");     //<a name>, as older books mark anchors
    expect_seek_id("anchor", BARTAP_TEXT_DELIVER_ELEMENTS, BARTAP_EPUB_RESULT_OK, "+a");
    expect_seek_id("divname", 0, BARTAP_EPUB_RESULT_NOT_FOUND, "");   //name counts on an a only
    expect_seek_id("missing", 0, BARTAP_EPUB_RESULT_NOT_FOUND, "");
}

//saved at any call boundary and restored into another object, which is fed differently, the rest of the
//items are exactly the ones the original goes on to deliver
static void text_snapshots_restore_anywhere(void)
{
    const uint32_t flags = BARTAP_TEXT_DELIVER_ELEMENTS | BARTAP_TEXT_DELIVER_ATTRIBUTES;
    items_t *whole = calloc(1, sizeof *whole);
    read_all(rich, flags, 5, whole);
    REQUIRE(whole->result == BARTAP_EPUB_RESULT_OK);
    size_t largest = 0;
    for (uint64_t k = 0; k <= whole->calls; k++) {
        bartap_text_t original;
        (void)bartap_text_init(&original, 4, flags);
        items_t *before = calloc(1, sizeof *before);
        drive(&original, rich, 0, 5, k, before, NULL, NULL);
        uint8_t snapshot[BARTAP_TEXT_SNAPSHOT_MAX];
        size_t length;
        REQUIRE(bartap_text_save(&original, snapshot, sizeof snapshot, &length) == BARTAP_EPUB_RESULT_OK);
        largest = length > largest ? length : largest;
        bartap_text_t copy;
        (void)bartap_text_init(&copy, 4, flags);
        bartap_offset_t resume_at;
        REQUIRE(bartap_text_restore(&copy, snapshot, length, &resume_at) == BARTAP_EPUB_RESULT_OK);
        EXPECT(resume_at <= before->consumed);
        items_t *after = calloc(1, sizeof *after);
        drive(&copy, rich, resume_at, 3, UINT64_MAX, after, NULL, NULL);
        EXPECT_EQ_U(after->result, BARTAP_EPUB_RESULT_OK);
        //what came before, then what came after, is the whole
        char joined[sizeof whole->tokens * 2];
        snprintf(joined, sizeof joined, "%s%s%s", before->tokens, before->tokens[0] && after->tokens[0] ? " " : "",
            after->tokens);
        EXPECT(strcmp(joined, whole->tokens) == 0);
        if (strcmp(joined, whole->tokens) != 0) {
            printf("    after call %llu (resume at %u):\n    got:      %s\n    expected: %s\n",
                (unsigned long long)k, resume_at, joined, whole->tokens);
            free(before);
            free(after);
            break;
        }
        free(before);
        free(after);
    }
    printf("    largest snapshot: %zu bytes\n", largest);
    EXPECT(largest <= BARTAP_TEXT_SNAPSHOT_MAX);
    free(whole);
}

//a snapshot is untrusted input: damaged, from another version or for another text, it is refused, and the
//text it was offered to is left as it was
static void text_snapshots_are_checked(void)
{
    bartap_text_t text;
    (void)bartap_text_init(&text, 4, 0);
    items_t some;
    memset(&some, 0, sizeof some);
    drive(&text, rich, 0, 50, 12, &some, NULL, NULL);
    uint8_t snapshot[BARTAP_TEXT_SNAPSHOT_MAX];
    size_t length;
    REQUIRE(bartap_text_save(&text, snapshot, sizeof snapshot, &length) == BARTAP_EPUB_RESULT_OK);
    size_t small;
    EXPECT_EQ_U(bartap_text_save(&text, snapshot, 16, &small), BARTAP_EPUB_RESULT_LIMIT_EXCEEDED);
    REQUIRE(bartap_text_save(&text, snapshot, sizeof snapshot, &length) == BARTAP_EPUB_RESULT_OK);

    bartap_text_t other;
    bartap_offset_t at;
    (void)bartap_text_init(&other, 5, 0);
    EXPECT_EQ_U(bartap_text_restore(&other, snapshot, length, &at), BARTAP_EPUB_RESULT_INVALID_ARGUMENT);
    (void)bartap_text_init(&other, 4, BARTAP_TEXT_STRICT);
    EXPECT_EQ_U(bartap_text_restore(&other, snapshot, length, &at), BARTAP_EPUB_RESULT_INVALID_ARGUMENT);
    (void)bartap_text_init(&other, 4, 0);
    EXPECT_EQ_U(bartap_text_restore(&other, snapshot, length - 1U, &at), BARTAP_EPUB_RESULT_CORRUPT);
    for (size_t i = 0; i < length; i++) {
        uint8_t damaged[BARTAP_TEXT_SNAPSHOT_MAX];
        memcpy(damaged, snapshot, length);
        damaged[i] ^= 0x40U;
        EXPECT_EQ_U(bartap_text_restore(&other, damaged, length, &at), BARTAP_EPUB_RESULT_CORRUPT);
    }
    //a failed restore leaves the text fresh: it reads the document from the start
    items_t fresh;
    memset(&fresh, 0, sizeof fresh);
    drive(&other, rich, 0, 50, UINT64_MAX, &fresh, NULL, NULL);
    items_t reference;
    read_all(rich, 0, 50, &reference);
    EXPECT(strcmp(fresh.tokens, reference.tokens) == 0);

    //a copy of the whole object, put back into the same object, carries on as if nothing happened
    bartap_text_t *saved = malloc(sizeof *saved);
    memcpy(saved, &text, sizeof text);
    items_t first;
    memset(&first, 0, sizeof first);
    drive(&text, rich, some.consumed, 50, UINT64_MAX, &first, NULL, NULL);
    memcpy(&text, saved, sizeof text);
    items_t second;
    memset(&second, 0, sizeof second);
    drive(&text, rich, some.consumed, 50, UINT64_MAX, &second, NULL, NULL);
    EXPECT(strcmp(first.tokens, second.tokens) == 0);
    free(saved);
}

static void text_strict_and_recovered(void)
{
    const char *bad = XHTML_START "<p>a &bogus; <b>b</p>c" XHTML_END;
    items_t lenient;
    read_all(bad, 0, 4, &lenient);
    EXPECT_EQ_U(lenient.result, BARTAP_EPUB_RESULT_OK);
    EXPECT(strcmp(lenient.tokens, "#a _&bogus; _b{2} #c") == 0);
    bartap_text_t text;
    (void)bartap_text_init(&text, 0, 0);
    items_t again;
    memset(&again, 0, sizeof again);
    drive(&text, bad, 0, 4, UINT64_MAX, &again, NULL, NULL);
    EXPECT_EQ_U(bartap_text_warnings(&text), BARTAP_TEXT_WARNING_ENTITY | BARTAP_TEXT_WARNING_END_TAG);
    items_t strict;
    read_all(bad, BARTAP_TEXT_STRICT, 4, &strict);
    EXPECT_EQ_U(strict.result, BARTAP_EPUB_RESULT_CORRUPT);
    EXPECT_EQ_U(bartap_text_init(&text, 0, 1UL << 20), BARTAP_EPUB_RESULT_INVALID_ARGUMENT);
}

const test_case_t text_tests[] = {
    TEST_CASE(text_words_breaks_and_styles),
    TEST_CASE(text_element_items),
    TEST_CASE(text_overrides),
    TEST_CASE(text_pieces_do_not_matter),
    TEST_CASE(text_positions_resolve_and_seek),
    TEST_CASE(text_seek_id),
    TEST_CASE(text_snapshots_restore_anywhere),
    TEST_CASE(text_snapshots_are_checked),
    TEST_CASE(text_strict_and_recovered),
};
const size_t text_test_count = sizeof text_tests / sizeof text_tests[0];
