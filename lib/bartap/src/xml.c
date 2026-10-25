// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* XML tokenizer (xml.h, internal): a byte-at-a-time state machine, so it stops and resumes anywhere.
 *
 * Every byte goes through step(), which either takes it or keeps it to be looked at again, possibly after
 * changing stage, and may produce one event. Line ends are normalized before step() sees a byte: a CR is
 * passed on as an LF, and an LF straight after a CR is dropped. Recovery from malformed markup is local:
 * literals come only from what the state already holds (the entity buffer, a '<', a held UTF-8 sequence),
 * so nothing is read twice. */

#include "xml.h"

#include <string.h>

enum {
    STAGE_CONTENT,                  //text, or outside the root at depth 0
    STAGE_TAG_OPEN,                 //after '<'
    STAGE_START_NAME,
    STAGE_ATTRIBUTE_SPACE,          //inside a start tag, between attributes
    STAGE_ATTRIBUTE_NAME,
    STAGE_ATTRIBUTE_EQUALS,
    STAGE_ATTRIBUTE_QUOTE,
    STAGE_ATTRIBUTE_VALUE,          //quoted
    STAGE_ATTRIBUTE_UNQUOTED,       //lenient only
    STAGE_UNQUOTED_SLASH,           //a '/' in an unquoted value: "/>" or part of the value
    STAGE_EMPTY_CLOSE,              //after the '/' of "<a/>"
    STAGE_END_NAME,
    STAGE_END_SPACE,
    STAGE_ENTITY,                   //after '&'
    STAGE_BANG,                     //after "<!"
    STAGE_COMMENT,
    STAGE_CDATA,
    STAGE_PROCESSING,               //"<?...?>"
    STAGE_DOCTYPE,
    STAGE_BOGUS,                    //an unknown "<!" construct, skipped to '>'
    STAGE_TRAILER,                  //lenient: markup after the root element, all ignored
    STAGE_DONE,
};

/* Room text keeps free before taking a byte, so that whatever one byte can add fits: an entity reference
 * passed through as written ('&', 12 bytes, ';'), a UTF-8 sequence, U+FFFD. */
#define TEXT_ROOM 16U

static const uint8_t replacement[3] = { 0xEF, 0xBF, 0xBD };    //U+FFFD
static const uint8_t byte_order_mark[3] = { 0xEF, 0xBB, 0xBF };

static bool is_space(uint8_t b)
{
    return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}

//names: ASCII letters, digits and "_:-.", and any byte of a multi-byte UTF-8 sequence
static bool is_name_start(uint8_t b)
{
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '_' || b == ':' || b >= 0x80U;
}

static bool is_name(uint8_t b)
{
    return is_name_start(b) || (b >= '0' && b <= '9') || b == '-' || b == '.';
}

static bool strict(const bartap_xml_t *x)
{
    return (x->flags & BARTAP_XML_FLAG_STRICT) != 0U;
}

/* Add a byte of text or of an attribute value. Text always has room (TEXT_ROOM); a value that runs out of
 * buffer is cut short there and marked truncated, and the rest of it is still read. */
static void put(bartap_xml_t *x, uint8_t b, uint32_t position)
{
    if (x->length >= x->buffer_size) {
        x->truncated = true;
        return;
    }
    if (x->length == 0U) {
        x->token_position = position;
    }
    x->buffer[x->length++] = b;
}

static void put_replacement(bartap_xml_t *x, uint32_t position)
{
    for (size_t i = 0; i < sizeof replacement; i++) {
        put(x, replacement[i], position);
    }
    x->decoded = true;
}

//a code point as UTF-8 (RFC 3629)
static void put_code_point(bartap_xml_t *x, uint32_t c, uint32_t position)
{
    if (c < 0x80U) {
        put(x, (uint8_t)c, position);
    } else if (c < 0x800U) {
        put(x, (uint8_t)(0xC0U | (c >> 6)), position);
        put(x, (uint8_t)(0x80U | (c & 0x3FU)), position);
    } else if (c < 0x10000UL) {
        put(x, (uint8_t)(0xE0U | (c >> 12)), position);
        put(x, (uint8_t)(0x80U | ((c >> 6) & 0x3FU)), position);
        put(x, (uint8_t)(0x80U | (c & 0x3FU)), position);
    } else {
        put(x, (uint8_t)(0xF0U | (c >> 18)), position);
        put(x, (uint8_t)(0x80U | ((c >> 12) & 0x3FU)), position);
        put(x, (uint8_t)(0x80U | ((c >> 6) & 0x3FU)), position);
        put(x, (uint8_t)(0x80U | (c & 0x3FU)), position);
    }
    x->decoded = true;
}

//malformed markup: recovered from, with a warning, or in strict mode an error
static bool recover(bartap_xml_t *x, uint32_t warning, bartap_xml_result_t *result)
{
    if (strict(x)) {
        *result = BARTAP_XML_RESULT_CORRUPT;
        return false;
    }
    x->warnings |= warning;
    return true;
}

//step: what one byte did. An event, if any, is reported through *e
typedef enum { TAKE, KEEP, EMIT, EMIT_TAKE } step_t;

