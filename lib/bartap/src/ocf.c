// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* OCF layer (ocf.h, internal): the EPUB's zip container.
 *
 * Record layouts are APPNOTE.TXT's (4.3.7 local header, 4.3.12 central directory header, 4.3.14-4.3.16
 * the ZIP64 and end of central directory records, 4.5.3 the ZIP64 extra field). Every call, and every
 * open step, makes at most one byte-source read, before changing any state (D14). */

#include "ocf.h"

#include <string.h>

#include "crc.h"

#define SIGNATURE_LOCAL     0x04034B50UL
#define SIGNATURE_CENTRAL   0x02014B50UL
#define SIGNATURE_END       0x06054B50UL
#define SIGNATURE_ZIP64_END 0x06064B50UL
#define SIGNATURE_LOCATOR   0x07064B50UL

#define LOCAL_SIZE 30U
#define CENTRAL_SIZE 46U
#define END_SIZE 22U
#define END_COMMENT_MAX 65535U
#define LOCATOR_SIZE 20U
#define ZIP64_END_SIZE 56U
#define ZIP64_EXTRA_ID 0x0001U

#define FLAG_ENCRYPTED 0x0001U
#define FLAG_UTF8 0x0800U

#define EXTRA_READ_MAX 32U          //an extra record's header and the largest ZIP64 field (28 bytes)
#define FIND_PIECE 64U              //find compares names this many bytes per step, on the stack

//which values of a directory entry are 0xFFFFFFFF (0xFFFF for the disk) and so in its ZIP64 field
enum {
    UNRESOLVED_UNCOMPRESSED = 1U << 0,
    UNRESOLVED_COMPRESSED = 1U << 1,
    UNRESOLVED_OFFSET = 1U << 2,
    UNRESOLVED_DISK = 1U << 3,
    UNRESOLVED_STARTED = 1U << 7,   //the header has been read; reading the extra field
};

enum {
    OPEN_TAIL,                      //the last 22 bytes: an end record with no comment
    OPEN_SCAN,                      //the backward scan, a chunk per step
    OPEN_CONFIRM,                   //a signature whose record the chunk did not hold
    OPEN_LOCATOR,                   //the ZIP64 locator, if one is just before the end record
    OPEN_ZIP64,
    OPEN_WALK,                      //the directory walk, an entry per step
    OPEN_WALK_NAME,                 //the name of an entry that needs checking
    OPEN_MIMETYPE_LOCAL,            //mimetype's local header
    OPEN_MIMETYPE_CONTENT,
};

enum {
    TARGET_NONE,
    TARGET_MIMETYPE,
    TARGET_CONTAINER,
};

static const char mimetype_name[] = "mimetype";
static const char container_name[] = "META-INF/container.xml";
static const char mimetype_content[] = "application/epub+zip";
static const uint8_t byte_order_mark[3] = { 0xEF, 0xBB, 0xBF };

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

//an 8-byte ZIP64 value, as two halves (D13): false if it needs more than 32 bits
static bool get64_fits(const uint8_t *p, bartap_offset_t *value)
{
    *value = get32(p);
    return get32(p + 4) == 0U;
}

//a + b, false on overflow
static bool add(bartap_offset_t a, bartap_offset_t b, bartap_offset_t *sum)
{
    if (b > UINT32_MAX - a) {
        return false;
    }
    *sum = a + b;
    return true;
}

static bartap_offset_t min_offset(bartap_offset_t a, size_t b)
{
    return b < (size_t)a ? (bartap_offset_t)b : a;
}

//read exactly size bytes: a short read means the archive is shorter than it says
static bartap_ocf_result_t read_exact(const bartap_ocf_archive_t *archive, bartap_offset_t offset, void *buffer, size_t size)
{
    size_t got;
    if (bartap_byte_source_read(archive->source, offset, buffer, size, &got) != BARTAP_BYTE_SOURCE_RESULT_OK) {
        return BARTAP_OCF_RESULT_IO_ERROR;
    }
    return got == size ? BARTAP_OCF_RESULT_OK : BARTAP_OCF_RESULT_CORRUPT;
}

static bartap_offset_t directory_end(const bartap_ocf_archive_t *archive)
{
    return archive->directory_offset + archive->directory_size;     //checked at open not to overflow
}

/* ------------------------------------------------------------ directory entries */

static void cursor_reset(const bartap_ocf_archive_t *archive, bartap_ocf_cursor_t *cursor)
{
    memset(cursor, 0, sizeof *cursor);
    cursor->offset = archive->directory_offset;
    cursor->status = BARTAP_OCF_RESULT_OK;
}

static void cursor_complete(bartap_ocf_cursor_t *cursor, bartap_ocf_entry_t *entry, bool *ready)
{
    *entry = cursor->pending;
    cursor->offset = cursor->next_offset;
    cursor->index++;
    cursor->unresolved = 0;
    *ready = true;
}

