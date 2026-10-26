// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_H
#define BARTAP_H

/* Bartap: an allocation-free EPUB reader library for small devices. The whole public surface.
 *
 * - No heap. Every state struct is caller-owned; every inflating call takes a caller-supplied
 *   BARTAP_WINDOW_SIZE window, which may be reused once that call's object is finished with.
 * - Synchronous. Opening a book and finding things in it run to completion. Only resource bytes
 *   (bartap_stream_*) and text tokens (bartap_text_*) are streamed. A caller with real-time work on the
 *   same core does it inside its byte source's read callback.
 * - Lenient. Damaged packaging and markup are recovered from and reported as BARTAP_WARN_* bits; only
 *   what makes the book unreadable is an error.
 * - Checked. Every entry's bytes are checked against its CRC-32 when its end is reached: a stream or
 *   iterator returns BARTAP_END only for an entry that matched, and BARTAP_ERR_CORRUPT for one that did
 *   not. Bytes and tokens delivered before the end are unverified until then.
 * - Read-only. Bartap never writes to the book or anywhere else; what a reader saves (positions,
 *   bookmarks) is the caller's to store.
 * - A reading position is a bartap_loc_t: a spine index, a byte offset in that chapter's uncompressed
 *   XHTML, and an anchor (the bytes at that offset) that lets a position survive small changes to the
 *   file it was saved against.
 *
 * Every state struct's layout is here so that the caller can declare one of the right type and Bartap
 * can use it as that type, with no casts between byte arrays and structs. The fields of the structs
 * under "State" are Bartap's: a caller declares them, passes them in, and reads nothing from them. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------ results */

typedef enum bartap_err {
    BARTAP_OK = 0,
    BARTAP_END,             // not an error: a stream, TOC or text iterator has nothing more
    BARTAP_ERR_ARG,         // a NULL pointer, a buffer too small, an index out of range
    BARTAP_ERR_IO,          // the byte source's read callback failed
    BARTAP_ERR_NOT_EPUB,    // no ZIP end record, no container.xml, or no OPF
    BARTAP_ERR_CORRUPT,     // ZIP structures or a DEFLATE stream that cannot be read, or an entry whose
                            // bytes do not match its CRC-32
    BARTAP_ERR_UNSUPPORTED, // encryption, a compression method other than stored or deflate
    BARTAP_ERR_NOT_FOUND,   // no such entry, spine item, cover or TOC
    BARTAP_ERR_LIMIT,       // past one of the fixed limits below
} bartap_err_t;

const char *bartap_strerror(bartap_err_t err);

#define BARTAP_WARN_TRAILING_BYTES   (1UL << 0)  // bytes after the ZIP end record
#define BARTAP_WARN_MIMETYPE         (1UL << 1)  // mimetype entry missing, not first, or compressed
#define BARTAP_WARN_MARKUP           (1UL << 2)  // container.xml, the OPF or the TOC was not well-formed
#define BARTAP_WARN_ENCODING         (1UL << 3)  // invalid UTF-8, read as Windows-1252
#define BARTAP_WARN_SPINE_MISSING    (1UL << 4)  // an itemref with no manifest item or no entry: skipped
#define BARTAP_WARN_SPINE_TRUNCATED  (1UL << 5)  // more itemrefs than the caller's spine array holds
#define BARTAP_WARN_META_TRUNCATED   (1UL << 6)  // a metadata field longer than BARTAP_META_MAX
#define BARTAP_WARN_NO_TOC           (1UL << 7)  // neither a navigation document nor an NCX

/* ------------------------------------------------------------ limits */

#define BARTAP_WINDOW_SIZE    32768U    // RFC 1951's largest distance
#define BARTAP_META_MAX       127U      // bytes kept of each metadata field, before the NUL
#define BARTAP_LABEL_MAX      127U      // bytes of a TOC label, before the NUL
#define BARTAP_FRAGMENT_MAX   63U       // bytes of a TOC target's fragment, before the NUL
#define BARTAP_WORD_MAX       63U       // bytes of one WORD token, before the NUL; longer words split
#define BARTAP_PATH_MAX       255U      // longest archive path resolved
#define BARTAP_ANCHOR_SIZE    32U       // raw XHTML bytes kept with a position; searched for up to twice
                                        // this either side of the offset when it is restored

/* ------------------------------------------------------------ byte source */

/* Read len bytes at offset into buf, all of them, blocking; return 0 on success and anything else on
 * failure (reported as BARTAP_ERR_IO). Bartap never asks for bytes past the source's size. */
typedef int (*bartap_read_fn)(void *user, uint32_t offset, void *buf, size_t len);

