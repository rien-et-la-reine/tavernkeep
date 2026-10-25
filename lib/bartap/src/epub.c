// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* EPUB book layer (bartap/epub.h) over the OCF layer and the XML tokenizer: opening, the spine, streams,
 * finding entries, hrefs and the table of contents. CFIs are in cfi.c. Every call and step makes at most
 * one byte-source read (D14); the work between reads is bounded by the buffers involved and the spine. */

#include "bartap/epub.h"

#include <string.h>

#include "names.h"
#include "ocf.h"
#include "xml.h"

#define TOKEN_SIZE 512U             //the tokenizer's buffer: names, and attribute values up to this
#define CHUNK_SIZE 512U             //decompressed bytes fed to the tokenizer at a time
#define DEPTH_MAX 64U               //XML nesting accepted

#define SPINE_HREF 0x01U            //the item's resolved href is known
#define SPINE_FOUND 0x02U           //and so is its entry

enum {
    TOC_NONE,
    TOC_NAV,
    TOC_NCX,
};

/* ------------------------------------------------------------ results */

static bartap_epub_result_t from_ocf(bartap_ocf_result_t r)
{
    switch (r) {
    case BARTAP_OCF_RESULT_OK: return BARTAP_EPUB_RESULT_OK;
    case BARTAP_OCF_RESULT_INVALID_ARGUMENT: return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    case BARTAP_OCF_RESULT_NOT_A_ZIP: return BARTAP_EPUB_RESULT_NOT_A_ZIP;
    case BARTAP_OCF_RESULT_UNSUPPORTED: return BARTAP_EPUB_RESULT_UNSUPPORTED;
    case BARTAP_OCF_RESULT_NOT_FOUND: return BARTAP_EPUB_RESULT_NOT_FOUND;
    case BARTAP_OCF_RESULT_IO_ERROR: return BARTAP_EPUB_RESULT_IO_ERROR;
    case BARTAP_OCF_RESULT_LIMIT_EXCEEDED: return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
    case BARTAP_OCF_RESULT_NOT_AN_EPUB: return BARTAP_EPUB_RESULT_NOT_AN_EPUB;
    default: return BARTAP_EPUB_RESULT_CORRUPT;
    }
}

static bartap_epub_result_t from_xml(bartap_xml_result_t r)
{
    switch (r) {
    case BARTAP_XML_RESULT_OK: return BARTAP_EPUB_RESULT_OK;
    case BARTAP_XML_RESULT_INVALID_ARGUMENT: return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    case BARTAP_XML_RESULT_LIMIT_EXCEEDED: return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
    case BARTAP_XML_RESULT_UNSUPPORTED: return BARTAP_EPUB_RESULT_UNSUPPORTED;
    default: return BARTAP_EPUB_RESULT_CORRUPT;
    }
}

static int hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

//the directory part of a path: up to and including its last '/'
static size_t directory_length(const uint8_t *path, size_t length)
{
    while (length > 0U && path[length - 1U] != '/') {
        length--;
    }
    return length;
}

/* ------------------------------------------------------------ hrefs */

/* Resolve the path part of href (up to its '#' or '?') against base, a file's archive path, into out
 * (NUL-terminated): percent-decoded, "." and ".." resolved, written in place. An empty path is base itself. */
static bartap_epub_result_t resolve_path(const uint8_t *base, size_t base_length, const uint8_t *href,
    size_t href_length, uint8_t *out, size_t out_size, size_t *out_length)
{
    size_t end = 0;
    while (end < href_length && href[end] != '#' && href[end] != '?') {
        end++;
    }
    for (size_t i = 0; i < end && href[i] != '/'; i++) {
        if (href[i] == ':') {
            return BARTAP_EPUB_RESULT_UNSUPPORTED;      //a scheme: not in the archive
        }
    }
    if (end > 0U && href[0] == '/') {
        return BARTAP_EPUB_RESULT_UNSUPPORTED;          //absolute: not relative to anything in the archive
    }
    const size_t keep = end == 0U ? base_length : directory_length(base, base_length);
    if (out_size == 0U || keep >= out_size) {
        return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
    }
    if (keep > 0U) {
        memmove(out, base, keep);   //base may be NULL when empty
    }
    size_t n = keep;
    size_t i = 0;
    while (i < end) {
        //one segment, decoded straight into out
        const size_t start = n;
        bool slash = false;
        while (i < end) {
            uint8_t c = href[i++];
            if (c == '/') {
                slash = true;
                break;
            }
            if (c == '%') {
                const int high = i < end ? hex_value(href[i]) : -1;
                const int low = i + 1U < end ? hex_value(href[i + 1U]) : -1;
                if (high < 0 || low < 0) {
                    return BARTAP_EPUB_RESULT_CORRUPT;
                }
                c = (uint8_t)(high * 16 + low);
                i += 2U;
            }
            if (n + 1U >= out_size) {
                return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
            }
            out[n++] = c;
        }
        const size_t length = n - start;
        if (length == 0U || (length == 1U && out[start] == '.')) {
            n = start;
            continue;
        }
        if (length == 2U && out[start] == '.' && out[start + 1U] == '.') {
            n = start;
            if (n == 0U) {
                return BARTAP_EPUB_RESULT_UNSUPPORTED;  //above the archive's root
            }
            n--;                                        //drop the last directory: back to the '/' before it
            while (n > 0U && out[n - 1U] != '/') {
                n--;
            }
            continue;
        }
        if (slash) {
            if (n + 1U >= out_size) {
                return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
            }
            out[n++] = '/';
        }
    }
    out[n] = 0;
    *out_length = n;
    return BARTAP_EPUB_RESULT_OK;
}

//the fragment after href's '#', into target: percent-decoded unless it is a CFI, cut at BARTAP_EPUB_FRAGMENT_MAX
static void take_fragment(const uint8_t *href, size_t href_length, bartap_epub_target_t *target)
{
    size_t i = 0;
    while (i < href_length && href[i] != '#') {
        i++;
    }
    target->fragment_length = 0;
    target->fragment_truncated = false;
    if (i == href_length) {
        target->fragment[0] = 0;
        return;
    }
    i++;
    static const char cfi_start[] = "epubcfi(";
    const bool cfi = href_length - i >= sizeof cfi_start - 1U && memcmp(href + i, cfi_start, sizeof cfi_start - 1U) == 0;
    size_t n = 0;
    while (i < href_length) {
        uint8_t c = href[i++];
        if (!cfi && c == '%' && i + 1U < href_length) {
            const int high = hex_value(href[i]);
            const int low = hex_value(href[i + 1U]);
            if (high >= 0 && low >= 0) {
                c = (uint8_t)(high * 16 + low);
                i += 2U;
            }
        }
        if (n == BARTAP_EPUB_FRAGMENT_MAX) {
            target->fragment_truncated = true;
            break;
        }
        target->fragment[n++] = c;
    }
    target->fragment[n] = 0;
    target->fragment_length = (uint8_t)n;
}

/* ------------------------------------------------------------ content streams */

enum {
    CONTENT_HEADER,                 //reading the directory entry
    CONTENT_NAME,                   //its name, when wanted
    CONTENT_OPEN,                   //opening it: the local header
    CONTENT_DATA,
};

typedef struct {
    const bartap_ocf_archive_t *archive;
    bartap_ocf_cursor_t cursor;
    bartap_ocf_entry_t entry;
    bartap_ocf_stream_t stream;
    bartap_offset_t max_size;
    uint8_t *window;
    size_t window_size;
    uint8_t *input;
    size_t input_size;
    uint8_t *name;                  //where the entry's name goes, if wanted
    size_t name_size;
    size_t name_length;
    uint8_t stage;
    bartap_epub_result_t status;
} content_t;

