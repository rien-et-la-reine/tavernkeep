// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Words, spaces, line breaks, blocks and images from a chapter's XHTML. Whitespace collapses to one SPACE
 * except inside <pre> (pre_depth > 0), where spaces and newlines are kept. Seeking backwards re-reads
 * the chapter from its start. Positions carry an anchor of raw bytes, taken from the tokenizer's lookahead
 * and searched for when restoring. Over a stream, the end of the chapter is BARTAP_END only once the stream
 * has matched the entry's CRC-32; its CORRUPT is passed on. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

bartap_err_t bartap_text_open(bartap_text_t *text, const bartap_book_t *book, uint16_t spine_index,
    uint8_t *window)
{
    // TODO(rewrite)
    (void)text;
    (void)book;
    (void)spine_index;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_text_open_memory(bartap_text_t *text, uint16_t spine_index, const uint8_t *xhtml,
    size_t len)
{
    // TODO(rewrite)
    (void)text;
    (void)spine_index;
    (void)xhtml;
    (void)len;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_text_next(bartap_text_t *text, bartap_token_t *token)
{
    // TODO(rewrite)
    (void)text;
    (void)token;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_text_loc(bartap_text_t *text, bartap_loc_t *loc)
{
    // TODO(rewrite): the anchor from the tokenizer's lookahead, or straight from memory
    (void)text;
    (void)loc;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_text_seek(bartap_text_t *text, const bartap_loc_t *loc, bartap_seek_t *how)
{
    // TODO(rewrite): read up to byte_offset - 2 * BARTAP_ANCHOR_SIZE, keep the next 5 * BARTAP_ANCHOR_SIZE
    // raw bytes, compare at the offset, else search for the nearest match; then resume tokenizing there
    (void)text;
    (void)loc;
    (void)how;
    return BARTAP_ERR_UNSUPPORTED;
}
