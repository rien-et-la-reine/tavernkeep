// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//the XML tokenizer (src/xml.h): XML 1.0's line-end normalization (2.11) and attribute-value normalization
//(3.3.3), the predefined and XHTML 1.x entities, value truncation, and the leniency table: each malformed
//construct recovered from with its warning, or CORRUPT in strict mode. events are flattened into a string,
//one letter per kind - S(name) A(name=value) O(name) or O/(name) for an empty element, E(name), T(text), D -
//so a whole document's events compare in one check

#include <stdio.h>
#include <string.h>

#include "xml.h"

#include "harness.h"

typedef struct {
    bartap_xml_result_t result;
    uint32_t warnings;
    char events[2048];
    bool decoded;                   //some TEXT event was marked decoded
    bool truncated;                 //some ATTRIBUTE event was marked truncated
    uint32_t positions[256];        //of each event
    size_t count;
} run_t;

static void append(run_t *r, const char *kind, const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
    size_t n = strlen(r->events);
    n += (size_t)snprintf(r->events + n, sizeof r->events - n, "%s(", kind);
    for (size_t i = 0; i < al && n + 1U < sizeof r->events; i++) {
        r->events[n++] = (char)a[i];
    }
    if (b != NULL && n + 1U < sizeof r->events) {
        r->events[n++] = '=';
        for (size_t i = 0; i < bl && n + 1U < sizeof r->events; i++) {
            r->events[n++] = (char)b[i];
        }
    }
    snprintf(r->events + n, sizeof r->events - n, ")");
}

//tokenize doc (doc_size bytes) fed piece bytes at a time, with a buffer of buffer_size bytes; adjacent TEXT
//pieces are joined, as a consumer reading text would see them
static void tokenize_n(const char *doc, size_t doc_size, uint32_t flags, size_t piece, size_t buffer_size, run_t *r)
{
    memset(r, 0, sizeof *r);
    uint8_t buffer[512];
    uint32_t stack[8];
    bartap_xml_t xml;
    r->result = bartap_xml_init(&xml, buffer, buffer_size, stack, 8, flags);
    size_t taken = 0;
    bool in_text = false;
    for (int calls = 0; r->result == BARTAP_XML_RESULT_OK && calls < 100000; calls++) {
        const size_t n = doc_size - taken < piece ? doc_size - taken : piece;
        size_t used;
        bartap_xml_event_t e;
        r->result = bartap_xml_next(&xml, (const uint8_t *)doc + taken, n, &used, taken + n == doc_size, &e);
        taken += used;
        if (r->result != BARTAP_XML_RESULT_OK || e.kind == BARTAP_XML_EVENT_NONE) {
            continue;
        }
        if (r->count < sizeof r->positions / sizeof r->positions[0]) {
            r->positions[r->count++] = e.position;
        }
        if (e.kind == BARTAP_XML_EVENT_TEXT) {
            r->decoded |= e.decoded;
            if (in_text) {
                //join: drop the ')' and carry on
                r->events[strlen(r->events) - 1U] = '\0';
                size_t m = strlen(r->events);
                for (size_t i = 0; i < e.value_length && m + 2U < sizeof r->events; i++) {
                    r->events[m++] = (char)e.value[i];
                }
                r->events[m++] = ')';
                r->events[m] = '\0';
            } else {
                append(r, "T", e.value, e.value_length, NULL, 0);
            }
            in_text = true;
            continue;
        }
        in_text = false;
        switch (e.kind) {
        case BARTAP_XML_EVENT_START: append(r, "S", e.name, e.name_length, NULL, 0); break;
        case BARTAP_XML_EVENT_ATTRIBUTE:
            r->truncated |= e.truncated;
            append(r, "A", e.name, e.name_length, e.value, e.value_length);
            break;
        case BARTAP_XML_EVENT_START_END: append(r, e.empty ? "O/" : "O", e.name, e.name_length, NULL, 0); break;
        case BARTAP_XML_EVENT_END: append(r, "E", e.name, e.name_length, NULL, 0); break;
        case BARTAP_XML_EVENT_DONE:
            strncat(r->events, "D", sizeof r->events - strlen(r->events) - 1U);
            r->warnings = xml.warnings;
            return;
        default: break;
        }
    }
    r->warnings = xml.warnings;
}

