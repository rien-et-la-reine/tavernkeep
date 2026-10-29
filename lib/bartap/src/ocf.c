// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The ZIP container (PKWARE APPNOTE): the end of central directory record, found by scanning back over
 * the last min(size, 65535 + 22) bytes so trailing padding is tolerated (BARTAP_WARN_TRAILING_BYTES);
 * then the central directory, walked to find entries by exact name. An entry's sizes and
 * CRC-32 come from its central directory record, never the local header or a data descriptor. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

#define BARTAP_ZIP_SIG_LFH      0x04034B50UL
#define BARTAP_ZIP_SIG_CDFH     0x02014B50UL
#define BARTAP_ZIP_SIG_EOCD     0x06054B50UL
#define BARTAP_ZIP_SIG_Z64_EOCD 0x06064B50UL
#define BARTAP_ZIP_SIG_Z64_LOC  0x07064B50UL

#define BARTAP_ZIP_EOCD_MIN     22U
#define BARTAP_ZIP_COMMENT_MAX  65535U
#define BARTAP_ZIP_CDFH_MIN     46U
#define BARTAP_ZIP_LFH_MIN      30U
#define BARTAP_ZIP_SCAN_CHUNK   512U

#define BARTAP_ZIP_METHOD_STORE   0U
#define BARTAP_ZIP_METHOD_DEFLATE 8U

static uint16_t bartap_read_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static uint32_t bartap_read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8U)
        | ((uint32_t)p[2] << 16U)
        | ((uint32_t)p[3] << 24U);
}

static uint64_t bartap_read_u64_le(const uint8_t *p)
{
    return (uint64_t)bartap_read_u32_le(p) | ((uint64_t)bartap_read_u32_le(p + 4) << 32U);
}

/* Validate a candidate EOCD at eocd_pos (with its 22-byte header in rec). Sets *matched = true if valid
 * or if it hits an explicit unsupported/corrupt condition inside a real EOCD. */