typedef struct {
    bartap_read_fn read;
    void *user;
    uint32_t size;          // the archive's size in bytes; ZIP64 is read only where it fits
} bartap_source_t;

/* ------------------------------------------------------------ book types */

/* One spine item: where its entry is in the archive. The caller allocates an array of these for
 * bartap_open to fill; the fields are Bartap's. */
typedef struct {
    uint32_t entry_offset;      // the entry's local header
    uint32_t compressed_size;
    uint32_t size;              // uncompressed
    uint32_t crc32;             // from the central directory
    uint16_t method;            // 0 stored, 8 deflate
    uint16_t flags;             // BARTAP_SPINE_*
} bartap_spine_item_t;

#define BARTAP_SPINE_NONLINEAR (1U << 0)    // linear="no": outside the main reading order

/* A position, filled by bartap_text_loc and restored by bartap_text_seek. Plain data: the caller stores
 * it as it likes (a bookmark file), and restores it into a book that may since have changed. */
typedef struct {
    uint16_t spine_index;
    uint8_t anchor_len;         // bytes in anchor: fewer than BARTAP_ANCHOR_SIZE near the item's end
    uint32_t byte_offset;       // in the spine item's uncompressed XHTML
    uint8_t anchor[BARTAP_ANCHOR_SIZE];  // the raw XHTML bytes starting at byte_offset
} bartap_loc_t;

/* How bartap_text_seek placed a position. */
typedef enum {
    BARTAP_SEEK_EXACT,          // the anchor matched at the offset
    BARTAP_SEEK_MOVED,          // the anchor matched within 2 * BARTAP_ANCHOR_SIZE bytes; the nearest
                                // match was taken
    BARTAP_SEEK_OFFSET,         // no match nearby, or no anchor: placed at the bare offset
} bartap_seek_t;

typedef enum {
    BARTAP_META_TITLE,
    BARTAP_META_AUTHOR,
    BARTAP_META_LANGUAGE,
    BARTAP_META_IDENTIFIER,     // the one <package unique-identifier> names
    BARTAP_META_COUNT_,
} bartap_meta_t;

typedef struct {
    char label[BARTAP_LABEL_MAX + 1];
    char fragment[BARTAP_FRAGMENT_MAX + 1];  // the target's #fragment, without '#'; empty if none
    uint16_t spine_index;
    uint8_t depth;                           // 0 for a top-level entry
} bartap_toc_entry_t;

/* ------------------------------------------------------------ text types */

typedef enum {
    BARTAP_TOK_WORD,            // text: a run of non-whitespace, at most BARTAP_WORD_MAX bytes
    BARTAP_TOK_SPACE,           // collapsed whitespace between words; a literal space inside <pre>
    BARTAP_TOK_LINE_BREAK,      // <br>, or a newline inside <pre>
    BARTAP_TOK_BLOCK_START,     // block: which one
    BARTAP_TOK_BLOCK_END,
    BARTAP_TOK_IMAGE,           // text: the resolved archive path of an <img> or SVG <image>
} bartap_tok_type_t;

typedef enum {
    BARTAP_BLOCK_PARAGRAPH,     // p, div and anything else block-level
    BARTAP_BLOCK_HEADING,       // h1-h6; level in bartap_token_t
    BARTAP_BLOCK_LIST_ITEM,
    BARTAP_BLOCK_QUOTE,
    BARTAP_BLOCK_PRE,
    BARTAP_BLOCK_RULE,          // hr: a start with no content, then its end
} bartap_block_t;

#define BARTAP_STYLE_ITALIC     (1U << 0)
#define BARTAP_STYLE_BOLD       (1U << 1)
#define BARTAP_STYLE_MONOSPACE  (1U << 2)
#define BARTAP_STYLE_SUPER      (1U << 3)
#define BARTAP_STYLE_SUB        (1U << 4)

typedef struct {
    bartap_tok_type_t type;
    bartap_block_t block;       // BLOCK_START and BLOCK_END
    uint8_t level;              // heading level 1-6
    uint8_t style;              // BARTAP_STYLE_* in force, for WORD
    uint16_t len;               // bytes in text
    uint32_t offset;            // where the token starts in the XHTML: its bartap_loc_t byte_offset
    char text[BARTAP_PATH_MAX + 1];
} bartap_token_t;

/* ------------------------------------------------------------ state (fields are Bartap's) */

/* The 8-bit literal/length lookup table (512 bytes more in every inflating struct) pays off where
 * branches are expensive (Cortex-M33), not on x86. It changes struct layouts, so the library and every
 * file including this header must agree: set it through the BARTAP_INFLATE_FAST_LUT CMake option,
 * which defines it for both. */