static void content_start(content_t *c, const bartap_ocf_archive_t *archive, bartap_offset_t max_size,
    uint8_t *window, size_t window_size, uint8_t *input, size_t input_size)
{
    memset(c, 0, sizeof *c);
    c->archive = archive;
    c->max_size = max_size;
    c->window = window;
    c->window_size = window_size;
    c->input = input;
    c->input_size = input_size;
    c->status = BARTAP_EPUB_RESULT_OK;
}

//from a remembered location: its directory entry is read afresh, ZIP64 field and all
static void content_start_at(content_t *c, const bartap_ocf_archive_t *archive, const bartap_epub_location_t *at,
    bartap_offset_t max_size, uint8_t *window, size_t window_size, uint8_t *input, size_t input_size)
{
    content_start(c, archive, max_size, window, window_size, input, input_size);
    bartap_ocf_cursor_at(archive, &c->cursor, at->entry_offset, at->entry_index);
    c->stage = CONTENT_HEADER;
}

//from an entry already read
static void content_start_entry(content_t *c, const bartap_ocf_archive_t *archive, const bartap_ocf_entry_t *entry,
    bartap_offset_t max_size, uint8_t *window, size_t window_size, uint8_t *input, size_t input_size)
{
    content_start(c, archive, max_size, window, window_size, input, input_size);
    c->entry = *entry;
    c->stage = CONTENT_OPEN;
}

static bartap_epub_result_t content_read(content_t *c, uint8_t *buffer, size_t size, size_t *got, bool *end)
{
    *got = 0;
    *end = false;
    if (c->status != BARTAP_EPUB_RESULT_OK) {
        return c->status;
    }
    bartap_ocf_result_t r = BARTAP_OCF_RESULT_OK;
    switch (c->stage) {
    case CONTENT_HEADER: {
        bool ready;
        bool last;
        r = bartap_ocf_next(c->archive, &c->cursor, &c->entry, &ready, &last);
        if (r == BARTAP_OCF_RESULT_OK && last) {
            r = BARTAP_OCF_RESULT_CORRUPT;      //placed past the last entry
        } else if (r == BARTAP_OCF_RESULT_OK && ready) {
            c->stage = c->name != NULL ? CONTENT_NAME : CONTENT_OPEN;
        }
        break;
    }
    case CONTENT_NAME:
        r = bartap_ocf_entry_name(c->archive, &c->entry, c->name, c->name_size);
        if (r == BARTAP_OCF_RESULT_OK) {
            c->name_length = c->entry.name_length;
            c->stage = CONTENT_OPEN;
        }
        break;
    case CONTENT_OPEN:
        r = bartap_ocf_entry_open(&c->stream, c->archive, &c->entry, c->max_size, c->window, c->window_size,
            c->input, c->input_size);
        if (r == BARTAP_OCF_RESULT_OK) {
            c->stage = CONTENT_DATA;
        }
        break;
    default:
        r = bartap_ocf_entry_read(&c->stream, buffer, size, got, end);
        break;
    }
    c->status = from_ocf(r);
    return c->status;
}

/* ------------------------------------------------------------ documents: content through the tokenizer */

typedef struct {
    content_t content;
    bartap_xml_t xml;
    uint32_t stack[DEPTH_MAX];
    uint8_t token[TOKEN_SIZE];
    uint8_t chunk[CHUNK_SIZE];
    size_t chunk_start;
    size_t chunk_end;
    bool ended;                     //the content has been read to its end, CRC-32 checked
} document_t;

static void document_xml(document_t *d, uint32_t flags)
{
    (void)bartap_xml_init(&d->xml, d->token, sizeof d->token, d->stack, DEPTH_MAX, flags);
    d->chunk_start = 0;
    d->chunk_end = 0;
    d->ended = false;
}

//one step: a read of more content, or one tokenizer call on what has been read (event NONE if neither
//gave an event)
static bartap_epub_result_t document_step(document_t *d, bartap_xml_event_t *event)
{
    memset(event, 0, sizeof *event);
    if (d->chunk_start == d->chunk_end && !d->ended) {
        size_t got;
        bool end;
        const bartap_epub_result_t r = content_read(&d->content, d->chunk, sizeof d->chunk, &got, &end);
        d->chunk_start = 0;
        d->chunk_end = got;
        d->ended = end;
        return r;
    }
    size_t used;
    const bartap_xml_result_t r = bartap_xml_next(&d->xml, d->chunk + d->chunk_start, d->chunk_end - d->chunk_start,
        &used, d->ended, event);
    d->chunk_start += used;
    return from_xml(r);
}

/* ------------------------------------------------------------ the book */

typedef struct {
    bartap_ocf_archive_t archive;
    bartap_epub_spine_item_t *spine;
    size_t spine_capacity;
    uint32_t spine_count;
    uint32_t warnings;
    bartap_offset_t max_document_size;
    uint32_t flags;
    uint8_t mode;
    bool open;
    uint8_t toc_kind;
    bool has_cover;
    uint8_t cover_type;
    bartap_epub_location_t toc;
    bartap_epub_location_t cover;
    uint16_t metadata_length[4];
    char metadata[4][BARTAP_EPUB_METADATA_MAX + 1U];
} book_t;

/* A path found in the manifest, kept as its hash and length; the directory walk at the end of open finds
 * its entry. */
typedef struct {
    uint32_t hash;
    uint16_t length;
    bool known;
    bool found;
    uint8_t type;                   //a cover's image type
    bartap_epub_location_t location;
} wanted_t;

enum {
    OPEN_ARCHIVE,                   //opening the archive
    OPEN_CONTAINER,                 //reading META-INF/container.xml
    OPEN_FIND_PACKAGE,              //looking for the OPF
    OPEN_PACKAGE_FIRST,             //metadata, spine (FULL) or manifest (METADATA_COVER)
    OPEN_PACKAGE_MANIFEST,          //FULL: the manifest
    OPEN_WALK,                      //the directory, for the spine items, table of contents and cover
    OPEN_WALK_NAME,                 //the name of an entry that may be one of them
    OPEN_DONE,
};

//which element's start tag is being read, as far as the OPF passes care
enum {
    ELEMENT_OTHER,
    ELEMENT_ROOTFILE,
    ELEMENT_ITEM,
    ELEMENT_ITEMREF,
    ELEMENT_META,
    ELEMENT_SPINE,
};

typedef struct {
    book_t *book;
    bartap_epub_memory_t memory;
    bartap_ocf_opener_t ocf;
    uint8_t stage;
    bartap_epub_result_t status;
    document_t document;
    bartap_ocf_finder_t finder;
    bartap_ocf_cursor_t walk;
    bartap_ocf_entry_t walk_entry;
    uint8_t name[BARTAP_EPUB_PATH_MAX + 1U];   //an entry's name in the walk, or a resolved href in the OPF
    uint8_t package[BARTAP_EPUB_PATH_MAX + 1U];
    size_t package_length;
    uint32_t package_count;
    bool rootfile_found;
    //the start tag being read
    uint8_t element;
    uint32_t element_depth;
    uint32_t id_hash;
    bool has_id;
    bool has_href;
    uint32_t href_hash;
    uint16_t href_length;
    bool rootfile_path;             //container.xml: full-path is in name
    bool media_opf;
    bool media_ncx;
    uint8_t media_image;
    bool property_nav;
    bool property_cover;
    bool meta_cover;                //<meta name="cover">
    uint32_t content_hash;
    bool has_content;
    //the OPF's elements
    uint32_t metadata_depth;
    uint32_t manifest_depth;
    uint32_t spine_depth;
    int capture;                    //the metadata field being captured, or -1
    uint32_t capture_depth;
    uint32_t toc_id_hash;           //the spine's toc attribute (EPUB 2's NCX)
    bool has_toc_id;
    uint32_t cover_id_hash;         //EPUB 2's <meta name="cover" content>
    bool has_cover_id;
    //what the walk looks for
    wanted_t nav;
    wanted_t ncx;
    wanted_t cover_property;        //EPUB 3: properties="cover-image"
    wanted_t cover_meta;            //EPUB 2: the item the meta names
    wanted_t cover;                 //the one of those two chosen
} opener_t;

