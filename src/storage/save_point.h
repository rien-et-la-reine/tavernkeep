#ifndef TAVERNKEEP_STORAGE_SAVE_POINT_H
#define TAVERNKEEP_STORAGE_SAVE_POINT_H

/* Save points: small records of persistent state (a bookmark, the settings)
 * that stay readable whatever moment the card is removed or power is lost.
 *
 * A record named "/x" lives in two slot files, "/x.a" and "/x.b". Each save
 * writes the whole record, with a sequence number one higher than the newest
 * valid record and a CRC over everything before it, into the slot that does
 * not hold that newest record. A read checks both slots and returns the valid
 * one with the higher sequence number. An interrupted save can only damage the
 * slot it was writing, which then fails its checks, so the other slot still
 * holds the previous record. Nothing is ever renamed or removed: an interrupted
 * rename can leave two names sharing one cluster chain (filesystem_rename).
 *
 * Slot file format, little-endian (the RP2350's own byte order, so fields are
 * stored as they are in memory):
 *   offset 0         magic, the four bytes "TKS1"
 *   offset 4         sequence number (uint32_t; compared across a wrap)
 *   offset 8         data length in bytes (at most SAVE_POINT_DATA_MAX)
 *   offset 12        the data
 *   offset 12+length CRC-16 of bytes 0 to 12+length-1 (uint16_t), as
 *                    crc_helper_16 in sd_crc.h computes it: polynomial 0x1021,
 *                    initial value 0, no reflection (CRC-16/XMODEM)
 * A slot is valid only if all of that holds and the file is exactly
 * 14 + length bytes long. With an initial value of 0, leading zero bytes do
 * not change the CRC and an all-zero block checks out; the magic, which is
 * never zero, is what keeps a zero-filled slot from passing.
 *
 * Execution model: as the filesystem layer's (filesystem.h), whose calls these
 * are built on: the storage coordinator's core, foreground only. Each call
 * opens one file at a time, so it needs one free file slot. A slot file open
 * for writing through another handle makes either call fail with
 * FILESYSTEM_RESULT_IN_USE, as does, for a save, the slot it would rewrite
 * being open at all; the call then changes nothing. Results are the filesystem
 * layer's; a slot that is missing or fails its checks is not an error, only a
 * slot without a record. A slot the card fails to read is read once more; if
 * it fails again (STORAGE_ERROR) while the other slot can be read, that is not
 * an error either: a read returns the other slot's record, and a save rewrites
 * the unreadable slot, which repairs it. */

#include <stddef.h>
#include <stdint.h>

#include "storage/filesystem.h"

/* Largest record, in bytes. Records are small by design: each save rewrites
 * the whole record. */
#define SAVE_POINT_DATA_MAX 1024U

/* Longest record name in bytes, excluding the terminator. Each call builds
 * both slot names (".a" and ".b" appended) in 64-byte buffers on its stack. */
#define SAVE_POINT_PATH_MAX 61U

/* Save size bytes as the record's new contents. OK once the new record is
 * confirmed on the card (the slot file's close succeeded). After any other
 * result a read returns either the previous record or the new one, never
 * anything else - where the previous record means the newest one the card
 * could still read when the save began: with the slot holding the newest
 * record unreadable, the other slot's older record stands in for it. INVALID_ARGUMENT for a name longer than SAVE_POINT_PATH_MAX,
 * more than SAVE_POINT_DATA_MAX bytes, or NULL data with a non-zero size. */
filesystem_result_t save_point_write(const char *path, const void *data, size_t size);

/* Read the newest valid record into data, a buffer of capacity bytes, and its
 * length into *size. NOT_FOUND if neither slot holds a valid record: nothing
 * has been saved yet, or the first save was interrupted. STORAGE_ERROR instead
 * if neither holds one and a slot could not be read. INVALID_ARGUMENT if
 * the record is longer than capacity, with *size set to its length. data is
 * unspecified after anything but OK. */
filesystem_result_t save_point_read(const char *path, void *data, size_t capacity, size_t *size);

#endif