//read the directory entry header at the cursor
static bartap_ocf_result_t entry_header(const bartap_ocf_archive_t *archive, bartap_ocf_cursor_t *cursor,
    bartap_ocf_entry_t *entry, bool *ready)
{
    bartap_offset_t header_end;
    if (!add(cursor->offset, CENTRAL_SIZE, &header_end) || header_end > directory_end(archive)) {
        return BARTAP_OCF_RESULT_CORRUPT;   //the directory ends before the entry count says
    }
    uint8_t h[CENTRAL_SIZE];
    const bartap_ocf_result_t result = read_exact(archive, cursor->offset, h, sizeof h);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    if (get32(h) != SIGNATURE_CENTRAL) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    const uint16_t name_length = get16(h + 28);
    const uint16_t extra_length = get16(h + 30);
    const uint16_t comment_length = get16(h + 32);
    const uint16_t disk = get16(h + 34);
    bartap_offset_t entry_end;
    if (!add(header_end, (bartap_offset_t)name_length + extra_length + comment_length, &entry_end)
            || entry_end > directory_end(archive)) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    bartap_ocf_entry_t *e = &cursor->pending;
    e->name_offset = header_end;
    e->name_length = name_length;
    e->index = cursor->index;
    e->flags = get16(h + 8);
    e->method = get16(h + 10);
    e->crc32 = get32(h + 16);
    e->compressed_size = get32(h + 20);
    e->uncompressed_size = get32(h + 24);
    e->local_header_offset = get32(h + 42);
    cursor->next_offset = entry_end;

    uint8_t unresolved = 0;
    if (e->uncompressed_size == UINT32_MAX) {
        unresolved |= UNRESOLVED_UNCOMPRESSED;
    }
    if (e->compressed_size == UINT32_MAX) {
        unresolved |= UNRESOLVED_COMPRESSED;
    }
    if (e->local_header_offset == UINT32_MAX) {
        unresolved |= UNRESOLVED_OFFSET;
    }
    if (disk == 0xFFFFU) {
        unresolved |= UNRESOLVED_DISK;
    } else if (disk != 0U) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;   //split archive
    }
    if (unresolved == 0U) {
        cursor_complete(cursor, entry, ready);
        return BARTAP_OCF_RESULT_OK;
    }
    cursor->unresolved = (uint8_t)(unresolved | UNRESOLVED_STARTED);
    cursor->extra_position = header_end + name_length;
    cursor->extra_end = cursor->extra_position + extra_length;
    return BARTAP_OCF_RESULT_OK;
}

//read one record of the pending entry's extra field, looking for its ZIP64 field
static bartap_ocf_result_t entry_extra(const bartap_ocf_archive_t *archive, bartap_ocf_cursor_t *cursor,
    bartap_ocf_entry_t *entry, bool *ready)
{
    const bartap_offset_t left = cursor->extra_end - cursor->extra_position;
    if (left < 4U) {
        return BARTAP_OCF_RESULT_CORRUPT;   //a 0xFFFFFFFF value with no ZIP64 field to resolve it
    }
    uint8_t x[EXTRA_READ_MAX];
    const size_t length = left < EXTRA_READ_MAX ? left : EXTRA_READ_MAX;
    const bartap_ocf_result_t result = read_exact(archive, cursor->extra_position, x, length);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    const uint16_t id = get16(x);
    const uint16_t size = get16(x + 2);
    if ((bartap_offset_t)size > left - 4U) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    if (id != ZIP64_EXTRA_ID) {
        cursor->extra_position += 4U + size;
        return BARTAP_OCF_RESULT_OK;
    }
    //the field holds only the values that were 0xFFFFFFFF, in this order (APPNOTE.TXT 4.5.3)
    const uint8_t u = cursor->unresolved;
    const unsigned needed = ((u & UNRESOLVED_UNCOMPRESSED) ? 8U : 0U) + ((u & UNRESOLVED_COMPRESSED) ? 8U : 0U)
        + ((u & UNRESOLVED_OFFSET) ? 8U : 0U) + ((u & UNRESOLVED_DISK) ? 4U : 0U);
    if (size < needed) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    const uint8_t *p = x + 4;
    bartap_ocf_entry_t *e = &cursor->pending;
    if ((u & UNRESOLVED_UNCOMPRESSED) && !get64_fits(p, &e->uncompressed_size)) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;
    }
    p += (u & UNRESOLVED_UNCOMPRESSED) ? 8 : 0;
    if ((u & UNRESOLVED_COMPRESSED) && !get64_fits(p, &e->compressed_size)) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;
    }
    p += (u & UNRESOLVED_COMPRESSED) ? 8 : 0;
    if ((u & UNRESOLVED_OFFSET) && !get64_fits(p, &e->local_header_offset)) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;
    }
    p += (u & UNRESOLVED_OFFSET) ? 8 : 0;
    if ((u & UNRESOLVED_DISK) && get32(p) != 0U) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;
    }
    cursor_complete(cursor, entry, ready);
    return BARTAP_OCF_RESULT_OK;
}

//one step of reading the next entry: one read, or none at the end
static bartap_ocf_result_t entry_step(const bartap_ocf_archive_t *archive, bartap_ocf_cursor_t *cursor,
    bartap_ocf_entry_t *entry, bool *ready, bool *end)
{
    *ready = false;
    *end = false;
    if (cursor->status != BARTAP_OCF_RESULT_OK) {
        return cursor->status;
    }
    bartap_ocf_result_t result;
    if (cursor->unresolved != 0U) {
        result = entry_extra(archive, cursor, entry, ready);
    } else if (cursor->index == archive->entry_count) {
        *end = true;
        return BARTAP_OCF_RESULT_OK;
    } else {
        result = entry_header(archive, cursor, entry, ready);
    }
    cursor->status = result;
    return result;
}

