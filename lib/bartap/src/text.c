// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Text layer (bartap/text.h): the caller's bytes through the XML tokenizer, its events turned into items.
 *
 * Each call makes one tokenizer call, or applies an event held from the call before. An event is held when
 * something must be delivered first: the word it ends, or an element's ELEMENT_OPEN item, after which the
 * caller may override what the element means before the start tag is applied. Positions are kept as each
 * open element's index among its parent's element children, and a count of UTF-16 code units since the
 * last element boundary, the same counts cfi.c resolves. */

#include "bartap/text.h"

#include <string.h>

#include "names.h"
#include "xml.h"

#define TOKEN_SIZE 512U
#define DEPTH_MAX BARTAP_TEXT_DEPTH_MAX

#define LEVEL_BLOCK 0x01U           //a paragraph break before and after
#define LEVEL_DROPPED 0x02U         //its words are dropped

#define FLAGS_ALL (BARTAP_TEXT_DELIVER_ELEMENTS | BARTAP_TEXT_DELIVER_ATTRIBUTES | BARTAP_TEXT_STRICT)

enum {
    SEEK_NONE,
    SEEK_CFI,
    SEEK_ID,
};

//held_used for a held START_END: its ELEMENT_OPEN item has gone out, and the start tag is applied next
#define HELD_APPLY 1U

//one open element
typedef struct {
    uint32_t index;                 //1-based, among its parent's element children
    uint32_t children;              //its element children so far
    uint16_t user;
    uint8_t style;
    uint8_t heading;
    uint8_t indent;
    uint8_t flags;                  //LEVEL_* bits
} level_t;

typedef struct {
    bartap_xml_t xml;
    uint32_t stack[DEPTH_MAX];
    uint8_t token[TOKEN_SIZE];
    level_t levels[DEPTH_MAX + 1U]; //[1] is the root element; [0] stands for the document
    uint32_t flags;
    uint32_t spine_index;
    uint32_t depth;
    uint32_t skip_depth;            //of the outermost element whose words are dropped; 0 if none
    uint32_t units;                 //UTF-16 code units of the current text so far
    bartap_xml_event_t held;        //an event to apply on the next call
    size_t held_used;               //of a held TEXT event, the bytes already taken
    bool holding;
    uint8_t pending;                //what comes before the next word
    //the word being built
    uint8_t word[BARTAP_TEXT_WORD_MAX];
    uint8_t word_length;
    uint8_t word_before;
    uint32_t word_depth;            //the element holding it
    uint32_t word_step;             //its text step (odd)
    uint32_t word_units;            //its offset in that text
    bool delivered;                 //the word buffer holds the word last delivered
    //the item last delivered, for its position
    bool has_item;
    uint32_t item_depth;
    uint32_t item_step;             //0 for an element's own position
    uint32_t item_units;
    //an override for the element whose ELEMENT_OPEN went out last
    bool override_allowed;
    bool has_override;
    bartap_text_override_t override;
    //a seek
    uint8_t seek;
    bool id_matched;                //the start tag being read has the id sought
    bool name_matched;              //or a name attribute with it
    uint8_t seek_id_length;
    uint8_t seek_id[BARTAP_EPUB_FRAGMENT_MAX];
    bartap_epub_cfi_t seek_cfi;
    bool finished;
    bartap_epub_result_t status;
} text_t;

_Static_assert(sizeof(text_t) <= BARTAP_TEXT_SIZE, "BARTAP_TEXT_SIZE is too small");
_Static_assert(BARTAP_TEXT_WORD_MAX >= 4U && BARTAP_TEXT_WORD_MAX <= UINT8_MAX, "a word holds any UTF-8 sequence");
_Static_assert(BARTAP_TEXT_WARNING_ENTITY == BARTAP_XML_WARNING_ENTITY
    && BARTAP_TEXT_WARNING_MARKUP == BARTAP_XML_WARNING_MARKUP
    && BARTAP_TEXT_WARNING_END_TAG == BARTAP_XML_WARNING_END_TAG
    && BARTAP_TEXT_WARNING_ENCODING == BARTAP_XML_WARNING_ENCODING, "the tokenizer's warning bits are the text layer's");

static text_t *text_state(bartap_text_t *text)
{
    return (text_t *)(void *)text->storage.bytes;
}

static const text_t *text_state_const(const bartap_text_t *text)
{
    return (const text_t *)(const void *)text->storage.bytes;
}