/* A byte of text or of a value, through the UTF-8 check (RFC 3629: no overlongs, surrogates or values past
 * U+10FFFF). A sequence is held until it is complete, so pieces of text never split one. KEEP when a held
 * sequence was cut short by b, which is then still to be looked at. */
static step_t put_utf8(bartap_xml_t *x, uint8_t b, bartap_xml_result_t *result)
{
    if (x->utf8_need > 0U) {
        if (b >= x->utf8_low && b <= x->utf8_high && x->utf8_have < sizeof x->utf8) {
            x->utf8[x->utf8_have++] = b;
            x->utf8_low = 0x80;
            x->utf8_high = 0xBF;
            if (--x->utf8_need == 0U) {
                for (uint8_t i = 0; i < x->utf8_have; i++) {
                    put(x, x->utf8[i], x->utf8_position);
                }
                x->utf8_have = 0;
            }
            return TAKE;
        }
        //cut short: one U+FFFD for what was held
        x->utf8_need = 0;
        x->utf8_have = 0;
        if (recover(x, BARTAP_XML_WARNING_ENCODING, result)) {
            put_replacement(x, x->utf8_position);
        }
        return KEEP;
    }
    if (b < 0x80U) {
        put(x, b, x->position);
        return TAKE;
    }
    x->utf8_low = 0x80;
    x->utf8_high = 0xBF;
    if (b >= 0xC2U && b <= 0xDFU) {
        x->utf8_need = 1;
    } else if (b >= 0xE0U && b <= 0xEFU) {
        x->utf8_need = 2;
        x->utf8_low = b == 0xE0U ? 0xA0U : 0x80U;   //no overlongs
        x->utf8_high = b == 0xEDU ? 0x9FU : 0xBFU;  //no surrogates
    } else if (b >= 0xF0U && b <= 0xF4U) {
        x->utf8_need = 3;
        x->utf8_low = b == 0xF0U ? 0x90U : 0x80U;
        x->utf8_high = b == 0xF4U ? 0x8FU : 0xBFU;  //nothing past U+10FFFF
    } else {
        if (recover(x, BARTAP_XML_WARNING_ENCODING, result)) {
            put_replacement(x, x->position);        //a byte that starts no sequence
        }
        return TAKE;
    }
    x->utf8[0] = b;
    x->utf8_have = 1;
    x->utf8_position = x->position;
    return TAKE;
}

//the entity reference held, as written, for a malformed or unknown one in lenient mode
static void put_entity_literal(bartap_xml_t *x, bool semicolon)
{
    put(x, '&', x->entity_position);
    for (uint8_t i = 0; i < x->entity_length; i++) {
        put(x, x->entity[i], x->entity_position);
    }
    if (semicolon) {
        put(x, ';', x->entity_position);
    }
}

static bool entity_is(const bartap_xml_t *x, const char *name, size_t n)
{
    return x->entity_length == n && memcmp(x->entity, name, n) == 0;
}

//the reference in x->entity, ended by ';'
static void decode_entity(bartap_xml_t *x, bartap_xml_result_t *result)
{
    const uint8_t *s = x->entity;
    const size_t n = x->entity_length;
    if (n >= 1U && s[0] == '#') {
        const bool hex = n >= 2U && s[1] == 'x';
        size_t i = hex ? 2U : 1U;
        uint32_t value = 0;
        bool valid = i < n;
        bool large = false;
        for (; valid && i < n; i++) {
            const uint8_t d = s[i];
            uint32_t digit;
            if (d >= '0' && d <= '9') {
                digit = d - '0';
            } else if (hex && d >= 'a' && d <= 'f') {
                digit = d - 'a' + 10U;
            } else if (hex && d >= 'A' && d <= 'F') {
                digit = d - 'A' + 10U;
            } else {
                valid = false;
                break;
            }
            if (!large) {
                value = value * (hex ? 16U : 10U) + digit;
                large = value > 0x10FFFFUL;     //at most 11 digits, so this cannot wrap first
            }
        }
        if (!valid) {
            if (recover(x, BARTAP_XML_WARNING_ENTITY, result)) {
                put_entity_literal(x, true);    //not a character reference at all
            }
        } else if (large || value == 0U || (value >= 0xD800UL && value <= 0xDFFFUL)) {
            if (recover(x, BARTAP_XML_WARNING_ENTITY, result)) {
                put_replacement(x, x->entity_position);
            }
        } else {
            put_code_point(x, value, x->entity_position);
        }
        return;
    }
    static const struct {
        char name[5];
        uint8_t length;
        uint8_t byte;
    } predefined[] = { { "amp", 3, '&' }, { "lt", 2, '<' }, { "gt", 2, '>' }, { "quot", 4, '"' }, { "apos", 4, '\'' } };
    for (size_t i = 0; i < sizeof predefined / sizeof predefined[0]; i++) {
        if (entity_is(x, predefined[i].name, predefined[i].length)) {
            put(x, predefined[i].byte, x->entity_position);
            x->decoded = true;
            return;
        }
    }
    uint32_t code_point;
    if ((x->flags & BARTAP_XML_FLAG_XHTML_ENTITIES) != 0U && bartap_xml_entity_lookup(s, n, &code_point)) {
        put_code_point(x, code_point, x->entity_position);
        return;
    }
    if (recover(x, BARTAP_XML_WARNING_ENTITY, result)) {
        put_entity_literal(x, true);            //an entity no DTD here defines: as written
    }
}