/* ------------------------------------------------------------ opening */

//a UTF-8 validator fed a byte at a time (RFC 3629): no overlongs, surrogates or values past U+10FFFF
static void utf8_feed(bartap_ocf_opener_t *ocf, uint8_t b)
{
    if (ocf->utf8_continuations > 0U) {
        if (b < ocf->utf8_low || b > ocf->utf8_high) {
            ocf->utf8_invalid = true;
        }
        ocf->utf8_continuations--;
        ocf->utf8_low = 0x80;
        ocf->utf8_high = 0xBF;
        return;
    }
    ocf->utf8_low = 0x80;
    ocf->utf8_high = 0xBF;
    if (b < 0x80U) {
        return;
    } else if (b >= 0xC2U && b <= 0xDFU) {
        ocf->utf8_continuations = 1;
    } else if (b >= 0xE0U && b <= 0xEFU) {
        ocf->utf8_continuations = 2;
        if (b == 0xE0U) {
            ocf->utf8_low = 0xA0;           //no overlongs
        } else if (b == 0xEDU) {
            ocf->utf8_high = 0x9F;          //no surrogates
        }
    } else if (b >= 0xF0U && b <= 0xF4U) {
        ocf->utf8_continuations = 3;
        if (b == 0xF0U) {
            ocf->utf8_low = 0x90;
        } else if (b == 0xF4U) {
            ocf->utf8_high = 0x8F;          //nothing past U+10FFFF
        }
    } else {
        ocf->utf8_invalid = true;
    }
}

//take the end of central directory record's values, from the 22 bytes at ocf->end_record
static void take_end_record(bartap_ocf_opener_t *ocf, const uint8_t *r)
{
    const uint16_t disk = get16(r + 4);
    const uint16_t directory_disk = get16(r + 6);
    const uint16_t entries_on_disk = get16(r + 8);
    const uint16_t entries = get16(r + 10);
    ocf->archive->directory_size = get32(r + 12);
    ocf->archive->directory_offset = get32(r + 16);
    ocf->archive->entry_count = entries;
    ocf->end_record_escaped = disk == 0xFFFFU || directory_disk == 0xFFFFU || entries_on_disk == 0xFFFFU
        || entries == 0xFFFFU || ocf->archive->directory_size == UINT32_MAX || ocf->archive->directory_offset == UINT32_MAX;
    ocf->split = disk != 0U || directory_disk != 0U || entries_on_disk != entries;
    ocf->stage = OPEN_LOCATOR;
}

//check the directory lies before limit (the end record, or the ZIP64 record), then start the walk
static bartap_ocf_result_t start_walk(bartap_ocf_opener_t *ocf, bartap_offset_t limit)
{
    bartap_offset_t end;
    if (!add(ocf->archive->directory_offset, ocf->archive->directory_size, &end) || end > limit
            || ocf->archive->entry_count > ocf->archive->directory_size / CENTRAL_SIZE) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    cursor_reset(ocf->archive, &ocf->walk);
    ocf->stage = OPEN_WALK;
    return BARTAP_OCF_RESULT_OK;
}

static bartap_ocf_result_t open_tail(bartap_ocf_opener_t *ocf)
{
    const bartap_offset_t size = ocf->archive->source->size;
    if (size < END_SIZE) {
        return BARTAP_OCF_RESULT_NOT_A_ZIP;
    }
    const bartap_ocf_result_t result = read_exact(ocf->archive, size - END_SIZE, ocf->scratch, END_SIZE);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    if (get32(ocf->scratch) == SIGNATURE_END && get16(ocf->scratch + 20) == 0U) {
        ocf->end_record = size - END_SIZE;
        take_end_record(ocf, ocf->scratch);
        return BARTAP_OCF_RESULT_OK;
    }
    //no luck: scan back over the longest comment there could be
    ocf->scan_end = size;
    ocf->scan_limit = size > END_SIZE + END_COMMENT_MAX ? size - (END_SIZE + END_COMMENT_MAX) : 0U;
    ocf->stage = OPEN_SCAN;
    return BARTAP_OCF_RESULT_OK;
}

static bartap_ocf_result_t open_scan(bartap_ocf_opener_t *ocf)
{
    const bartap_offset_t size = ocf->archive->source->size;
    const bartap_offset_t length = min_offset(ocf->scan_end - ocf->scan_limit, ocf->scratch_size);
    const bartap_offset_t start = ocf->scan_end - length;
    if (length < 4U) {
        return BARTAP_OCF_RESULT_NOT_A_ZIP;
    }
    const bartap_ocf_result_t result = read_exact(ocf->archive, start, ocf->scratch, length);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    //nearest the end first: the record is the last valid one. A signature is confirmed from its record,
    //read in the next step (D15), whether or not this chunk holds it
    for (bartap_offset_t i = length - 4U + 1U; i-- > 0U;) {
        const bartap_offset_t p = start + i;
        if (get32(ocf->scratch + i) == SIGNATURE_END && size - p >= END_SIZE) {
            ocf->candidate = p;
            ocf->stage = OPEN_CONFIRM;
            return BARTAP_OCF_RESULT_OK;
        }
    }
    if (start == ocf->scan_limit) {
        return BARTAP_OCF_RESULT_NOT_A_ZIP;
    }
    ocf->scan_end = start + 3U;     //overlap by 3, so no signature straddles two chunks
    return BARTAP_OCF_RESULT_OK;
}

