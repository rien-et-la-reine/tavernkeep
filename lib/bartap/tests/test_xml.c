// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include <string.h>
#include "harness.h"
#include "bartap_internal.h"

typedef struct {
    const char *data;
    size_t remaining;
    size_t chunk;
} xml_feed_t;

static bartap_err_t fill(void *user, uint8_t *buf, size_t len, size_t *got)
{
    xml_feed_t *f = user;
    size_t n = f->remaining < len ? f->remaining : len;
    if (n > f->chunk) n = f->chunk;
    memcpy(buf, f->data, n);
    f->data += n;
    f->remaining -= n;
    *got = n;
    return BARTAP_OK;
}

static void xml_attributes_are_complete_or_unavailable(void)
{
    char document[2048];
    memset(document, 'x', sizeof document);
    memcpy(document, "<package prefix='", 17U);
    const char *tail = "' unique-identifier='chosen' href='caf&#233;.xhtml'/>";
    memcpy(document + 1500U, tail, strlen(tail) + 1U);
    xml_feed_t feed = { document, strlen(document), 1U };
    bartap_xml_t xml;
    bartap_xml_token_t token;
    bartap_xml_init(&xml, fill, &feed);
    REQUIRE(bartap_xml_next(&xml, &token) == BARTAP_OK);
    EXPECT_EQ_U(token.type, BARTAP_XML_START_TAG);
    char value[64];
    size_t len;
    REQUIRE(bartap_xml_attr(&xml, "unique-identifier", value, sizeof value, &len) == BARTAP_OK);
    EXPECT(strcmp(value, "chosen") == 0);
    REQUIRE(bartap_xml_attr(&xml, "href", value, sizeof value, &len) == BARTAP_OK);
    EXPECT(strcmp(value, "caf\xc3\xa9.xhtml") == 0);
    EXPECT_EQ_U(xml.warnings, 0U);
    EXPECT_EQ_U(bartap_xml_attr(&xml, "href", value, 5U, &len), BARTAP_ERR_LIMIT);
    EXPECT_EQ_U(value[0], 0U);
    EXPECT((xml.warnings & BARTAP_WARN_MARKUP) != 0U);

    memset(document, 'a', sizeof document);
    memcpy(document, "<img src='", 10U);
    tail = "' id='still-visible'/>";
    memcpy(document + 1000U, tail, strlen(tail) + 1U);
    feed = (xml_feed_t){ document, strlen(document), 7U };
    bartap_xml_init(&xml, fill, &feed);
    REQUIRE(bartap_xml_next(&xml, &token) == BARTAP_OK);
    EXPECT_EQ_U(bartap_xml_attr(&xml, "src", value, sizeof value, &len), BARTAP_ERR_LIMIT);
    EXPECT_EQ_U(value[0], 0U);
    REQUIRE(bartap_xml_attr(&xml, "id", value, sizeof value, &len) == BARTAP_OK);
    EXPECT(strcmp(value, "still-visible") == 0);
}

static void xml_uri_fragments_and_utf8_copy(void)
{
    char path[BARTAP_PATH_MAX + 1], fragment[BARTAP_FRAGMENT_MAX + 1];
    REQUIRE(bartap_resolve_href("OEBPS/text/", "../caf%C3%A9.xhtml#caf%C3%A9", path,
        fragment, sizeof fragment) == BARTAP_OK);
    EXPECT(strcmp(path, "OEBPS/caf\xc3\xa9.xhtml") == 0);
    EXPECT(strcmp(fragment, "caf\xc3\xa9") == 0);
    EXPECT_EQ_U(bartap_resolve_href("", "../bad", path, NULL, 0U), BARTAP_ERR_CORRUPT);
    EXPECT_EQ_U(bartap_resolve_href("", "bad%00.xhtml", path, NULL, 0U), BARTAP_ERR_CORRUPT);
    EXPECT_EQ_U(bartap_resolve_href("", "https://example.org/x", path, NULL, 0U), BARTAP_ERR_UNSUPPORTED);
    EXPECT_EQ_U(bartap_resolve_href("", "x#abc", path, fragment, 3U), BARTAP_ERR_LIMIT);
    // RFC 3986 2.2: an escaped '/' is data in a segment, never a separator; no archive name holds it
    EXPECT_EQ_U(bartap_resolve_href("OEBPS/", "a%2F..%2Fb.xhtml", path, NULL, 0U), BARTAP_ERR_CORRUPT);
    EXPECT_EQ_U(bartap_resolve_href("OEBPS/", "a%2fb.xhtml", path, NULL, 0U), BARTAP_ERR_CORRUPT);
    // '.' is unreserved, so an escaped dot segment normalizes like a literal one (RFC 3986 6.2.2.2)
    REQUIRE(bartap_resolve_href("OEBPS/text/", "%2E%2E/x.xhtml#a%2Fb", path, fragment, sizeof fragment)
        == BARTAP_OK);
    EXPECT(strcmp(path, "OEBPS/x.xhtml") == 0);
    EXPECT(strcmp(fragment, "a/b") == 0);    // an id may contain '/'
    char buf[8];
    static const char input[] = "a\xe2\x82\xac";
    for (size_t cap = 1U; cap < 5U; cap++) {
        EXPECT(bartap_copy_utf8(buf, cap, input, sizeof input - 1U));
        EXPECT(strcmp(buf, cap == 1U ? "" : "a") == 0);
    }
    EXPECT(!bartap_copy_utf8(buf, 5U, input, sizeof input - 1U));
    EXPECT(strcmp(buf, input) == 0);
}

const test_case_t xml_tests[] = {
    TEST_CASE(xml_attributes_are_complete_or_unavailable),
    TEST_CASE(xml_uri_fragments_and_utf8_copy),
};
const size_t xml_test_count = sizeof xml_tests / sizeof xml_tests[0];