bartap_xml_result_t bartap_xml_init(bartap_xml_t *xml, uint8_t *buffer, size_t buffer_size, uint32_t *stack,
    uint32_t max_depth, uint32_t flags)
{
    if (xml == NULL) {
        return BARTAP_XML_RESULT_INVALID_ARGUMENT;
    }
    memset(xml, 0, sizeof *xml);
    if (buffer == NULL || buffer_size < BARTAP_XML_BUFFER_MIN || stack == NULL || max_depth == 0U) {
        xml->status = BARTAP_XML_RESULT_INVALID_ARGUMENT;
        return xml->status;
    }
    xml->buffer = buffer;
    xml->buffer_size = buffer_size;
    xml->stack = stack;
    xml->max_depth = max_depth;
    xml->flags = flags;
    xml->stage = STAGE_CONTENT;
    return BARTAP_XML_RESULT_OK;
}

static void set_event(const bartap_xml_t *x, bartap_xml_event_t *e, uint8_t kind)
{
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->depth = x->depth;
    e->position = x->token_position;
}

static void text_event(bartap_xml_t *x, bartap_xml_event_t *e, bool more)
{
    set_event(x, e, BARTAP_XML_EVENT_TEXT);
    e->value = x->buffer;
    e->value_length = x->length;
    e->more = more;
    e->decoded = x->decoded;
    e->cdata = x->stage == STAGE_CDATA;
    x->length = 0;
    x->decoded = false;
}

static void attribute_event(bartap_xml_t *x, bartap_xml_event_t *e)
{
    set_event(x, e, BARTAP_XML_EVENT_ATTRIBUTE);
    e->name = x->buffer + x->name_length;
    e->name_length = x->value_start - x->name_length;
    e->value = x->buffer + x->value_start;
    e->value_length = x->length - x->value_start;
    e->truncated = x->truncated;
    x->stage = STAGE_ATTRIBUTE_SPACE;           //the next attribute overwrites this one
}

//an attribute with no value (lenient): its name is all there is
static void empty_attribute_event(bartap_xml_t *x, bartap_xml_event_t *e)
{
    x->value_start = x->length;
    x->truncated = false;
    attribute_event(x, e);
}

static void start_end_event(bartap_xml_t *x, bartap_xml_event_t *e, bool empty)
{
    set_event(x, e, BARTAP_XML_EVENT_START_END);
    e->name = x->buffer;
    e->name_length = x->name_length;
    e->empty = empty;
    x->stage = STAGE_CONTENT;
    if (empty) {
        x->pending_ends = 1;                    //its END comes on the next call, placed after the tag
        x->end_name_length = x->name_length;
        x->token_position = x->position + 1U;
    } else {
        x->length = 0;
    }
}

//report one of the pending END events; the last carries the end tag's (or empty element's) name
static void pop_end(bartap_xml_t *x, bartap_xml_event_t *e)
{
    set_event(x, e, BARTAP_XML_EVENT_END);
    if (x->pending_ends == 1U) {
        e->name = x->buffer;
        e->name_length = x->end_name_length;
    }
    x->depth--;
    x->pending_ends--;
    x->root_closed = x->depth == 0U;
    if (x->pending_ends == 0U) {
        x->length = 0;
    }
}

//"</name>" complete: close up to the nearest open element of that name
static step_t end_tag(bartap_xml_t *x, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    x->stage = STAGE_CONTENT;
    uint32_t level = x->depth;
    while (level > 0U && x->stack[level - 1U] != x->name_hash) {
        level--;
    }
    if (level == 0U || x->length == 0U) {
        //names no open element
        if (recover(x, BARTAP_XML_WARNING_END_TAG, result)) {
            x->length = 0;
        }
        return TAKE;
    }
    if (level != x->depth && !recover(x, BARTAP_XML_WARNING_END_TAG, result)) {
        return TAKE;
    }
    x->pending_ends = x->depth - level + 1U;
    x->end_name_length = x->length;
    pop_end(x, e);
    return EMIT_TAKE;
}

//content outside the root element: whitespace, a byte order mark, markup; nothing else
static step_t outside_root(bartap_xml_t *x, uint8_t b, bartap_xml_result_t *result)
{
    if (b == '<') {
        x->token_position = x->position;
        x->stage = STAGE_TAG_OPEN;
        return TAKE;
    }
    if (is_space(b)) {
        return TAKE;
    }
    if (x->position < 3U && x->bom == x->position && b == byte_order_mark[x->bom]) {
        x->bom++;
        return TAKE;
    }
    if (x->position == 0U && (b == 0xFEU || b == 0xFFU)) {
        *result = BARTAP_XML_RESULT_UNSUPPORTED;    //a UTF-16 byte order mark
        return TAKE;
    }
    (void)recover(x, BARTAP_XML_WARNING_MARKUP, result);
    return TAKE;
}