static bool is_space(uint8_t b)
{
    return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}

//the local part of a qualified name is one of the names in list, NUL-separated and ending in an empty name.
//Lengths are found by a bounded loop: an unbounded one can be compiled into a call to strlen
static bool local_in(const uint8_t *name, size_t length, const char *list)
{
    const size_t start = bartap_local_start(name, length);
    const char *p = list;
    while (*p != '\0') {
        size_t n = 0;
        while (n < 16U && p[n] != '\0') {
            n++;
        }
        if (bartap_bytes_are(name + start, length - start, p, n)) {
            return true;
        }
        p += n + 1U;
    }
    return false;
}

//element names and what they mean, matched by local name, so a prefixed XHTML name counts too
static const char blocks[] = "address\0article\0aside\0blockquote\0body\0caption\0dd\0details\0div\0dl\0dt\0"
    "figcaption\0figure\0footer\0h1\0h2\0h3\0h4\0h5\0h6\0header\0hgroup\0li\0main\0nav\0ol\0p\0pre\0section\0"
    "summary\0table\0td\0th\0tr\0ul\0";
static const char dropped[] = "head\0script\0style\0template\0";
static const char italics[] = "em\0i\0cite\0dfn\0var\0";
static const char bolds[] = "strong\0b\0";
static const char monospaced[] = "code\0kbd\0samp\0tt\0";
static const char underlined[] = "u\0ins\0";
static const char struck[] = "s\0strike\0del\0";
static const char indents[] = "blockquote\0ol\0ul\0dl\0";

static unsigned heading_level(const uint8_t *name, size_t length)
{
    const size_t start = bartap_local_start(name, length);
    if (length - start == 2U && name[start] == 'h' && name[start + 1U] >= '1' && name[start + 1U] <= '6') {
        return (unsigned)(name[start + 1U] - '0');
    }
    return 0;
}