_Static_assert(sizeof(book_t) <= BARTAP_EPUB_BOOK_SIZE, "BARTAP_EPUB_BOOK_SIZE is too small");
_Static_assert(sizeof(opener_t) <= BARTAP_EPUB_OPENER_SIZE, "BARTAP_EPUB_OPENER_SIZE is too small");
_Static_assert(sizeof(content_t) <= BARTAP_EPUB_STREAM_SIZE, "BARTAP_EPUB_STREAM_SIZE is too small");
_Static_assert(BARTAP_EPUB_WARNING_NAME_NOT_UTF8 == BARTAP_OCF_WARNING_NAME_NOT_UTF8
    && BARTAP_EPUB_WARNING_MIMETYPE_MISSING == BARTAP_OCF_WARNING_MIMETYPE_MISSING
    && BARTAP_EPUB_WARNING_MIMETYPE_EXTRA_FIELD == BARTAP_OCF_WARNING_MIMETYPE_EXTRA_FIELD,
    "the book's warning bits must match the OCF layer's");
_Static_assert(BARTAP_EPUB_SCRATCH_MIN >= BARTAP_OCF_OPEN_SCRATCH_MIN, "scratch minimum");
_Static_assert(BARTAP_EPUB_PATH_MAX <= UINT16_MAX && BARTAP_EPUB_METADATA_MAX <= UINT16_MAX, "lengths kept in 16 bits");

//the opaque storage holds the real state; it is only ever used through these
static book_t *book_state(bartap_epub_t *book)
{
    return (book_t *)(void *)book->storage.bytes;
}

static const book_t *book_state_const(const bartap_epub_t *book)
{
    return (const book_t *)(const void *)book->storage.bytes;
}

static opener_t *opener_state(bartap_epub_opener_t *opener)
{
    return (opener_t *)(void *)opener->storage.bytes;
}

static bool is_open(const bartap_epub_t *book)
{
    return book != NULL && book_state_const(book)->open;
}

//open with everything: spine, streams, table of contents
static bool is_full(const bartap_epub_t *book)
{
    return is_open(book) && book_state_const(book)->mode == BARTAP_EPUB_OPEN_FULL;
}

static uint32_t xml_flags(const book_t *b, bool xhtml)
{
    return ((b->flags & BARTAP_EPUB_OPEN_STRICT_MARKUP) != 0U ? BARTAP_XML_FLAG_STRICT : 0U)
        | (xhtml ? BARTAP_XML_FLAG_XHTML_ENTITIES : 0U);
}

static bool spine_lookup(const book_t *b, uint32_t hash, size_t length, uint32_t *index)
{
    for (uint32_t i = 0; i < b->spine_count; i++) {
        const bartap_epub_spine_item_t *item = &b->spine[i];
        if ((item->flags & SPINE_HREF) && item->href_hash == hash && item->href_length == length) {
            *index = i;
            return true;
        }
    }
    return false;
}

static uint8_t image_type(const uint8_t *media, size_t length)
{
    if (bartap_bytes_are(media, length, BARTAP_LITERAL("image/jpeg"))) {
        return BARTAP_EPUB_IMAGE_JPEG;
    }
    if (bartap_bytes_are(media, length, BARTAP_LITERAL("image/png"))) {
        return BARTAP_EPUB_IMAGE_PNG;
    }
    if (bartap_bytes_are(media, length, BARTAP_LITERAL("image/gif"))) {
        return BARTAP_EPUB_IMAGE_GIF;
    }
    if (bartap_bytes_are(media, length, BARTAP_LITERAL("image/svg+xml"))) {
        return BARTAP_EPUB_IMAGE_SVG;
    }
    if (bartap_bytes_are(media, length, BARTAP_LITERAL("image/webp"))) {
        return BARTAP_EPUB_IMAGE_WEBP;
    }
    return BARTAP_EPUB_IMAGE_OTHER;
}

static void start_document(opener_t *o, const bartap_ocf_entry_t *entry)
{
    content_start_entry(&o->document.content, &o->book->archive, entry, o->book->max_document_size,
        o->memory.window, o->memory.window_size, o->memory.input, o->memory.input_size);
    document_xml(&o->document, xml_flags(o->book, false));
}

//a pass over a document is over: note whether its markup needed recovering from
static void finish_document(opener_t *o)
{
    if (o->document.xml.warnings != 0U) {
        o->book->warnings |= BARTAP_EPUB_WARNING_MARKUP_RECOVERED;
    }
}

static void start_tag(opener_t *o, const bartap_xml_event_t *e, uint8_t element)
{
    o->element = element;
    o->element_depth = e->depth;
    o->has_id = false;
    o->has_href = false;
    o->rootfile_path = false;
    o->media_opf = false;
    o->media_ncx = false;
    o->media_image = BARTAP_EPUB_IMAGE_OTHER;
    o->property_nav = false;
    o->property_cover = false;
    o->meta_cover = false;
    o->has_content = false;
}

//container.xml: the first rootfile whose media-type is the OPF's
static bartap_epub_result_t container_event(opener_t *o, const bartap_xml_event_t *e)
{
    if (o->rootfile_found) {
        return BARTAP_EPUB_RESULT_OK;
    }
    switch (e->kind) {
    case BARTAP_XML_EVENT_START:
        start_tag(o, e, bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("rootfile")) ? ELEMENT_ROOTFILE
            : ELEMENT_OTHER);
        break;
    case BARTAP_XML_EVENT_ATTRIBUTE:
        if (o->element != ELEMENT_ROOTFILE) {
            break;
        }
        if (bartap_bytes_are(e->name, e->name_length, BARTAP_LITERAL("full-path")) && e->value_length > 0U) {
            if (e->truncated || e->value_length > BARTAP_EPUB_PATH_MAX) {
                return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
            }
            memcpy(o->name, e->value, e->value_length);
            o->href_length = (uint16_t)e->value_length;
            o->rootfile_path = true;
        } else if (bartap_bytes_are(e->name, e->name_length, BARTAP_LITERAL("media-type"))) {
            o->media_opf = !e->truncated
                && bartap_bytes_are(e->value, e->value_length, BARTAP_LITERAL("application/oebps-package+xml"));
        }
        break;
    case BARTAP_XML_EVENT_START_END:
        if (o->element == ELEMENT_ROOTFILE && o->rootfile_path && o->media_opf) {
            memcpy(o->package, o->name, o->href_length);
            o->package_length = o->href_length;
            o->package[o->package_length] = 0;
            o->rootfile_found = true;
        }
        break;
    default:
        break;
    }
    return BARTAP_EPUB_RESULT_OK;
}

static const char metadata_names[4][11] = { "title", "creator", "language", "identifier" };
static const uint8_t metadata_name_lengths[4] = { 5, 7, 8, 10 };

static void capture_text(opener_t *o, const bartap_xml_event_t *e)
{
    book_t *b = o->book;
    const int f = o->capture;
    for (size_t i = 0; i < e->value_length; i++) {
        if (b->metadata_length[f] == BARTAP_EPUB_METADATA_MAX) {
            b->warnings |= BARTAP_EPUB_WARNING_METADATA_TRUNCATED;
            break;
        }
        const uint8_t c = e->value[i];
        //collapse whitespace, and drop it at the start
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (b->metadata_length[f] > 0U && b->metadata[f][b->metadata_length[f] - 1U] != ' ') {
                b->metadata[f][b->metadata_length[f]++] = ' ';
            }
        } else {
            b->metadata[f][b->metadata_length[f]++] = (char)c;
        }
    }
}