static step_t content(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    if (x->depth == 0U) {
        return outside_root(x, b, result);
    }
    if (x->utf8_need > 0U) {
        return put_utf8(x, b, result);              //a continuation, or a sequence cut short
    }
    if (b == '<') {
        if (x->length > 0U) {
            text_event(x, e, false);
            return EMIT;                            //the '<' is read on the next call
        }
        x->token_position = x->position;
        x->stage = STAGE_TAG_OPEN;
        return TAKE;
    }
    if (x->length + TEXT_ROOM > x->buffer_size) {
        text_event(x, e, true);
        return EMIT;
    }
    if (b == '&') {
        x->entity_length = 0;
        x->entity_position = x->position;
        x->entity_return = STAGE_CONTENT;
        x->stage = STAGE_ENTITY;
        return TAKE;
    }
    return put_utf8(x, b, result);
}

static void start_name(bartap_xml_t *x, uint8_t b)
{
    x->length = 0;
    x->name_hash = BARTAP_XML_HASH_INIT;
    if (b != 0U) {
        x->buffer[x->length++] = b;
        x->name_hash = bartap_xml_hash_byte(x->name_hash, b);
    }
}

static step_t name_byte(bartap_xml_t *x, uint8_t b, bartap_xml_result_t *result)
{
    if (x->length + 1U >= x->buffer_size) {
        *result = BARTAP_XML_RESULT_LIMIT_EXCEEDED;
    } else {
        x->buffer[x->length++] = b;
        x->name_hash = bartap_xml_hash_byte(x->name_hash, b);
    }
    return TAKE;
}

static step_t tag_open(bartap_xml_t *x, uint8_t b, bartap_xml_result_t *result)
{
    if (b == '/') {
        start_name(x, 0);
        x->stage = STAGE_END_NAME;
        return TAKE;
    }
    if (b == '?') {
        x->match = 0;
        x->stage = STAGE_PROCESSING;
        return TAKE;
    }
    if (b == '!') {
        x->entity_length = 0;
        x->stage = STAGE_BANG;
        return TAKE;
    }
    if (is_name_start(b)) {
        if (x->root_closed) {
            //a second root element, or other markup after the root: all of it ignored
            if (recover(x, BARTAP_XML_WARNING_MARKUP, result)) {
                x->stage = STAGE_TRAILER;
            }
            return TAKE;
        }
        start_name(x, b);
        x->stage = STAGE_START_NAME;
        return TAKE;
    }
    //a '<' that starts no markup: literal text
    x->stage = STAGE_CONTENT;
    if (!recover(x, BARTAP_XML_WARNING_MARKUP, result)) {
        return TAKE;
    }
    if (x->depth > 0U) {
        put(x, '<', x->token_position);
    }
    return KEEP;
}

static step_t start_name_byte(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    if (is_name(b)) {
        return name_byte(x, b, result);
    }
    const bool ends = is_space(b) || b == '>' || b == '/';
    if (!ends && !recover(x, BARTAP_XML_WARNING_MARKUP, result)) {
        return TAKE;
    }
    if (x->depth >= x->max_depth) {
        *result = BARTAP_XML_RESULT_LIMIT_EXCEEDED;
        return TAKE;
    }
    x->stack[x->depth++] = x->name_hash;
    x->root_seen = true;
    x->name_length = x->length;
    set_event(x, e, BARTAP_XML_EVENT_START);
    e->name = x->buffer;
    e->name_length = x->name_length;
    x->stage = STAGE_ATTRIBUTE_SPACE;
    return is_space(b) ? EMIT_TAKE : EMIT;
}

static step_t attribute_space(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    if (is_space(b)) {
        return TAKE;
    }
    if (b == '>') {
        start_end_event(x, e, false);
        return EMIT_TAKE;
    }
    if (b == '/') {
        x->stage = STAGE_EMPTY_CLOSE;
        return TAKE;
    }
    if (is_name_start(b)) {
        x->length = x->name_length;
        if (x->length + 1U >= x->buffer_size) {
            *result = BARTAP_XML_RESULT_LIMIT_EXCEEDED;
            return TAKE;
        }
        x->buffer[x->length++] = b;
        x->stage = STAGE_ATTRIBUTE_NAME;
        return TAKE;
    }
    (void)recover(x, BARTAP_XML_WARNING_MARKUP, result);  //a stray byte in the tag: skipped
    return TAKE;
}

static step_t attribute_name(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    if (is_name(b)) {
        if (x->length + 1U >= x->buffer_size) {
            *result = BARTAP_XML_RESULT_LIMIT_EXCEEDED;
        } else {
            x->buffer[x->length++] = b;
        }
        return TAKE;
    }
    if (is_space(b)) {
        x->stage = STAGE_ATTRIBUTE_EQUALS;
        return TAKE;
    }
    if (b == '=') {
        x->stage = STAGE_ATTRIBUTE_QUOTE;
        return TAKE;
    }
    if (!recover(x, BARTAP_XML_WARNING_MARKUP, result)) {
        return TAKE;
    }
    empty_attribute_event(x, e);                    //"<td nowrap>"
    return EMIT;
}