typedef struct {
    uint8_t *window;            // BARTAP_WINDOW_SIZE bytes, caller-owned
    uint32_t window_pos;
    uint32_t bits;
    uint8_t bit_count;
    uint8_t state;              // block header, stored, Huffman, done
    bool final_block;
    uint16_t copy_length;       // of a match not yet fully written
    uint16_t copy_distance;
    uint16_t length_count[16];
    uint16_t length_symbol[288];
    uint16_t distance_count[16];
    uint16_t distance_symbol[30];
#if defined(BARTAP_INFLATE_FAST_LUT)
    uint16_t fast[256];
#endif
} bartap_inflate_t;

typedef struct {
    uint32_t cd_offset;         // central directory
    uint32_t cd_size;
    uint16_t entry_count;
    uint32_t warnings;          // BARTAP_WARN_TRAILING_BYTES, BARTAP_WARN_MIMETYPE
} bartap_zip_t;

typedef struct {
    uint32_t header_offset;     // local header
    uint32_t data_offset;       // resolved past the local header's name and extra field
    uint32_t compressed_size;
    uint32_t size;
    uint32_t crc32;             // from the central directory, which is authoritative over the local header
    uint16_t method;
} bartap_entry_t;

#define BARTAP_XML_NAME_MAX  31U
#define BARTAP_XML_ATTRS_MAX 255U      // bytes of one tag's attributes, kept raw
#define BARTAP_XML_IN_SIZE   256U

/* Where the XML tokenizer pulls its bytes from, so it runs the same over a stream and over memory. */
typedef bartap_err_t (*bartap_xml_fill_fn)(void *user, uint8_t *buf, size_t len, size_t *got);

typedef struct {
    bartap_xml_fill_fn fill;
    void *user;
    uint32_t offset;            // document bytes consumed
    uint32_t warnings;          // BARTAP_WARN_MARKUP, BARTAP_WARN_ENCODING
    uint16_t in_pos;
    uint16_t in_len;
    uint8_t in[BARTAP_XML_IN_SIZE];   // kept at least BARTAP_ANCHOR_SIZE bytes ahead of the next token
                                      // where the document has them, for bartap_text_loc's anchor
    char name[BARTAP_XML_NAME_MAX + 1];
    char attrs[BARTAP_XML_ATTRS_MAX + 1];
    uint16_t attrs_len;
    char text[BARTAP_WORD_MAX + 1];
} bartap_xml_t;

#define BARTAP_STREAM_IN_SIZE 512U

typedef struct {
    const bartap_source_t *source;
    bartap_entry_t entry;
    uint32_t in_offset;         // compressed bytes read from the entry so far
    uint32_t out_offset;        // uncompressed bytes delivered: bartap_stream_tell
    uint32_t crc32;             // of the bytes delivered so far, checked against entry.crc32 at the end
    uint16_t in_pos;
    uint16_t in_len;
    bartap_inflate_t inflate;
    uint8_t in[BARTAP_STREAM_IN_SIZE];
} bartap_stream_t;

typedef struct {
    const bartap_source_t *source;
    bartap_spine_item_t *spine;
    uint16_t spine_count;
    uint32_t warnings;
    bartap_zip_t zip;
    bartap_entry_t toc;         // the nav document or the NCX
    bool toc_is_ncx;
    bool has_toc;
    bartap_entry_t cover;
    bool has_cover;
    char opf_dir[BARTAP_PATH_MAX + 1];              // hrefs in the OPF are relative to it
    char meta[BARTAP_META_COUNT_][BARTAP_META_MAX + 1];
} bartap_book_t;

/* bartap_open's working memory: its parsing state and an inflate window. Needed only during the call;
 * afterwards window can serve as the window for any stream, TOC or text iterator. */
typedef struct {
    bartap_stream_t stream;
    bartap_xml_t xml;
    char path[BARTAP_PATH_MAX + 1];
    char unique_id[BARTAP_META_MAX + 1];    // <package unique-identifier>, matched against id= in pass 1
    uint8_t window[BARTAP_WINDOW_SIZE];
} bartap_work_t;

typedef struct {
    const bartap_book_t *book;
    bartap_stream_t stream;
    bartap_xml_t xml;
    uint8_t depth;
} bartap_toc_t;

typedef struct {
    uint16_t spine_index;
    bool from_memory;
    const uint8_t *memory;      // bartap_text_open_memory
    size_t memory_len;
    size_t memory_pos;
    bartap_stream_t stream;
    bartap_xml_t xml;
    uint8_t pre_depth;          // inside <pre>: whitespace kept
    uint8_t style;
    bool pending_space;
} bartap_text_t;