static bartap_err_t bartap_zip_try_eocd(const bartap_source_t *source, uint32_t eocd_pos,
    const uint8_t rec[BARTAP_ZIP_EOCD_MIN], bartap_zip_t *zip, bool *matched)
{
    *matched = false;
    const uint16_t disk_num = bartap_read_u16_le(rec + 4);
    const uint16_t cd_disk = bartap_read_u16_le(rec + 6);
    const uint16_t disk_entries = bartap_read_u16_le(rec + 8);
    const uint16_t total_entries = bartap_read_u16_le(rec + 10);
    const uint32_t cd_size32 = bartap_read_u32_le(rec + 12);
    const uint32_t cd_offset32 = bartap_read_u32_le(rec + 16);
    const uint16_t comment_len = bartap_read_u16_le(rec + 20);

    /* Comment cannot extend past the end of the source. */
    if (eocd_pos + BARTAP_ZIP_EOCD_MIN > source->size
            || comment_len > source->size - (eocd_pos + BARTAP_ZIP_EOCD_MIN)) {
        return BARTAP_OK;
    }

    uint64_t cd_offset = cd_offset32;
    uint64_t cd_size = cd_size32;
    uint64_t count = total_entries;
    uint32_t cd_limit = eocd_pos;

    /* Check for ZIP64 EOCD Locator (20 bytes immediately preceding EOCD) if any field is saturated
     * or if a locator is present. */
    const bool needs_zip64 = (disk_num == 0xFFFFU || cd_disk == 0xFFFFU
        || disk_entries == 0xFFFFU || total_entries == 0xFFFFU
        || cd_size32 == 0xFFFFFFFFUL || cd_offset32 == 0xFFFFFFFFUL);

    if (eocd_pos >= 20U) {
        uint8_t loc[20];
        const bartap_err_t err = bartap_source_read(source, eocd_pos - 20U, loc, sizeof loc);
        if (err != BARTAP_OK) {
            return err;
        }
        if (bartap_read_u32_le(loc) == BARTAP_ZIP_SIG_Z64_LOC) {
            const uint32_t z64_disk = bartap_read_u32_le(loc + 4);
            const uint64_t z64_offset = bartap_read_u64_le(loc + 8);
            const uint32_t total_disks = bartap_read_u32_le(loc + 16);
            if (z64_disk != 0U || total_disks > 1U) {
                *matched = true;
                return BARTAP_ERR_UNSUPPORTED;
            }
            if (z64_offset > eocd_pos - 20U || eocd_pos - 20U - (uint32_t)z64_offset < 56U) {
                *matched = true;
                return BARTAP_ERR_CORRUPT;
            }
            uint8_t z64[56];
            const bartap_err_t zerr = bartap_source_read(source, (uint32_t)z64_offset, z64, sizeof z64);
            if (zerr != BARTAP_OK) {
                return zerr;
            }
            if (bartap_read_u32_le(z64) != BARTAP_ZIP_SIG_Z64_EOCD) {
                *matched = true;
                return BARTAP_ERR_CORRUPT;
            }
            const uint32_t z64_this_disk = bartap_read_u32_le(z64 + 16);
            const uint32_t z64_cd_disk = bartap_read_u32_le(z64 + 20);
            const uint64_t z64_disk_entries = bartap_read_u64_le(z64 + 24);
            const uint64_t z64_total_entries = bartap_read_u64_le(z64 + 32);
            if (z64_this_disk != 0U || z64_cd_disk != 0U || z64_disk_entries != z64_total_entries) {
                *matched = true;
                return BARTAP_ERR_UNSUPPORTED;
            }
            count = z64_total_entries;
            cd_size = bartap_read_u64_le(z64 + 40);
            cd_offset = bartap_read_u64_le(z64 + 48);
            cd_limit = (uint32_t)z64_offset;
        } else if (needs_zip64) {
            *matched = true;
            return BARTAP_ERR_CORRUPT;
        }
    } else if (needs_zip64) {
        *matched = true;
        return BARTAP_ERR_CORRUPT;
    }

    if (!needs_zip64 && (disk_num != 0U || cd_disk != 0U || disk_entries != total_entries)) {
        *matched = true;
        return BARTAP_ERR_UNSUPPORTED;
    }

    if (count > UINT16_MAX || cd_size > UINT32_MAX || cd_offset > UINT32_MAX) {
        *matched = true;
        return BARTAP_ERR_UNSUPPORTED;
    }

    if (cd_offset > cd_limit || cd_size > cd_limit - (uint32_t)cd_offset) {
        return BARTAP_ERR_CORRUPT;
    }

    if (count > 0U) {
        if (cd_size < count * BARTAP_ZIP_CDFH_MIN) {
            return BARTAP_OK;
        }
        uint8_t sig[4];
        const bartap_err_t err = bartap_source_read(source, (uint32_t)cd_offset, sig, sizeof sig);
        if (err != BARTAP_OK) {
            return err;
        }
        if (bartap_read_u32_le(sig) != BARTAP_ZIP_SIG_CDFH) {
            return BARTAP_OK;
        }
    }

    *matched = true;
    zip->cd_offset = (uint32_t)cd_offset;
    zip->cd_size = (uint32_t)cd_size;
    zip->entry_count = (uint16_t)count;
    zip->warnings = 0U;
    if (eocd_pos + BARTAP_ZIP_EOCD_MIN + comment_len < source->size) {
        zip->warnings |= BARTAP_WARN_TRAILING_BYTES;
    }
    return BARTAP_OK;
}

/* Parse a single Central Directory entry at *pos, advance *pos to the next CD entry, and populate
 * *entry (except data_offset, which is resolved when the entry is matched). */