static step_t attribute_equals(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    if (is_space(b)) {
        return TAKE;
    }
    if (b == '=') {
        x->stage = STAGE_ATTRIBUTE_QUOTE;
        return TAKE;
    }
    if (!recover(x, BARTAP_XML_WARNING_MARKUP, result)) {
        return TAKE;
    }
    empty_attribute_event(x, e);                    //"<td nowrap width=...>"
    return EMIT;
}

static step_t attribute_quote(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    if (is_space(b)) {
        return TAKE;
    }
    x->value_start = x->length;
    x->truncated = false;
    if (b == '"' || b == '\'') {
        x->quote = b;
        x->stage = STAGE_ATTRIBUTE_VALUE;
        return TAKE;
    }
    if (!recover(x, BARTAP_XML_WARNING_MARKUP, result)) {
        return TAKE;
    }
    if (b == '>') {
        attribute_event(x, e);                      //"<a href=>"
        return EMIT;
    }
    x->stage = STAGE_ATTRIBUTE_UNQUOTED;
    return KEEP;
}

static step_t enter_entity(bartap_xml_t *x)
{
    x->entity_length = 0;
    x->entity_position = x->position;
    x->entity_return = x->stage;
    x->stage = STAGE_ENTITY;
    return TAKE;
}

static step_t attribute_value(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    if (x->utf8_need > 0U) {
        return put_utf8(x, b, result);
    }
    if (b == x->quote) {
        attribute_event(x, e);
        return EMIT_TAKE;
    }
    if (b == '&') {
        return enter_entity(x);
    }
    if (b == '<' && !recover(x, BARTAP_XML_WARNING_MARKUP, result)) {
        return TAKE;
    }
    return put_utf8(x, is_space(b) ? (uint8_t)' ' : b, result);  //attribute-value normalization
}

static step_t attribute_unquoted(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    if (x->utf8_need > 0U) {
        return put_utf8(x, b, result);
    }
    if (is_space(b)) {
        attribute_event(x, e);
        return EMIT_TAKE;
    }
    if (b == '>') {
        attribute_event(x, e);
        return EMIT;                                //the '>' closes the tag on the next call
    }
    if (b == '/') {
        x->stage = STAGE_UNQUOTED_SLASH;
        return TAKE;
    }
    if (b == '&') {
        return enter_entity(x);
    }
    return put_utf8(x, b, result);
}

static step_t unquoted_slash(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e)
{
    if (b == '>') {
        attribute_event(x, e);
        x->stage = STAGE_EMPTY_CLOSE;               //"<a href=x/>"
        return EMIT;
    }
    put(x, '/', x->position);                       //part of the value, as in "href=a/b"
    x->stage = STAGE_ATTRIBUTE_UNQUOTED;
    return KEEP;
}

static step_t entity(bartap_xml_t *x, uint8_t b, bartap_xml_result_t *result)
{
    if (b == ';') {
        decode_entity(x, result);
        x->stage = x->entity_return;
        return TAKE;
    }
    if (x->entity_length < sizeof x->entity && ((is_name(b) && b < 0x80U) || b == '#')) {
        x->entity[x->entity_length++] = b;
        return TAKE;
    }
    //a bare '&', or a reference far too long: the '&' and what followed it are text
    if (recover(x, BARTAP_XML_WARNING_ENTITY, result)) {
        put_entity_literal(x, false);
        x->stage = x->entity_return;
        return KEEP;
    }
    return TAKE;
}

static step_t bang(bartap_xml_t *x, uint8_t b, bartap_xml_result_t *result)
{
    static const char keywords[3][8] = { "--", "[CDATA[", "DOCTYPE" };
    static const uint8_t lengths[3] = { 2, 7, 7 };
    static const uint8_t next[3] = { STAGE_COMMENT, STAGE_CDATA, STAGE_DOCTYPE };
    if (x->entity_length >= 7U) {
        x->entity_length = 0;       //cannot happen from input; a guard for a restored snapshot
    }
    x->entity[x->entity_length++] = b;
    for (size_t i = 0; i < 3U; i++) {
        if (x->entity_length <= lengths[i] && memcmp(x->entity, keywords[i], x->entity_length) == 0) {
            if (x->entity_length == lengths[i]) {
                x->match = 0;
                x->brackets = 0;
                x->quote = 0;
                x->stage = next[i];
                if (next[i] == STAGE_CDATA && x->depth == 0U) {
                    //CDATA outside the root: skipped as unknown markup
                    x->stage = recover(x, BARTAP_XML_WARNING_MARKUP, result) ? STAGE_BOGUS : STAGE_CONTENT;
                }
            }
            return TAKE;
        }
    }
    //an unknown construct ("<!doctype" in lower case among them): skipped to the next '>'
    if (recover(x, BARTAP_XML_WARNING_MARKUP, result)) {
        x->stage = b == '>' ? STAGE_CONTENT : STAGE_BOGUS;
    }
    return TAKE;
}