static bartap_ocf_result_t open_confirm(bartap_ocf_opener_t *ocf)
{
    const bartap_offset_t p = ocf->candidate;
    const bartap_ocf_result_t result = read_exact(ocf->archive, p, ocf->scratch, END_SIZE);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    if (get32(ocf->scratch) == SIGNATURE_END && get16(ocf->scratch + 20) == ocf->archive->source->size - p - END_SIZE) {
        ocf->end_record = p;
        take_end_record(ocf, ocf->scratch);
        return BARTAP_OCF_RESULT_OK;
    }
    ocf->scan_end = p + 3U;         //a signature inside the comment: go on below it
    ocf->stage = OPEN_SCAN;
    return BARTAP_OCF_RESULT_OK;
}

//the end record's own values, when there is no ZIP64 locator
static bartap_ocf_result_t no_zip64(bartap_ocf_opener_t *ocf)
{
    if (ocf->end_record_escaped) {
        return BARTAP_OCF_RESULT_CORRUPT;   //"see ZIP64", with no ZIP64 record
    }
    if (ocf->split) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;
    }
    return start_walk(ocf, ocf->end_record);
}

static bartap_ocf_result_t open_locator(bartap_ocf_opener_t *ocf)
{
    if (ocf->end_record < LOCATOR_SIZE) {
        return no_zip64(ocf);
    }
    const bartap_offset_t at = ocf->end_record - LOCATOR_SIZE;
    const bartap_ocf_result_t result = read_exact(ocf->archive, at, ocf->scratch, LOCATOR_SIZE);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    const uint8_t *l = ocf->scratch;
    if (get32(l) != SIGNATURE_LOCATOR) {
        return no_zip64(ocf);
    }
    if (!get64_fits(l + 8, &ocf->zip64_record)) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;
    }
    if (get32(l + 4) != 0U || get32(l + 16) > 1U) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;   //on another disk, or more than one disk
    }
    bartap_offset_t record_end;
    if (!add(ocf->zip64_record, ZIP64_END_SIZE, &record_end) || record_end > at) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    ocf->stage = OPEN_ZIP64;
    return BARTAP_OCF_RESULT_OK;
}

static bartap_ocf_result_t open_zip64(bartap_ocf_opener_t *ocf)
{
    const bartap_ocf_result_t result = read_exact(ocf->archive, ocf->zip64_record, ocf->scratch, ZIP64_END_SIZE);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    const uint8_t *r = ocf->scratch;
    if (get32(r) != SIGNATURE_ZIP64_END) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    bartap_offset_t entries_on_disk;
    bartap_offset_t entries;
    if (!get64_fits(r + 24, &entries_on_disk) || !get64_fits(r + 32, &entries)
            || !get64_fits(r + 40, &ocf->archive->directory_size) || !get64_fits(r + 48, &ocf->archive->directory_offset)) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;
    }
    if (get32(r + 16) != 0U || get32(r + 20) != 0U || entries_on_disk != entries) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;
    }
    ocf->archive->entry_count = entries;
    return start_walk(ocf, ocf->zip64_record);
}

static bartap_ocf_result_t open_walk(bartap_ocf_opener_t *ocf)
{
    bool ready;
    bool end;
    const bartap_ocf_result_t result = entry_step(ocf->archive, &ocf->walk, &ocf->walk_entry, &ready, &end);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    if (end) {
        //everything after the walk is checks on what it found; the next step reads mimetype if needed
        if (ocf->container_count == 0U) {
            return BARTAP_OCF_RESULT_NOT_AN_EPUB;
        }
        if (ocf->mimetype_count == 0U) {
            ocf->archive->warnings |= BARTAP_OCF_WARNING_MIMETYPE_MISSING;
            ocf->done = true;
            return BARTAP_OCF_RESULT_OK;
        }
        const bartap_ocf_entry_t *m = &ocf->mimetype;
        if (m->flags & FLAG_ENCRYPTED) {
            return BARTAP_OCF_RESULT_UNSUPPORTED;   //no zip encryption (D10), mimetype included
        }
        if (m->method == BARTAP_OCF_METHOD_STORED && m->compressed_size != m->uncompressed_size) {
            return BARTAP_OCF_RESULT_CORRUPT;       //D22
        }
        ocf->stage = OPEN_MIMETYPE_LOCAL;
        return BARTAP_OCF_RESULT_OK;
    }
    if (!ready) {
        return BARTAP_OCF_RESULT_OK;
    }
    const bartap_ocf_entry_t *e = &ocf->walk_entry;
    ocf->name_target = e->name_length == sizeof mimetype_name - 1U ? TARGET_MIMETYPE
        : e->name_length == sizeof container_name - 1U ? TARGET_CONTAINER : TARGET_NONE;
    if (ocf->name_target != TARGET_NONE || ((e->flags & FLAG_UTF8) && e->name_length > 0U)) {
        ocf->name_read = 0;
        ocf->utf8_continuations = 0;
        ocf->utf8_invalid = false;
        ocf->stage = OPEN_WALK_NAME;
    }
    return BARTAP_OCF_RESULT_OK;
}

