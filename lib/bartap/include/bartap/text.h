// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_TEXT_H
#define BARTAP_TEXT_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * Text: an XHTML content document as a stream of styled items, an optional
 * layer over epub.h (Q17 option D).
 *
 * Input. The text layer does no reading of its own: the caller feeds it the
 * document's bytes, from wherever they are - a bartap_epub_stream_t as it is
 * read, a copy in PSRAM, a file on SD - in pieces of any size, and each call
 * says how many it took. A loop over a stream:
 *
 *     while (!end) {
 *         if (start == length && !stream_end) {
 *             bartap_epub_stream_read(&stream, chunk, sizeof chunk, &length, &stream_end);
 *             start = 0;
 *             continue;
 *         }
 *         bartap_text_next(&text, chunk + start, length - start, &used, stream_end,
 *             &item, &ready, &end);
 *         start += used;
 *         if (ready) { ... }
 *     }
 *
 * *end means the document was read to its end; it is verified, CRC-32 and
 * all, only when input_ends came from the stream read that set its *end.
 *
 * Items. A WORD is a run of text with no whitespace in it: a word, or part of
 * one where an element boundary or BARTAP_TEXT_WORD_MAX splits it. What comes
 * between words is the next one's `before`: nothing (the two are one word),
 * collapsed whitespace, a line break (br) or a paragraph break (the start or
 * end of a block element). A RULE is an hr. With BARTAP_TEXT_DELIVER_ELEMENTS
 * every element is also delivered, as ELEMENT_START (its name), then with
 * BARTAP_TEXT_DELIVER_ATTRIBUTES one ATTRIBUTE item per attribute, then
 * ELEMENT_OPEN once the start tag is complete, and ELEMENT_END after its
 * content. Element items are delivered inside dropped elements too, so that
 * a caller can see a head's links; words there are not.
 *
 * Styling comes from element names, not CSS: italic (em, i, cite, dfn, var),
 * bold (strong, b), monospace (code, kbd, samp, tt), superscript (sup),
 * subscript (sub), underline (u, ins), strike (s, strike, del), headings
 * (h1-h6), and an indent level for blockquote and list nesting. head, script,
 * style and template are dropped. Right after an ELEMENT_OPEN item the caller
 * may override what the element means (bartap_text_override): the style it
 * sets or clears, whether it is a block, inline or dropped, its heading level,
 * its indent, and a caller-defined tag that its descendants inherit. A caller
 * that reads the book's CSS maps classes to overrides this way, and uses the
 * tag for anything Bartap does not model (alignment, small caps).
 *
 * Whitespace is XML's (space, tab, CR, LF); U+00A0 and other spaces are text.
 * Line ends are normalized, the XHTML 1.x named entities decoded, and invalid
 * UTF-8 replaced by U+FFFD (the tokenizer's rules, src/xml.h). Markup errors
 * are recovered from, and reported by bartap_text_warnings, unless
 * BARTAP_TEXT_STRICT is set; then they are CORRUPT.
 *
 * Positions. Every item has a position, an EPUB CFI (bartap_text_position),
 * in the form bartap_epub_cfi_parse produces and bartap_epub_cfi_resolver_*
 * takes: a word's is its first character's, any other item's its element's.
 * Character offsets count UTF-16 code units of the normalized, decoded text,
 * as a DOM does. In a document whose markup was recovered from, they are
 * Bartap's own: consistent between the text layer and the resolver when both
 * are given the same flags, but a DOM-based reader may refuse such a document
 * or repair it differently.
 *
 * Moving about. bartap_text_seek and bartap_text_seek_id suppress delivery
 * until a position or an element id. Keep a position as its CFI.
 *
 * Bounded work: every call does at most one tokenizer call, so work linear
 * in in_size plus the nesting depth. No call reads. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bartap/epub.h"

#define BARTAP_TEXT_WORD_MAX 64U                //bytes of text in one WORD item
#define BARTAP_TEXT_DEPTH_MAX 64U               //element nesting accepted; deeper is LIMIT_EXCEEDED

#define BARTAP_TEXT_SIZE 2560U

/* A content document being read as text. Private. */
typedef struct {
    BARTAP_EPUB_OPAQUE(BARTAP_TEXT_SIZE) storage;
} bartap_text_t;

/* bartap_text_init flags. */
#define BARTAP_TEXT_DELIVER_ELEMENTS   (1UL << 0)
#define BARTAP_TEXT_DELIVER_ATTRIBUTES (1UL << 1)
#define BARTAP_TEXT_STRICT             BARTAP_EPUB_RESOLVE_STRICT

/* Warning bits: what leniency recovered from. Append-only. */
#define BARTAP_TEXT_WARNING_ENTITY   (1UL << 0) //a malformed or unknown entity reference, kept as text
#define BARTAP_TEXT_WARNING_MARKUP   (1UL << 1) //malformed markup, recovered from
#define BARTAP_TEXT_WARNING_END_TAG  (1UL << 2) //an end tag that did not match the open element
#define BARTAP_TEXT_WARNING_ENCODING (1UL << 3) //invalid UTF-8, replaced by U+FFFD

typedef enum {
    BARTAP_TEXT_ITEM_WORD = 0,
    BARTAP_TEXT_ITEM_RULE = 1,                  //an hr
    BARTAP_TEXT_ITEM_ELEMENT_START = 2,         //text is the element's qualified name
    BARTAP_TEXT_ITEM_ATTRIBUTE = 3,             //text is the name, value the value
    BARTAP_TEXT_ITEM_ELEMENT_OPEN = 4,          //the start tag is complete: bartap_text_override allowed now
    BARTAP_TEXT_ITEM_ELEMENT_END = 5,           //text is the name, or empty for an element recovery closed
} bartap_text_item_kind_t;

/* What comes before a WORD or RULE, in increasing strength. */
typedef enum {
    BARTAP_TEXT_BEFORE_NONE = 0,                //same word as the previous one
    BARTAP_TEXT_BEFORE_SPACE = 1,
    BARTAP_TEXT_BEFORE_LINE = 2,                //a br
    BARTAP_TEXT_BEFORE_PARAGRAPH = 3,           //a new block; also the document's first word
} bartap_text_before_t;

#define BARTAP_TEXT_STYLE_ITALIC      (1U << 0)
#define BARTAP_TEXT_STYLE_BOLD        (1U << 1)
#define BARTAP_TEXT_STYLE_MONOSPACE   (1U << 2)
#define BARTAP_TEXT_STYLE_SUPERSCRIPT (1U << 3)
#define BARTAP_TEXT_STYLE_SUBSCRIPT   (1U << 4)
#define BARTAP_TEXT_STYLE_UNDERLINE   (1U << 5)
#define BARTAP_TEXT_STYLE_STRIKE      (1U << 6)

typedef struct {
    uint8_t kind;                               //a bartap_text_item_kind_t
    const uint8_t *text;                        //UTF-8; valid until the next call. NULL for a RULE
    size_t length;
    const uint8_t *value;                       //ATTRIBUTE only
    size_t value_length;
    bool value_truncated;                       //ATTRIBUTE: longer than the tokenizer's buffer allows
    uint8_t before;                             //WORD, RULE: a bartap_text_before_t
    uint8_t style;                              //BARTAP_TEXT_STYLE_* bits
    uint8_t heading;                            //1-6 inside h1-h6, else 0
    uint8_t indent;                             //blockquote and list nesting
    uint16_t user;                              //the caller's tag, inherited
    uint32_t depth;                             //element items: the element's depth, 1 for the root
} bartap_text_item_t;

/* An element's style, heading, indent and user fields on its ELEMENT_START,
 * ATTRIBUTE and ELEMENT_OPEN items are those it inherits; on its ELEMENT_END
 * they are its own. */

typedef enum {
    BARTAP_TEXT_DISPLAY_DEFAULT = 0,            //what its name means
    BARTAP_TEXT_DISPLAY_INLINE = 1,             //no paragraph breaks, not dropped, no rule
    BARTAP_TEXT_DISPLAY_BLOCK = 2,              //a paragraph break before and after
    BARTAP_TEXT_DISPLAY_NONE = 3,               //its words dropped, as head and script are
} bartap_text_display_t;

typedef struct {
    uint8_t style_clear;                        //cleared from the inherited and built-in style
    uint8_t style_set;                          //then set
    uint8_t display;                            //a bartap_text_display_t
    uint8_t heading;                            //0: keep; 1-6: set; 7: clear
    int8_t indent;                              //added to the element's indent, kept within 0-255
    bool set_user;
    uint16_t user;
} bartap_text_override_t;

/* Start a document. spine_index only labels positions. flags are
 * BARTAP_TEXT_* bits. No read. */
bartap_epub_result_t bartap_text_init(bartap_text_t *text, uint32_t spine_index, uint32_t flags);

/* Feed bytes from in (in_size bytes; *in_used says how many were taken, and
 * the rest must be offered again). input_ends: in holds the last of the
 * document. OK with *ready set: *item is the next item; OK with *end set: the
 * document has ended and there are no more; OK with neither: call again, with
 * more input if all of it was taken. At most one item per call. Errors are
 * final. If a seek is still waiting when the document ends, the result is
 * NOT_FOUND, with *end set. */
bartap_epub_result_t bartap_text_next(bartap_text_t *text, const uint8_t *in, size_t in_size, size_t *in_used,
    bool input_ends, bartap_text_item_t *item, bool *ready, bool *end);

/* The position of the item the last call delivered (see above), so to be
 * asked before the next call. No read. LIMIT_EXCEEDED if it sits deeper than
 * BARTAP_EPUB_CFI_STEPS_MAX steps; INVALID_ARGUMENT if the last call
 * delivered no item. */
bartap_epub_result_t bartap_text_position(const bartap_text_t *text, bartap_epub_cfi_t *cfi);

/* Change what the element of the ELEMENT_OPEN item just delivered means. It
 * takes effect on the next call. INVALID_ARGUMENT unless the last item was
 * ELEMENT_OPEN, or for values out of range. */
bartap_epub_result_t bartap_text_override(bartap_text_t *text, const bartap_text_override_t *override);

/* What leniency has recovered from so far: BARTAP_TEXT_WARNING_* bits. */
uint32_t bartap_text_warnings(const bartap_text_t *text);

/* Deliver nothing until the item that holds *target, or the first one after
 * it: for a character, the word holding it; for an element, its first item.
 * Everything before is read as usual, just not delivered. A target that no
 * longer exists lands on the next item. Replaces any seek waiting. */
bartap_epub_result_t bartap_text_seek(bartap_text_t *text, const bartap_epub_cfi_t *target);

/* Deliver nothing until the element whose id (or, on an a element, name)
 * attribute is id (id_length bytes, at most BARTAP_EPUB_FRAGMENT_MAX), as a
 * table-of-contents target's fragment gives it. Delivery resumes with that
 * element's ELEMENT_OPEN item, or without element items its first word or
 * rule; bartap_text_position then gives a CFI to keep. Replaces any seek
 * waiting. */
bartap_epub_result_t bartap_text_seek_id(bartap_text_t *text, const uint8_t *id, size_t id_length);

#endif
