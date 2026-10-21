//tests for src/storage/save_point.c through its interface (save_point.h), with the real filesystem layer,
//adapter and FatFs underneath and the fake block device at the bottom
//
//what is asserted, and where it comes from:
//  - the contract written in save_point.h: which record a read returns, what a failed or interrupted save
//    may leave, and the argument rules
//  - the slot file format documented there, checked against records this file encodes itself, with its
//    own CRC-16 checked against the standard check value
//  - interruption at every device write of a save, by sweeping a removal across all of them

#include <string.h>

#include "fake_block_device.h"
#include "fixture.h"
#include "harness.h"
#include "storage/filesystem.h"
#include "storage/save_point.h"
#include "storage/sd_crc.h"

#define R(x) FILESYSTEM_RESULT_##x

#define RECORD "/state"
#define SLOT_A RECORD ".a"
#define SLOT_B RECORD ".b"

/* ------------------------------------------------------------ helpers */

static bool mount_fresh(void)
{
    return fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK
        && fixture_format(&disk)
        && filesystem_mount(&disk.device) == R(OK);
}

//the card was pulled; the coordinator unmounts, and once it is back in, brings it up and mounts afresh
static bool remount_after_removal(void)
{
    fake_disk_remove(&disk);
    filesystem_unmount();
    fake_disk_insert(&disk);
    return fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK
        && filesystem_mount(&disk.device) == R(OK);
}