static void finish_capture(opener_t *o)
{
    book_t *b = o->book;
    const int f = o->capture;
    while (b->metadata_length[f] > 0U && b->metadata[f][b->metadata_length[f] - 1U] == ' ') {
        b->metadata_length[f]--;
    }
    b->metadata[f][b->metadata_length[f]] = '\0';
    o->capture = -1;
}

static void want(wanted_t *w, uint32_t hash, uint16_t length, uint8_t type)
{
    if (!w->known) {
        w->known = true;
        w->hash = hash;
        w->length = length;
        w->type = type;
    }
}

//a manifest item, its start tag complete
static bartap_epub_result_t manifest_item(opener_t *o, bool spine_hrefs)
{
    if (!o->has_id || !o->has_href) {
        return BARTAP_EPUB_RESULT_OK;
    }
    book_t *b = o->book;
    if (spine_hrefs) {
        for (uint32_t i = 0; i < b->spine_count; i++) {
            bartap_epub_spine_item_t *item = &b->spine[i];
            if (!(item->flags & SPINE_HREF) && item->key == o->id_hash) {
                item->href_hash = o->href_hash;
                item->href_length = o->href_length;
                item->flags |= SPINE_HREF;
            }
        }
        if (o->property_nav) {
            want(&o->nav, o->href_hash, o->href_length, 0);
        }
        if (o->media_ncx && (!o->has_toc_id || o->toc_id_hash == o->id_hash)) {
            want(&o->ncx, o->href_hash, o->href_length, 0);
        }
    }
    if (o->property_cover) {
        want(&o->cover_property, o->href_hash, o->href_length, o->media_image);
    }
    if (o->has_cover_id && o->cover_id_hash == o->id_hash) {
        want(&o->cover_meta, o->href_hash, o->href_length, o->media_image);
    }
    return BARTAP_EPUB_RESULT_OK;
}

/* The OPF. The first pass takes the metadata, the cover's meta, and in FULL mode the spine's itemrefs as
 * idref hashes; in METADATA_COVER mode it takes the manifest's cover too. FULL mode's second pass takes the
 * manifest. *stop is set where a METADATA mode has what it came for. */
static bartap_epub_result_t package_event(opener_t *o, const bartap_xml_event_t *e, bool first, bool *stop)
{
    book_t *b = o->book;
    const bool full = b->mode == BARTAP_EPUB_OPEN_FULL;
    const bool want_manifest = first ? b->mode == BARTAP_EPUB_OPEN_METADATA_COVER : true;
    switch (e->kind) {
    case BARTAP_XML_EVENT_START: {
        const uint8_t *n = e->name;
        const size_t l = e->name_length;
        uint8_t element = ELEMENT_OTHER;
        if (e->depth == 2U && bartap_local_is(n, l, BARTAP_LITERAL("metadata"))) {
            o->metadata_depth = first ? e->depth : 0U;
        } else if (e->depth == 2U && bartap_local_is(n, l, BARTAP_LITERAL("manifest"))) {
            o->manifest_depth = want_manifest ? e->depth : 0U;
        } else if (e->depth == 2U && bartap_local_is(n, l, BARTAP_LITERAL("spine"))) {
            o->spine_depth = first && full ? e->depth : 0U;
            element = ELEMENT_SPINE;
        } else if (o->manifest_depth != 0U && e->depth == o->manifest_depth + 1U
                && bartap_local_is(n, l, BARTAP_LITERAL("item"))) {
            element = ELEMENT_ITEM;
        } else if (o->spine_depth != 0U && e->depth == o->spine_depth + 1U
                && bartap_local_is(n, l, BARTAP_LITERAL("itemref"))) {
            element = ELEMENT_ITEMREF;
        } else if (o->metadata_depth != 0U && e->depth == o->metadata_depth + 1U) {
            if (bartap_local_is(n, l, BARTAP_LITERAL("meta"))) {
                element = ELEMENT_META;
            } else if (o->capture < 0) {
                for (int f = 0; f < 4; f++) {
                    if (b->metadata_length[f] == 0U
                            && bartap_local_is(n, l, metadata_names[f], metadata_name_lengths[f])) {
                        o->capture = f;
                        o->capture_depth = e->depth;
                    }
                }
            }
        }
        start_tag(o, e, element);
        break;
    }
    case BARTAP_XML_EVENT_ATTRIBUTE: {
        if (e->depth != o->element_depth) {
            break;
        }
        const uint8_t *n = e->name;
        const size_t l = e->name_length;
        const uint8_t *v = e->value;
        const size_t vl = e->value_length;
        switch (o->element) {
        case ELEMENT_ITEM:
            if (bartap_bytes_are(n, l, BARTAP_LITERAL("id")) && !e->truncated) {
                o->id_hash = bartap_hash_bytes(v, vl);
                o->has_id = true;
            } else if (bartap_bytes_are(n, l, BARTAP_LITERAL("href")) && !e->truncated) {
                size_t length;
                const size_t base = directory_length(o->package, o->package_length);
                //an href naming nothing in the archive, or too long, leaves the item without one
                o->has_href = vl > 0U && resolve_path(o->package, base, v, vl, o->name, sizeof o->name, &length)
                    == BARTAP_EPUB_RESULT_OK;
                if (o->has_href) {
                    o->href_hash = bartap_hash_bytes(o->name, length);
                    o->href_length = (uint16_t)length;
                }
            } else if (bartap_bytes_are(n, l, BARTAP_LITERAL("properties"))) {
                o->property_nav = bartap_has_token(v, vl, e->truncated, BARTAP_LITERAL("nav"));
                o->property_cover = bartap_has_token(v, vl, e->truncated, BARTAP_LITERAL("cover-image"));
            } else if (bartap_bytes_are(n, l, BARTAP_LITERAL("media-type")) && !e->truncated) {
                o->media_ncx = bartap_bytes_are(v, vl, BARTAP_LITERAL("application/x-dtbncx+xml"));
                o->media_image = image_type(v, vl);
            }
            break;
        case ELEMENT_ITEMREF:
            if (bartap_bytes_are(n, l, BARTAP_LITERAL("idref")) && !e->truncated) {
                o->id_hash = bartap_hash_bytes(v, vl);
                o->has_id = true;
            }
            break;
        case ELEMENT_SPINE:
            if (bartap_bytes_are(n, l, BARTAP_LITERAL("toc")) && !e->truncated && first) {
                o->toc_id_hash = bartap_hash_bytes(v, vl);
                o->has_toc_id = true;
            }
            break;
        case ELEMENT_META:
            if (bartap_bytes_are(n, l, BARTAP_LITERAL("name"))) {
                o->meta_cover = !e->truncated && bartap_bytes_are(v, vl, BARTAP_LITERAL("cover"));
            } else if (bartap_bytes_are(n, l, BARTAP_LITERAL("content")) && !e->truncated) {
                o->content_hash = bartap_hash_bytes(v, vl);
                o->has_content = true;
            }
            break;
        default:
            break;
        }
        break;
    }
    case BARTAP_XML_EVENT_START_END:
        if (e->depth != o->element_depth) {
            break;
        }
        if (o->element == ELEMENT_ITEM) {
            return manifest_item(o, !first);
        }
        if (o->element == ELEMENT_ITEMREF && o->has_id) {
            if (b->spine_count == b->spine_capacity) {
                return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
            }
            bartap_epub_spine_item_t *item = &b->spine[b->spine_count++];
            memset(item, 0, sizeof *item);
            item->key = o->id_hash;
        } else if (o->element == ELEMENT_META && o->meta_cover && o->has_content && !o->has_cover_id) {
            o->cover_id_hash = o->content_hash;
            o->has_cover_id = true;
        }
        break;
    case BARTAP_XML_EVENT_TEXT:
        if (o->capture >= 0 && e->depth == o->capture_depth) {
            capture_text(o, e);
        }
        break;
    case BARTAP_XML_EVENT_END:
        //by depth: an END that recovery made has no name
        if (o->capture >= 0 && e->depth <= o->capture_depth) {
            finish_capture(o);
        }
        if (e->depth == o->metadata_depth) {
            o->metadata_depth = 0;
            *stop = b->mode == BARTAP_EPUB_OPEN_METADATA;
        } else if (e->depth == o->manifest_depth) {
            o->manifest_depth = 0;
            *stop = first && b->mode == BARTAP_EPUB_OPEN_METADATA_COVER;
        } else if (e->depth == o->spine_depth) {
            o->spine_depth = 0;
        }
        break;
    default:
        break;
    }
    return BARTAP_EPUB_RESULT_OK;
}

