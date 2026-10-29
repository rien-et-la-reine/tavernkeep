// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* A small formatting stack and one-token lookahead. XML chunks retain their raw source anchor;
 * a token index distinguishes the few text/synthetic tokens emitted from a single chunk. Seeking
 * replays this same iterator, so there is only one implementation of formatting and recovery. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

static bartap_err_t bartap_text_fill_stream(void *user, uint8_t *buf, size_t len, size_t *got)
{
    bartap_text_t *const text = user;
    const bartap_err_t err = bartap_stream_read(&text->stream, buf, len, got);
    return err == BARTAP_END ? BARTAP_OK : err;
}

static bartap_err_t bartap_text_fill_memory(void *user, uint8_t *buf, size_t len, size_t *got)
{
    bartap_text_t *const text = user;
    const size_t rem = text->memory_len - text->memory_pos;
    *got = len < rem ? len : rem;
    if (*got > 0U) {
        memcpy(buf, text->memory + text->memory_pos, *got);
        text->memory_pos += *got;
    }
    return BARTAP_OK;
}

static void bartap_text_reset_state(bartap_text_t *text)
{
    text->warnings |= text->xml.warnings;
    text->text_pos = 0U;
    text->text_len = 0U;
    text->style = 0U;
    text->depth = 0U;
    text->close_to = 0U;
    text->skip_depth = 0U;
    memset(text->ignored, 0, sizeof text->ignored);
    text->pre_depth = 0U;
    text->line_content = false;
    text->pending_space = false;
    text->previous_cr = false;
    text->event_ready = false;
    text->ended = false;
    text->event_offset = 0U;
    text->event_index = 0U;
    text->ready = false;
    text->context_depth = 0U;
    text->memory_pos = 0U;
    bartap_xml_init(&text->xml,
        text->from_memory ? bartap_text_fill_memory : bartap_text_fill_stream, text);
}

static bartap_err_t bartap_text_rewind(bartap_text_t *text)
{
    if (!text->from_memory) {
        const bartap_err_t err = bartap_stream_rewind(&text->stream);
        if (err != BARTAP_OK) {
            return err;
        }
    }
    bartap_text_reset_state(text);
    return BARTAP_OK;
}

bartap_err_t bartap_text_open(bartap_text_t *text, const bartap_book_t *book, uint16_t spine_index,
    uint8_t *window)
{
    if (text == NULL || book == NULL || spine_index >= book->spine_count || window == NULL) {
        return BARTAP_ERR_ARG;
    }
    memset(text, 0, sizeof *text);
    text->spine_index = spine_index;
    bartap_err_t err = bartap_stream_open(&text->stream, book, spine_index, window);
    if (err == BARTAP_OK) {
        err = bartap_entry_base_dir(book->source, book->spine[spine_index].entry_offset, text->base_dir);
    }
    if (err != BARTAP_OK) {
        return err;
    }
    bartap_text_reset_state(text);
    return BARTAP_OK;
}

bartap_err_t bartap_text_open_memory(bartap_text_t *text, const bartap_book_t *book,
    uint16_t spine_index, const uint8_t *xhtml, size_t len)
{
    if (text == NULL || book == NULL || spine_index >= book->spine_count
            || (xhtml == NULL && len > 0U) || len > UINT32_MAX) {
        return BARTAP_ERR_ARG;
    }
    memset(text, 0, sizeof *text);
    text->spine_index = spine_index;
    text->from_memory = true;
    text->memory = xhtml;
    text->memory_len = len;
    /* The only archive read in memory mode: the chapter's name, for resolving image paths. */
    const bartap_err_t err = bartap_entry_base_dir(book->source, book->spine[spine_index].entry_offset,
        text->base_dir);
    if (err != BARTAP_OK) {
        return err;
    }
    bartap_text_reset_state(text);
    return BARTAP_OK;
}

/* Table indices identify tags exactly, even when several map to the same block/style. Zero is
 * reserved for an implicit paragraph. Inline frames restore the style that existed before them. */