static void tokenize(const char *doc, uint32_t flags, run_t *r)
{
    tokenize_n(doc, strlen(doc), flags, 3, 512, r);
}

#define LENIENT BARTAP_XML_FLAG_XHTML_ENTITIES
#define STRICT (BARTAP_XML_FLAG_XHTML_ENTITIES | BARTAP_XML_FLAG_STRICT)

static void expect_events(const char *doc, uint32_t flags, const char *events, uint32_t warnings)
{
    run_t r;
    tokenize(doc, flags, &r);
    EXPECT_EQ_U(r.result, BARTAP_XML_RESULT_OK);
    EXPECT(strcmp(r.events, events) == 0);
    if (strcmp(r.events, events) != 0) {
        printf("    doc:      %s\n    got:      %s\n    expected: %s\n", doc, r.events, events);
    }
    EXPECT_EQ_U(r.warnings, warnings);
}

static void expect_strict_corrupt(const char *doc)
{
    run_t r;
    tokenize(doc, STRICT, &r);
    EXPECT_EQ_U(r.result, BARTAP_XML_RESULT_CORRUPT);
}

static void xml_well_formed_document(void)
{
    const char *doc = "<?xml version=\"1.0\"?>\n<!DOCTYPE html [ <!ENTITY x \"]>\"> ]>\n"
        "<r a=\"1\" b='two'><!-- c --><e/>t<![CDATA[<x>]]]]></r>\n";
    expect_events(doc, STRICT, "S(r)A(a=1)A(b=two)O(r)S(e)O/(e)E(e)T(t<x>]])E(r)D", 0);
}

//XML 1.0 2.11: CR LF and a lone CR become LF before parsing; 3.3.3: in a value each becomes one space
static void xml_line_ends_are_normalized(void)
{
    expect_events("<r a=\"x\r\ny\rz\">a\r\nb\rc\r\r\nd</r>", STRICT, "S(r)A(a=x y z)O(r)T(a\nb\nc\n\nd)E(r)D", 0);
    run_t r;
    tokenize("<r>a\r\nb</r>", STRICT, &r);
    EXPECT(r.decoded);              //a byte dropped: positions within the text no longer map one to one
    tokenize("<r>a\rb</r>", STRICT, &r);
    EXPECT(!r.decoded);             //a lone CR is replaced, not dropped
}

static void xml_entities_are_decoded(void)
{
    expect_events("<r a=\"&amp;&lt;&#65;&#x42;\">&quot;&apos;&gt;&nbsp;&eacute;&thetasym;&#x1F600;</r>", STRICT,
        "S(r)A(a=&<AB)O(r)T(\"'>\xC2\xA0\xC3\xA9\xCF\x91\xF0\x9F\x98\x80)E(r)D", 0);
    //XHTML entities only where asked for: the OPF and container.xml have none
    expect_events("<r>&nbsp;</r>", 0, "S(r)O(r)T(&nbsp;)E(r)D", BARTAP_XML_WARNING_ENTITY);
    run_t r;
    tokenize("<r>&nbsp;</r>", BARTAP_XML_FLAG_STRICT, &r);
    EXPECT_EQ_U(r.result, BARTAP_XML_RESULT_CORRUPT);
}