//CRC-16 as save_point.h specifies it (CRC-16/XMODEM: polynomial 0x1021, initial value 0, no
//reflection), written independently of sd_crc.c
static uint16_t test_crc16(const uint8_t *bytes, size_t count)
{
    uint16_t crc = 0U;
    for (size_t i = 0; i < count; i++) {
        crc ^= (uint16_t)(bytes[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) != 0U ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void put_le32(uint8_t *out, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(value >> (8 * i));
    }
}

static uint32_t get_le32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

//a slot file's bytes, laid out as save_point.h documents; returns the length
static size_t encode_slot(uint8_t *out, uint32_t sequence, const void *data, uint32_t length)
{
    memcpy(out, "TKS1", 4);
    put_le32(out + 4, sequence);
    put_le32(out + 8, length);
    memcpy(out + 12, data, length);
    const uint16_t crc = test_crc16(out, 12 + length);
    out[12 + length] = (uint8_t)crc;
    out[13 + length] = (uint8_t)(crc >> 8);
    return 14U + length;
}

static bool write_raw(const char *path, const void *bytes, size_t count)
{
    filesystem_file_t file;
    size_t written = 0;
    if (filesystem_open(path, FILESYSTEM_OPEN_REPLACE, &file) != R(OK)) {
        return false;
    }
    const bool ok = count == 0U
        || (filesystem_write(file, bytes, count, &written) == R(OK) && written == count);
    return filesystem_close(file) == R(OK) && ok;
}

static bool read_raw(const char *path, uint8_t *bytes, size_t capacity, size_t *count)
{
    filesystem_file_t file;
    if (filesystem_open(path, FILESYSTEM_OPEN_READ, &file) != R(OK)) {
        return false;
    }
    const bool ok = filesystem_read(file, bytes, capacity, count) == R(OK);
    return filesystem_close(file) == R(OK) && ok;
}

static bool save_text(const char *text)
{
    return save_point_write(RECORD, text, strlen(text)) == R(OK);
}

//the record a read returns, as a string; false if the read did not succeed
static bool read_text(char *text, size_t capacity)
{
    size_t size = 0;
    if (save_point_read(RECORD, text, capacity - 1U, &size) != R(OK)) {
        return false;
    }
    text[size] = '\0';
    return true;
}

static bool record_is(const char *expected)
{
    char text[64];
    return read_text(text, sizeof text) && strcmp(text, expected) == 0;
}

//the sequence number in a slot file, or 0 if it cannot be read
static uint32_t slot_sequence(const char *path)
{
    uint8_t bytes[64];
    size_t count = 0;
    return read_raw(path, bytes, sizeof bytes, &count) && count >= 8U ? get_le32(bytes + 4) : 0U;
}

/* ---------------------------------------------------------- the format */

static void sp_slot_format_is_as_documented(void)
{
    //the test's own CRC, and the helpers the module uses (sd_crc.h), against the standard check value for
    //CRC-16/XMODEM; the rolling helper fed one byte at a time must agree with the whole-message one
    const uint8_t *const check = (const uint8_t *)"123456789";
    EXPECT_EQ_U(test_crc16(check, 9), 0x31C3U);
    EXPECT_EQ_U(crc_helper_16(check, 9), 0x31C3U);
    uint16_t rolling = 0;
    for (size_t i = 0; i < 9; i++) {
        rolling = crc_helper_rolling_16(rolling, check[i]);
    }
    EXPECT_EQ_U(rolling, 0x31C3U);

    REQUIRE(mount_fresh());
    REQUIRE(save_text("hello"));
    uint8_t expected[64];
    const size_t expected_count = encode_slot(expected, 1U, "hello", 5U);
    uint8_t actual[64];
    size_t actual_count = 0;
    REQUIRE(read_raw(SLOT_A, actual, sizeof actual, &actual_count));
    EXPECT_EQ_U(actual_count, expected_count);
    EXPECT(memcmp(actual, expected, expected_count) == 0);

    //and the module reads a slot this file encoded
    REQUIRE(write_raw(SLOT_B, expected, encode_slot(expected, 2U, "encoded here", 12U)));
    EXPECT(record_is("encoded here"));
}

/* ----------------------------------------------------- saving and reading */

static void sp_nothing_saved_reads_not_found(void)
{
    REQUIRE(mount_fresh());
    char text[16];
    size_t size = 0;
    EXPECT_RESULT(save_point_read(RECORD, text, sizeof text, &size), R(NOT_FOUND));

    //slot files that hold no valid record are the same as none
    REQUIRE(write_raw(SLOT_A, "not a record", 12));
    REQUIRE(write_raw(SLOT_B, "", 0));
    EXPECT_RESULT(save_point_read(RECORD, text, sizeof text, &size), R(NOT_FOUND));
}

static void sp_saves_alternate_slots_and_the_newest_is_read(void)
{
    REQUIRE(mount_fresh());
    filesystem_dirent_t entry;

    REQUIRE(save_text("one"));
    EXPECT_EQ_U(slot_sequence(SLOT_A), 1);
    EXPECT_RESULT(filesystem_stat(SLOT_B, &entry), R(NOT_FOUND));
    EXPECT(record_is("one"));

    REQUIRE(save_text("two"));
    EXPECT_EQ_U(slot_sequence(SLOT_A), 1);                      //the newest record is never rewritten
    EXPECT_EQ_U(slot_sequence(SLOT_B), 2);
    EXPECT(record_is("two"));

    REQUIRE(save_text("three"));
    EXPECT_EQ_U(slot_sequence(SLOT_A), 3);
    EXPECT_EQ_U(slot_sequence(SLOT_B), 2);
    EXPECT(record_is("three"));

    //nothing else is created: no temporary, no third file
    EXPECT_RESULT(filesystem_stat(RECORD, &entry), R(NOT_FOUND));
}

static void sp_records_of_every_size_round_trip(void)
{
    static uint8_t data[SAVE_POINT_DATA_MAX + 1U];
    static uint8_t back[SAVE_POINT_DATA_MAX];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = fixture_pattern((uint32_t)i);
    }
    REQUIRE(mount_fresh());
    size_t size = 1;

    //empty
    REQUIRE_RESULT(save_point_write(RECORD, NULL, 0), R(OK));
    EXPECT_RESULT(save_point_read(RECORD, NULL, 0, &size), R(OK));
    EXPECT_EQ_U(size, 0);

    //larger than the module reads in one piece when only checking, and the largest allowed
    static const size_t sizes[] = { 1, 63, 64, 65, 512, SAVE_POINT_DATA_MAX };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        REQUIRE_RESULT(save_point_write(RECORD, data, sizes[i]), R(OK));
        memset(back, 0, sizeof back);
        EXPECT_RESULT(save_point_read(RECORD, back, sizeof back, &size), R(OK));
        EXPECT_EQ_U(size, sizes[i]);
        EXPECT(memcmp(back, data, sizes[i]) == 0);
    }

    //one byte over is refused, and changes nothing
    EXPECT_RESULT(save_point_write(RECORD, data, SAVE_POINT_DATA_MAX + 1U), R(INVALID_ARGUMENT));
    EXPECT_RESULT(save_point_read(RECORD, back, sizeof back, &size), R(OK));
    EXPECT_EQ_U(size, SAVE_POINT_DATA_MAX);
}