static void start_package_pass(opener_t *o, uint8_t stage)
{
    start_document(o, &o->finder.entry);
    o->metadata_depth = 0;
    o->manifest_depth = 0;
    o->spine_depth = 0;
    o->capture = -1;
    o->element = ELEMENT_OTHER;
    o->element_depth = 0;
    o->stage = stage;
}

//the cover: EPUB 3's property first, then EPUB 2's meta
static void choose_cover(opener_t *o)
{
    o->cover = o->cover_property.known ? o->cover_property : o->cover_meta;
}

static void start_walk(opener_t *o)
{
    bartap_ocf_cursor_start(&o->book->archive, &o->walk);
    o->stage = OPEN_WALK;
}

//the end of a package pass: what comes next
static void package_done(opener_t *o, bool first)
{
    finish_document(o);
    book_t *b = o->book;
    if (o->capture >= 0) {
        finish_capture(o);
    }
    if (first && b->mode == BARTAP_EPUB_OPEN_FULL) {
        start_package_pass(o, OPEN_PACKAGE_MANIFEST);
        return;
    }
    choose_cover(o);
    if (b->mode == BARTAP_EPUB_OPEN_METADATA || (b->mode == BARTAP_EPUB_OPEN_METADATA_COVER && !o->cover.known)) {
        o->stage = OPEN_DONE;
        return;
    }
    start_walk(o);
}

//whether a walked entry may be one the walk looks for, by its name's length
static bool walk_wants(const opener_t *o, size_t length)
{
    if (length > BARTAP_EPUB_PATH_MAX) {
        return false;
    }
    if (o->cover.known && !o->cover.found && o->cover.length == length) {
        return true;
    }
    if (o->book->mode != BARTAP_EPUB_OPEN_FULL) {
        return false;
    }
    if (length == o->package_length || (o->nav.known && o->nav.length == length)
            || (o->ncx.known && o->ncx.length == length)) {
        return true;
    }
    const book_t *b = o->book;
    for (uint32_t i = 0; i < b->spine_count; i++) {
        if ((b->spine[i].flags & (SPINE_HREF | SPINE_FOUND)) == SPINE_HREF && b->spine[i].href_length == length) {
            return true;
        }
    }
    return false;
}

static void walk_match(wanted_t *w, uint32_t hash, size_t length, const bartap_epub_location_t *at)
{
    if (w->known && !w->found && w->hash == hash && w->length == length) {
        w->location = *at;
        w->found = true;
    }
}

//a walked entry's name, read into o->name: match it against everything wanted
static bartap_epub_result_t walk_name(opener_t *o)
{
    book_t *b = o->book;
    const size_t length = o->walk_entry.name_length;
    const uint32_t h = bartap_hash_bytes(o->name, length);
    const bartap_epub_location_t at = { bartap_ocf_entry_header_offset(&o->walk_entry), o->walk_entry.index };
    walk_match(&o->cover, h, length, &at);
    if (b->mode != BARTAP_EPUB_OPEN_FULL) {
        if (o->cover.found) {
            o->stage = OPEN_DONE;                       //METADATA_COVER: nothing more to look for
        }
        return BARTAP_EPUB_RESULT_OK;
    }
    if (length == o->package_length && memcmp(o->name, o->package, length) == 0 && ++o->package_count > 1U) {
        return BARTAP_EPUB_RESULT_CORRUPT;              //the OPF appears twice (D21)
    }
    walk_match(&o->nav, h, length, &at);
    walk_match(&o->ncx, h, length, &at);
    for (uint32_t i = 0; i < b->spine_count; i++) {
        bartap_epub_spine_item_t *item = &b->spine[i];
        if ((item->flags & (SPINE_HREF | SPINE_FOUND)) == SPINE_HREF && item->href_hash == h
                && item->href_length == length) {
            item->entry_offset = at.entry_offset;
            item->entry_index = at.entry_index;
            item->flags |= SPINE_FOUND;
        }
    }
    return BARTAP_EPUB_RESULT_OK;
}

//the walk is over: what it found becomes the book's
static void walk_done(opener_t *o)
{
    book_t *b = o->book;
    if (b->mode == BARTAP_EPUB_OPEN_FULL) {
        for (uint32_t i = 0; i < b->spine_count; i++) {
            if (!(b->spine[i].flags & SPINE_FOUND)) {
                b->warnings |= BARTAP_EPUB_WARNING_SPINE_ITEM_MISSING;
            }
        }
        if (o->nav.found) {
            b->toc_kind = TOC_NAV;
            b->toc = o->nav.location;
        } else if (o->ncx.found) {
            b->toc_kind = TOC_NCX;
            b->toc = o->ncx.location;
        } else {
            b->warnings |= BARTAP_EPUB_WARNING_NO_TOC;
        }
    }
    o->stage = OPEN_DONE;
}

static bartap_epub_result_t open_step(opener_t *o)
{
    book_t *b = o->book;
    switch (o->stage) {
    case OPEN_ARCHIVE: {
        bool done;
        const bartap_ocf_result_t r = bartap_ocf_open_step(&o->ocf, &done);
        if (r != BARTAP_OCF_RESULT_OK) {
            return from_ocf(r);
        }
        if (done) {
            b->warnings |= b->archive.warnings;
            //the OCF layer found container.xml in its walk (D21): no second walk for it
            start_document(o, &o->ocf.container);
            o->stage = OPEN_CONTAINER;
        }
        return BARTAP_EPUB_RESULT_OK;
    }
    case OPEN_FIND_PACKAGE: {
        bartap_ocf_entry_t entry;
        bool done;
        const bartap_ocf_result_t r = bartap_ocf_find_step(&o->finder, &entry, &done);
        if (r == BARTAP_OCF_RESULT_NOT_FOUND) {
            return BARTAP_EPUB_RESULT_CORRUPT;          //container.xml names an OPF that is not there
        }
        if (r != BARTAP_OCF_RESULT_OK) {
            return from_ocf(r);
        }
        if (done) {
            start_package_pass(o, OPEN_PACKAGE_FIRST);
        }
        return BARTAP_EPUB_RESULT_OK;
    }
    case OPEN_CONTAINER:
    case OPEN_PACKAGE_FIRST:
    case OPEN_PACKAGE_MANIFEST: {
        bartap_xml_event_t e;
        bartap_epub_result_t r = document_step(&o->document, &e);
        if (r != BARTAP_EPUB_RESULT_OK || e.kind == BARTAP_XML_EVENT_NONE) {
            return r;
        }
        bool stop = e.kind == BARTAP_XML_EVENT_DONE;
        if (!stop) {
            r = o->stage == OPEN_CONTAINER ? container_event(o, &e)
                : package_event(o, &e, o->stage == OPEN_PACKAGE_FIRST, &stop);
            if (r != BARTAP_EPUB_RESULT_OK) {
                return r;
            }
        }
        if (!stop) {
            return BARTAP_EPUB_RESULT_OK;
        }
        if (o->stage == OPEN_CONTAINER) {
            finish_document(o);
            if (!o->rootfile_found) {
                return BARTAP_EPUB_RESULT_NOT_AN_EPUB;
            }
            bartap_ocf_find_begin(&o->finder, &b->archive, o->package, o->package_length);
            o->stage = OPEN_FIND_PACKAGE;
        } else {
            package_done(o, o->stage == OPEN_PACKAGE_FIRST);
        }
        return BARTAP_EPUB_RESULT_OK;
    }
    case OPEN_WALK: {
        bool ready;
        bool end;
        const bartap_ocf_result_t r = bartap_ocf_next(&b->archive, &o->walk, &o->walk_entry, &ready, &end);
        if (r != BARTAP_OCF_RESULT_OK) {
            return from_ocf(r);
        }
        if (end) {
            walk_done(o);
        } else if (ready && walk_wants(o, o->walk_entry.name_length)) {
            o->stage = OPEN_WALK_NAME;
        }
        return BARTAP_EPUB_RESULT_OK;
    }
    case OPEN_WALK_NAME: {
        const bartap_ocf_result_t r = bartap_ocf_entry_name(&b->archive, &o->walk_entry, o->name, sizeof o->name);
        if (r != BARTAP_OCF_RESULT_OK) {
            return from_ocf(r);
        }
        o->stage = OPEN_WALK;
        return walk_name(o);
    }
    default:
        return BARTAP_EPUB_RESULT_OK;
    }
}

