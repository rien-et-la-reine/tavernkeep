// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_XML_H
#define BARTAP_XML_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * XML: a streaming, non-validating XML tokenizer. Internal to Bartap (D9),
 * used for META-INF/container.xml, the OPF, the navigation document or NCX,
 * and content documents (the text layer and CFI resolution).
 *
 * - Pull model: each call returns at most one event, from input handed over
 *   in pieces of any size. Work per call is linear in the input offered, plus
 *   the depth for an end tag (D5).
 * - Tokens go into a caller-supplied buffer. An element or attribute name
 *   that does not fit is LIMIT_EXCEEDED; an attribute value that does not fit
 *   is cut short and its event marked truncated (the rest is still read, and
 *   checked in strict mode); text is delivered in pieces (event.more).
 * - Nesting deeper than max_depth is LIMIT_EXCEEDED (D6). The caller supplies
 *   an array of max_depth hashes, one per open element, so that end tags are
 *   matched to start tags by a 32-bit FNV-1a hash of the name.
 * - Line ends are normalized as XML 1.0 2.11 requires: CR LF and a lone CR
 *   become LF, in text and attribute values alike. Attribute values have
 *   their whitespace normalized to spaces (XML 1.0 3.3.3, CDATA type).
 * - Entities: the five predefined ones and character references, and with
 *   BARTAP_XML_FLAG_XHTML_ENTITIES the 253 named entities of XHTML 1.x. No
 *   DTD is read: a DOCTYPE, comments and processing instructions are skipped,
 *   and CDATA is text.
 * - UTF-8 only; text and attribute values are validated as they are read. A
 *   UTF-16 byte order mark is UNSUPPORTED.
 * - Namespaces are not resolved: names are raw qualified names.
 * - Each event carries the byte offset in the document where its token
 *   starts. A TEXT event whose bytes do not map one to one onto the document
 *   (an entity decoded, an LF dropped after a CR, a byte replaced) is marked
 *   decoded; any other maps byte for byte from its position.
 *
 * Leniency. By default malformed markup is recovered from, locally, every
 * byte consumed once and none read again, and a warning bit is set:
 * - a '&' that does not start a reference ending in ';' within 12 bytes is
 *   literal text, and so is a reference to an entity not known (ENTITY);
 * - a character reference to 0, a surrogate or past U+10FFFF is U+FFFD
 *   (ENTITY);
 * - a '<' that starts no markup is literal text; an unknown "<!" construct is
 *   skipped to the next '>'; an attribute without a value has an empty one;
 *   an unquoted value runs to whitespace, '>' or "/>"; a '<' in a value is
 *   literal; stray bytes inside a tag are skipped; anything outside the root
 *   element is ignored (MARKUP);
 * - an end tag closes up to the nearest open element of its name, each
 *   element it closes on the way getting an END with no name, and one naming
 *   no open element is ignored (END_TAG);
 * - an invalid UTF-8 sequence in text or a value becomes U+FFFD (ENCODING).
 * With BARTAP_XML_FLAG_STRICT each of these is CORRUPT instead, and so is a
 * DOCTYPE keyword not in upper case. Limits, and input that ends inside the
 * root element, are errors either way. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "snapshot.h"

/* Values are fixed and the list is append-only (D18). */
typedef enum {
    BARTAP_XML_RESULT_OK = 0,
    BARTAP_XML_RESULT_INVALID_ARGUMENT = 1,
    BARTAP_XML_RESULT_CORRUPT = 2,              //not well-formed, and not recovered from; final
    BARTAP_XML_RESULT_LIMIT_EXCEEDED = 3,       //a caller-set limit (D18); final
    BARTAP_XML_RESULT_TRUNCATED = 4,            //the input ended inside the document; final
    BARTAP_XML_RESULT_UNSUPPORTED = 5,          //not UTF-8 (a UTF-16 byte order mark); final
} bartap_xml_result_t;

#define BARTAP_XML_FLAG_XHTML_ENTITIES (1U << 0)
#define BARTAP_XML_FLAG_STRICT         (1U << 1)

/* Warning bits, append-only: what leniency recovered from. */
#define BARTAP_XML_WARNING_ENTITY   (1UL << 0)
#define BARTAP_XML_WARNING_MARKUP   (1UL << 1)
#define BARTAP_XML_WARNING_END_TAG  (1UL << 2)
#define BARTAP_XML_WARNING_ENCODING (1UL << 3)

/* Smallest token buffer accepted. */
#define BARTAP_XML_BUFFER_MIN 32U

typedef enum {
    BARTAP_XML_EVENT_NONE = 0,                  //the input ran out first: call again with more
    BARTAP_XML_EVENT_START = 1,                 //an element starts: name
    BARTAP_XML_EVENT_ATTRIBUTE = 2,             //name and value
    BARTAP_XML_EVENT_START_END = 3,             //the start tag closed: name; empty for <a/>
    BARTAP_XML_EVENT_END = 4,                   //an element ends: name, or none when recovery closed it
    BARTAP_XML_EVENT_TEXT = 5,                  //text inside the root, a piece at a time
    BARTAP_XML_EVENT_DONE = 6,                  //the input ended after the root element closed
} bartap_xml_event_kind_t;