static void sp_sequence_compares_across_wrap(void)
{
    REQUIRE(mount_fresh());
    uint8_t bytes[64];
    REQUIRE(write_raw(SLOT_A, bytes, encode_slot(bytes, 0xFFFFFFFFU, "before", 6U)));
    REQUIRE(write_raw(SLOT_B, bytes, encode_slot(bytes, 0U, "after", 5U)));
    EXPECT(record_is("after"));                                 //0 follows 0xFFFFFFFF

    REQUIRE(save_text("next"));
    EXPECT_EQ_U(slot_sequence(SLOT_A), 1);                      //written over the older slot
    EXPECT(record_is("next"));
}

/* --------------------------------------------------- damage and interruption */

//a newest slot that fails any of its checks is passed over for the other one, and the next save goes
//into the damaged slot, never over the good one
static void sp_damaged_newest_slot_falls_back(void)
{
    enum { FLIP_DATA, FLIP_CRC, BAD_MAGIC, TRUNCATED, EXTENDED, LENGTH_TOO_LARGE, DAMAGE_KINDS };
    for (int kind = 0; kind < DAMAGE_KINDS; kind++) {
        filesystem_unmount();
        REQUIRE(mount_fresh());
        REQUIRE(save_text("older"));
        REQUIRE(save_text("newer"));                            //slot b, sequence 2

        uint8_t bytes[64];
        size_t count = encode_slot(bytes, 2U, "newer", 5U);
        switch (kind) {
        case FLIP_DATA: bytes[12] ^= 0x01U; break;
        case FLIP_CRC: bytes[count - 1U] ^= 0x80U; break;
        case BAD_MAGIC: bytes[3] = '2'; break;
        case TRUNCATED: count--; break;
        case EXTENDED: bytes[count++] = 0U; break;
        case LENGTH_TOO_LARGE: put_le32(bytes + 8, SAVE_POINT_DATA_MAX + 1U); break;
        default: break;
        }
        REQUIRE(write_raw(SLOT_B, bytes, count));
        EXPECT(record_is("older"));

        REQUIRE(save_text("repaired"));
        EXPECT_EQ_U(slot_sequence(SLOT_A), 1);                  //the good slot was left alone
        EXPECT_EQ_U(slot_sequence(SLOT_B), 2);                  //sequence one past the newest valid
        EXPECT(record_is("repaired"));
    }
}

//the card pulled at each device write of a save in turn: whatever the moment, a read afterwards returns
//the previous record or the new one, never anything else, and the next save works
//a read checks slot a straight into the caller's buffer, so it reads less when slot a holds the newest
//record than when slot b does, which it must read again to copy. either way it returns the newest
static void sp_read_of_slot_a_reads_it_once(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(save_text("one"));                                  //slot a
    REQUIRE(save_text("two"));                                  //slot b, newest
    unsigned reads = disk.read_calls;
    EXPECT(record_is("two"));
    const unsigned newest_in_b = disk.read_calls - reads;
    REQUIRE(save_text("three"));                                //slot a, newest
    reads = disk.read_calls;
    EXPECT(record_is("three"));
    const unsigned newest_in_a = disk.read_calls - reads;
    EXPECT(newest_in_a < newest_in_b);
}

