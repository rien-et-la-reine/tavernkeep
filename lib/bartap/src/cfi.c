// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* EPUB CFIs (bartap/epub.h): parsing, formatting, comparing, and resolving one over a content document the
 * caller feeds. The resolver counts elements and characters exactly as the text layer (text.c) does when
 * it makes a position, so that the two agree on every document both accept with the same flags. */

#include "bartap/epub.h"

#include <string.h>

#include "bartap/text.h"
#include "xml.h"

#define TOKEN_SIZE 512U             //as text.c's
#define DEPTH_MAX BARTAP_TEXT_DEPTH_MAX

/* ------------------------------------------------------------ text form */

static bool parse_integer(const char **p, uint32_t *value)
{
    const char *s = *p;
    if (*s < '0' || *s > '9') {
        return false;
    }
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        const uint32_t digit = (uint32_t)(*s - '0');
        if (v > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        v = v * 10U + digit;
        s++;
    }
    *p = s;
    *value = v;
    return true;
}

//an id assertion, "[...]" with '^' escaping: skipped, not checked
static bool skip_assertion(const char **p)
{
    const char *s = *p;
    if (*s != '[') {
        return true;
    }
    s++;
    while (*s != ']') {
        if (*s == '\0') {
            return false;
        }
        if (*s == '^' && s[1] != '\0') {
            s++;
        }
        s++;
    }
    *p = s + 1;
    return true;
}

bartap_epub_result_t bartap_epub_cfi_parse(const char *text, bartap_epub_cfi_t *cfi)
{
    if (text == NULL || cfi == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    memset(cfi, 0, sizeof *cfi);
    const char *p = text;
    bool wrapped = false;
    static const char prefix[] = "epubcfi(";
    size_t matched = 0;
    while (matched < sizeof prefix - 1U && p[matched] == prefix[matched]) {
        matched++;
    }
    if (matched == sizeof prefix - 1U) {
        p += matched;
        wrapped = true;
    }
    //the package part: /6 (the spine) and /N (an itemref), then "!"
    uint32_t package_steps[2];
    unsigned count = 0;
    while (*p == '/') {
        p++;
        uint32_t v;
        if (count == 2U || !parse_integer(&p, &v) || !skip_assertion(&p)) {
            return count == 2U ? BARTAP_EPUB_RESULT_UNSUPPORTED : BARTAP_EPUB_RESULT_CORRUPT;
        }
        package_steps[count++] = v;
    }
    if (count != 2U || *p != '!') {
        //a path that stays in the package document, or a range or offset on it, is valid but not handled
        return count == 2U && (*p == ',' || *p == '~' || *p == '@' || *p == ':' || *p == ')' || *p == '\0')
            ? BARTAP_EPUB_RESULT_UNSUPPORTED : BARTAP_EPUB_RESULT_CORRUPT;
    }
    if (package_steps[0] != 6U || package_steps[1] < 2U || (package_steps[1] & 1U) != 0U) {
        return BARTAP_EPUB_RESULT_CORRUPT;
    }
    cfi->spine_index = package_steps[1] / 2U - 1U;
    p++;
    while (*p == '/') {
        p++;
        uint32_t v;
        if (!parse_integer(&p, &v) || v == 0U || !skip_assertion(&p)) {
            return BARTAP_EPUB_RESULT_CORRUPT;
        }
        if (cfi->step_count == BARTAP_EPUB_CFI_STEPS_MAX) {
            return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
        }
        if (cfi->step_count > 0U && (cfi->steps[cfi->step_count - 1U] & 1U) != 0U) {
            return BARTAP_EPUB_RESULT_CORRUPT;      //a text step can only be the last
        }
        cfi->steps[cfi->step_count++] = v;
    }
    if (*p == ':') {
        p++;
        if (!parse_integer(&p, &cfi->offset) || !skip_assertion(&p)) {
            return BARTAP_EPUB_RESULT_CORRUPT;
        }
        cfi->has_offset = true;
    }
    if (*p == '!' || *p == ',' || *p == '~' || *p == '@') {
        return BARTAP_EPUB_RESULT_UNSUPPORTED;
    }
    if (wrapped && *p++ != ')') {
        return BARTAP_EPUB_RESULT_CORRUPT;
    }
    return *p == '\0' ? BARTAP_EPUB_RESULT_OK : BARTAP_EPUB_RESULT_CORRUPT;
}

//append prefix and v in decimal; false if it does not fit, leaving room for the NUL
static bool append_number(char *buffer, size_t size, size_t *n, char prefix, uint32_t v)
{
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v != 0U);
    if (*n + 1U + count >= size) {
        return false;
    }
    buffer[(*n)++] = prefix;
    while (count > 0U) {
        buffer[(*n)++] = digits[--count];
    }
    return true;
}