static bartap_err_t bartap_zip_read_cd_entry(const bartap_zip_t *zip, const bartap_source_t *source,
    uint32_t *pos, bartap_entry_t *entry, uint32_t *name_offset, uint16_t *name_len, uint16_t *flags_out)
{
    const uint32_t cd_end = zip->cd_offset + zip->cd_size;
    if (*pos > cd_end || cd_end - *pos < BARTAP_ZIP_CDFH_MIN) {
        return BARTAP_ERR_CORRUPT;
    }

    uint8_t hdr[BARTAP_ZIP_CDFH_MIN];
    bartap_err_t err = bartap_source_read(source, *pos, hdr, sizeof hdr);
    if (err != BARTAP_OK) {
        return err;
    }
    if (bartap_read_u32_le(hdr) != BARTAP_ZIP_SIG_CDFH) {
        return BARTAP_ERR_CORRUPT;
    }

    const uint16_t flags = bartap_read_u16_le(hdr + 8);
    const uint16_t method = bartap_read_u16_le(hdr + 10);
    const uint32_t crc32 = bartap_read_u32_le(hdr + 16);
    uint64_t comp_size = bartap_read_u32_le(hdr + 20);
    uint64_t uncomp_size = bartap_read_u32_le(hdr + 24);
    const uint16_t fn_len = bartap_read_u16_le(hdr + 28);
    const uint16_t extra_len = bartap_read_u16_le(hdr + 30);
    const uint16_t comment_len = bartap_read_u16_le(hdr + 32);
    uint16_t disk_start = bartap_read_u16_le(hdr + 34);
    uint64_t lfh_offset = bartap_read_u32_le(hdr + 42);

    const uint32_t var_len = (uint32_t)fn_len + (uint32_t)extra_len + (uint32_t)comment_len;
    if (cd_end - *pos - BARTAP_ZIP_CDFH_MIN < var_len) {
        return BARTAP_ERR_CORRUPT;
    }

    /* Parse ZIP64 extended information extra field (0x0001) if any field is saturated. */
    if (uncomp_size == 0xFFFFFFFFULL || comp_size == 0xFFFFFFFFULL
            || lfh_offset == 0xFFFFFFFFULL || disk_start == 0xFFFFU) {
        const uint32_t extra_start = *pos + BARTAP_ZIP_CDFH_MIN + fn_len;
        uint32_t cur = 0U;
        bool found_z64 = false;
        while (cur + 4U <= extra_len) {
            uint8_t eh[4];
            err = bartap_source_read(source, extra_start + cur, eh, sizeof eh);
            if (err != BARTAP_OK) {
                return err;
            }
            const uint16_t tag = bartap_read_u16_le(eh);
            const uint16_t sz = bartap_read_u16_le(eh + 2);
            cur += 4U;
            if (cur + sz > extra_len) {
                return BARTAP_ERR_CORRUPT;
            }
            if (tag == 0x0001U) {
                uint32_t zpos = 0U;
                uint8_t buf8[8];
                if (uncomp_size == 0xFFFFFFFFULL) {
                    if (zpos + 8U > sz) {
                        return BARTAP_ERR_CORRUPT;
                    }
                    err = bartap_source_read(source, extra_start + cur + zpos, buf8, 8U);
                    if (err != BARTAP_OK) {
                        return err;
                    }
                    uncomp_size = bartap_read_u64_le(buf8);
                    zpos += 8U;
                }
                if (comp_size == 0xFFFFFFFFULL) {
                    if (zpos + 8U > sz) {
                        return BARTAP_ERR_CORRUPT;
                    }
                    err = bartap_source_read(source, extra_start + cur + zpos, buf8, 8U);
                    if (err != BARTAP_OK) {
                        return err;
                    }
                    comp_size = bartap_read_u64_le(buf8);
                    zpos += 8U;
                }
                if (lfh_offset == 0xFFFFFFFFULL) {
                    if (zpos + 8U > sz) {
                        return BARTAP_ERR_CORRUPT;
                    }
                    err = bartap_source_read(source, extra_start + cur + zpos, buf8, 8U);
                    if (err != BARTAP_OK) {
                        return err;
                    }
                    lfh_offset = bartap_read_u64_le(buf8);
                    zpos += 8U;
                }
                if (disk_start == 0xFFFFU) {
                    if (zpos + 4U > sz) {
                        return BARTAP_ERR_CORRUPT;
                    }
                    err = bartap_source_read(source, extra_start + cur + zpos, buf8, 4U);
                    if (err != BARTAP_OK) {
                        return err;
                    }
                    if (bartap_read_u32_le(buf8) != 0U) {
                        return BARTAP_ERR_UNSUPPORTED;
                    }
                    disk_start = 0U;
                }
                found_z64 = true;
                break;
            }
            cur += sz;
        }
        if (!found_z64) {
            return BARTAP_ERR_CORRUPT;
        }
    }

    if (disk_start != 0U || uncomp_size > UINT32_MAX || comp_size > UINT32_MAX || lfh_offset > UINT32_MAX) {
        return BARTAP_ERR_UNSUPPORTED;
    }

    *name_offset = *pos + BARTAP_ZIP_CDFH_MIN;
    *name_len = fn_len;
    if (flags_out != NULL) {
        *flags_out = flags;
    }

    entry->header_offset = (uint32_t)lfh_offset;
    entry->data_offset = 0U;
    entry->compressed_size = (uint32_t)comp_size;
    entry->size = (uint32_t)uncomp_size;
    entry->crc32 = crc32;
    entry->method = method;

    *pos += BARTAP_ZIP_CDFH_MIN + var_len;
    return BARTAP_OK;
}