bartap_epub_result_t bartap_epub_open_begin(bartap_epub_opener_t *opener, bartap_epub_t *book,
    const bartap_byte_source_t *source, const bartap_epub_memory_t *memory)
{
    if (opener == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    opener_t *o = opener_state(opener);
    memset(o, 0, sizeof *o);
    o->status = BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    o->capture = -1;
    if (book == NULL) {
        return o->status;
    }
    book_t *b = book_state(book);
    memset(b, 0, sizeof *b);
    if (source == NULL || memory == NULL || memory->scratch == NULL || memory->scratch_size < BARTAP_EPUB_SCRATCH_MIN
            || memory->input == NULL || memory->input_size == 0U || memory->window == NULL
            || memory->mode > BARTAP_EPUB_OPEN_METADATA_COVER
            || (memory->mode == BARTAP_EPUB_OPEN_FULL && memory->spine == NULL && memory->spine_capacity != 0U)) {
        return o->status;
    }
    o->book = b;
    o->memory = *memory;
    b->spine = memory->spine;
    b->spine_capacity = memory->mode == BARTAP_EPUB_OPEN_FULL ? memory->spine_capacity : 0U;
    b->max_document_size = memory->max_document_size;
    b->flags = memory->flags;
    b->mode = memory->mode;
    o->status = from_ocf(bartap_ocf_open_begin(&o->ocf, &b->archive, source, memory->scratch, memory->scratch_size));
    o->stage = OPEN_ARCHIVE;
    return o->status;
}

bartap_epub_result_t bartap_epub_open_step(bartap_epub_opener_t *opener, bool *done)
{
    if (opener == NULL || done == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    opener_t *o = opener_state(opener);
    *done = o->stage == OPEN_DONE && o->status == BARTAP_EPUB_RESULT_OK;
    if (o->status != BARTAP_EPUB_RESULT_OK || *done) {
        return o->status;
    }
    o->status = open_step(o);
    if (o->status == BARTAP_EPUB_RESULT_OK && o->stage == OPEN_DONE) {
        book_t *b = o->book;
        if (o->cover.found) {
            b->has_cover = true;
            b->cover = o->cover.location;
            b->cover_type = o->cover.type;
        }
        b->open = true;
    }
    *done = o->stage == OPEN_DONE && o->status == BARTAP_EPUB_RESULT_OK;
    return o->status;
}

bartap_epub_result_t bartap_epub_open(bartap_epub_opener_t *opener, bartap_epub_t *book,
    const bartap_byte_source_t *source, const bartap_epub_memory_t *memory)
{
    bartap_epub_result_t r = bartap_epub_open_begin(opener, book, source, memory);
    bool done = false;
    while (r == BARTAP_EPUB_RESULT_OK && !done) {
        r = bartap_epub_open_step(opener, &done);
    }
    return r;
}

uint32_t bartap_epub_warnings(const bartap_epub_t *book)
{
    return book == NULL ? 0U : book_state_const(book)->warnings;
}

bartap_epub_result_t bartap_epub_metadata(const bartap_epub_t *book, bartap_epub_field_t field, char *buffer,
    size_t buffer_size, size_t *length)
{
    if (!is_open(book) || buffer == NULL || (unsigned)field > 3U) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    const book_t *b = book_state_const(book);
    const size_t n = b->metadata_length[field];
    if (n == 0U) {
        return BARTAP_EPUB_RESULT_NOT_FOUND;
    }
    if (buffer_size < n + 1U) {
        return BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
    }
    memcpy(buffer, b->metadata[field], n + 1U);
    if (length != NULL) {
        *length = n;
    }
    return BARTAP_EPUB_RESULT_OK;
}

/* ------------------------------------------------------------ spine */

uint32_t bartap_epub_spine_count(const bartap_epub_t *book)
{
    return is_full(book) ? book_state_const(book)->spine_count : 0U;
}

bartap_epub_result_t bartap_epub_spine_index(const bartap_epub_t *book, const char *name, uint32_t *index)
{
    if (!is_full(book) || name == NULL || index == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    size_t n = 0;
    while (n <= BARTAP_EPUB_PATH_MAX && name[n] != '\0') {
        n++;
    }
    return spine_lookup(book_state_const(book), bartap_hash_bytes((const uint8_t *)name, n), n, index)
        ? BARTAP_EPUB_RESULT_OK : BARTAP_EPUB_RESULT_NOT_FOUND;
}

//spine item index's location, if its entry was found
static bartap_epub_result_t spine_location(const bartap_epub_t *book, uint32_t index, bartap_epub_location_t *at)
{
    if (!is_full(book) || index >= book_state_const(book)->spine_count) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    const bartap_epub_spine_item_t *item = &book_state_const(book)->spine[index];
    if (!(item->flags & SPINE_FOUND)) {
        return BARTAP_EPUB_RESULT_NOT_FOUND;
    }
    at->entry_offset = item->entry_offset;
    at->entry_index = item->entry_index;
    return BARTAP_EPUB_RESULT_OK;
}

bartap_epub_result_t bartap_epub_spine_location(const bartap_epub_t *book, uint32_t index,
    bartap_epub_location_t *location)
{
    if (location == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    return spine_location(book, index, location);
}

bartap_epub_result_t bartap_epub_spine_name(const bartap_epub_t *book, uint32_t index, uint8_t *buffer,
    size_t buffer_size)
{
    bartap_epub_location_t at;
    bartap_epub_result_t r = buffer == NULL ? BARTAP_EPUB_RESULT_INVALID_ARGUMENT : spine_location(book, index, &at);
    if (r != BARTAP_EPUB_RESULT_OK) {
        return r;
    }
    const book_t *b = book_state_const(book);
    bartap_ocf_cursor_t cursor;
    bartap_ocf_cursor_at(&b->archive, &cursor, at.entry_offset, at.entry_index);
    bartap_ocf_entry_t entry;
    bool ready = false;
    bool end = false;
    while (!ready) {
        const bartap_ocf_result_t o = bartap_ocf_next(&b->archive, &cursor, &entry, &ready, &end);
        if (o != BARTAP_OCF_RESULT_OK || end) {
            return o != BARTAP_OCF_RESULT_OK ? from_ocf(o) : BARTAP_EPUB_RESULT_CORRUPT;
        }
    }
    return from_ocf(bartap_ocf_entry_name(&b->archive, &entry, buffer, buffer_size));
}

/* ------------------------------------------------------------ any entry */

typedef struct {
    bartap_ocf_finder_t finder;
    uint8_t path[BARTAP_EPUB_PATH_MAX];
    bartap_epub_result_t status;
} finder_t;

_Static_assert(sizeof(finder_t) <= BARTAP_EPUB_FINDER_SIZE, "BARTAP_EPUB_FINDER_SIZE is too small");

static finder_t *finder_state(bartap_epub_finder_t *finder)
{
    return (finder_t *)(void *)finder->storage.bytes;
}

bartap_epub_result_t bartap_epub_find_begin(bartap_epub_finder_t *finder, const bartap_epub_t *book,
    const uint8_t *path, size_t path_length)
{
    if (finder == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    finder_t *f = finder_state(finder);
    memset(f, 0, sizeof *f);
    f->status = BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    if (!is_open(book) || (path == NULL && path_length != 0U)) {
        return f->status;
    }
    if (path_length > sizeof f->path) {
        f->status = BARTAP_EPUB_RESULT_LIMIT_EXCEEDED;
        return f->status;
    }
    if (path_length > 0U) {
        memcpy(f->path, path, path_length);
    }
    bartap_ocf_find_begin(&f->finder, &book_state_const(book)->archive, f->path, path_length);
    f->status = from_ocf(f->finder.status);
    return f->status;
}

bartap_epub_result_t bartap_epub_find_step(bartap_epub_finder_t *finder, bool *done,
    bartap_epub_location_t *location)
{
    if (finder == NULL || done == NULL || location == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    finder_t *f = finder_state(finder);
    *done = false;
    if (f->status != BARTAP_EPUB_RESULT_OK) {
        return f->status;
    }
    bartap_ocf_entry_t entry;
    f->status = from_ocf(bartap_ocf_find_step(&f->finder, &entry, done));
    if (f->status == BARTAP_EPUB_RESULT_OK && *done) {
        location->entry_offset = bartap_ocf_entry_header_offset(&entry);
        location->entry_index = entry.index;
    }
    return f->status;
}

bartap_epub_result_t bartap_epub_cover(const bartap_epub_t *book, bartap_epub_location_t *location, uint8_t *type)
{
    if (!is_open(book) || location == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    const book_t *b = book_state_const(book);
    if (!b->has_cover) {
        return BARTAP_EPUB_RESULT_NOT_FOUND;
    }
    *location = b->cover;
    if (type != NULL) {
        *type = b->cover_type;
    }
    return BARTAP_EPUB_RESULT_OK;
}

/* ------------------------------------------------------------ streams */

static content_t *stream_state(bartap_epub_stream_t *stream)
{
    return (content_t *)(void *)stream->storage.bytes;
}

static const content_t *stream_state_const(const bartap_epub_stream_t *stream)
{
    return (const content_t *)(const void *)stream->storage.bytes;
}

bartap_epub_result_t bartap_epub_stream_open_at(bartap_epub_stream_t *stream, const bartap_epub_t *book,
    const bartap_epub_location_t *location, bartap_offset_t max_size, uint8_t *window, size_t window_size,
    uint8_t *input, size_t input_size)
{
    if (stream == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    content_t *c = stream_state(stream);
    memset(c, 0, sizeof *c);
    c->status = BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    if (!is_open(book) || location == NULL) {
        return c->status;
    }
    content_start_at(c, &book_state_const(book)->archive, location, max_size, window, window_size, input,
        input_size);
    return BARTAP_EPUB_RESULT_OK;
}

bartap_epub_result_t bartap_epub_stream_open(bartap_epub_stream_t *stream, const bartap_epub_t *book,
    uint32_t index, bartap_offset_t max_size, uint8_t *window, size_t window_size, uint8_t *input,
    size_t input_size)
{
    if (stream == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    bartap_epub_location_t at;
    const bartap_epub_result_t r = spine_location(book, index, &at);
    if (r != BARTAP_EPUB_RESULT_OK) {
        content_t *c = stream_state(stream);
        memset(c, 0, sizeof *c);
        c->status = r;
        return r;
    }
    return bartap_epub_stream_open_at(stream, book, &at, max_size, window, window_size, input, input_size);
}

bartap_epub_result_t bartap_epub_stream_read(bartap_epub_stream_t *stream, void *buffer, size_t size,
    size_t *bytes_read, bool *end)
{
    if (stream == NULL || bytes_read == NULL || end == NULL || (buffer == NULL && size != 0U)) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    return content_read(stream_state(stream), buffer, size, bytes_read, end);
}

bartap_epub_result_t bartap_epub_stream_size(const bartap_epub_stream_t *stream, bartap_offset_t *size)
{
    if (stream == NULL || size == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    const content_t *c = stream_state_const(stream);
    if (c->status != BARTAP_EPUB_RESULT_OK || c->stage == CONTENT_HEADER) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    *size = c->entry.uncompressed_size;
    return BARTAP_EPUB_RESULT_OK;
}

/* ------------------------------------------------------------ hrefs */

bartap_epub_result_t bartap_epub_href_resolve(const bartap_epub_t *book, const uint8_t *base,
    size_t base_length, const uint8_t *href, size_t href_length, uint8_t *path, size_t path_size,
    size_t *path_length, bartap_epub_target_t *target)
{
    if ((base == NULL && base_length != 0U) || (href == NULL && href_length != 0U) || path == NULL
            || path_length == NULL || target == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    memset(target, 0, sizeof *target);
    target->spine_index = UINT32_MAX;
    const bartap_epub_result_t r = resolve_path(base, base_length, href, href_length, path, path_size, path_length);
    if (r != BARTAP_EPUB_RESULT_OK) {
        if (path_size > 0U) {
            path[0] = 0;
        }
        *path_length = 0;
        return r;
    }
    take_fragment(href, href_length, target);
    uint32_t index;
    if (is_full(book) && spine_lookup(book_state_const(book), bartap_hash_bytes(path, *path_length), *path_length,
            &index)) {
        target->spine_index = index;
    }
    return BARTAP_EPUB_RESULT_OK;
}

/* ------------------------------------------------------------ table of contents */

typedef struct {
    const book_t *book;
    document_t document;
    uint8_t kind;
    uint8_t base[BARTAP_EPUB_PATH_MAX + 1U];   //the table of contents' own path, for its hrefs
    uint8_t path[BARTAP_EPUB_PATH_MAX + 1U];
    uint32_t nav_candidate;         //depth of a nav whose epub:type is not known yet
    uint32_t nav_depth;             //depth of the toc nav (or the NCX's navMap); 0 outside it
    uint32_t list_depth;            //ol (or navPoint) nesting inside it
    uint8_t lists[DEPTH_MAX];       //the XML depth of each: an END that recovery made has no name
    uint32_t label_depth;           //depth of the element collecting a label; 0 if none
    bool nav_closed;                //the nav is over; the rest of the document is only checked
    bool finished;
    bartap_epub_toc_entry_t entry;  //the entry being built
} toc_t;

_Static_assert(sizeof(toc_t) <= BARTAP_EPUB_TOC_SIZE, "BARTAP_EPUB_TOC_SIZE is too small");

static toc_t *toc_state(bartap_epub_toc_t *toc)
{
    return (toc_t *)(void *)toc->storage.bytes;
}

bartap_epub_result_t bartap_epub_toc_open(bartap_epub_toc_t *toc, const bartap_epub_t *book, uint8_t *window,
    size_t window_size, uint8_t *input, size_t input_size)
{
    if (toc == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    toc_t *t = toc_state(toc);
    memset(t, 0, sizeof *t);
    t->finished = true;
    if (!is_full(book)) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    const book_t *b = book_state_const(book);
    if (b->toc_kind == TOC_NONE) {
        return BARTAP_EPUB_RESULT_NOT_FOUND;
    }
    t->finished = false;
    t->book = b;
    t->kind = b->toc_kind;
    content_start_at(&t->document.content, &b->archive, &b->toc, b->max_document_size, window, window_size, input,
        input_size);
    //its name first, for the directory its hrefs are relative to
    t->document.content.name = t->base;
    t->document.content.name_size = sizeof t->base;
    document_xml(&t->document, xml_flags(b, t->kind == TOC_NAV));
    return BARTAP_EPUB_RESULT_OK;
}

static void list_open(toc_t *t, uint32_t depth)
{
    if (t->list_depth < DEPTH_MAX) {
        t->lists[t->list_depth++] = (uint8_t)depth;     //depth is at most DEPTH_MAX, the tokenizer's limit
    }
}

//an element at depth ends: so does every list opened at or below it
static void lists_close(toc_t *t, uint32_t depth)
{
    while (t->list_depth > 0U && t->lists[t->list_depth - 1U] >= depth) {
        t->list_depth--;
    }
}

static void entry_reset(toc_t *t)
{
    memset(&t->entry, 0, sizeof t->entry);
    t->entry.target.spine_index = UINT32_MAX;
}

static void label_append(bartap_epub_toc_entry_t *entry, const uint8_t *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        const uint8_t c = text[i];
        const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (space && (entry->label_length == 0U || entry->label[entry->label_length - 1U] == ' ')) {
            continue;
        }
        if (entry->label_length == BARTAP_EPUB_LABEL_MAX) {
            entry->label_truncated = true;
            return;
        }
        entry->label[entry->label_length++] = space ? ' ' : (char)c;
    }
}

static void label_finish(bartap_epub_toc_entry_t *entry)
{
    while (entry->label_length > 0U && entry->label[entry->label_length - 1U] == ' ') {
        entry->label_length--;
    }
    entry->label[entry->label_length] = '\0';
}

static void target(toc_t *t, const bartap_xml_event_t *e)
{
    if (e->truncated) {
        return;                     //an href cut short leads nowhere
    }
    size_t n;
    bartap_epub_target_t found;
    const size_t base_length = t->document.content.name_length;
    if (bartap_epub_href_resolve(NULL, t->base, base_length, e->value, e->value_length, t->path, sizeof t->path, &n,
            &found) != BARTAP_EPUB_RESULT_OK) {
        return;
    }
    uint32_t index;
    if (spine_lookup(t->book, bartap_hash_bytes(t->path, n), n, &index)) {
        found.spine_index = index;
    }
    t->entry.target = found;
}

//the EPUB 3 navigation document's toc nav: ol nesting, and an a (or a span, for a heading) per li
static bool nav_event(toc_t *t, const bartap_xml_event_t *e)
{
    switch (e->kind) {
    case BARTAP_XML_EVENT_START:
        if (t->nav_depth == 0U && bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("nav"))) {
            t->nav_candidate = e->depth;
        } else if (t->nav_depth != 0U && bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("ol"))) {
            list_open(t, e->depth);
        } else if (t->nav_depth != 0U && t->label_depth == 0U && t->list_depth > 0U
                && (bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("a"))
                    || bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("span")))) {
            t->label_depth = e->depth;
            entry_reset(t);
            t->entry.depth = t->list_depth;
        }
        return false;
    case BARTAP_XML_EVENT_ATTRIBUTE:
        if (t->nav_candidate == e->depth && t->nav_depth == 0U
                && bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("type"))
                && bartap_has_token(e->value, e->value_length, e->truncated, BARTAP_LITERAL("toc"))) {
            t->nav_depth = e->depth;
        } else if (t->label_depth == e->depth && bartap_bytes_are(e->name, e->name_length, BARTAP_LITERAL("href"))) {
            target(t, e);
        }
        return false;
    case BARTAP_XML_EVENT_TEXT:
        if (t->label_depth != 0U) {
            label_append(&t->entry, e->value, e->value_length);
        }
        return false;
    case BARTAP_XML_EVENT_END:
        //by depth: an END that recovery made has no name
        lists_close(t, e->depth);
        if (t->nav_depth != 0U && e->depth <= t->nav_depth) {
            t->nav_closed = true;
        }
        if (t->label_depth != 0U && e->depth <= t->label_depth) {
            t->label_depth = 0;
            label_finish(&t->entry);
            return true;
        }
        if (e->depth == t->nav_candidate) {
            t->nav_candidate = 0;
        }
        return false;
    default:
        return false;
    }
}

//the EPUB 2 NCX: navPoint nesting, a navLabel's text, then a content src
static bool ncx_event(toc_t *t, const bartap_xml_event_t *e)
{
    switch (e->kind) {
    case BARTAP_XML_EVENT_START:
        if (t->nav_depth == 0U && bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("navMap"))) {
            t->nav_depth = e->depth;
        } else if (t->nav_depth != 0U && bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("navPoint"))) {
            list_open(t, e->depth);
            entry_reset(t);
            t->entry.depth = t->list_depth;
        } else if (t->nav_depth != 0U && t->list_depth > 0U
                && bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("text"))) {
            t->label_depth = e->depth;
        }
        return false;
    case BARTAP_XML_EVENT_ATTRIBUTE:
        if (t->nav_depth != 0U && t->list_depth > 0U
                && bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("src"))) {
            target(t, e);
        }
        return false;
    case BARTAP_XML_EVENT_START_END:
        if (t->nav_depth != 0U && t->list_depth > 0U
                && bartap_local_is(e->name, e->name_length, BARTAP_LITERAL("content"))) {
            label_finish(&t->entry);
            return true;
        }
        return false;
    case BARTAP_XML_EVENT_TEXT:
        if (t->label_depth != 0U && e->depth == t->label_depth) {
            label_append(&t->entry, e->value, e->value_length);
        }
        return false;
    case BARTAP_XML_EVENT_END:
        if (t->label_depth != 0U && e->depth <= t->label_depth) {
            t->label_depth = 0;
        }
        lists_close(t, e->depth);
        if (t->nav_depth != 0U && e->depth <= t->nav_depth) {
            t->nav_closed = true;
        }
        return false;
    default:
        return false;
    }
}

bartap_epub_result_t bartap_epub_toc_next(bartap_epub_toc_t *toc, bartap_epub_toc_entry_t *entry, bool *ready,
    bool *end)
{
    if (toc == NULL || entry == NULL || ready == NULL || end == NULL) {
        return BARTAP_EPUB_RESULT_INVALID_ARGUMENT;
    }
    toc_t *t = toc_state(toc);
    *ready = false;
    *end = t->finished;
    if (t->finished) {
        return t->book == NULL ? BARTAP_EPUB_RESULT_INVALID_ARGUMENT : BARTAP_EPUB_RESULT_OK;
    }
    bartap_xml_event_t e;
    const bartap_epub_result_t r = document_step(&t->document, &e);
    if (r != BARTAP_EPUB_RESULT_OK) {
        return r;
    }
    if (e.kind == BARTAP_XML_EVENT_DONE) {
        t->finished = true;
        *end = true;
        return BARTAP_EPUB_RESULT_OK;
    }
    if (e.kind != BARTAP_XML_EVENT_NONE && !t->nav_closed
            && (t->kind == TOC_NAV ? nav_event(t, &e) : ncx_event(t, &e))) {
        *entry = t->entry;
        *ready = true;
    }
    return BARTAP_EPUB_RESULT_OK;
}

uint32_t bartap_epub_toc_warnings(const bartap_epub_toc_t *toc)
{
    return toc == NULL ? 0U : ((const toc_t *)(const void *)toc->storage.bytes)->document.xml.warnings;
}