static bartap_ocf_result_t open_walk_name(bartap_ocf_opener_t *ocf)
{
    const bartap_ocf_entry_t *e = &ocf->walk_entry;
    const bartap_offset_t length = min_offset(e->name_length - ocf->name_read, ocf->scratch_size);
    const bartap_ocf_result_t result = read_exact(ocf->archive, e->name_offset + ocf->name_read, ocf->scratch, length);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    const char *target = ocf->name_target == TARGET_MIMETYPE ? mimetype_name
        : ocf->name_target == TARGET_CONTAINER ? container_name : NULL;
    if (target != NULL && memcmp(ocf->scratch, target + ocf->name_read, length) != 0) {
        ocf->name_target = TARGET_NONE;
    }
    if (e->flags & FLAG_UTF8) {
        for (bartap_offset_t i = 0; i < length; i++) {
            utf8_feed(ocf, ocf->scratch[i]);
        }
    }
    ocf->name_read += length;
    if (ocf->name_read < e->name_length) {
        return BARTAP_OCF_RESULT_OK;
    }
    if ((e->flags & FLAG_UTF8) && (ocf->utf8_invalid || ocf->utf8_continuations > 0U)) {
        ocf->archive->warnings |= BARTAP_OCF_WARNING_NAME_NOT_UTF8;
    }
    if (ocf->name_target == TARGET_MIMETYPE) {
        if (++ocf->mimetype_count > 1U) {
            return BARTAP_OCF_RESULT_CORRUPT;       //D21
        }
        if (e->index != 0U) {
            ocf->archive->warnings |= BARTAP_OCF_WARNING_MIMETYPE_NOT_FIRST;
        }
        ocf->mimetype = *e;
    } else if (ocf->name_target == TARGET_CONTAINER) {
        if (++ocf->container_count > 1U) {
            return BARTAP_OCF_RESULT_CORRUPT;
        }
        ocf->container = *e;
    }
    ocf->stage = OPEN_WALK;
    return BARTAP_OCF_RESULT_OK;
}

static bartap_ocf_result_t open_mimetype_local(bartap_ocf_opener_t *ocf)
{
    const bartap_ocf_entry_t *m = &ocf->mimetype;
    bartap_offset_t header_end;
    if (!add(m->local_header_offset, LOCAL_SIZE, &header_end) || header_end > ocf->archive->directory_offset) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    uint8_t h[LOCAL_SIZE];
    const bartap_ocf_result_t result = read_exact(ocf->archive, m->local_header_offset, h, sizeof h);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    if (get32(h) != SIGNATURE_LOCAL || get16(h + 8) != m->method || get16(h + 26) != m->name_length) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    const uint16_t extra_length = get16(h + 28);
    if (extra_length != 0U) {
        ocf->archive->warnings |= BARTAP_OCF_WARNING_MIMETYPE_EXTRA_FIELD;
    }
    bartap_offset_t data_end;
    ocf->mimetype_data = header_end + m->name_length + extra_length;
    if (ocf->mimetype_data < header_end || !add(ocf->mimetype_data, m->compressed_size, &data_end)
            || data_end > ocf->archive->directory_offset) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    if (m->method != BARTAP_OCF_METHOD_STORED) {
        ocf->archive->warnings |= BARTAP_OCF_WARNING_MIMETYPE_COMPRESSED;    //its content is not checked (D19)
        ocf->done = true;
        return BARTAP_OCF_RESULT_OK;
    }
    if (m->uncompressed_size == 0U) {
        ocf->archive->warnings |= BARTAP_OCF_WARNING_MIMETYPE_MISSING;       //empty counts as missing
        ocf->done = true;
        return BARTAP_OCF_RESULT_OK;
    }
    ocf->mimetype_read = 0;
    ocf->mimetype_bom = 0;
    ocf->mimetype_text = 0;
    ocf->stage = OPEN_MIMETYPE_CONTENT;
    return BARTAP_OCF_RESULT_OK;
}

//accept an optional byte order mark, "application/epub+zip", then only ASCII whitespace (D19)
static bool mimetype_byte(bartap_ocf_opener_t *ocf, uint8_t b)
{
    const uint8_t text_length = (uint8_t)(sizeof mimetype_content - 1U);
    if (ocf->mimetype_text == text_length) {
        return b == ' ' || b == '\t' || b == '\r' || b == '\n';
    }
    if (ocf->mimetype_text == 0U && ocf->mimetype_bom < 3U && ocf->mimetype_read == ocf->mimetype_bom
            && b == byte_order_mark[ocf->mimetype_bom]) {
        ocf->mimetype_bom++;
        return true;
    }
    if ((ocf->mimetype_bom == 0U || ocf->mimetype_bom == 3U) && b == (uint8_t)mimetype_content[ocf->mimetype_text]) {
        ocf->mimetype_text++;
        return true;
    }
    return false;
}

static bartap_ocf_result_t open_mimetype_content(bartap_ocf_opener_t *ocf)
{
    const bartap_ocf_entry_t *m = &ocf->mimetype;
    const bartap_offset_t length = min_offset(m->uncompressed_size - ocf->mimetype_read, ocf->scratch_size);
    const bartap_ocf_result_t result = read_exact(ocf->archive, ocf->mimetype_data + ocf->mimetype_read, ocf->scratch,
        length);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    for (bartap_offset_t i = 0; i < length; i++) {
        if (!mimetype_byte(ocf, ocf->scratch[i])) {
            return BARTAP_OCF_RESULT_NOT_AN_EPUB;
        }
        ocf->mimetype_read++;
    }
    if (ocf->mimetype_read < m->uncompressed_size) {
        return BARTAP_OCF_RESULT_OK;
    }
    if (ocf->mimetype_text < sizeof mimetype_content - 1U) {
        return BARTAP_OCF_RESULT_NOT_AN_EPUB;       //cut short
    }
    ocf->done = true;
    return BARTAP_OCF_RESULT_OK;
}