bartap_epub_result_t bartap_text_init(bartap_text_t *text, uint32_t spine_index, uint32_t flags)
{
    if (text == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    text_t *t = text_state(text);
    memset(t, 0, sizeof *t);
    if ((flags & ~FLAGS_ALL) != 0U) {
        t->status = BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
        return t->status;
    }
    (void)bartap_xml_init(&t->xml, t->token, sizeof t->token, t->stack, DEPTH_MAX,
        BARTAP_XML_FLAG_XHTML_ENTITIES | ((flags & BARTAP_TEXT_STRICT) != 0U ? BARTAP_XML_FLAG_STRICT : 0U));
    t->flags = flags;
    t->spine_index = spine_index;
    t->pending = BARTAP_TEXT_BEFORE_PARAGRAPH;
    t->status = BARTAP_EPUB_RESULT_OK;
    return BARTAP_EPUB_RESULT_OK;
}

static void raise_pending(text_t *t, uint8_t before)
{
    if (before > t->pending) {
        t->pending = before;
    }
}

//hold e for the next call, so that what it ends goes out first
static void hold(text_t *t, const bartap_xml_event_t *e, size_t used)
{
    t->held = *e;
    t->holding = true;
    t->held_used = used;
}

/* ------------------------------------------------------------ items */

//what apply() produced, if anything
typedef struct {
    bool made;
    uint8_t kind;
    const bartap_xml_event_t *event;   //for element items: their name and value
    uint32_t depth;                 //the element; for a WORD, the one holding it
} made_t;

static void make(made_t *m, uint8_t kind, const bartap_xml_event_t *e, uint32_t depth)
{
    m->made = true;
    m->kind = kind;
    m->event = e;
    m->depth = depth;
}

//an element starts: a level, inheriting its parent's styling
static void start_element(text_t *t, const bartap_xml_event_t *e, made_t *m)
{
    const uint32_t d = e->depth;
    if (d == 0U || d > DEPTH_MAX) {
        return;
    }
    level_t *parent = &t->levels[d - 1U];
    level_t *l = &t->levels[d];
    parent->children++;
    l->index = parent->children;
    l->children = 0;
    l->style = parent->style;
    l->heading = parent->heading;
    l->indent = parent->indent;
    l->user = parent->user;
    l->flags = 0;
    t->depth = d;
    t->units = 0;
    t->id_matched = false;
    t->name_matched = false;
    if (t->flags & BARTAP_TEXT_DELIVER_ELEMENTS) {
        make(m, BARTAP_TEXT_ITEM_ELEMENT_START, e, d);
    }
}

static void attribute(text_t *t, const bartap_xml_event_t *e, made_t *m)
{
    if (t->seek == SEEK_ID && !e->truncated && bartap_bytes_are(e->value, e->value_length,
            (const char *)t->seek_id, t->seek_id_length)) {
        if (bartap_bytes_are(e->name, e->name_length, BARTAP_LITERAL("id"))
                || bartap_bytes_are(e->name, e->name_length, BARTAP_LITERAL("xml:id"))) {
            t->id_matched = true;
        } else if (bartap_bytes_are(e->name, e->name_length, BARTAP_LITERAL("name"))) {
            t->name_matched = true;
        }
    }
    if (t->flags & BARTAP_TEXT_DELIVER_ATTRIBUTES) {
        make(m, BARTAP_TEXT_ITEM_ATTRIBUTE, e, e->depth);
    }
}

//the start tag is complete: apply what the element's name means, and the caller's override
static void open_element(text_t *t, const bartap_xml_event_t *e, made_t *m)
{
    const uint32_t d = e->depth;
    if (d == 0U || d > DEPTH_MAX) {
        return;
    }
    level_t *l = &t->levels[d];
    const uint8_t *name = e->name;
    const size_t n = e->name_length;
    bool drop = local_in(name, n, dropped);
    bool block = local_in(name, n, blocks);
    bool rule = bartap_local_is(name, n, BARTAP_LITERAL("hr"));
    bool line = bartap_local_is(name, n, BARTAP_LITERAL("br"));
    uint8_t style = 0;
    style |= local_in(name, n, italics) ? BARTAP_TEXT_STYLE_ITALIC : 0U;
    style |= local_in(name, n, bolds) ? BARTAP_TEXT_STYLE_BOLD : 0U;
    style |= local_in(name, n, monospaced) ? BARTAP_TEXT_STYLE_MONOSPACE : 0U;
    style |= bartap_local_is(name, n, BARTAP_LITERAL("sup")) ? BARTAP_TEXT_STYLE_SUPERSCRIPT : 0U;
    style |= bartap_local_is(name, n, BARTAP_LITERAL("sub")) ? BARTAP_TEXT_STYLE_SUBSCRIPT : 0U;
    style |= local_in(name, n, underlined) ? BARTAP_TEXT_STYLE_UNDERLINE : 0U;
    style |= local_in(name, n, struck) ? BARTAP_TEXT_STYLE_STRIKE : 0U;
    l->style = (uint8_t)(l->style | style);
    int indent = l->indent + (local_in(name, n, indents) ? 1 : 0);
    const unsigned heading = heading_level(name, n);
    if (heading != 0U) {
        l->heading = (uint8_t)heading;
    }
    if (t->has_override) {
        const bartap_text_override_t *o = &t->override;
        switch (o->display) {
        case BARTAP_TEXT_DISPLAY_INLINE:
            block = false;
            drop = false;
            rule = false;
            break;
        case BARTAP_TEXT_DISPLAY_BLOCK:
            block = true;
            drop = false;
            rule = false;
            break;
        case BARTAP_TEXT_DISPLAY_NONE:
            block = false;
            drop = true;
            rule = false;
            line = false;
            break;
        default:
            break;
        }
        l->style = (uint8_t)((l->style & ~o->style_clear) | o->style_set);
        if (o->heading >= 1U && o->heading <= 6U) {
            l->heading = o->heading;
        } else if (o->heading == 7U) {
            l->heading = 0;
        }
        indent += o->indent;
        if (o->set_user) {
            l->user = o->user;
        }
        t->has_override = false;
    }
    l->indent = (uint8_t)(indent < 0 ? 0 : indent > UINT8_MAX ? UINT8_MAX : indent);
    if (drop) {
        l->flags |= LEVEL_DROPPED;
        if (t->skip_depth == 0U) {
            t->skip_depth = d;
        }
    }
    if (block) {
        l->flags |= LEVEL_BLOCK;
    }
    if (t->skip_depth != 0U) {
        return;
    }
    if (block) {
        raise_pending(t, BARTAP_TEXT_BEFORE_PARAGRAPH);
    }
    if (line) {
        raise_pending(t, BARTAP_TEXT_BEFORE_LINE);
    }
    if (rule) {
        t->pending = BARTAP_TEXT_BEFORE_PARAGRAPH;      //what follows a rule starts a new paragraph
        make(m, BARTAP_TEXT_ITEM_RULE, e, d);
    }
}

static void end_element(text_t *t, const bartap_xml_event_t *e, made_t *m)
{
    const uint32_t d = e->depth;
    if (d == 0U || d > DEPTH_MAX) {
        return;
    }
    if (t->skip_depth == d) {
        t->skip_depth = 0;
    } else if (t->skip_depth == 0U && (t->levels[d].flags & LEVEL_BLOCK)) {
        raise_pending(t, BARTAP_TEXT_BEFORE_PARAGRAPH);
    }
    if (t->skip_depth > d - 1U) {
        t->skip_depth = 0;          //closed along with an element recovery skipped past
    }
    t->depth = d - 1U;
    t->units = 0;                   //the parent's next text starts after this element
    if (t->flags & BARTAP_TEXT_DELIVER_ELEMENTS) {
        make(m, BARTAP_TEXT_ITEM_ELEMENT_END, e, d);
    }
}

//take text from a TEXT event, from byte *used on; true when a word is complete (the byte that ended it not
//taken)
static bool take_text(text_t *t, const bartap_xml_event_t *e, size_t *used)
{
    for (size_t i = *used; i < e->value_length; i++) {
        const uint8_t b = e->value[i];
        if (is_space(b)) {
            if (t->word_length > 0U) {
                *used = i;
                return true;
            }
            t->units++;
            raise_pending(t, BARTAP_TEXT_BEFORE_SPACE);
            continue;
        }
        const bool lead = (b & 0xC0U) != 0x80U;
        if (lead) {
            const size_t sequence = b >= 0xF0U ? 4U : b >= 0xE0U ? 3U : b >= 0xC0U ? 2U : 1U;
            if (t->word_length > 0U && t->word_length + sequence > sizeof t->word) {
                *used = i;          //full: the rest of the word is a word of its own
                return true;
            }
        } else if (t->word_length == sizeof t->word) {
            continue;               //cannot happen with valid UTF-8, which the tokenizer ensures
        }
        if (t->word_length == 0U) {
            t->word_before = t->pending;
            t->pending = BARTAP_TEXT_BEFORE_NONE;
            t->word_depth = t->depth;
            t->word_step = 2U * t->levels[t->depth].children + 1U;
            t->word_units = t->units;
        }
        t->word[t->word_length++] = b;
        if (lead) {
            t->units += b >= 0xF0U ? 2U : 1U;   //outside the BMP: a surrogate pair in UTF-16
        }
    }
    *used = e->value_length;
    return false;
}

static bool word_pending(const text_t *t)
{
    return t->word_length > 0U && !t->delivered;
}

//apply one event, from byte used of a TEXT event; *m says what item it made
static void apply(text_t *t, const bartap_xml_event_t *e, size_t used, made_t *m)
{
    switch (e->kind) {
    case BARTAP_XML_EVENT_START:
    case BARTAP_XML_EVENT_END:
    case BARTAP_XML_EVENT_DONE:
        if (word_pending(t)) {
            hold(t, e, 0);
            make(m, BARTAP_TEXT_ITEM_WORD, NULL, t->word_depth);
            return;
        }
        if (e->kind == BARTAP_XML_EVENT_START) {
            start_element(t, e, m);
        } else if (e->kind == BARTAP_XML_EVENT_END) {
            end_element(t, e, m);
        } else {
            t->finished = true;
        }
        return;
    case BARTAP_XML_EVENT_ATTRIBUTE:
        attribute(t, e, m);
        return;
    case BARTAP_XML_EVENT_START_END:
        if (used != HELD_APPLY && t->seek == SEEK_ID
                && (t->id_matched || (t->name_matched && bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("a"))))) {
            t->seek = SEEK_NONE;    //found: this element's first item goes out
        }
        if (used != HELD_APPLY && (t->flags & BARTAP_TEXT_DELIVER_ELEMENTS)) {
            hold(t, e, HELD_APPLY);
            make(m, BARTAP_TEXT_ITEM_ELEMENT_OPEN, e, e->depth);
            return;
        }
        open_element(t, e, m);
        return;
    case BARTAP_XML_EVENT_TEXT:
        if (t->skip_depth != 0U || e->depth != t->depth) {
            return;
        }
        if (take_text(t, e, &used)) {
            hold(t, e, used);
            make(m, BARTAP_TEXT_ITEM_WORD, NULL, t->word_depth);
        }
        return;
    default:
        return;
    }
}