bartap_epub_result_t bartap_epub_cfi_format(const bartap_epub_cfi_t *cfi, char *buffer, size_t buffer_size)
{
    if (cfi == NULL || buffer == NULL || buffer_size == 0U || cfi->step_count > BARTAP_EPUB_CFI_STEPS_MAX
            || cfi->spine_index >= UINT32_MAX / 2U) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    buffer[0] = '\0';
    static const char head[] = "epubcfi(/6";
    if (buffer_size < sizeof head) {
        return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
    }
    memcpy(buffer, head, sizeof head - 1U);
    size_t n = sizeof head - 1U;
    bool fits = append_number(buffer, buffer_size, &n, '/', 2U * (cfi->spine_index + 1U));
    if (fits && n + 2U < buffer_size) {
        buffer[n++] = '!';
    } else {
        fits = false;
    }
    for (uint32_t i = 0; fits && i < cfi->step_count; i++) {
        fits = append_number(buffer, buffer_size, &n, '/', cfi->steps[i]);
    }
    if (fits && cfi->has_offset) {
        fits = append_number(buffer, buffer_size, &n, ':', cfi->offset);
    }
    if (!fits || n + 2U > buffer_size) {
        buffer[0] = '\0';
        return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
    }
    buffer[n++] = ')';
    buffer[n] = '\0';
    return BARTAP_EPUB_RESULT_OK;
}

int bartap_epub_cfi_compare(const bartap_epub_cfi_t *a, const bartap_epub_cfi_t *b)
{
    if (a->spine_index != b->spine_index) {
        return a->spine_index < b->spine_index ? -1 : 1;
    }
    const uint32_t a_count = a->step_count < BARTAP_EPUB_CFI_STEPS_MAX ? a->step_count : BARTAP_EPUB_CFI_STEPS_MAX;
    const uint32_t b_count = b->step_count < BARTAP_EPUB_CFI_STEPS_MAX ? b->step_count : BARTAP_EPUB_CFI_STEPS_MAX;
    const uint32_t count = a_count < b_count ? a_count : b_count;
    for (uint32_t i = 0; i < count; i++) {
        if (a->steps[i] != b->steps[i]) {
            return a->steps[i] < b->steps[i] ? -1 : 1;
        }
    }
    if (a_count != b_count) {
        return a_count < b_count ? -1 : 1;          //an ancestor first
    }
    const uint32_t ao = a->has_offset ? a->offset : 0U;
    const uint32_t bo = b->has_offset ? b->offset : 0U;
    return ao == bo ? 0 : ao < bo ? -1 : 1;
}

/* ------------------------------------------------------------ resolving */

typedef struct {
    bartap_xml_t xml;
    uint32_t stack[DEPTH_MAX];
    uint8_t token[TOKEN_SIZE];
    bartap_epub_cfi_t cfi;
    uint32_t matched;               //steps matched so far
    uint32_t depth;                 //XML depth of the element reached (1 for the root)
    uint32_t children;              //its child elements seen so far
    uint32_t units;                 //UTF-16 code units of the target text seen
    bool started;
    bool in_text;                   //inside the target text
    bool done;
    uint32_t position;
    bartap_epub_result_t status;
} resolver_t;

_Static_assert(sizeof(resolver_t) <= BARTAP_EPUB_RESOLVER_SIZE, "BARTAP_EPUB_RESOLVER_SIZE is too small");
_Static_assert(BARTAP_TEXT_STRICT == BARTAP_EPUB_RESOLVE_STRICT, "one flag for both");

static resolver_t *resolver_state(bartap_epub_resolver_t *resolver)
{
    return (resolver_t *)(void *)resolver->storage.bytes;
}