/* ------------------------------------------------------------ calls: source and book */

/* A source over size bytes in memory. The bytes are not copied and must outlive the source. */
void bartap_source_memory(bartap_source_t *source, const void *data, uint32_t size);

/* Open the book in source: find container.xml and the OPF, copy the metadata into book, fill spine
 * with up to spine_capacity items in reading order, and find the TOC and the cover. work is used only
 * during the call. source and spine must outlive book. */
bartap_err_t bartap_open(bartap_book_t *book, const bartap_source_t *source, bartap_spine_item_t *spine,
    uint16_t spine_capacity, bartap_work_t *work);

uint32_t bartap_warnings(const bartap_book_t *book);
uint16_t bartap_spine_count(const bartap_book_t *book);
bool bartap_spine_linear(const bartap_book_t *book, uint16_t spine_index);

/* Copy a metadata field into buf as NUL-terminated UTF-8, cut at a character boundary if it does not
 * fit. An absent field is an empty string. */
bartap_err_t bartap_metadata(const bartap_book_t *book, bartap_meta_t field, char *buf, size_t size);

/* ------------------------------------------------------------ calls: resource streams */

bartap_err_t bartap_stream_open(bartap_stream_t *stream, const bartap_book_t *book, uint16_t spine_index,
    uint8_t *window);
/* path is relative to the archive root, as in a ZIP entry name. */
bartap_err_t bartap_stream_open_path(bartap_stream_t *stream, const bartap_book_t *book, const char *path,
    uint8_t *window);
bartap_err_t bartap_stream_open_cover(bartap_stream_t *stream, const bartap_book_t *book, uint8_t *window);

/* Read up to len bytes; *got is 0 only with BARTAP_END, which comes only once the whole entry has
 * matched its CRC-32. A mismatch is BARTAP_ERR_CORRUPT in its place. Rewinding restarts the check. */
bartap_err_t bartap_stream_read(bartap_stream_t *stream, void *buf, size_t len, size_t *got);
uint32_t bartap_stream_size(const bartap_stream_t *stream);    // uncompressed
uint32_t bartap_stream_tell(const bartap_stream_t *stream);
bartap_err_t bartap_stream_rewind(bartap_stream_t *stream);

/* ------------------------------------------------------------ calls: table of contents */

/* The navigation document's toc nav if there is one, else the NCX. */
bartap_err_t bartap_toc_open(bartap_toc_t *toc, const bartap_book_t *book, uint8_t *window);
/* Entries in document order; BARTAP_END after the last. Entries whose target is not in the spine are
 * skipped. */
bartap_err_t bartap_toc_next(bartap_toc_t *toc, bartap_toc_entry_t *entry);

/* ------------------------------------------------------------ calls: text */

/* Tokens of one spine item, inflated as they are read. */
bartap_err_t bartap_text_open(bartap_text_t *text, const bartap_book_t *book, uint16_t spine_index,
    uint8_t *window);
/* Tokens of one spine item's XHTML already in memory (a chapter inflated once into PSRAM, say). xhtml
 * must outlive text. */
bartap_err_t bartap_text_open_memory(bartap_text_t *text, uint16_t spine_index, const uint8_t *xhtml,
    size_t len);
/* The next token; BARTAP_END at the end of the spine item, once its CRC-32 has matched, or
 * BARTAP_ERR_CORRUPT if it did not. Text opened from memory is not checked: it was checked when it was
 * inflated. */
bartap_err_t bartap_text_next(bartap_text_t *text, bartap_token_t *token);
/* Where the next token starts, with its anchor: the next BARTAP_ANCHOR_SIZE raw bytes from there, or
 * as many as the item has left. Take it at the top of a page. May refill the iterator's input to have
 * the anchor's bytes in hand, so it can return the stream's errors. */
bartap_err_t bartap_text_loc(bartap_text_t *text, bartap_loc_t *loc);
/* Restore a position in this spine item. With an anchor, the raw bytes at loc->byte_offset are compared
 * with it; on a mismatch the anchor is searched for up to 2 * BARTAP_ANCHOR_SIZE bytes either side and
 * the match nearest the offset is taken; with no match, or no anchor, the bare offset is used. The
 * iterator then continues from the first token starting at or after the position chosen, and *how
 * says which it was. A seek backwards re-reads from the start of the item; forwards, from where the
 * iterator is. Either way every byte of the item still passes through the CRC-32. */
bartap_err_t bartap_text_seek(bartap_text_t *text, const bartap_loc_t *loc, bartap_seek_t *how);

#endif
