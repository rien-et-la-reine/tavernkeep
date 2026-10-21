/* Save points (save_point.h): each record in two slot files, written alternately, each carrying a
 * sequence number and a CRC, so that the newest record that checks out is always one that was
 * written completely.
 *
 * Built only on filesystem.h. A save reads both slots to find the newest valid record, then rewrites
 * the other slot with FILESYSTEM_OPEN_REPLACE. Rewriting a slot truncates it first, so for the length
 * of the save that slot holds no valid record - which is safe precisely because it is never the slot
 * holding the newest one. */

#include "storage/save_point.h"

#include <stdbool.h>
#include <string.h>

#include "storage/sd_crc.h"

enum {
    HEADER_BYTES = 12,      //magic, sequence number, length
    TRAILER_BYTES = 2,      //CRC-16
    //a slot whose data is only being checked, not kept, is read in pieces this size
    SCRATCH_BYTES = 64,
};

static const uint8_t magic[4] = { 'T', 'K', 'S', '1' };

//what checking a slot file found
typedef struct {
    bool valid;
    bool unreadable;        //the card failed to read it twice (STORAGE_ERROR); never valid
    uint32_t sequence;
    uint32_t length;
} slot_t;

/* ----------------------------------------------------------- helpers */

//true if sequence number a is newer than b. half the range either way counts, so the comparison
//survives a wrap from 0xFFFFFFFF to 0
static bool newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

//the two slot file names for a record; false for an empty name or one longer than SAVE_POINT_PATH_MAX.
//reads at most SAVE_POINT_PATH_MAX + 1 bytes of the name
static bool slot_names(const char *path, char a[SAVE_POINT_PATH_MAX + 3U], char b[SAVE_POINT_PATH_MAX + 3U])
{
    const char *const end = memchr(path, '\0', SAVE_POINT_PATH_MAX + 1U);
    if (end == NULL || end == path) {
        return false;
    }
    const size_t length = (size_t)(end - path);
    memcpy(a, path, length);
    memcpy(a + length, ".a", 3);
    memcpy(b, path, length);
    memcpy(b + length, ".b", 3);
    return true;
}

//read exactly count bytes. a short read means the file ended early: not an error, so the result is OK
//with *complete false
static filesystem_result_t read_exact(filesystem_file_t file, void *buffer, size_t count, bool *complete)
{
    size_t got = 0;
    const filesystem_result_t result = filesystem_read(file, buffer, count, &got);
    *complete = result == FILESYSTEM_RESULT_OK && got == count;
    return result;
}

//write all count bytes; filesystem_write writes fewer only when the volume is full
static filesystem_result_t write_exact(filesystem_file_t file, const void *buffer, size_t count)
{
    size_t written = 0;
    const filesystem_result_t result = filesystem_write(file, buffer, count, &written);
    if (result == FILESYSTEM_RESULT_OK && written != count) {
        return FILESYSTEM_RESULT_DENIED;
    }
    return result;
}

/* ------------------------------------------------------------- slots */