/* ------------------------------------------------------------ positions */

//the CFI of an element at depth, and with step a text position in it: the root element is not a step
static bool position_of(const text_t *t, uint32_t depth, uint32_t step, uint32_t units, bartap_epub_cfi_t *cfi)
{
    memset(cfi, 0, sizeof *cfi);
    cfi->spine_index = t->spine_index;
    bool fits = true;
    for (uint32_t d = 2; d <= depth && d <= DEPTH_MAX; d++) {
        if (cfi->step_count == BARTAP_EPUB_CFI_STEPS_MAX) {
            fits = false;
            break;
        }
        cfi->steps[cfi->step_count++] = 2U * t->levels[d].index;
    }
    if (fits && step != 0U) {
        if (cfi->step_count == BARTAP_EPUB_CFI_STEPS_MAX) {
            return false;
        }
        cfi->steps[cfi->step_count++] = step;
        cfi->has_offset = true;
        cfi->offset = units;
    }
    return fits;
}

//whether an item may go out under a seek for a position: it holds the target, or comes after it
static bool past_target(const text_t *t, const made_t *m)
{
    bartap_epub_cfi_t here;
    if (m->kind == BARTAP_TEXT_ITEM_WORD) {
        //its last character, so that a word holding the target counts
        uint32_t units = t->word_units;
        for (uint8_t i = 0; i < t->word_length; i++) {
            if ((t->word[i] & 0xC0U) != 0x80U) {
                units += t->word[i] >= 0xF0U ? 2U : 1U;
            }
        }
        (void)position_of(t, t->word_depth, t->word_step, units - 1U, &here);
    } else {
        (void)position_of(t, m->depth, 0, 0, &here);
    }
    const bartap_epub_cfi_t *target = &t->seek_cfi;
    if (m->kind == BARTAP_TEXT_ITEM_ELEMENT_END && here.spine_index == target->spine_index
            && here.step_count <= target->step_count
            && memcmp(here.steps, target->steps, here.step_count * sizeof here.steps[0]) == 0) {
        return true;                //the end of an element the target is in: after it
    }
    return bartap_epub_cfi_compare(&here, target) >= 0;
}