/* Resolve entry->data_offset by reading its Local File Header and checking bounds and method. */
bartap_err_t bartap_zip_resolve_local_header(const bartap_zip_t *zip, const bartap_source_t *source,
    uint16_t cd_flags, bartap_entry_t *entry, uint16_t *lfh_extra_len)
{
    /* Bit 0 of general purpose bit flag indicates encryption. */
    if ((cd_flags & 1U) != 0U) {
        return BARTAP_ERR_UNSUPPORTED;
    }
    if (entry->method != BARTAP_ZIP_METHOD_STORE && entry->method != BARTAP_ZIP_METHOD_DEFLATE) {
        return BARTAP_ERR_UNSUPPORTED;
    }
    if (entry->method == BARTAP_ZIP_METHOD_STORE && entry->compressed_size != entry->size) {
        return BARTAP_ERR_CORRUPT;
    }
    if (entry->header_offset > zip->cd_offset
            || zip->cd_offset - entry->header_offset < BARTAP_ZIP_LFH_MIN) {
        return BARTAP_ERR_CORRUPT;
    }

    uint8_t lfh[BARTAP_ZIP_LFH_MIN];
    const bartap_err_t err = bartap_source_read(source, entry->header_offset, lfh, sizeof lfh);
    if (err != BARTAP_OK) {
        return err;
    }
    if (bartap_read_u32_le(lfh) != BARTAP_ZIP_SIG_LFH) {
        return BARTAP_ERR_CORRUPT;
    }
    const uint16_t lfh_flags = bartap_read_u16_le(lfh + 6);
    if ((lfh_flags & 1U) != 0U) {
        return BARTAP_ERR_UNSUPPORTED;
    }
    const uint16_t fn_len = bartap_read_u16_le(lfh + 26);
    const uint16_t extra_len = bartap_read_u16_le(lfh + 28);
    if (lfh_extra_len != NULL) {
        *lfh_extra_len = extra_len;
    }

    const uint32_t hdr_total = BARTAP_ZIP_LFH_MIN + (uint32_t)fn_len + (uint32_t)extra_len;
    if (zip->cd_offset - entry->header_offset < hdr_total) {
        return BARTAP_ERR_CORRUPT;
    }
    const uint32_t data_offset = entry->header_offset + hdr_total;
    if (entry->compressed_size > zip->cd_offset - data_offset) {
        return BARTAP_ERR_CORRUPT;
    }

    entry->data_offset = data_offset;
    return BARTAP_OK;
}