bartap_ocf_result_t bartap_ocf_open_begin(bartap_ocf_opener_t *ocf, bartap_ocf_archive_t *archive,
    const bartap_byte_source_t *source, uint8_t *scratch, size_t scratch_size)
{
    if (ocf == NULL) {
        return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    }
    memset(ocf, 0, sizeof *ocf);
    if (archive == NULL || source == NULL || source->read == NULL || scratch == NULL
            || scratch_size < BARTAP_OCF_OPEN_SCRATCH_MIN) {
        ocf->status = BARTAP_OCF_RESULT_INVALID_ARGUMENT;   //and no scratch pointer kept
        return ocf->status;
    }
    memset(archive, 0, sizeof *archive);
    ocf->archive = archive;
    archive->source = source;
    ocf->scratch = scratch;
    ocf->scratch_size = scratch_size;
    ocf->stage = OPEN_TAIL;
    ocf->status = BARTAP_OCF_RESULT_OK;
    return BARTAP_OCF_RESULT_OK;
}

bartap_ocf_result_t bartap_ocf_open_step(bartap_ocf_opener_t *ocf, bool *done)
{
    if (ocf == NULL || done == NULL) {
        return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    }
    *done = ocf->done;
    if (ocf->status != BARTAP_OCF_RESULT_OK || ocf->done) {
        return ocf->status;
    }
    bartap_ocf_result_t result;
    switch (ocf->stage) {
    case OPEN_TAIL: result = open_tail(ocf); break;
    case OPEN_SCAN: result = open_scan(ocf); break;
    case OPEN_CONFIRM: result = open_confirm(ocf); break;
    case OPEN_LOCATOR: result = open_locator(ocf); break;
    case OPEN_ZIP64: result = open_zip64(ocf); break;
    case OPEN_WALK: result = open_walk(ocf); break;
    case OPEN_WALK_NAME: result = open_walk_name(ocf); break;
    case OPEN_MIMETYPE_LOCAL: result = open_mimetype_local(ocf); break;
    case OPEN_MIMETYPE_CONTENT: result = open_mimetype_content(ocf); break;
    default: result = BARTAP_OCF_RESULT_INVALID_ARGUMENT; break;
    }
    ocf->status = result;
    if (result != BARTAP_OCF_RESULT_OK || ocf->done) {
        //finished either way: the caller may reuse the scratch memory at once (D15)
        ocf->scratch = NULL;
        ocf->scratch_size = 0;
        ocf->done = result == BARTAP_OCF_RESULT_OK;
        ocf->archive->open = ocf->done;
    }
    *done = ocf->done;
    return result;
}

bartap_ocf_result_t bartap_ocf_open(bartap_ocf_opener_t *ocf, bartap_ocf_archive_t *archive,
    const bartap_byte_source_t *source, uint8_t *scratch, size_t scratch_size)
{
    bartap_ocf_result_t result = bartap_ocf_open_begin(ocf, archive, source, scratch, scratch_size);
    bool done = false;
    while (result == BARTAP_OCF_RESULT_OK && !done) {
        result = bartap_ocf_open_step(ocf, &done);
    }
    return result;
}

/* ------------------------------------------------------------ directory */

void bartap_ocf_cursor_start(const bartap_ocf_archive_t *archive, bartap_ocf_cursor_t *cursor)
{
    if (archive == NULL || cursor == NULL) {
        return;
    }
    cursor_reset(archive, cursor);
    if (!archive->open) {
        cursor->status = BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    }
}

bartap_ocf_result_t bartap_ocf_next(
    const bartap_ocf_archive_t *archive,
    bartap_ocf_cursor_t *cursor,
    bartap_ocf_entry_t *entry,
    bool *ready,
    bool *end)
{
    if (archive == NULL || cursor == NULL || entry == NULL || ready == NULL || end == NULL || !archive->open) {
        return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    }
    return entry_step(archive, cursor, entry, ready, end);
}

void bartap_ocf_cursor_at(const bartap_ocf_archive_t *archive, bartap_ocf_cursor_t *cursor,
    bartap_offset_t offset, uint32_t index)
{
    if (archive == NULL || cursor == NULL) {
        return;
    }
    cursor_reset(archive, cursor);
    cursor->offset = offset;
    cursor->index = index;
    if (!archive->open) {
        cursor->status = BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    } else if (index >= archive->entry_count || offset < archive->directory_offset) {
        cursor->status = BARTAP_OCF_RESULT_CORRUPT;    //a remembered place that is not in this directory
    }
}

void bartap_ocf_find_begin(bartap_ocf_finder_t *finder, const bartap_ocf_archive_t *archive, const uint8_t *name,
    size_t name_length)
{
    if (finder == NULL) {
        return;
    }
    memset(finder, 0, sizeof *finder);
    finder->archive = archive;
    finder->name = name;
    if (archive == NULL || !archive->open || (name == NULL && name_length != 0U)) {
        finder->status = BARTAP_OCF_RESULT_INVALID_ARGUMENT;
        return;
    }
    if (name_length > UINT16_MAX) {
        finder->status = BARTAP_OCF_RESULT_NOT_FOUND;   //no zip name is this long
        return;
    }
    finder->name_length = (uint16_t)name_length;
    cursor_reset(archive, &finder->cursor);
    finder->status = BARTAP_OCF_RESULT_OK;
}