/* ------------------------------------------------------------ the stream of items */

static void deliver(text_t *t, const made_t *m, bartap_text_item_t *item)
{
    memset(item, 0, sizeof *item);
    item->kind = m->kind;
    const uint32_t d = m->depth <= DEPTH_MAX ? m->depth : DEPTH_MAX;
    const level_t *l = &t->levels[d];
    item->style = l->style;
    item->heading = l->heading;
    item->indent = l->indent;
    item->user = l->user;
    item->depth = d;
    t->has_item = true;
    t->item_depth = d;
    t->item_step = 0;
    t->item_units = 0;
    switch (m->kind) {
    case BARTAP_TEXT_ITEM_WORD:
        item->text = t->word;
        item->length = t->word_length;
        item->before = t->word_before;
        t->item_step = t->word_step;
        t->item_units = t->word_units;
        break;
    case BARTAP_TEXT_ITEM_RULE:
        item->before = BARTAP_TEXT_BEFORE_PARAGRAPH;
        break;
    default:
        item->text = m->event->name;
        item->length = m->event->name_length;
        if (m->kind == BARTAP_TEXT_ITEM_ATTRIBUTE) {
            item->value = m->event->value;
            item->value_length = m->event->value_length;
            item->value_truncated = m->event->truncated;
        }
        if (item->text == NULL) {
            item->text = t->word;   //an END that recovery made: an empty name, never NULL
        }
        break;
    }
    t->override_allowed = m->kind == BARTAP_TEXT_ITEM_ELEMENT_OPEN;
}

static bartap_epub_result_t from_xml(bartap_xml_result_t r)
{
    switch (r) {
    case BARTAP_XML_RESULT_OK: return BARTAP_EPUB_RESULT_OK;
    case BARTAP_XML_RESULT_LIMIT_EXCEEDED: return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
    case BARTAP_XML_RESULT_UNSUPPORTED: return BARTAP_EPUB_RESULT_UNSUPPORTED;
    case BARTAP_XML_RESULT_INVALID_ARGUMENT: return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    default: return BARTAP_EPUB_RESULT_CORRUPT;
    }
}