bartap_epub_result_t bartap_epub_cfi_resolver_init(bartap_epub_resolver_t *resolver, const bartap_epub_cfi_t *cfi,
    uint32_t flags)
{
    if (resolver == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    resolver_t *r = resolver_state(resolver);
    memset(r, 0, sizeof *r);
    r->status = BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    if (cfi == NULL || cfi->step_count > BARTAP_EPUB_CFI_STEPS_MAX || (flags & ~BARTAP_EPUB_RESOLVE_STRICT) != 0U) {
        return r->status;
    }
    r->cfi = *cfi;
    (void)bartap_xml_init(&r->xml, r->token, sizeof r->token, r->stack, DEPTH_MAX,
        BARTAP_XML_FLAG_XHTML_ENTITIES | ((flags & BARTAP_EPUB_RESOLVE_STRICT) != 0U ? BARTAP_XML_FLAG_STRICT : 0U));
    r->status = BARTAP_EPUB_RESULT_OK;
    return BARTAP_EPUB_RESULT_OK;
}

//whether the last step names a text position (an odd index) and everything before it has been matched
static bool text_target(const resolver_t *r)
{
    return r->cfi.step_count > 0U && r->matched == r->cfi.step_count - 1U
        && (r->cfi.steps[r->cfi.step_count - 1U] & 1U) != 0U;
}

static void found(resolver_t *r, uint32_t position)
{
    r->done = true;
    r->position = position;
}

static bartap_epub_result_t resolve_event(resolver_t *r, const bartap_xml_event_t *e)
{
    const uint32_t offset = r->cfi.has_offset ? r->cfi.offset : 0U;
    switch (e->kind) {
    case BARTAP_XML_EVENT_START:
    case BARTAP_XML_EVENT_END:
        if (r->in_text) {
            //the target text ends here: an offset just past its last character is this tag
            if (r->units == offset) {
                found(r, e->position);
                return BARTAP_EPUB_RESULT_OK;
            }
            return BARTAP_EPUB_RESULT_NOT_FOUND;
        }
        if (e->kind == BARTAP_XML_EVENT_END) {
            //the element the next step is in ends without it
            return r->started && e->depth <= r->depth ? BARTAP_EPUB_RESULT_NOT_FOUND : BARTAP_EPUB_RESULT_OK;
        }
        if (!r->started) {
            if (e->depth == 1U) {
                r->started = true;
                r->depth = 1;
                if (r->cfi.step_count == 0U) {
                    found(r, e->position);
                }
            }
            return BARTAP_EPUB_RESULT_OK;
        }
        if (e->depth != r->depth + 1U) {
            return BARTAP_EPUB_RESULT_OK;
        }
        r->children++;
        if (r->matched < r->cfi.step_count) {
            const uint32_t s = r->cfi.steps[r->matched];
            if ((s & 1U) == 0U && s / 2U == r->children) {
                r->matched++;
                r->depth = e->depth;
                r->children = 0;
                if (r->matched == r->cfi.step_count) {
                    found(r, e->position);      //an element: its start tag
                }
            } else if ((s & 1U) == 0U && s / 2U < r->children) {
                return BARTAP_EPUB_RESULT_NOT_FOUND;
            }
        }
        return BARTAP_EPUB_RESULT_OK;
    case BARTAP_XML_EVENT_TEXT:
        if (!r->started || e->depth != r->depth || !text_target(r)
                || r->cfi.steps[r->cfi.step_count - 1U] != 2U * r->children + 1U) {
            return BARTAP_EPUB_RESULT_OK;
        }
        r->in_text = true;
        for (size_t i = 0; i < e->value_length; i++) {
            const uint8_t c = e->value[i];
            if ((c & 0xC0U) == 0x80U) {
                continue;                       //a continuation byte: the same character
            }
            if (r->units == offset) {
                found(r, e->decoded ? e->position : e->position + (uint32_t)i);
                return BARTAP_EPUB_RESULT_OK;
            }
            r->units += c >= 0xF0U ? 2U : 1U;   //outside the BMP: a surrogate pair in UTF-16
        }
        return BARTAP_EPUB_RESULT_OK;
    case BARTAP_XML_EVENT_DONE:
        return BARTAP_EPUB_RESULT_NOT_FOUND;
    default:
        return BARTAP_EPUB_RESULT_OK;
    }
}

bartap_epub_result_t bartap_epub_cfi_resolve_next(bartap_epub_resolver_t *resolver, const uint8_t *in,
    size_t in_size, size_t *in_used, bool input_ends, bool *done, uint32_t *position)
{
    if (resolver == NULL || in_used == NULL || done == NULL || position == NULL || (in == NULL && in_size != 0U)) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    resolver_t *r = resolver_state(resolver);
    *in_used = 0;
    *done = r->done;
    *position = r->position;
    if (r->status != BARTAP_EPUB_RESULT_OK || r->done) {
        return r->status;
    }
    bartap_xml_event_t e;
    const bartap_xml_result_t x = bartap_xml_next(&r->xml, in, in_size, in_used, input_ends, &e);
    bartap_epub_result_t result;
    switch (x) {
    case BARTAP_XML_RESULT_OK:
        result = e.kind == BARTAP_XML_EVENT_NONE ? BARTAP_EPUB_RESULT_OK : resolve_event(r, &e);
        break;
    case BARTAP_XML_RESULT_LIMIT_EXCEEDED:
        result = BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
        break;
    case BARTAP_XML_RESULT_UNSUPPORTED:
        result = BARTAP_EPUB_RESULT_UNSUPPORTED;
        break;
    default:
        result = BARTAP_EPUB_RESULT_CORRUPT;
        break;
    }
    r->status = result;
    *done = r->done;
    *position = r->position;
    return result;
}