static step_t cdata(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    //']' bytes are held back in match until it is clear they are not "]]>"
    if (x->utf8_need > 0U) {
        return put_utf8(x, b, result);
    }
    if (x->length > 0U && x->length + TEXT_ROOM > x->buffer_size) {
        text_event(x, e, true);
        return EMIT;
    }
    if (b == ']') {
        if (x->match == 2U) {
            put(x, ']', x->position - 2U);
        } else {
            x->match++;
        }
        return TAKE;
    }
    if (b == '>' && x->match == 2U) {
        x->match = 0;
        if (x->length > 0U) {
            //its own event: the text after "]]>" has a position of its own
            text_event(x, e, false);
            x->stage = STAGE_CONTENT;
            return EMIT_TAKE;
        }
        x->stage = STAGE_CONTENT;
        return TAKE;
    }
    for (; x->match > 0U; x->match--) {
        put(x, ']', x->position - x->match);
    }
    return put_utf8(x, b, result);
}

//one byte in stage x->stage; *result set on an error
static step_t step(bartap_xml_t *x, uint8_t b, bartap_xml_event_t *e, bartap_xml_result_t *result)
{
    switch (x->stage) {
    case STAGE_CONTENT:
        return content(x, b, e, result);
    case STAGE_TAG_OPEN:
        return tag_open(x, b, result);
    case STAGE_START_NAME:
        return start_name_byte(x, b, e, result);
    case STAGE_ATTRIBUTE_SPACE:
        return attribute_space(x, b, e, result);
    case STAGE_ATTRIBUTE_NAME:
        return attribute_name(x, b, e, result);
    case STAGE_ATTRIBUTE_EQUALS:
        return attribute_equals(x, b, e, result);
    case STAGE_ATTRIBUTE_QUOTE:
        return attribute_quote(x, b, e, result);
    case STAGE_ATTRIBUTE_VALUE:
        return attribute_value(x, b, e, result);
    case STAGE_ATTRIBUTE_UNQUOTED:
        return attribute_unquoted(x, b, e, result);
    case STAGE_UNQUOTED_SLASH:
        return unquoted_slash(x, b, e);
    case STAGE_EMPTY_CLOSE:
        if (b == '>') {
            start_end_event(x, e, true);
            return EMIT_TAKE;
        }
        //a stray '/' in the tag
        if (recover(x, BARTAP_XML_WARNING_MARKUP, result)) {
            x->stage = STAGE_ATTRIBUTE_SPACE;
            return KEEP;
        }
        return TAKE;
    case STAGE_END_NAME:
        if (is_name(b)) {
            return name_byte(x, b, result);
        }
        if (b == '>') {
            return end_tag(x, e, result);
        }
        if (!is_space(b)) {
            (void)recover(x, BARTAP_XML_WARNING_MARKUP, result);
        }
        x->stage = STAGE_END_SPACE;
        return TAKE;
    case STAGE_END_SPACE:
        if (b == '>') {
            return end_tag(x, e, result);
        }
        if (!is_space(b)) {
            (void)recover(x, BARTAP_XML_WARNING_MARKUP, result);
        }
        return TAKE;
    case STAGE_ENTITY:
        return entity(x, b, result);
    case STAGE_BANG:
        return bang(x, b, result);
    case STAGE_COMMENT:
        if (b == '-') {
            x->match = x->match < 2U ? (uint8_t)(x->match + 1U) : 2U;
        } else if (b == '>' && x->match == 2U) {
            x->stage = STAGE_CONTENT;
        } else {
            x->match = 0;
        }
        return TAKE;
    case STAGE_PROCESSING:
        if (b == '>' && x->match == 1U) {
            x->stage = STAGE_CONTENT;
        } else {
            x->match = b == '?' ? 1U : 0U;
        }
        return TAKE;
    case STAGE_DOCTYPE:
        //quoted literals may hold any of the bytes that end or nest the declaration
        if (x->quote != 0U) {
            if (b == x->quote) {
                x->quote = 0;
            }
        } else if (b == '"' || b == '\'') {
            x->quote = b;
        } else if (b == '[') {
            x->brackets = x->brackets < UINT8_MAX ? (uint8_t)(x->brackets + 1U) : UINT8_MAX;
        } else if (b == ']' && x->brackets > 0U) {
            x->brackets--;
        } else if (b == '>' && x->brackets == 0U) {
            x->stage = STAGE_CONTENT;
        }
        return TAKE;
    case STAGE_CDATA:
        return cdata(x, b, e, result);
    case STAGE_BOGUS:
        if (b == '>') {
            x->stage = STAGE_CONTENT;
        }
        return TAKE;
    case STAGE_TRAILER:
        return TAKE;
    default:
        *result = BARTAP_XML_RESULT_CORRUPT;
        return TAKE;
    }
}