static const struct {
    char name[11];
    uint8_t block;              // bartap_block_t + 1, or zero for inline
    uint8_t style;
    uint8_t level;
} bartap_tags[] = {
    { "", BARTAP_BLOCK_PARAGRAPH + 1, 0, 0 },
    { "p", BARTAP_BLOCK_PARAGRAPH + 1, 0, 0 },
    { "div", BARTAP_BLOCK_PARAGRAPH + 1, 0, 0 },
    { "dt", BARTAP_BLOCK_PARAGRAPH + 1, 0, 0 },
    { "dd", BARTAP_BLOCK_PARAGRAPH + 1, 0, 0 },
    { "tr", BARTAP_BLOCK_PARAGRAPH + 1, 0, 0 },
    { "figcaption", BARTAP_BLOCK_PARAGRAPH + 1, 0, 0 },
    { "li", BARTAP_BLOCK_LIST_ITEM + 1, 0, 0 },
    { "blockquote", BARTAP_BLOCK_QUOTE + 1, 0, 0 },
    { "pre", BARTAP_BLOCK_PRE + 1, BARTAP_STYLE_MONOSPACE, 0 },
    { "hr", BARTAP_BLOCK_RULE + 1, 0, 0 },
    { "h1", BARTAP_BLOCK_HEADING + 1, 0, 1 },
    { "h2", BARTAP_BLOCK_HEADING + 1, 0, 2 },
    { "h3", BARTAP_BLOCK_HEADING + 1, 0, 3 },
    { "h4", BARTAP_BLOCK_HEADING + 1, 0, 4 },
    { "h5", BARTAP_BLOCK_HEADING + 1, 0, 5 },
    { "h6", BARTAP_BLOCK_HEADING + 1, 0, 6 },
    { "i", 0, BARTAP_STYLE_ITALIC, 0 },
    { "em", 0, BARTAP_STYLE_ITALIC, 0 },
    { "cite", 0, BARTAP_STYLE_ITALIC, 0 },
    { "dfn", 0, BARTAP_STYLE_ITALIC, 0 },
    { "var", 0, BARTAP_STYLE_ITALIC, 0 },
    { "b", 0, BARTAP_STYLE_BOLD, 0 },
    { "strong", 0, BARTAP_STYLE_BOLD, 0 },
    { "code", 0, BARTAP_STYLE_MONOSPACE, 0 },
    { "tt", 0, BARTAP_STYLE_MONOSPACE, 0 },
    { "kbd", 0, BARTAP_STYLE_MONOSPACE, 0 },
    { "samp", 0, BARTAP_STYLE_MONOSPACE, 0 },
    { "sup", 0, BARTAP_STYLE_SUPER, 0 },
    { "sub", 0, BARTAP_STYLE_SUB, 0 },
};

static uint8_t bartap_tag_number(const char *name)
{
    for (uint8_t i = 1U; i < sizeof bartap_tags / sizeof bartap_tags[0]; i++) {
        if (strcmp(name, bartap_tags[i].name) == 0) {
            return i;
        }
    }
    return 0U;
}

_Static_assert(sizeof bartap_tags / sizeof bartap_tags[0] <= BARTAP_TEXT_TAGS,
    "bartap_text_t.ignored needs a count per recognized tag");

/* Structural containers Bartap does not emit as blocks. Each start or end separates words, and ends an
 * implicit paragraph, so table cells and sibling sections do not run together. */