bartap_epub_result_t bartap_text_next(bartap_text_t *text, const uint8_t *in, size_t in_size, size_t *in_used,
    bool input_ends, bartap_text_item_t *item, bool *ready, bool *end)
{
    if (text == NULL || in_used == NULL || item == NULL || ready == NULL || end == NULL
            || (in == NULL && in_size != 0U)) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    text_t *t = text_state(text);
    *in_used = 0;
    *ready = false;
    *end = t->finished;
    if (t->status != BARTAP_EPUB_RESULT_OK) {
        return t->status;
    }
    //what was delivered last is over: its word, its position and the chance to override
    if (t->delivered) {
        t->delivered = false;
        t->word_length = 0;
    }
    t->has_item = false;
    t->override_allowed = false;
    if (t->finished) {
        return BARTAP_EPUB_RESULT_OK;
    }
    made_t m = { false, 0, NULL, 0 };
    bartap_xml_event_t e;
    if (t->holding) {
        t->holding = false;
        e = t->held;
        apply(t, &e, t->held_used, &m);
    } else {
        const bartap_xml_result_t r = bartap_xml_next(&t->xml, in, in_size, in_used, input_ends, &e);
        if (r != BARTAP_XML_RESULT_OK) {
            t->status = from_xml(r);
            return t->status;
        }
        if (e.kind != BARTAP_XML_EVENT_NONE) {
            apply(t, &e, 0, &m);
        }
    }
    if (m.made) {
        if (m.kind == BARTAP_TEXT_ITEM_WORD) {
            t->delivered = true;    //cleared on the next call, delivered or not
        }
        if (t->seek == SEEK_CFI && past_target(t, &m)) {
            t->seek = SEEK_NONE;
        }
        if (t->seek == SEEK_NONE) {
            deliver(t, &m, item);
            *ready = true;
        }
    }
    *end = t->finished;
    if (t->finished && t->seek != SEEK_NONE) {
        t->status = BARTAP_EPUB_RESULT_NOT_FOUND;   //the seek's target is not in this document
        return t->status;
    }
    return BARTAP_EPUB_RESULT_OK;
}

bartap_epub_result_t bartap_text_position(const bartap_text_t *text, bartap_epub_cfi_t *cfi)
{
    if (text == NULL || cfi == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    const text_t *t = text_state_const(text);
    if (!t->has_item) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    return position_of(t, t->item_depth, t->item_step, t->item_units, cfi) ? BARTAP_EPUB_RESULT_OK
        : BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
}

bartap_epub_result_t bartap_text_override(bartap_text_t *text, const bartap_text_override_t *override)
{
    if (text == NULL || override == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    text_t *t = text_state(text);
    if (!t->override_allowed || t->status != BARTAP_EPUB_RESULT_OK
            || override->display > BARTAP_TEXT_DISPLAY_NONE || override->heading > 7U) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    t->override = *override;
    t->has_override = true;
    return BARTAP_EPUB_RESULT_OK;
}

uint32_t bartap_text_warnings(const bartap_text_t *text)
{
    return text == NULL ? 0U : text_state_const(text)->xml.warnings;
}

bartap_epub_result_t bartap_text_seek(bartap_text_t *text, const bartap_epub_cfi_t *target)
{
    if (text == NULL || target == NULL || target->step_count > BARTAP_EPUB_CFI_STEPS_MAX) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    text_t *t = text_state(text);
    if (t->status != BARTAP_EPUB_RESULT_OK) {
        return t->status;
    }
    t->seek_cfi = *target;
    t->seek = SEEK_CFI;
    return BARTAP_EPUB_RESULT_OK;
}

bartap_epub_result_t bartap_text_seek_id(bartap_text_t *text, const uint8_t *id, size_t id_length)
{
    if (text == NULL || id == NULL || id_length == 0U || id_length > BARTAP_EPUB_FRAGMENT_MAX) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    text_t *t = text_state(text);
    if (t->status != BARTAP_EPUB_RESULT_OK) {
        return t->status;
    }
    memcpy(t->seek_id, id, id_length);
    t->seek_id_length = (uint8_t)id_length;
    t->seek = SEEK_ID;
    t->id_matched = false;
    t->name_matched = false;
    return BARTAP_EPUB_RESULT_OK;
}