//an empty record reads back from either slot, into no buffer at all
static void sp_empty_record_reads_from_either_slot(void)
{
    REQUIRE(mount_fresh());
    size_t size = 99;
    for (int i = 0; i < 2; i++) {                               //slot a, then slot b
        REQUIRE_RESULT(save_point_write(RECORD, NULL, 0), R(OK));
        size = 99;
        EXPECT_RESULT(save_point_read(RECORD, NULL, 0, &size), R(OK));
        EXPECT_EQ_U(size, 0);
    }
}

//the card sector holding the slot record with this sequence number (each is written once), or -1
static int64_t record_sector(uint32_t sequence)
{
    for (uint64_t block = 0; block < disk.backed_blocks; block++) {
        const uint8_t *const sector = disk.data + block * FAKE_BLOCK_SIZE;
        if (memcmp(sector, "TKS1", 4) == 0 && get_le32(sector + 4) == sequence) {
            return (int64_t)block;
        }
    }
    return -1;
}

//a slot the card cannot read must not make the other unusable (save_point.h): a read returns the other
//slot's record, the previous one, and a save rewrites the unreadable slot, which repairs it
static void sp_unreadable_slot_falls_back_to_the_other(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(save_text("one"));                                  //slot a, sequence 1
    REQUIRE(save_text("two"));                                  //slot b, sequence 2
    disk.bad_block = record_sector(2);
    REQUIRE(disk.bad_block >= 0);

    EXPECT(record_is("one"));
    EXPECT(save_text("three"));
    //written over the unreadable slot, numbered one above the newest record that could be read
    EXPECT_EQ_U(slot_sequence(SLOT_B), 2);
    EXPECT_EQ_U(slot_sequence(SLOT_A), 1);                      //and not over the good one
    EXPECT(record_is("three"));
}

//a passing read error on the slot with the newest record changes nothing: the slot is read once more, so
//the read still returns the newest record, and a save still goes to the other slot (save_point.h)
static void sp_passing_read_error_is_read_again(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(save_text("one"));                                  //slot a, sequence 1
    REQUIRE(save_text("two"));                                  //slot b, sequence 2
    disk.bad_block = record_sector(2);
    REQUIRE(disk.bad_block >= 0);
    disk.bad_block_reads = 1;
    EXPECT(record_is("two"));
    EXPECT(disk.bad_block < 0);                                 //the error happened, and passed

    disk.bad_block = record_sector(2);
    disk.bad_block_reads = 1;
    EXPECT(save_text("three"));
    EXPECT_EQ_U(slot_sequence(SLOT_A), 3);                      //written over the older slot, a
    EXPECT_EQ_U(slot_sequence(SLOT_B), 2);
    EXPECT(record_is("three"));
}

//with the only record unreadable, a read cannot say nothing was saved; a save still rewrites the slot
static void sp_unreadable_only_slot_is_not_reported_missing(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(save_text("one"));                                  //slot a, sequence 1
    disk.bad_block = record_sector(1);
    REQUIRE(disk.bad_block >= 0);

    char text[16];
    size_t size = 0;
    EXPECT_RESULT(save_point_read(RECORD, text, sizeof text, &size), R(STORAGE_ERROR));
    EXPECT(save_text("two"));
    EXPECT(record_is("two"));
}

//neither slot readable: both calls fail, and the save writes nothing. the two entries share a directory
//sector, so a bad one makes both slots unreadable
static void sp_both_slots_unreadable_fail(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(save_text("one"));
    REQUIRE(save_text("two"));
    //remount so that FatFs is not holding the directory sector in its window
    filesystem_unmount();
    REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
    //the short-name entry of slot a, "STATE   A  " (FAT's padded 8.3 form), sits on a 32-byte boundary
    for (uint64_t block = 0; block < disk.backed_blocks && disk.bad_block < 0; block++) {
        for (size_t entry = 0; entry < FAKE_BLOCK_SIZE; entry += 32U) {
            if (memcmp(disk.data + block * FAKE_BLOCK_SIZE + entry, "STATE   A  ", 11) == 0) {
                disk.bad_block = (int64_t)block;
            }
        }
    }
    REQUIRE(disk.bad_block >= 0);

    char text[16];
    size_t size = 0;
    EXPECT_RESULT(save_point_read(RECORD, text, sizeof text, &size), R(STORAGE_ERROR));
    const unsigned writes = disk.write_calls;
    EXPECT_RESULT(save_point_write(RECORD, "three", 5), R(STORAGE_ERROR));
    EXPECT_EQ_U(disk.write_calls, writes);
}