typedef struct {
    uint8_t kind;                               //a bartap_xml_event_kind_t
    const uint8_t *name;                        //in the token buffer, valid until the next call
    size_t name_length;
    const uint8_t *value;                       //attribute value or text, also in the token buffer
    size_t value_length;
    bool empty;                                 //START_END: the element was <a/>
    bool more;                                  //TEXT: more of the same text follows
    bool decoded;                               //TEXT: does not map byte for byte onto the document
    bool cdata;                                 //TEXT: from a CDATA section
    bool truncated;                             //ATTRIBUTE: the value was cut short
    uint32_t depth;                             //of the element (1 for the root); TEXT: of its parent
    uint32_t position;                          //byte offset in the document where the token starts
} bartap_xml_event_t;

/* Tokenizer state. Private: a struct so that it can be embedded. */
typedef struct {
    uint8_t *buffer;
    size_t buffer_size;
    uint32_t *stack;                            //name hashes of the open elements, [0] the root's
    uint32_t max_depth;
    uint32_t flags;
    uint32_t warnings;
    size_t length;                              //bytes in the buffer: name, then attribute name and value
    size_t name_length;                         //of the element whose start tag is being read
    size_t value_start;
    size_t end_name_length;                     //of the end tag whose END events are pending
    uint32_t depth;
    uint32_t pending_ends;                      //END events still to report before more input
    uint32_t name_hash;                         //of the start or end tag name being read
    uint32_t position;                          //bytes consumed so far, in the whole document
    uint32_t token_position;
    uint32_t entity_position;                   //of the '&' of the reference being read
    uint32_t utf8_position;                     //of the lead byte of the sequence being read
    uint8_t stage;                              //an enum in xml.c
    uint8_t quote;
    uint8_t entity[12];                         //a reference being read, without '&' and ';'
    uint8_t entity_length;
    uint8_t entity_return;                      //the stage to go back to after it
    uint8_t match;                              //progress through "-->", "]]>", "?>" or a keyword
    uint8_t brackets;                           //'[' nesting inside a DOCTYPE
    uint8_t bom;                                //byte order mark bytes matched at the start
    uint8_t utf8[4];                            //a UTF-8 sequence being read
    uint8_t utf8_have;
    uint8_t utf8_need;                          //continuation bytes still expected
    uint8_t utf8_low;                           //range of the next one
    uint8_t utf8_high;
    bool after_cr;                              //the last byte was a CR: an LF next is dropped
    bool decoded;
    bool truncated;
    bool root_seen;
    bool root_closed;
    bartap_xml_result_t status;                 //sticky once an error
} bartap_xml_t;

/* Start a document. buffer holds one token at a time (at least
 * BARTAP_XML_BUFFER_MIN bytes); stack holds max_depth hashes (max_depth at
 * least 1); flags are BARTAP_XML_FLAG_* bits. Both buffers are the caller's
 * and must outlive the tokenizer. */
bartap_xml_result_t bartap_xml_init(bartap_xml_t *xml, uint8_t *buffer, size_t buffer_size, uint32_t *stack,
    uint32_t max_depth, uint32_t flags);

/* Read the next event from in (in_size bytes; *in_used set to how many were
 * taken; offer the rest again). input_ends is true when in holds the last of
 * the document. OK with event->kind NONE when the input ran out before an
 * event completed. Errors are final. */
bartap_xml_result_t bartap_xml_next(bartap_xml_t *xml, const uint8_t *in, size_t in_size, size_t *in_used,
    bool input_ends, bartap_xml_event_t *event);

/* Snapshots, for text.h's save and restore. bartap_xml_save writes the state, with the token buffer's
 * first keep bytes (at least what the buffer holds; more where an event the caller still holds points
 * past that). With rewind_position not NULL, it writes the state as it would be had the tokenizer just
 * reached that offset inside text that maps byte for byte onto the document (rewind_cdata: inside a CDATA
 * section) with nothing buffered; the caller then feeds input from there. bartap_xml_restore reads a
 * snapshot back into a tokenizer initialized with the same buffer, depth and flags: with apply false it
 * only checks, and changes nothing. false if the snapshot is inconsistent; every value that could take a
 * later call out of bounds is checked. *position gets the restored document offset. */
void bartap_xml_save(const bartap_xml_t *xml, bartap_writer_t *w, size_t keep, const uint32_t *rewind_position,
    bool rewind_cdata);
bool bartap_xml_restore(bartap_xml_t *xml, bartap_reader_t *r, bool apply, uint32_t *position);

/* 32-bit FNV-1a, as used for end-tag matching (and by the book layer). */
#define BARTAP_XML_HASH_INIT 2166136261UL

static inline uint32_t bartap_xml_hash_byte(uint32_t hash, uint8_t byte)
{
    return (hash ^ byte) * 16777619UL;
}

/* An XHTML 1.x named entity (entities.c): its code point, if name is one. */
bool bartap_xml_entity_lookup(const uint8_t *name, size_t length, uint32_t *code_point);

#endif