//the leniency table in xml.h, a row at a time: the recovery, its warning, and CORRUPT when strict
static void xml_malformed_entities(void)
{
    expect_events("<r>a & b &unknown; c &#xZ; &toolongtobeanentity;</r>", LENIENT,
        "S(r)O(r)T(a & b &unknown; c &#xZ; &toolongtobeanentity;)E(r)D", BARTAP_XML_WARNING_ENTITY);
    expect_events("<r>&#0;&#xD800;&#x110000;</r>", LENIENT, "S(r)O(r)T(\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD)E(r)D",
        BARTAP_XML_WARNING_ENTITY);
    expect_strict_corrupt("<r>a & b</r>");
    expect_strict_corrupt("<r>&#0;</r>");
    expect_strict_corrupt("<r>&unknown;</r>");
}

static void xml_malformed_markup(void)
{
    expect_events("<r>1 < 2</r>", LENIENT, "S(r)O(r)T(1 < 2)E(r)D", BARTAP_XML_WARNING_MARKUP);
    expect_events("<r><!weird thing>x</r>", LENIENT, "S(r)O(r)T(x)E(r)D", BARTAP_XML_WARNING_MARKUP);
    expect_events("<r><td nowrap>x</td></r>", LENIENT, "S(r)O(r)S(td)A(nowrap=)O(td)T(x)E(td)E(r)D",
        BARTAP_XML_WARNING_MARKUP);
    expect_events("<r><a b c=d e=f/g h=i/></r>", LENIENT,
        "S(r)O(r)S(a)A(b=)A(c=d)A(e=f/g)A(h=i)O/(a)E(a)E(r)D", BARTAP_XML_WARNING_MARKUP);
    expect_events("<r a=\"x<y\"/>", LENIENT, "S(r)A(a=x<y)O/(r)E(r)D", BARTAP_XML_WARNING_MARKUP);
    expect_events("<r/>after <x>junk</x>", LENIENT, "S(r)O/(r)E(r)D", BARTAP_XML_WARNING_MARKUP);
    expect_events("<!doctype html><r/>", LENIENT, "S(r)O/(r)E(r)D", BARTAP_XML_WARNING_MARKUP);
    expect_strict_corrupt("<r>1 < 2</r>");
    expect_strict_corrupt("<r><td nowrap>x</td></r>");
    expect_strict_corrupt("<r a=b/>");
    expect_strict_corrupt("<r a=\"x<y\"/>");
    expect_strict_corrupt("<r/>after");
    expect_strict_corrupt("<!doctype html><r/>");
}

//an end tag closes up to the nearest open element of its name; one naming nothing open is ignored
static void xml_end_tags_are_matched(void)
{
    expect_events("<r><b>x<i>y</b>z</i></r>", LENIENT, "S(r)O(r)S(b)O(b)T(x)S(i)O(i)T(y)E()E(b)T(z)E(r)D",
        BARTAP_XML_WARNING_END_TAG);
    expect_events("<r><p>a<p>b</r>", LENIENT, "S(r)O(r)S(p)O(p)T(a)S(p)O(p)T(b)E()E()E(r)D",
        BARTAP_XML_WARNING_END_TAG);
    expect_strict_corrupt("<r><b>x</i></b></r>");
    expect_strict_corrupt("<r><b>x</r>");
}

static void xml_invalid_utf8_becomes_replacement(void)
{
    //a lone continuation byte, a lead byte cut short, an overlong form and a surrogate
    expect_events("<r>a\x80" "b\xC3" "c\xC0\xAF" "d\xED\xA0\x80" "e\xC3\xA9</r>", LENIENT,
        "S(r)O(r)T(a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c\xEF\xBF\xBD\xEF\xBF\xBD" "d\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
        "e\xC3\xA9)E(r)D", BARTAP_XML_WARNING_ENCODING);
    expect_strict_corrupt("<r>a\x80</r>");
}