static void sp_interrupted_save_keeps_the_previous_record(void)
{
    //how many device writes a save over the older of two valid slots makes
    REQUIRE(mount_fresh());
    REQUIRE(save_text("old-1"));
    REQUIRE(save_text("old-2"));
    const unsigned before = disk.write_calls;
    REQUIRE(save_text("new"));
    const unsigned writes = disk.write_calls - before;
    REQUIRE(writes > 0U);

    unsigned kept_old = 0;
    for (unsigned n = 0; n < writes; n++) {
        filesystem_unmount();
        REQUIRE(mount_fresh());
        REQUIRE(save_text("old-1"));
        REQUIRE(save_text("old-2"));
        fake_disk_fail_write(&disk, (int)n, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
        EXPECT(save_point_write(RECORD, "new", 3) != R(OK));
        REQUIRE(remount_after_removal());

        char text[64];
        REQUIRE(read_text(text, sizeof text));
        EXPECT(strcmp(text, "old-2") == 0 || strcmp(text, "new") == 0);
        kept_old += strcmp(text, "old-2") == 0;

        REQUIRE(save_text("after"));
        EXPECT(record_is("after"));
    }
    EXPECT(kept_old > 0U);
}

//the same sweep across a card's very first save: afterwards there is no record or the whole new one
static void sp_interrupted_first_save_leaves_no_partial_record(void)
{
    REQUIRE(mount_fresh());
    const unsigned before = disk.write_calls;
    REQUIRE(save_text("first"));
    const unsigned writes = disk.write_calls - before;
    REQUIRE(writes > 0U);

    for (unsigned n = 0; n < writes; n++) {
        filesystem_unmount();
        REQUIRE(mount_fresh());
        fake_disk_fail_write(&disk, (int)n, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
        EXPECT(save_point_write(RECORD, "first", 5) != R(OK));
        REQUIRE(remount_after_removal());

        char text[64];
        size_t size = 0;
        const filesystem_result_t result = save_point_read(RECORD, text, sizeof text - 1U, &size);
        if (result == R(OK)) {
            text[size] = '\0';
            EXPECT(strcmp(text, "first") == 0);
        } else {
            EXPECT_RESULT(result, R(NOT_FOUND));
        }

        REQUIRE(save_text("after"));
        EXPECT(record_is("after"));
    }
}

//a removal the coordinator has not acted on yet: reads and saves fail, and write nothing
static void sp_removed_card_fails_without_writing(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(save_text("kept"));
    fake_disk_remove(&disk);
    const unsigned writes = disk.write_calls;
    char text[16];
    size_t size = 0;
    EXPECT(save_point_read(RECORD, text, sizeof text, &size) != R(OK));
    EXPECT(save_point_write(RECORD, "lost", 4) != R(OK));
    EXPECT_EQ_U(disk.write_calls, writes);

    REQUIRE(remount_after_removal());
    EXPECT(record_is("kept"));
}

//a slot file open through another handle: the save fails with IN_USE before writing anything, and works
//once the handle is closed
static void sp_open_slot_file_is_in_use(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(save_text("one"));
    REQUIRE(save_text("two"));                                  //the next save goes into slot a
    filesystem_file_t file;
    REQUIRE_RESULT(filesystem_open(SLOT_A, FILESYSTEM_OPEN_READ, &file), R(OK));
    const unsigned writes = disk.write_calls;
    EXPECT_RESULT(save_point_write(RECORD, "three", 5), R(IN_USE));
    EXPECT_EQ_U(disk.write_calls, writes);
    EXPECT(record_is("two"));                                   //reading alongside another reader is fine
    REQUIRE_RESULT(filesystem_close(file), R(OK));

    REQUIRE(save_text("three"));
    EXPECT(record_is("three"));
}

/* ------------------------------------------------------------- arguments */

static void sp_arguments_are_checked(void)
{
    char longest[SAVE_POINT_PATH_MAX + 1U];
    char too_long[SAVE_POINT_PATH_MAX + 2U];
    longest[0] = '/';
    memset(longest + 1, 'n', SAVE_POINT_PATH_MAX - 1U);
    longest[SAVE_POINT_PATH_MAX] = '\0';
    memcpy(too_long, longest, SAVE_POINT_PATH_MAX);
    too_long[SAVE_POINT_PATH_MAX] = 'n';
    too_long[SAVE_POINT_PATH_MAX + 1U] = '\0';

    REQUIRE(mount_fresh());
    char text[16];
    size_t size = 0;

    EXPECT_RESULT(save_point_write(NULL, "x", 1), R(INVALID_ARGUMENT));
    EXPECT_RESULT(save_point_write("", "x", 1), R(INVALID_ARGUMENT));
    EXPECT_RESULT(save_point_write(too_long, "x", 1), R(INVALID_ARGUMENT));
    EXPECT_RESULT(save_point_write(RECORD, NULL, 1), R(INVALID_ARGUMENT));
    EXPECT_RESULT(save_point_read(NULL, text, sizeof text, &size), R(INVALID_ARGUMENT));
    EXPECT_RESULT(save_point_read("", text, sizeof text, &size), R(INVALID_ARGUMENT));
    EXPECT_RESULT(save_point_read(too_long, text, sizeof text, &size), R(INVALID_ARGUMENT));
    EXPECT_RESULT(save_point_read(RECORD, text, sizeof text, NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(save_point_read(RECORD, NULL, 4, &size), R(INVALID_ARGUMENT));

    //the longest name is accepted, and its slot names still fit
    EXPECT_RESULT(save_point_write(longest, "x", 1), R(OK));
    EXPECT_RESULT(save_point_read(longest, text, sizeof text, &size), R(OK));
    EXPECT_EQ_U(size, 1);

    //a buffer too small for the record is refused, with the length the record needs
    REQUIRE(save_text("eleven char"));
    size = 0;
    EXPECT_RESULT(save_point_read(RECORD, text, 10, &size), R(INVALID_ARGUMENT));
    EXPECT_EQ_U(size, 11);
    EXPECT_RESULT(save_point_read(RECORD, text, 11, &size), R(OK));

    //and nothing works without a mounted volume
    filesystem_unmount();
    EXPECT_RESULT(save_point_read(RECORD, text, sizeof text, &size), R(NOT_MOUNTED));
    EXPECT_RESULT(save_point_write(RECORD, "x", 1), R(NOT_MOUNTED));
}

const test_case_t save_point_tests[] = {
    TEST_CASE(sp_slot_format_is_as_documented),
    TEST_CASE(sp_nothing_saved_reads_not_found),
    TEST_CASE(sp_saves_alternate_slots_and_the_newest_is_read),
    TEST_CASE(sp_records_of_every_size_round_trip),
    TEST_CASE(sp_sequence_compares_across_wrap),
    TEST_CASE(sp_damaged_newest_slot_falls_back),
    TEST_CASE(sp_read_of_slot_a_reads_it_once),
    TEST_CASE(sp_empty_record_reads_from_either_slot),
    TEST_CASE(sp_passing_read_error_is_read_again),
    TEST_CASE(sp_unreadable_slot_falls_back_to_the_other),
    TEST_CASE(sp_unreadable_only_slot_is_not_reported_missing),
    TEST_CASE(sp_both_slots_unreadable_fail),
    TEST_CASE(sp_interrupted_save_keeps_the_previous_record),
    TEST_CASE(sp_interrupted_first_save_leaves_no_partial_record),
    TEST_CASE(sp_removed_card_fails_without_writing),
    TEST_CASE(sp_open_slot_file_is_in_use),
    TEST_CASE(sp_arguments_are_checked),
};
const size_t save_point_test_count = sizeof save_point_tests / sizeof save_point_tests[0];