bartap_ocf_result_t bartap_ocf_find_step(bartap_ocf_finder_t *finder, bartap_ocf_entry_t *entry, bool *done)
{
    if (finder == NULL || entry == NULL || done == NULL) {
        return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    }
    *done = finder->done;
    if (finder->status != BARTAP_OCF_RESULT_OK || finder->done) {
        if (finder->done) {
            *entry = finder->entry;
        }
        return finder->status;
    }
    bartap_ocf_result_t result = BARTAP_OCF_RESULT_OK;
    if (!finder->comparing) {
        bool ready;
        bool end;
        result = entry_step(finder->archive, &finder->cursor, &finder->entry, &ready, &end);
        if (result == BARTAP_OCF_RESULT_OK && end) {
            result = BARTAP_OCF_RESULT_NOT_FOUND;
        } else if (result == BARTAP_OCF_RESULT_OK && ready && finder->entry.name_length == finder->name_length) {
            //lengths first: most names are never read
            finder->compared = 0;
            finder->comparing = true;
            finder->done = finder->name_length == 0U;
        }
    } else {
        uint8_t piece[FIND_PIECE];
        const size_t left = (size_t)finder->name_length - finder->compared;
        const size_t n = left < FIND_PIECE ? left : FIND_PIECE;
        result = read_exact(finder->archive, finder->entry.name_offset + finder->compared, piece, n);
        if (result == BARTAP_OCF_RESULT_OK) {
            if (memcmp(piece, finder->name + finder->compared, n) != 0) {
                finder->comparing = false;
            } else {
                finder->compared = (uint16_t)(finder->compared + n);
                finder->done = finder->compared == finder->name_length;     //the first match (D21)
            }
        }
    }
    finder->status = result;
    *done = finder->done;
    if (finder->done) {
        *entry = finder->entry;
    }
    return result;
}

bartap_ocf_result_t bartap_ocf_entry_name(
    const bartap_ocf_archive_t *archive,
    const bartap_ocf_entry_t *entry,
    uint8_t *buffer,
    size_t buffer_size)
{
    if (archive == NULL || entry == NULL || buffer == NULL || !archive->open) {
        return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    }
    if (buffer_size < (size_t)entry->name_length + 1U) {
        return BARTAP_OCF_RESULT_LIMIT_EXCEEDED;
    }
    const bartap_ocf_result_t result = read_exact(archive, entry->name_offset, buffer, entry->name_length);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    buffer[entry->name_length] = 0;
    return BARTAP_OCF_RESULT_OK;
}

/* ------------------------------------------------------------ entries */

static bartap_ocf_result_t entry_open(
    bartap_ocf_stream_t *stream,
    const bartap_ocf_archive_t *archive,
    const bartap_ocf_entry_t *entry,
    bartap_offset_t max_size,
    uint8_t *window,
    size_t window_size,
    uint8_t *input,
    size_t input_size)
{
    if (archive == NULL || entry == NULL || !archive->open) {
        return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    }
    //checks that need no read come first
    const bool stored = entry->method == BARTAP_OCF_METHOD_STORED;
    if (stored && entry->compressed_size != entry->uncompressed_size) {
        return BARTAP_OCF_RESULT_CORRUPT;           //D22
    }
    if ((entry->flags & FLAG_ENCRYPTED) || (!stored && entry->method != BARTAP_OCF_METHOD_DEFLATED)) {
        return BARTAP_OCF_RESULT_UNSUPPORTED;
    }
    if (entry->uncompressed_size > max_size) {
        return BARTAP_OCF_RESULT_LIMIT_EXCEEDED;
    }
    if (!stored) {
        if (input == NULL || input_size == 0U) {
            return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
        }
        if (bartap_inflate_init(&stream->inflate, window, window_size) != BARTAP_INFLATE_RESULT_OK) {
            return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
        }
    }
    bartap_offset_t header_end;
    if (!add(entry->local_header_offset, LOCAL_SIZE, &header_end) || header_end > archive->directory_offset) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    uint8_t h[LOCAL_SIZE];
    const bartap_ocf_result_t result = read_exact(archive, entry->local_header_offset, h, sizeof h);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    //the directory is the authority: the local header must agree on these, and its name bytes are not
    //compared (D17)
    if (get32(h) != SIGNATURE_LOCAL || get16(h + 8) != entry->method || get16(h + 26) != entry->name_length) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    bartap_offset_t data_end;
    stream->data_offset = header_end + entry->name_length + get16(h + 28);
    if (stream->data_offset < header_end || !add(stream->data_offset, entry->compressed_size, &data_end)
            || data_end > archive->directory_offset) {
        return BARTAP_OCF_RESULT_CORRUPT;           //the data runs into the directory
    }
    stream->archive = archive;
    stream->entry = *entry;
    stream->crc32 = UINT32_MAX;
    stream->input = stored ? NULL : input;
    stream->input_size = stored ? 0U : input_size;
    return BARTAP_OCF_RESULT_OK;
}