//a value longer than the buffer allows is cut short and marked; the rest of it is still read, and checked
//in strict mode; a name that does not fit is LIMIT_EXCEEDED
static void xml_long_values_are_truncated(void)
{
    char doc[2048];
    char value[1500];
    memset(value, 'v', sizeof value - 1U);
    value[sizeof value - 1U] = '\0';
    snprintf(doc, sizeof doc, "<r a=\"%s&amp;\" b=\"ok\">t</r>", value);
    run_t r;
    tokenize_n(doc, strlen(doc), STRICT, 7, 64, &r);
    EXPECT_EQ_U(r.result, BARTAP_XML_RESULT_OK);
    EXPECT(r.truncated);
    char expected[128];
    snprintf(expected, sizeof expected, "S(r)A(a=%.*s)A(b=ok)O(r)T(t)E(r)D", 64 - 2, value);
    EXPECT(strcmp(r.events, expected) == 0);
    snprintf(doc, sizeof doc, "<r a=\"%s&bogus;\"/>", value);
    tokenize_n(doc, strlen(doc), STRICT, 7, 64, &r);
    EXPECT_EQ_U(r.result, BARTAP_XML_RESULT_CORRUPT);
    snprintf(doc, sizeof doc, "<r><%.*s/></r>", 100, value);
    tokenize_n(doc, strlen(doc), LENIENT, 7, 64, &r);
    EXPECT_EQ_U(r.result, BARTAP_XML_RESULT_LIMIT_EXCEEDED);
}

static void xml_limits_and_encodings(void)
{
    run_t r;
    tokenize("<a><a><a><a><a><a><a><a><a/></a></a></a></a></a></a></a></a>", LENIENT, &r);
    EXPECT_EQ_U(r.result, BARTAP_XML_RESULT_LIMIT_EXCEEDED);     //nine levels, eight allowed
    tokenize("\xFF\xFE<\0r\0/\0>\0", LENIENT, &r);
    EXPECT_EQ_U(r.result, BARTAP_XML_RESULT_UNSUPPORTED);
    expect_events("\xEF\xBB\xBF<r/>", STRICT, "S(r)O/(r)E(r)D", 0);
    tokenize("<r><p>unclosed", LENIENT, &r);
    EXPECT_EQ_U(r.result, BARTAP_XML_RESULT_TRUNCATED);
}

//the events are the same whatever pieces the input comes in, and positions never go back
static void xml_pieces_do_not_matter(void)
{
    const char *doc = "<?xml version='1.0'?><r a='&amp;x'>one &eacute; <b>two</b>\r\n<![CDATA[th]]]>ree]]>"
        "<!-- c --><e x=\"&#x1F600;\"/>\xF0\x9F\x98\x80 &bad <i>i</r>";
    run_t whole;
    tokenize_n(doc, strlen(doc), LENIENT, strlen(doc), 512, &whole);
    EXPECT_EQ_U(whole.result, BARTAP_XML_RESULT_OK);
    for (size_t piece = 1; piece <= 9; piece++) {
        for (size_t buffer = 32; buffer <= 512; buffer *= 4) {
            run_t r;
            tokenize_n(doc, strlen(doc), LENIENT, piece, buffer, &r);
            EXPECT(strcmp(r.events, whole.events) == 0);
            EXPECT_EQ_U(r.warnings, whole.warnings);
            for (size_t i = 1; i < r.count; i++) {
                EXPECT(r.positions[i] >= r.positions[i - 1U]);
            }
        }
    }
}

const test_case_t xml_tests[] = {
    TEST_CASE(xml_well_formed_document),
    TEST_CASE(xml_line_ends_are_normalized),
    TEST_CASE(xml_entities_are_decoded),
    TEST_CASE(xml_malformed_entities),
    TEST_CASE(xml_malformed_markup),
    TEST_CASE(xml_end_tags_are_matched),
    TEST_CASE(xml_invalid_utf8_becomes_replacement),
    TEST_CASE(xml_long_values_are_truncated),
    TEST_CASE(xml_limits_and_encodings),
    TEST_CASE(xml_pieces_do_not_matter),
};
const size_t xml_test_count = sizeof xml_tests / sizeof xml_tests[0];