//check an open slot file against the format in save_point.h. with keep non-NULL the data is also copied
//there if it fits keep_capacity; a slot too long for it is still checked, and still valid. kept out
//of line so that its buffers are on the stack only while a slot is read, not under the f_open beside it,
//the deepest storage path (docs/architecture.md, "Stacks")
__attribute__((noinline)) static filesystem_result_t check_open_slot(
    filesystem_file_t file,
    slot_t *slot,
    uint8_t *keep,
    size_t keep_capacity)
{
    uint32_t file_size = 0;
    filesystem_result_t result = filesystem_size(file, &file_size);
    if (result != FILESYSTEM_RESULT_OK) {
        return result;
    }
    uint8_t header[HEADER_BYTES];
    bool complete = false;
    result = read_exact(file, header, sizeof header, &complete);
    if (result != FILESYSTEM_RESULT_OK || !complete) {
        return result;
    }
    uint32_t length;
    memcpy(&length, header + 8, sizeof length);
    if (memcmp(header, magic, sizeof magic) != 0 || length > SAVE_POINT_DATA_MAX
            || file_size != HEADER_BYTES + length + TRAILER_BYTES) {
        return FILESYSTEM_RESULT_OK;
    }
    if (keep != NULL && length > keep_capacity) {
        keep = NULL;
    }

    uint16_t crc = crc_helper_16(header, sizeof header);
    uint8_t scratch[SCRATCH_BYTES];
    uint32_t remaining = length;
    while (remaining > 0U) {
        uint8_t *const into = keep != NULL ? keep + (length - remaining) : scratch;
        const size_t chunk = keep != NULL || remaining < SCRATCH_BYTES ? remaining : SCRATCH_BYTES;
        result = read_exact(file, into, chunk, &complete);
        if (result != FILESYSTEM_RESULT_OK || !complete) {
            return result;
        }
        for (size_t i = 0; i < chunk; i++) {
            crc = crc_helper_rolling_16(crc, into[i]);
        }
        remaining -= (uint32_t)chunk;
    }
    uint8_t trailer[TRAILER_BYTES];
    result = read_exact(file, trailer, sizeof trailer, &complete);
    uint16_t stored_crc;
    memcpy(&stored_crc, trailer, sizeof stored_crc);
    if (result != FILESYSTEM_RESULT_OK || !complete || stored_crc != crc) {
        return result;
    }

    uint32_t sequence;
    memcpy(&sequence, header + 4, sizeof sequence);
    *slot = (slot_t){ .valid = true, .sequence = sequence, .length = length };
    return FILESYSTEM_RESULT_OK;
}

//check one slot file by name. a missing file is a slot without a record, not an error
static filesystem_result_t check_slot(const char *name, slot_t *slot, uint8_t *keep, size_t keep_capacity)
{
    *slot = (slot_t){ .valid = false };
    filesystem_file_t file;
    const filesystem_result_t opened = filesystem_open(name, FILESYSTEM_OPEN_READ, &file);
    if (opened == FILESYSTEM_RESULT_NOT_FOUND) {
        return FILESYSTEM_RESULT_OK;
    }
    if (opened != FILESYSTEM_RESULT_OK) {
        return opened;
    }
    const filesystem_result_t result = check_open_slot(file, slot, keep, keep_capacity);
    const filesystem_result_t closed = filesystem_close(file);
    return result != FILESYSTEM_RESULT_OK ? result : closed;
}

//check both slots of a record. a slot the card fails to read is read once more, so that a passing error
//changes nothing; failing again, it is marked unreadable instead of ending the check, so that one bad slot
//cannot make the other unusable: a read can still return the other's record, and a save rewrites the
//bad one. any other error ends it (the card gone, the volume unmounted, a slot open for writing), and so
//does neither slot being readable. slot a's data is kept as check_open_slot keeps it, when keep is
//non-NULL
static filesystem_result_t check_slots(
    char names[2][SAVE_POINT_PATH_MAX + 3U],
    slot_t slots[2],
    uint8_t *keep,
    size_t keep_capacity)
{
    for (size_t i = 0; i < 2U; i++) {
        uint8_t *const into = i == 0U ? keep : NULL;
        const size_t capacity = i == 0U ? keep_capacity : 0U;
        filesystem_result_t result = check_slot(names[i], &slots[i], into, capacity);
        if (result == FILESYSTEM_RESULT_STORAGE_ERROR) {
            result = check_slot(names[i], &slots[i], into, capacity);
        }
        if (result == FILESYSTEM_RESULT_STORAGE_ERROR) {
            slots[i] = (slot_t){ .valid = false, .unreadable = true };
        } else if (result != FILESYSTEM_RESULT_OK) {
            return result;
        }
    }
    if (slots[0].unreadable && slots[1].unreadable) {
        return FILESYSTEM_RESULT_STORAGE_ERROR;
    }
    return FILESYSTEM_RESULT_OK;
}

//the slot holding the newest valid record: 0 or 1, or -1 if neither holds one. two valid slots with
//the same sequence number cannot come from save_point_write; slot a wins if they ever do
static int newest_slot(const slot_t slots[2])
{
    if (slots[0].valid && (!slots[1].valid || !newer(slots[1].sequence, slots[0].sequence))) {
        return 0;
    }
    return slots[1].valid ? 1 : -1;
}