static bool bartap_tag_is_boundary(const char *name)
{
    static const char names[][8] = {
        "td", "th", "caption", "table", "section", "article", "aside", "header", "footer", "nav",
        "figure", "ul", "ol", "dl", "address", "body"
    };
    for (size_t i = 0U; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool bartap_tag_is_skip(const char *name)
{
    return strcmp(name, "head") == 0 || strcmp(name, "style") == 0
        || strcmp(name, "script") == 0 || strcmp(name, "title") == 0 || strcmp(name, "desc") == 0;
}

static bool bartap_text_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void bartap_text_save_context(bartap_text_t *text)
{
    text->context_depth = 0U;
    for (uint8_t i = 0U; i < text->depth; i++) {
        const bartap_text_frame_t *const frame = &text->frames[i];
        if (frame->block != 0U) {
            bartap_block_context_t *const b = &text->context[text->context_depth++];
            b->block = (bartap_block_t)(frame->block - 1U);
            b->level = frame->level;
        }
    }
}

static bartap_err_t bartap_text_push(bartap_text_t *text, uint8_t tag, bartap_token_t *token)
{
    /* Not reached: explicit elements stop one frame short of the limit, and the implicit paragraph is
     * pushed only when no block, and so no frame, is open. Kept so a broken invariant cannot write past
     * frames[]. */
    if (text->depth == BARTAP_TEXT_DEPTH_MAX) {
        return BARTAP_ERR_LIMIT;
    }
    bartap_text_frame_t *const frame = &text->frames[text->depth++];
    frame->tag = tag;
    frame->block = bartap_tags[tag].block;
    frame->level = bartap_tags[tag].level;
    frame->previous_style = text->style;
    text->style |= bartap_tags[tag].style;
    text->close_to = text->depth;
    if (frame->block != 0U) {
        token->type = BARTAP_TOK_BLOCK_START;
        token->block = (bartap_block_t)(frame->block - 1U);
        token->level = frame->level;
        text->pending_space = false;
        text->line_content = false;
        text->previous_cr = false;
        if (token->block == BARTAP_BLOCK_PRE) {
            text->pre_depth++;
        } else if (token->block == BARTAP_BLOCK_RULE) {
            text->close_to--;   // hr always owes an end, even without an XML '/>'
        }
    }
    return BARTAP_OK;
}

/* Schedule the implicit paragraph, if one is open, to be popped; true if it was. */
static bool bartap_text_close_implicit(bartap_text_t *text)
{
    for (uint8_t i = 0U; i < text->depth; i++) {
        if (text->frames[i].tag == 0U) {
            text->close_to = i;
            memset(text->ignored, 0, sizeof text->ignored);
            return true;
        }
    }
    return false;
}

/* Returns a token, END, or (only during fragment search) found=true with the matching start tag
 * still pending. context is captured before each emitted token changes the stack. */
static bartap_err_t bartap_text_generate(bartap_text_t *text, bartap_token_t *token,
    const char *fragment, bool *found)
{
    memset(token, 0, sizeof *token);
    for (;;) {
        bartap_text_save_context(text);
        if (text->depth > text->close_to) {
            const bartap_text_frame_t frame = text->frames[--text->depth];
            text->style = frame.previous_style;
            if (frame.block != 0U) {
                token->type = BARTAP_TOK_BLOCK_END;
                token->block = (bartap_block_t)(frame.block - 1U);
                token->level = frame.level;
                if (token->block == BARTAP_BLOCK_PRE) {
                    text->pre_depth--;
                }
                text->pending_space = false;
                text->line_content = false;
                text->previous_cr = false;
                return BARTAP_OK;
            }
            continue;
        }
        if (text->text_pos < text->text_len) {
            const char *const buf = text->xml.text;
            size_t pos = text->text_pos;
            if (text->pre_depth != 0U && bartap_text_space(buf[pos])) {
                const char c = buf[pos++];
                text->text_pos = (uint16_t)pos;
                if (c == '\n' && text->previous_cr) {
                    text->previous_cr = false;
                    continue;
                }
                text->previous_cr = c == '\r';
                token->type = c == '\n' || c == '\r' ? BARTAP_TOK_LINE_BREAK : BARTAP_TOK_SPACE;
                return BARTAP_OK;
            }
            text->previous_cr = false;
            if (text->pre_depth == 0U) {
                while (pos < text->text_len && bartap_text_space(buf[pos])) {
                    text->pending_space = text->line_content;
                    pos++;
                }
                text->text_pos = (uint16_t)pos;
                if (pos == text->text_len) {
                    continue;
                }
            }
            if (text->context_depth == 0U) {
                return bartap_text_push(text, 0U, token);
            }
            if (text->pending_space) {
                text->pending_space = false;
                token->type = BARTAP_TOK_SPACE;
                return BARTAP_OK;
            }
            size_t end = pos;
            while (end < text->text_len && !bartap_text_space(buf[end])) {
                end++;
            }
            token->type = BARTAP_TOK_WORD;
            token->style = text->style;
            token->len = (uint16_t)(end - pos);
            memcpy(token->text, buf + pos, token->len);
            token->text[token->len] = '\0';
            text->text_pos = (uint16_t)end;
            text->line_content = true;
            return BARTAP_OK;
        }
        if (text->ended) {
            return BARTAP_END;
        }
        if (!text->event_ready) {
            bartap_xml_token_t xtok;
            const bartap_err_t err = bartap_xml_next(&text->xml, &xtok);
            if (err != BARTAP_OK) {
                return err;
            }
            if (xtok.offset != text->event_offset) {
                text->event_index = 0U;
            }
            text->event_offset = xtok.offset;
            text->event_type = (uint8_t)xtok.type;
            text->event_ready = true;
            if (xtok.type == BARTAP_XML_TEXT && text->skip_depth == 0U) {
                text->text_pos = 0U;
                text->text_len = (uint16_t)xtok.text_len;
                text->event_ready = false;
                continue;
            }
            if (fragment != NULL && xtok.type == BARTAP_XML_START_TAG && text->skip_depth == 0U) {
                char id[BARTAP_FRAGMENT_MAX + 1];
                size_t len;
                if (bartap_xml_attr(&text->xml, "id", id, sizeof id, &len) == BARTAP_OK
                        && strcmp(id, fragment) == 0) {
                    *found = true;
                    return BARTAP_OK;
                }
            }
        }
        const bartap_xml_type_t type = (bartap_xml_type_t)text->event_type;
        text->event_ready = false;
        if (type == BARTAP_XML_END) {
            if (text->skip_depth != 0U) {
                text->warnings |= BARTAP_WARN_MARKUP;
            }
            for (uint8_t i = 0U; i < text->depth; i++) {
                if (text->frames[i].tag != 0U) {
                    text->warnings |= BARTAP_WARN_MARKUP;
                }
            }
            text->ended = true;
            text->close_to = 0U;
            continue;
        }
        if (bartap_tag_is_skip(text->xml.name)) {
            if (type == BARTAP_XML_START_TAG) {
                /* Saturates: past 255 nested skipped elements, an early close shows hidden text. */
                if (text->skip_depth == UINT8_MAX) {
                    text->warnings |= BARTAP_WARN_MARKUP;
                } else {
                    text->skip_depth++;
                }
            } else if (type == BARTAP_XML_END_TAG && text->skip_depth != 0U) {
                text->skip_depth--;
            }
            continue;
        }
        if (text->skip_depth != 0U || type == BARTAP_XML_TEXT) {
            continue;
        }
        if (bartap_tag_is_boundary(text->xml.name)) {
            if (bartap_text_close_implicit(text)) {
                continue;
            }
            text->pending_space = text->pending_space || text->line_content;
            continue;
        }
        const uint8_t tag = bartap_tag_number(text->xml.name);
        if (type == BARTAP_XML_START_TAG) {
            if (strcmp(text->xml.name, "br") == 0) {
                token->type = BARTAP_TOK_LINE_BREAK;
                text->pending_space = false;
                text->line_content = false;
                text->previous_cr = false;
                return BARTAP_OK;
            }
            if (strcmp(text->xml.name, "img") == 0 || strcmp(text->xml.name, "image") == 0) {
                char src[BARTAP_PATH_MAX + 1];
                size_t len;
                bartap_err_t err = bartap_xml_attr(&text->xml, "src", src, sizeof src, &len);
                if (err == BARTAP_ERR_NOT_FOUND) {
                    err = bartap_xml_attr(&text->xml, "href", src, sizeof src, &len);
                }
                if (err == BARTAP_OK && len > 0U
                        && bartap_resolve_href(text->base_dir, src, token->text, NULL, 0U) == BARTAP_OK) {
                    if (text->context_depth == 0U) {
                        /* An image outside any block opens an implicit paragraph, as text does. */
                        token->text[0] = '\0';
                        text->event_ready = true;
                        return bartap_text_push(text, 0U, token);
                    }
                    if (text->pending_space) {
                        text->pending_space = false;
                        text->event_ready = true;
                        token->text[0] = '\0';
                        token->type = BARTAP_TOK_SPACE;
                        return BARTAP_OK;
                    }
                    token->type = BARTAP_TOK_IMAGE;
                    token->len = (uint16_t)strlen(token->text);
                    text->line_content = true;
                    return BARTAP_OK;
                }
                text->warnings |= BARTAP_WARN_MARKUP;
                continue;
            }
            if (tag != 0U) {
                if (bartap_tags[tag].block == 0U && text->context_depth == 0U) {
                    text->event_ready = true;
                    return bartap_text_push(text, 0U, token);
                }
                /* Close an implicit paragraph before opening an explicit block. */
                if (bartap_tags[tag].block != 0U && bartap_text_close_implicit(text)) {
                    text->event_ready = true;
                    continue;
                }
                /* Past the depth limit the element is ignored: its content stays in the enclosing block
                 * and style. The last frame is kept for an implicit paragraph, which therefore always fits. */
                if (text->depth >= BARTAP_TEXT_DEPTH_MAX - 1U) {
                    text->warnings |= BARTAP_WARN_MARKUP;
                    if (bartap_tags[tag].block != BARTAP_BLOCK_RULE + 1U && text->ignored[tag] != UINT16_MAX) {
                        text->ignored[tag]++;
                    }
                    continue;
                }
                const bartap_err_t err = bartap_text_push(text, tag, token);
                if (err != BARTAP_OK || bartap_tags[tag].block != 0U) {
                    return err;
                }
            }
        } else if (type == BARTAP_XML_END_TAG && tag != 0U
                && bartap_tags[tag].block != BARTAP_BLOCK_RULE + 1U) {
            if (text->ignored[tag] != 0U) {
                text->ignored[tag]--;
                continue;
            }
            uint8_t match = text->depth;
            while (match > 0U && text->frames[match - 1U].tag != tag) {
                match--;
            }
            if (match == 0U) {
                text->warnings |= BARTAP_WARN_MARKUP;
            } else {
                if (match != text->depth) {
                    text->warnings |= BARTAP_WARN_MARKUP;
                }
                text->close_to = (uint8_t)(match - 1U);
                /* Ignored elements were nested inside every open frame: closing one closes them too. */
                memset(text->ignored, 0, sizeof text->ignored);
            }
        }
    }
}

static bartap_err_t bartap_text_prepare(bartap_text_t *text)
{
    if (text->ready) {
        return BARTAP_OK;
    }
    const bartap_err_t err = bartap_text_generate(text, &text->next, NULL, NULL);
    if (err != BARTAP_OK && err != BARTAP_END) {
        return err;
    }
    memset(&text->next_loc, 0, sizeof text->next_loc);
    text->next_loc.spine_index = text->spine_index;
    text->next_loc.byte_offset = text->event_offset;
    text->next_loc.token_index = text->event_index;
    text->next_loc.anchor_len = text->xml.anchor_len;
    memcpy(text->next_loc.anchor, text->xml.anchor, text->xml.anchor_len);
    text->next.offset = text->event_offset;
    if (err == BARTAP_OK) {
        text->event_index++;
        text->ready = true;
    }
    return err;
}

bartap_err_t bartap_text_next(bartap_text_t *text, bartap_token_t *token)
{
    if (text == NULL || token == NULL) {
        return BARTAP_ERR_ARG;
    }
    const bartap_err_t err = bartap_text_prepare(text);
    if (err == BARTAP_OK) {
        *token = text->next;
        text->ready = false;
    }
    return err;
}

bartap_err_t bartap_text_loc(bartap_text_t *text, bartap_loc_t *loc)
{
    if (text == NULL || loc == NULL) {
        return BARTAP_ERR_ARG;
    }
    const bartap_err_t err = bartap_text_prepare(text);
    if (err != BARTAP_OK && err != BARTAP_END) {
        return err;
    }
    *loc = text->next_loc;
    return BARTAP_OK;
}

bartap_err_t bartap_text_context(bartap_text_t *text, bartap_block_context_t *blocks,
    size_t capacity, size_t *count)
{
    if (text == NULL || count == NULL || (blocks == NULL && capacity != 0U)) {
        return BARTAP_ERR_ARG;
    }
    const bartap_err_t err = bartap_text_prepare(text);
    if (err != BARTAP_OK && err != BARTAP_END) {
        return err;
    }
    *count = text->context_depth;
    if (capacity < *count) {
        return BARTAP_ERR_LIMIT;
    }
    if (*count != 0U) {
        memcpy(blocks, text->context, *count * sizeof *blocks);
    }
    return BARTAP_OK;
}

uint32_t bartap_text_warnings(const bartap_text_t *text)
{
    return text != NULL ? text->warnings | text->xml.warnings : 0U;
}

/* Only the small anchor neighborhood is buffered. No inflater or parser snapshots are needed. */
static bartap_err_t bartap_text_match_anchor(bartap_text_t *text, const bartap_loc_t *loc,
    uint32_t *target, bartap_seek_t *how)
{
    const uint32_t radius = 2U * BARTAP_ANCHOR_SIZE;
    const uint32_t size = text->from_memory ? (uint32_t)text->memory_len : text->stream.entry.size;
    const uint32_t start = loc->byte_offset > radius ? loc->byte_offset - radius : 0U;
    *target = loc->byte_offset;
    *how = BARTAP_SEEK_OFFSET;
    if (loc->anchor_len == 0U || start >= size) {
        return BARTAP_OK;
    }
    uint8_t buffer[5U * BARTAP_ANCHOR_SIZE];
    const uint8_t *bytes = buffer;
    const size_t length = size - start < sizeof buffer ? size - start : sizeof buffer;
    if (text->from_memory) {
        bytes = text->memory + start;
    } else {
        bartap_err_t err = bartap_stream_rewind(&text->stream);
        if (err != BARTAP_OK) {
            return err;
        }
        uint32_t skipped = 0U;
        while (skipped < start) {
            const size_t n = start - skipped < sizeof buffer ? start - skipped : sizeof buffer;
            size_t got;
            err = bartap_stream_read(&text->stream, buffer, n, &got);
            if (err != BARTAP_OK) {
                return err == BARTAP_END ? BARTAP_ERR_CORRUPT : err;
            }
            skipped += (uint32_t)got;
        }
        size_t used = 0U;
        while (used < length) {
            size_t got;
            err = bartap_stream_read(&text->stream, buffer + used, length - used, &got);
            if (err != BARTAP_OK) {
                return err == BARTAP_END ? BARTAP_ERR_CORRUPT : err;
            }
            used += got;
        }
    }
    uint32_t best = UINT32_MAX;
    for (size_t i = 0U; i + loc->anchor_len <= length; i++) {
        const uint32_t pos = start + (uint32_t)i;
        const uint32_t distance = pos > loc->byte_offset ? pos - loc->byte_offset : loc->byte_offset - pos;
        if (distance <= radius && distance < best && memcmp(bytes + i, loc->anchor, loc->anchor_len) == 0) {
            best = distance;
            *target = pos;
            *how = distance == 0U ? BARTAP_SEEK_EXACT : BARTAP_SEEK_MOVED;
        }
    }
    return BARTAP_OK;
}

bartap_err_t bartap_text_seek(bartap_text_t *text, const bartap_loc_t *loc, bartap_seek_t *how)
{
    if (text == NULL || loc == NULL || how == NULL || loc->spine_index != text->spine_index
            || loc->anchor_len > BARTAP_ANCHOR_SIZE) {
        return BARTAP_ERR_ARG;
    }
    uint32_t target;
    bartap_err_t err = bartap_text_match_anchor(text, loc, &target, how);
    if (err != BARTAP_OK) {
        return err;
    }
    err = bartap_text_rewind(text);
    if (err != BARTAP_OK) {
        return err;
    }
    while ((err = bartap_text_prepare(text)) == BARTAP_OK) {
        if (text->next_loc.byte_offset > target || (text->next_loc.byte_offset == target
                && text->next_loc.token_index >= loc->token_index)) {
            return BARTAP_OK;
        }
        text->ready = false;
    }
    return err == BARTAP_END ? BARTAP_OK : err;
}

bartap_err_t bartap_text_seek_fragment(bartap_text_t *text, const char *fragment)
{
    if (text == NULL || fragment == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (strlen(fragment) > BARTAP_FRAGMENT_MAX) {
        return BARTAP_ERR_LIMIT;
    }
    bartap_err_t err = bartap_text_rewind(text);
    if (err != BARTAP_OK || fragment[0] == '\0') {
        return err;
    }
    bool found = false;
    while ((err = bartap_text_generate(text, &text->next, fragment, &found)) == BARTAP_OK) {
        if (found) {
            return BARTAP_OK;
        }
        text->event_index++;
    }
    return err == BARTAP_END ? BARTAP_ERR_NOT_FOUND : err;
}