bartap_ocf_result_t bartap_ocf_entry_open(
    bartap_ocf_stream_t *stream,
    const bartap_ocf_archive_t *archive,
    const bartap_ocf_entry_t *entry,
    bartap_offset_t max_size,
    uint8_t *window,
    size_t window_size,
    uint8_t *input,
    size_t input_size)
{
    if (stream == NULL) {
        return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    }
    memset(stream, 0, sizeof *stream);
    stream->status = entry_open(stream, archive, entry, max_size, window, window_size, input, input_size);
    return stream->status;
}

//the read that reaches the end: check what was delivered against the directory
static bartap_ocf_result_t finish(bartap_ocf_stream_t *stream, bool *end)
{
    if (stream->uncompressed_produced != stream->entry.uncompressed_size
            || (stream->crc32 ^ UINT32_MAX) != stream->entry.crc32) {
        return BARTAP_OCF_RESULT_CORRUPT;
    }
    stream->ended = true;
    *end = true;
    return BARTAP_OCF_RESULT_OK;
}

static bartap_ocf_result_t read_stored(bartap_ocf_stream_t *stream, uint8_t *buffer, size_t size,
    size_t *bytes_read, bool *end)
{
    const bartap_offset_t left = stream->entry.uncompressed_size - stream->uncompressed_produced;
    if (left == 0U) {
        return finish(stream, end);
    }
    const bartap_offset_t length = min_offset(left, size);
    if (length == 0U) {
        return BARTAP_OCF_RESULT_OK;
    }
    //straight into the caller's buffer: stored data needs no input buffer (D16)
    const bartap_ocf_result_t result = read_exact(stream->archive, stream->data_offset + stream->uncompressed_produced,
        buffer, length);
    if (result != BARTAP_OCF_RESULT_OK) {
        return result;
    }
    stream->crc32 = bartap_crc32_update(stream->crc32, buffer, length);
    stream->uncompressed_produced += length;
    *bytes_read = length;
    return stream->uncompressed_produced == stream->entry.uncompressed_size ? finish(stream, end)
        : BARTAP_OCF_RESULT_OK;
}

static bartap_ocf_result_t read_deflated(bartap_ocf_stream_t *stream, uint8_t *buffer, size_t size,
    size_t *bytes_read, bool *end)
{
    //at most one refill, and only of an empty buffer (D12)
    if (stream->input_start == stream->input_end && stream->compressed_read < stream->entry.compressed_size) {
        const bartap_offset_t length = min_offset(stream->entry.compressed_size - stream->compressed_read,
            stream->input_size);
        const bartap_ocf_result_t result = read_exact(stream->archive, stream->data_offset + stream->compressed_read,
            stream->input, length);
        if (result != BARTAP_OCF_RESULT_OK) {
            return result;
        }
        stream->compressed_read += length;
        stream->input_start = 0;
        stream->input_end = length;
    }
    //never deliver past the declared size: once it is reached, a one-byte probe shows whether the
    //stream would go on (corrupt) or ends there
    const bartap_offset_t left = stream->entry.uncompressed_size - stream->uncompressed_produced;
    uint8_t probe;
    uint8_t *out = left == 0U ? &probe : buffer;
    const size_t out_size = left == 0U ? 1U : (size_t)min_offset(left, size);
    size_t used;
    size_t produced;
    const bartap_inflate_result_t inflated = bartap_inflate_run(&stream->inflate,
        stream->input + stream->input_start, stream->input_end - stream->input_start, &used,
        stream->compressed_read == stream->entry.compressed_size, out, out_size, &produced);
    stream->input_start += used;
    if (left == 0U && produced > 0U) {
        return BARTAP_OCF_RESULT_CORRUPT;           //decompresses to more than the directory says
    }
    if (left > 0U) {
        stream->crc32 = bartap_crc32_update(stream->crc32, buffer, produced);
        stream->uncompressed_produced += (bartap_offset_t)produced;
        *bytes_read = produced;
    }
    switch (inflated) {
    case BARTAP_INFLATE_RESULT_OK:
        return BARTAP_OCF_RESULT_OK;
    case BARTAP_INFLATE_RESULT_DONE:
        return finish(stream, end);
    case BARTAP_INFLATE_RESULT_INVALID_ARGUMENT:
        return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    default:
        return BARTAP_OCF_RESULT_CORRUPT;           //truncated, or not valid DEFLATE
    }
}

bartap_ocf_result_t bartap_ocf_entry_read(
    bartap_ocf_stream_t *stream,
    void *buffer,
    size_t size,
    size_t *bytes_read,
    bool *end)
{
    if (stream == NULL || bytes_read == NULL || end == NULL || (buffer == NULL && size != 0U)) {
        return BARTAP_OCF_RESULT_INVALID_ARGUMENT;
    }
    *bytes_read = 0;
    *end = stream->ended;
    if (stream->status != BARTAP_OCF_RESULT_OK || stream->ended) {
        return stream->status;                      //errors are sticky; after the end, OK and nothing (D22)
    }
    const bartap_ocf_result_t result = stream->entry.method == BARTAP_OCF_METHOD_STORED
        ? read_stored(stream, buffer, size, bytes_read, end)
        : read_deflated(stream, buffer, size, bytes_read, end);
    if (result != BARTAP_OCF_RESULT_OK) {
        *bytes_read = 0;
        *end = false;
    }
    stream->status = result;
    return result;
}