/* Check the mimetype entry (if present) for OCF packaging warnings. */
static bartap_err_t bartap_zip_check_mimetype(bartap_zip_t *zip, const bartap_source_t *source)
{
    uint32_t pos = zip->cd_offset;
    uint32_t found_count = 0U;
    bartap_entry_t mime_entry;
    uint16_t mime_flags = 0U;

    for (uint16_t i = 0; i < zip->entry_count; i++) {
        bartap_entry_t cur;
        uint32_t name_off = 0U;
        uint16_t name_len = 0U;
        uint16_t flags = 0U;
        const bartap_err_t err = bartap_zip_read_cd_entry(zip, source, &pos, &cur, &name_off, &name_len,
            &flags);
        if (err != BARTAP_OK) {
            return err;
        }
        if (name_len == 8U) {
            char name[8];
            const bartap_err_t nerr = bartap_source_read(source, name_off, name, sizeof name);
            if (nerr != BARTAP_OK) {
                return nerr;
            }
            if (memcmp(name, "mimetype", 8U) == 0) {
                found_count++;
                if (found_count == 1U) {
                    mime_entry = cur;
                    mime_flags = flags;
                }
            }
        }
    }

    if (pos != zip->cd_offset + zip->cd_size) {
        return BARTAP_ERR_CORRUPT;
    }

    if (found_count == 0U) {
        zip->warnings |= BARTAP_WARN_MIMETYPE;
        return BARTAP_OK;
    }
    if (found_count > 1U) {
        zip->warnings |= BARTAP_WARN_MIMETYPE;
    }

    uint16_t extra_len = 0U;
    const bartap_err_t rerr = bartap_zip_resolve_local_header(zip, source, mime_flags, &mime_entry,
        &extra_len);
    if (rerr != BARTAP_OK) {
        return rerr;
    }

    if (mime_entry.header_offset != 0U || mime_entry.method != BARTAP_ZIP_METHOD_STORE || extra_len != 0U) {
        zip->warnings |= BARTAP_WARN_MIMETYPE;
    }

    if (mime_entry.method == BARTAP_ZIP_METHOD_STORE) {
        if (mime_entry.size == 0U) {
            zip->warnings |= BARTAP_WARN_MIMETYPE;
            return BARTAP_OK;
        }
        uint8_t buf[64];
        const size_t to_read = mime_entry.size < sizeof buf ? mime_entry.size : sizeof buf;
        const bartap_err_t derr = bartap_source_read(source, mime_entry.data_offset, buf, to_read);
        if (derr != BARTAP_OK) {
            return derr;
        }
        size_t start = 0U;
        if (to_read >= 3U && buf[0] == 0xEFU && buf[1] == 0xBBU && buf[2] == 0xBFU) {
            start = 3U;
        }
        size_t end = to_read;
        while (end > start && (buf[end - 1U] == ' ' || buf[end - 1U] == '\t'
                || buf[end - 1U] == '\r' || buf[end - 1U] == '\n')) {
            end--;
        }
        static const char expected[] = "application/epub+zip";
        const size_t expected_len = sizeof expected - 1U;
        if (end - start != expected_len || memcmp(buf + start, expected, expected_len) != 0) {
            zip->warnings |= BARTAP_WARN_MIMETYPE;
        }
    }

    return BARTAP_OK;
}