bartap_xml_result_t bartap_xml_next(bartap_xml_t *xml, const uint8_t *in, size_t in_size, size_t *in_used,
    bool input_ends, bartap_xml_event_t *event)
{
    if (xml == NULL || in_used == NULL || event == NULL || (in == NULL && in_size != 0U)) {
        return BARTAP_XML_RESULT_INVALID_ARGUMENT;
    }
    *in_used = 0;
    memset(event, 0, sizeof *event);
    if (xml->status != BARTAP_XML_RESULT_OK) {
        return xml->status;
    }
    if (xml->stage == STAGE_DONE) {
        event->kind = BARTAP_XML_EVENT_DONE;
        event->position = xml->position;
        return BARTAP_XML_RESULT_OK;
    }
    if (xml->pending_ends > 0U) {
        pop_end(xml, event);
        return BARTAP_XML_RESULT_OK;
    }
    size_t used = 0;
    while (used < in_size) {
        const uint8_t b = in[used];
        bool emitted = false;
        if (xml->after_cr && b == '\n') {
            //the LF of a CR LF: the CR already stood for both
            xml->after_cr = false;
            if ((xml->stage == STAGE_CONTENT || xml->stage == STAGE_CDATA) && xml->length > 0U) {
                xml->decoded = true;
            }
        } else {
            bartap_xml_result_t result = BARTAP_XML_RESULT_OK;
            const step_t s = step(xml, b == '\r' ? (uint8_t)'\n' : b, event, &result);
            if (result != BARTAP_XML_RESULT_OK) {
                *in_used = used + 1U;
                memset(event, 0, sizeof *event);
                xml->status = result;
                return result;
            }
            if (s == EMIT) {
                *in_used = used;
                return BARTAP_XML_RESULT_OK;
            }
            if (s == KEEP) {
                continue;
            }
            xml->after_cr = b == '\r';
            if (s == EMIT_TAKE) {
                emitted = true;
            }
        }
        used++;
        if (++xml->position == UINT32_MAX) {
            memset(event, 0, sizeof *event);
            xml->status = BARTAP_XML_RESULT_LIMIT_EXCEEDED;     //no document is this large
            *in_used = used;
            return xml->status;
        }
        if (emitted) {
            *in_used = used;
            return BARTAP_XML_RESULT_OK;
        }
    }
    *in_used = used;
    if (!input_ends) {
        return BARTAP_XML_RESULT_OK;                        //NONE: more input wanted
    }
    if (xml->root_closed && ((xml->stage == STAGE_CONTENT && xml->depth == 0U) || !strict(xml))) {
        xml->stage = STAGE_DONE;
        event->kind = BARTAP_XML_EVENT_DONE;
        event->position = xml->position;    //the end of the document
        return BARTAP_XML_RESULT_OK;
    }
    xml->status = BARTAP_XML_RESULT_TRUNCATED;
    return xml->status;
}

/* ------------------------------------------------------------ snapshots */

enum {
    BIT_AFTER_CR = 1U << 0,
    BIT_DECODED = 1U << 1,
    BIT_TRUNCATED = 1U << 2,
    BIT_ROOT_SEEN = 1U << 3,
    BIT_ROOT_CLOSED = 1U << 4,
};

void bartap_xml_save(const bartap_xml_t *xml, bartap_writer_t *w, size_t keep, const uint32_t *rewind_position,
    bool rewind_cdata)
{
    bartap_xml_t x = *xml;
    if (rewind_position != NULL) {
        //inside a run of text with nothing buffered: every partial construct is behind the offset
        x.stage = rewind_cdata ? STAGE_CDATA : STAGE_CONTENT;
        x.length = 0;
        x.match = 0;
        x.entity_length = 0;
        x.entity_return = STAGE_CONTENT;
        x.utf8_have = 0;
        x.utf8_need = 0;
        x.after_cr = false;
        x.decoded = false;
        x.truncated = false;
        x.pending_ends = 0;
        x.position = *rewind_position;
        x.token_position = *rewind_position;
        x.entity_position = *rewind_position;
        x.utf8_position = *rewind_position;
        keep = 0;
    }
    if (keep < x.length) {
        keep = x.length;
    }
    bartap_put32(w, x.flags);
    bartap_put32(w, x.warnings);
    bartap_put32(w, x.depth);
    bartap_put32(w, x.pending_ends);
    bartap_put32(w, x.name_hash);
    bartap_put32(w, x.position);
    bartap_put32(w, x.token_position);
    bartap_put32(w, x.entity_position);
    bartap_put32(w, x.utf8_position);
    bartap_put16(w, (uint32_t)x.length);
    bartap_put16(w, (uint32_t)x.name_length);
    bartap_put16(w, (uint32_t)x.value_start);
    bartap_put16(w, (uint32_t)x.end_name_length);
    bartap_put16(w, (uint32_t)keep);
    bartap_put8(w, x.stage);
    bartap_put8(w, x.quote);
    bartap_put8(w, x.entity_return);
    bartap_put8(w, x.match);
    bartap_put8(w, x.brackets);
    bartap_put8(w, x.bom);
    bartap_put8(w, x.utf8_need);
    bartap_put8(w, x.utf8_low);
    bartap_put8(w, x.utf8_high);
    bartap_put8(w, (x.after_cr ? BIT_AFTER_CR : 0U) | (x.decoded ? BIT_DECODED : 0U) | (x.truncated ? BIT_TRUNCATED : 0U)
        | (x.root_seen ? BIT_ROOT_SEEN : 0U) | (x.root_closed ? BIT_ROOT_CLOSED : 0U));
    bartap_put8(w, x.entity_length);
    bartap_put_bytes(w, x.entity, x.entity_length);
    bartap_put8(w, x.utf8_have);
    bartap_put_bytes(w, x.utf8, x.utf8_have);
    for (uint32_t i = 0; i < x.depth && i < x.max_depth; i++) {
        bartap_put32(w, x.stack[i]);
    }
    bartap_put_bytes(w, x.buffer, keep);
}