//rewrite a slot file with a complete record
static filesystem_result_t write_slot(const char *name, uint32_t sequence, const uint8_t *data, size_t size)
{
    uint8_t header[HEADER_BYTES];
    memcpy(header, magic, sizeof magic);
    const uint32_t length = (uint32_t)size;
    memcpy(header + 4, &sequence, sizeof sequence);
    memcpy(header + 8, &length, sizeof length);
    uint16_t crc = crc_helper_16(header, sizeof header);
    for (size_t i = 0; i < size; i++) {
        crc = crc_helper_rolling_16(crc, data[i]);
    }
    uint8_t trailer[TRAILER_BYTES];
    memcpy(trailer, &crc, sizeof crc);

    filesystem_file_t file;
    filesystem_result_t result = filesystem_open(name, FILESYSTEM_OPEN_REPLACE, &file);
    if (result != FILESYSTEM_RESULT_OK) {
        return result;
    }
    result = write_exact(file, header, sizeof header);
    if (result == FILESYSTEM_RESULT_OK && size > 0U) {
        result = write_exact(file, data, size);
    }
    if (result == FILESYSTEM_RESULT_OK) {
        result = write_exact(file, trailer, sizeof trailer);
    }
    //the close confirms the record on the card, and releases the handle whatever happened before it
    const filesystem_result_t closed = filesystem_close(file);
    return result != FILESYSTEM_RESULT_OK ? result : closed;
}

/* ------------------------------------------------------------ interface */

filesystem_result_t save_point_write(const char *path, const void *data, size_t size)
{
    char names[2][SAVE_POINT_PATH_MAX + 3U];
    if (path == NULL || !slot_names(path, names[0], names[1]) || size > SAVE_POINT_DATA_MAX
            || (data == NULL && size != 0U)) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    slot_t slots[2];
    const filesystem_result_t result = check_slots(names, slots, NULL, 0U);
    if (result != FILESYSTEM_RESULT_OK) {
        return result;
    }
    //write over the slot that does not hold the newest record, never the one that does. an unreadable
    //slot is always the one written: rewriting it is what repairs it, and left alone it could later read
    //back holding a record that looks newer than this one
    const int current = newest_slot(slots);
    int target = current == 0 ? 1 : 0;
    if (slots[0].unreadable || slots[1].unreadable) {
        target = slots[0].unreadable ? 0 : 1;
    }
    const uint32_t sequence = current < 0 ? 1U : slots[current].sequence + 1U;
    return write_slot(names[target], sequence, data, size);
}

filesystem_result_t save_point_read(const char *path, void *data, size_t capacity, size_t *size)
{
    char names[2][SAVE_POINT_PATH_MAX + 3U];
    if (path == NULL || size == NULL || (data == NULL && capacity != 0U)
            || !slot_names(path, names[0], names[1])) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    //slot a is checked straight into the caller's buffer, so when it holds the newest record - every other
    //save - nothing is read twice
    slot_t slots[2];
    filesystem_result_t result = check_slots(names, slots, data, capacity);
    if (result != FILESYSTEM_RESULT_OK) {
        return result;
    }
    const int current = newest_slot(slots);
    if (current < 0) {
        //with a slot unreadable, nothing found does not mean nothing saved
        return slots[0].unreadable || slots[1].unreadable ? FILESYSTEM_RESULT_STORAGE_ERROR
                                                         : FILESYSTEM_RESULT_NOT_FOUND;
    }
    *size = slots[current].length;
    if (slots[current].length > capacity) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (current == 0) {
        return FILESYSTEM_RESULT_OK;        //it fits, so check_slots kept it (an empty record has nothing to keep)
    }

    //slot b, read again, now into the buffer. it checked out a moment ago and nothing else writes it, so
    //if reading it again gives a different answer the card is not returning what it holds
    slot_t kept;
    result = check_slot(names[1], &kept, data, capacity);
    if (result != FILESYSTEM_RESULT_OK) {
        return result;
    }
    if (!kept.valid || kept.sequence != slots[1].sequence || kept.length != slots[1].length) {
        return FILESYSTEM_RESULT_STORAGE_ERROR;
    }
    return FILESYSTEM_RESULT_OK;
}