bartap_err_t bartap_zip_open(bartap_zip_t *zip, const bartap_source_t *source)
{
    if (zip == NULL || source == NULL || source->read == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (source->size < BARTAP_ZIP_EOCD_MIN) {
        return BARTAP_ERR_NOT_EPUB;
    }

    const uint32_t max_window = BARTAP_ZIP_COMMENT_MAX + BARTAP_ZIP_EOCD_MIN;
    const uint32_t search_len = source->size < max_window ? source->size : max_window;
    const uint32_t search_start = source->size - search_len;

    uint8_t buf[BARTAP_ZIP_SCAN_CHUNK];
    uint32_t cursor = source->size;
    bool found = false;
    bartap_err_t candidate_error = BARTAP_ERR_NOT_EPUB;

    while (cursor > search_start) {
        const uint32_t span = cursor - search_start;
        const uint32_t chunk_len = span < BARTAP_ZIP_SCAN_CHUNK ? span : BARTAP_ZIP_SCAN_CHUNK;
        const uint32_t chunk_off = cursor - chunk_len;

        bartap_err_t err = bartap_source_read(source, chunk_off, buf, chunk_len);
        if (err != BARTAP_OK) {
            return err;
        }

        if (chunk_len >= BARTAP_ZIP_EOCD_MIN) {
            uint32_t i = chunk_len - BARTAP_ZIP_EOCD_MIN + 1U;
            while (i > 0U) {
                i--;
                if (bartap_read_u32_le(buf + i) == BARTAP_ZIP_SIG_EOCD) {
                    bool matched = false;
                    err = bartap_zip_try_eocd(source, chunk_off + i, buf + i, zip, &matched);
                    if (err == BARTAP_ERR_CORRUPT || err == BARTAP_ERR_UNSUPPORTED) {
                        /* A signature inside a comment may look like a broken end record. Keep
                         * searching for a valid one, but report damage if none exists. */
                        candidate_error = err;
                        continue;
                    }
                    if (err != BARTAP_OK) {
                        return err;
                    }
                    if (matched) {
                        found = true;
                        break;
                    }
                }
            }
        }
        if (found) {
            break;
        }
        if (chunk_off == search_start) {
            break;
        }
        const uint32_t step = chunk_len > (BARTAP_ZIP_EOCD_MIN - 1U)
            ? chunk_len - (BARTAP_ZIP_EOCD_MIN - 1U)
            : 1U;
        cursor -= step;
    }

    if (!found) {
        return candidate_error;
    }

    return bartap_zip_check_mimetype(zip, source);
}

bartap_err_t bartap_zip_find(const bartap_zip_t *zip, const bartap_source_t *source, const char *path,
    size_t len, bartap_entry_t *entry)
{
    if (zip == NULL || source == NULL || path == NULL || entry == NULL) {
        return BARTAP_ERR_ARG;
    }

    if (len == 0U) {
        return BARTAP_ERR_NOT_FOUND;
    }
    uint32_t pos = zip->cd_offset;
    for (uint16_t i = 0; i < zip->entry_count; i++) {
        bartap_entry_t cur;
        uint32_t name_off = 0U;
        uint16_t name_len = 0U;
        uint16_t flags = 0U;
        bartap_err_t err = bartap_zip_read_cd_entry(zip, source, &pos, &cur, &name_off, &name_len, &flags);
        if (err != BARTAP_OK) {
            return err;
        }
        if (name_len != len) {
            continue;
        }

        uint8_t chunk[64];
        size_t rem = len;
        size_t off = 0U;
        bool match = true;
        while (rem > 0U) {
            const size_t piece = rem < sizeof chunk ? rem : sizeof chunk;
            err = bartap_source_read(source, name_off + (uint32_t)off, chunk, piece);
            if (err != BARTAP_OK) {
                return err;
            }
            if (memcmp(chunk, path + off, piece) != 0) {
                match = false;
                break;
            }
            off += piece;
            rem -= piece;
        }
        if (match) {
            err = bartap_zip_resolve_local_header(zip, source, flags, &cur, NULL);
            if (err != BARTAP_OK) {
                return err;
            }
            *entry = cur;
            return BARTAP_OK;
        }
    }

    return BARTAP_ERR_NOT_FOUND;
}

bartap_err_t bartap_zip_next(const bartap_zip_t *zip, const bartap_source_t *source,
    uint32_t *pos, bartap_entry_t *entry, char path[BARTAP_PATH_MAX + 1], uint16_t *flags)
{
    uint32_t offset;
    uint16_t len;
    bartap_err_t err = bartap_zip_read_cd_entry(zip, source, pos, entry, &offset, &len, flags);
    path[0] = '\0';
    if (err != BARTAP_OK || len > BARTAP_PATH_MAX) {
        return err;
    }
    err = bartap_source_read(source, offset, path, len);
    path[len] = '\0';
    /* Embedded NULs must not turn an unmatchable archive name into a shorter matching path. */
    if (err == BARTAP_OK && strlen(path) != len) {
        path[0] = '\0';
    }
    return err;
}