static bool in_tag(uint8_t stage, uint8_t entity_return)
{
    const uint8_t s = stage == STAGE_ENTITY ? entity_return : stage;
    return s >= STAGE_ATTRIBUTE_SPACE && s <= STAGE_EMPTY_CLOSE;     //name_length is set once the name is complete
}

static bool in_value(uint8_t stage, uint8_t entity_return)
{
    const uint8_t s = stage == STAGE_ENTITY ? entity_return : stage;
    return s == STAGE_ATTRIBUTE_VALUE || s == STAGE_ATTRIBUTE_UNQUOTED || s == STAGE_UNQUOTED_SLASH;
}

bool bartap_xml_restore(bartap_xml_t *xml, bartap_reader_t *r, bool apply, uint32_t *position)
{
    bartap_xml_t x = *xml;
    if (bartap_get32(r) != x.flags || x.status != BARTAP_XML_RESULT_OK) {
        return false;
    }
    x.warnings = bartap_get32(r);
    x.depth = bartap_get32(r);
    x.pending_ends = bartap_get32(r);
    x.name_hash = bartap_get32(r);
    x.position = bartap_get32(r);
    x.token_position = bartap_get32(r);
    x.entity_position = bartap_get32(r);
    x.utf8_position = bartap_get32(r);
    x.length = bartap_get16(r);
    x.name_length = bartap_get16(r);
    x.value_start = bartap_get16(r);
    x.end_name_length = bartap_get16(r);
    const size_t keep = bartap_get16(r);
    x.stage = (uint8_t)bartap_get8(r);
    x.quote = (uint8_t)bartap_get8(r);
    x.entity_return = (uint8_t)bartap_get8(r);
    x.match = (uint8_t)bartap_get8(r);
    x.brackets = (uint8_t)bartap_get8(r);
    x.bom = (uint8_t)bartap_get8(r);
    x.utf8_need = (uint8_t)bartap_get8(r);
    x.utf8_low = (uint8_t)bartap_get8(r);
    x.utf8_high = (uint8_t)bartap_get8(r);
    const uint32_t bits = bartap_get8(r);
    x.after_cr = (bits & BIT_AFTER_CR) != 0U;
    x.decoded = (bits & BIT_DECODED) != 0U;
    x.truncated = (bits & BIT_TRUNCATED) != 0U;
    x.root_seen = (bits & BIT_ROOT_SEEN) != 0U;
    x.root_closed = (bits & BIT_ROOT_CLOSED) != 0U;
    x.entity_length = (uint8_t)bartap_get8(r);
    if (x.entity_length > sizeof x.entity) {
        return false;
    }
    const uint8_t *entity = bartap_get_bytes(r, x.entity_length);
    x.utf8_have = (uint8_t)bartap_get8(r);
    if (x.utf8_have > sizeof x.utf8 || x.utf8_need > 3U || x.utf8_have + x.utf8_need > sizeof x.utf8) {
        return false;
    }
    const uint8_t *utf8 = bartap_get_bytes(r, x.utf8_have);
    if (r->short_input || bits > 0x1FU || x.stage > STAGE_DONE || x.depth > x.max_depth || x.pending_ends > x.depth
            || x.length > x.buffer_size || x.name_length > x.buffer_size || x.value_start > x.buffer_size
            || x.end_name_length > x.buffer_size || keep > x.buffer_size || keep < x.length
            || (x.pending_ends > 0U && x.end_name_length > keep)
            || x.token_position > x.position || x.entity_position > x.position || x.utf8_position > x.position
            || (x.entity_return != STAGE_CONTENT && x.entity_return != STAGE_ATTRIBUTE_VALUE
                && x.entity_return != STAGE_ATTRIBUTE_UNQUOTED)
            || (in_tag(x.stage, x.entity_return) && x.name_length > x.length)
            || (in_value(x.stage, x.entity_return) && (x.name_length > x.value_start || x.value_start > x.length))
            || (x.stage == STAGE_BANG && x.entity_length >= 7U)) {
        return false;
    }
    //the stack, then the buffer: read in place, copied only when applying
    const uint8_t *stack = bartap_get_bytes(r, (size_t)x.depth * 4U);
    const uint8_t *buffer = bartap_get_bytes(r, keep);
    if (r->short_input) {
        return false;
    }
    *position = x.position;
    if (!apply) {
        return true;
    }
    memcpy(x.entity, entity, x.entity_length);
    memcpy(x.utf8, utf8, x.utf8_have);
    for (uint32_t i = 0; i < x.depth; i++) {
        const uint8_t *p = stack + 4U * i;
        x.stack[i] = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (keep > 0U) {
        memcpy(x.buffer, buffer, keep);
    }
    *xml = x;
    return true;
}
