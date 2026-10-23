// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//tests for src/usb/msc_disk.c, the disk the USB mass-storage class presents to a host, through
//msc_disk.h, with the real storage coordinator lending the fake card
//
//what is asserted comes from msc_disk.h: no medium until the card is lent and ready, one UNIT ATTENTION
//for each medium the host has not been told about, transfers that reach exactly the blocks addressed,
//writes synced before they complete, and the sense each failure gives. the sense values are SPC-4's, as
//key/ASC/ASCQ written 0xKKAAQQ:
//  0x023A00  NOT READY, MEDIUM NOT PRESENT
//  0x062800  UNIT ATTENTION, NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED
//  0x052100  ILLEGAL REQUEST, LOGICAL BLOCK ADDRESS OUT OF RANGE
//  0x052400  ILLEGAL REQUEST, INVALID FIELD IN CDB
//  0x031100  MEDIUM ERROR, UNRECOVERED READ ERROR
//  0x030C00  MEDIUM ERROR, WRITE ERROR
//  0x044400  HARDWARE ERROR, INTERNAL TARGET FAILURE
//
//the coordinator and this disk are boot-once state, so these cases rely on the runner's default of one
//process per case. the cards are blank: USB is lent a card whatever its filesystem, and the host's blocks
//are checked directly in the fake's backing store.

#include <string.h>

#include "fake_block_device.h"
#include "fixture.h"
#include "harness.h"
#include "storage/storage.h"
#include "usb/msc_disk.h"

#define LOCAL STORAGE_OWNER_LOCAL
#define USB STORAGE_OWNER_USB

enum {
    NO_MEDIUM = 0x023A00,
    MEDIUM_CHANGED = 0x062800,
    LBA_OUT_OF_RANGE = 0x052100,
    INVALID_FIELD = 0x052400,
    READ_ERROR = 0x031100,
    WRITE_ERROR = 0x030C00,
    TARGET_FAILURE = 0x044400,
};

//one buffer of TinyUSB's (CFG_TUD_MSC_EP_BUFSIZE), the most a single call carries
#define BUFFER_BYTES 4096U

/* ------------------------------------------------------------ helpers */

static uint32_t now;

static void step_after(uint32_t ms)
{
    now += ms;
    storage_step(now);
}

static uint32_t sense_value(const msc_disk_sense_t *sense)
{
    return ((uint32_t)sense->key << 16) | ((uint32_t)sense->code << 8) | sense->qualifier;
}

static void reset_counters(fake_disk_t *card)
{
    card->init_calls = 0;
    card->deinit_calls = 0;
    card->read_calls = 0;
    card->write_calls = 0;
    card->info_calls = 0;
    card->sync_calls = 0;
    card->presence_calls = 0;
}

//a card the socket has just started reporting: the step that sees it starts the settle wait, and the
//step STORAGE_INSERT_SETTLE_MS later brings it up
static void settle(void)
{
    step_after(10);
    step_after(STORAGE_INSERT_SETTLE_MS);
}

//the blank card in the socket at boot, brought up for the local owner (unformatted: it has no volume)
static bool card_up(void)
{
    if (!storage_init(&disk.device)) {
        return false;
    }
    settle();
    return storage_owner() == LOCAL && storage_medium() == STORAGE_MEDIUM_UNFORMATTED;
}

//hand the card over to USB and let the coordinator carry it out
static bool lend(void)
{
    if (!storage_request_owner(USB)) {
        return false;
    }
    step_after(10);
    return storage_usb_device() == &disk.device;
}

static bool give_back(void)
{
    if (!storage_request_owner(LOCAL)) {
        return false;
    }
    step_after(10);
    return storage_owner() == LOCAL && storage_usb_device() == NULL;
}

//the host has been told about the lent card: its first TEST UNIT READY got UNIT ATTENTION, the next one
//found it ready
static bool lent_and_seen(void)
{
    msc_disk_sense_t sense;
    return card_up() && lend() && !msc_disk_test_unit_ready(&sense)
        && sense_value(&sense) == MEDIUM_CHANGED && msc_disk_test_unit_ready(&sense);
}

static uint8_t buffer[BUFFER_BYTES];

static void fill(uint8_t *bytes, size_t size, uint32_t seed)
{
    for (size_t i = 0; i < size; i++) {
        bytes[i] = fixture_pattern((uint32_t)i + seed);
    }
}

static const uint8_t *block_data(uint64_t lba)
{
    return disk.data + lba * FAKE_BLOCK_SIZE;
}

/* ------------------------------------------------------- no medium */

static void msc_no_medium_until_lent(void)
{
    REQUIRE(card_up());
    reset_counters(&disk);
    //every field of the sense is set, the qualifier included
    msc_disk_sense_t sense = { .key = 0xFF, .code = 0xFF, .qualifier = 0xFF };
    uint32_t count = 1;
    uint16_t size = 1;

    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    sense = (msc_disk_sense_t){ 0 };
    EXPECT(!msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    EXPECT_EQ_U(count, 0);
    EXPECT_EQ_U(size, 0);
    sense = (msc_disk_sense_t){ 0 };
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    sense = (msc_disk_sense_t){ 0 };
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    //with no medium, a write is not refused as write-protected: the write reports the missing medium
    EXPECT(msc_disk_writable());
    //the card in local hands was never touched
    EXPECT_EQ_U(fake_disk_io_calls(&disk), 0);
}

//lent with an empty socket: no medium, and a card inserted while lent is news to the host
static void msc_card_arriving_while_lent(void)
{
    disk.present = false;
    REQUIRE(storage_init(&disk.device));
    step_after(10);
    REQUIRE(storage_request_owner(USB));
    step_after(10);
    REQUIRE(storage_owner() == USB);
    msc_disk_sense_t sense;
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);

    fake_disk_insert(&disk);
    settle();
    REQUIRE(storage_usb_device() == &disk.device);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), MEDIUM_CHANGED);
    EXPECT(msc_disk_test_unit_ready(&sense));
}

/* -------------------------------------------------- unit attention */

static void msc_unit_attention_once_per_lend(void)
{
    REQUIRE(card_up());
    REQUIRE(lend());
    msc_disk_sense_t sense;
    uint32_t count = 0;
    uint16_t size = 0;

    //whichever command comes first gets it: here READ CAPACITY
    EXPECT(!msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(sense_value(&sense), MEDIUM_CHANGED);
    EXPECT_EQ_U(count, 0);
    EXPECT(msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(count, FIXTURE_DEFAULT_BLOCKS);
    EXPECT_EQ_U(size, FAKE_BLOCK_SIZE);
    EXPECT(msc_disk_test_unit_ready(&sense));
    EXPECT(msc_disk_test_unit_ready(&sense));
}

static void msc_unit_attention_fails_a_first_transfer(void)
{
    REQUIRE(card_up());
    REQUIRE(lend());
    reset_counters(&disk);
    msc_disk_sense_t sense;
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), MEDIUM_CHANGED);
    EXPECT_EQ_U(disk.read_calls, 0);
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), FAKE_BLOCK_SIZE);
}

//returned to the local owner and lent again: no medium in between, and the new lend is announced
static void msc_return_and_new_lend(void)
{
    REQUIRE(lent_and_seen());
    REQUIRE(give_back());
    msc_disk_sense_t sense;
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);

    REQUIRE(lend());
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), MEDIUM_CHANGED);
    EXPECT(msc_disk_test_unit_ready(&sense));
}

//returned and lent again between two of the host's commands, so it never saw the medium go: the epoch
//has moved on, and that alone makes it news
static void msc_lend_changed_unseen_is_announced(void)
{
    REQUIRE(lent_and_seen());
    REQUIRE(give_back());
    REQUIRE(lend());
    msc_disk_sense_t sense;
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), MEDIUM_CHANGED);
    EXPECT(msc_disk_test_unit_ready(&sense));
}

/* ------------------------------------------------------- transfers */

static void msc_writes_reach_the_blocks_addressed(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;

    //a whole buffer is one multi-block write, synced before it is reported done
    fill(buffer, BUFFER_BYTES, 1);
    reset_counters(&disk);
    EXPECT_EQ_U(msc_disk_write(100, 0, buffer, BUFFER_BYTES, &sense), BUFFER_BYTES);
    EXPECT_EQ_U(disk.write_calls, 1);
    EXPECT_EQ_U(disk.sync_calls, 1);
    EXPECT(memcmp(block_data(100), buffer, BUFFER_BYTES) == 0);

    //the offset counts on from the command's first block: the third buffer of a command at 200 starts
    //two buffers' worth of blocks later
    fill(buffer, FAKE_BLOCK_SIZE, 2);
    EXPECT_EQ_U(msc_disk_write(200, 2U * BUFFER_BYTES, buffer, FAKE_BLOCK_SIZE, &sense), FAKE_BLOCK_SIZE);
    EXPECT(memcmp(block_data(200U + 2U * BUFFER_BYTES / FAKE_BLOCK_SIZE), buffer, FAKE_BLOCK_SIZE) == 0);
    static const uint8_t zeros[FAKE_BLOCK_SIZE];
    EXPECT(memcmp(block_data(200), zeros, FAKE_BLOCK_SIZE) == 0);
}

static void msc_reads_return_the_blocks_addressed(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;
    fill(disk.data + 300U * FAKE_BLOCK_SIZE, 4U * FAKE_BLOCK_SIZE, 3);

    reset_counters(&disk);
    memset(buffer, 0xA5, sizeof buffer);
    EXPECT_EQ_U(msc_disk_read(300, FAKE_BLOCK_SIZE, buffer, 2U * FAKE_BLOCK_SIZE, &sense),
        2U * FAKE_BLOCK_SIZE);
    EXPECT_EQ_U(disk.read_calls, 1);
    EXPECT(memcmp(buffer, block_data(301), 2U * FAKE_BLOCK_SIZE) == 0);
    //nothing beyond the size asked for is written
    EXPECT_EQ_U(buffer[2U * FAKE_BLOCK_SIZE], 0xA5);
}

static void msc_transfers_outside_the_medium_are_refused(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;
    const uint32_t last = FIXTURE_DEFAULT_BLOCKS - 1U;

    EXPECT_EQ_U(msc_disk_read(last, 0, buffer, FAKE_BLOCK_SIZE, &sense), FAKE_BLOCK_SIZE);
    reset_counters(&disk);

    EXPECT_EQ_U(msc_disk_read(last + 1U, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), LBA_OUT_OF_RANGE);
    sense = (msc_disk_sense_t){ 0 };
    EXPECT_EQ_U(msc_disk_read(last, 0, buffer, 2U * FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), LBA_OUT_OF_RANGE);
    sense = (msc_disk_sense_t){ 0 };
    EXPECT_EQ_U(msc_disk_write(last, FAKE_BLOCK_SIZE, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), LBA_OUT_OF_RANGE);
    //an address near 2^32 plus an offset does not wrap round to a small block
    sense = (msc_disk_sense_t){ 0 };
    EXPECT_EQ_U(msc_disk_write(UINT32_MAX, FAKE_BLOCK_SIZE, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), LBA_OUT_OF_RANGE);
    EXPECT_EQ_U(disk.read_calls + disk.write_calls, 0);

    //refused before the device is asked, so the medium is still there
    EXPECT(msc_disk_test_unit_ready(&sense));
}

static void msc_partial_blocks_are_refused(void)
{
    REQUIRE(lent_and_seen());
    reset_counters(&disk);
    msc_disk_sense_t sense = { 0 };

    EXPECT_EQ_U(msc_disk_read(0, 100, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), INVALID_FIELD);
    sense = (msc_disk_sense_t){ 0 };
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, 100, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), INVALID_FIELD);
    sense = (msc_disk_sense_t){ 0 };
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, 0, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), INVALID_FIELD);
    EXPECT_EQ_U(disk.read_calls + disk.write_calls, 0);
}

/* -------------------------------------------------------- failures */

static void msc_read_failures(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;

    fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), READ_ERROR);
    fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), READ_ERROR);
    fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_OUT_OF_RANGE);
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), LBA_OUT_OF_RANGE);
    fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_INVALID_ARGUMENT);
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), TARGET_FAILURE);

    //none of those is a removal: the medium is still there, and is not news
    EXPECT(msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), FAKE_BLOCK_SIZE);
}

static void msc_write_failures(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;

    fake_disk_fail_write(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), WRITE_ERROR);
    fake_disk_fail_write(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), WRITE_ERROR);
    fake_disk_fail_write(&disk, 0, BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED);
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), TARGET_FAILURE);

    //accepted by the device but not confirmed on the card: the write fails all the same
    reset_counters(&disk);
    fake_disk_fail_sync(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), WRITE_ERROR);
    EXPECT_EQ_U(disk.write_calls, 1);
    EXPECT_EQ_U(disk.sync_calls, 1);

    EXPECT(msc_disk_test_unit_ready(&sense));
}

//a block size SCSI cannot carry, or a device that cannot say its geometry, is a fault of ours
static void msc_unusable_geometry_is_a_target_failure(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;
    uint32_t count = 0;
    uint16_t size = 0;

    disk.reported_block_size = 0;
    EXPECT(!msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(sense_value(&sense), TARGET_FAILURE);
    disk.reported_block_size = 65536U;
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), TARGET_FAILURE);
    disk.reported_block_size = FAKE_BLOCK_SIZE;
    disk.reported_block_count = 0;
    EXPECT(!msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(sense_value(&sense), TARGET_FAILURE);
    disk.reported_block_count = FIXTURE_DEFAULT_BLOCKS;

    fake_disk_fail_info(&disk, 0, BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED);
    EXPECT(!msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(sense_value(&sense), TARGET_FAILURE);
    fake_disk_fail_info(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    EXPECT(!msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(sense_value(&sense), READ_ERROR);
    fake_disk_fail_info(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), WRITE_ERROR);

    EXPECT(msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(count, FIXTURE_DEFAULT_BLOCKS);
}

static void msc_capacity_beyond_32_bits_is_clamped(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;
    uint32_t count = 0;
    uint16_t size = 0;
    //a 2 TB card: 2^32 blocks
    disk.reported_block_count = (uint64_t)1 << 32;
    EXPECT(msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(count, UINT32_MAX);
    EXPECT_EQ_U(size, FAKE_BLOCK_SIZE);
}

/* -------------------------------------------------------- removal */

//the card is pulled and a transfer finds out before the coordinator does: no medium from then on, for
//every command, though the coordinator still lists the device until its next step
static void msc_removal_found_by_a_transfer(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;

    fake_disk_remove(&disk);
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    REQUIRE(storage_usb_device() == &disk.device);
    reset_counters(&disk);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    EXPECT_EQ_U(fake_disk_io_calls(&disk), 0);

    step_after(10);
    EXPECT(storage_usb_device() == NULL);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);

    fake_disk_insert(&disk);
    settle();
    REQUIRE(storage_usb_device() == &disk.device);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), MEDIUM_CHANGED);
    EXPECT(msc_disk_test_unit_ready(&sense));
}

//the same found by a failed write, and by the geometry a command looks up first
static void msc_removal_found_by_a_write_or_lookup(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;
    uint32_t count = 0;
    uint16_t size = 0;

    fake_disk_fail_write(&disk, 0, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    EXPECT_EQ_U(msc_disk_write(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);

    //a new lend: the device is deinitialized under the coordinator, and READ CAPACITY finds it out
    REQUIRE(give_back());
    REQUIRE(lend());
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT(msc_disk_test_unit_ready(&sense));
    fake_disk_fail_info(&disk, 0, BLOCK_DEVICE_RESULT_NOT_INITIALIZED);
    EXPECT(!msc_disk_capacity(&count, &size, &sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
}

//the card is swapped between two of the host's commands, so it never saw it missing: the epoch moved
//on, so the card it finds is news
static void msc_card_swapped_unseen_is_announced(void)
{
    REQUIRE(lent_and_seen());
    fake_disk_remove(&disk);
    step_after(10);
    fake_disk_insert(&disk);
    settle();
    REQUIRE(storage_usb_device() == &disk.device);
    msc_disk_sense_t sense;
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), MEDIUM_CHANGED);
    EXPECT(msc_disk_test_unit_ready(&sense));
}

/* ------------------------------------------------- eject, writable */

static void msc_eject_withholds_the_medium(void)
{
    REQUIRE(lent_and_seen());
    msc_disk_sense_t sense;

    //a power condition or a load changes nothing
    msc_disk_start_stop(false, false);
    msc_disk_start_stop(true, false);
    msc_disk_start_stop(true, true);
    EXPECT(msc_disk_test_unit_ready(&sense));
    EXPECT(!msc_disk_take_eject());

    msc_disk_start_stop(false, true);
    EXPECT(storage_usb_device() == &disk.device);   //still lent: returning it is the policy's business
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    EXPECT_EQ_U(msc_disk_read(0, 0, buffer, FAKE_BLOCK_SIZE, &sense), -1);
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);
    EXPECT(msc_disk_take_eject());
    EXPECT(!msc_disk_take_eject());

    //the host cannot load it back
    msc_disk_start_stop(true, true);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), NO_MEDIUM);

    //once it has been returned and lent again, it is a new medium
    REQUIRE(give_back());
    REQUIRE(lend());
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense_value(&sense), MEDIUM_CHANGED);
    EXPECT(msc_disk_test_unit_ready(&sense));
    EXPECT(!msc_disk_take_eject());
}

static void msc_writable_follows_the_device(void)
{
    REQUIRE(lent_and_seen());
    EXPECT(msc_disk_writable());
    disk.reported_writable = false;
    EXPECT(!msc_disk_writable());
    //it changes nothing: the medium is not news afterwards
    msc_disk_sense_t sense;
    EXPECT(msc_disk_test_unit_ready(&sense));
    REQUIRE(give_back());
    EXPECT(msc_disk_writable());
}

const test_case_t msc_disk_tests[] = {
    TEST_CASE(msc_no_medium_until_lent),
    TEST_CASE(msc_card_arriving_while_lent),
    TEST_CASE(msc_unit_attention_once_per_lend),
    TEST_CASE(msc_unit_attention_fails_a_first_transfer),
    TEST_CASE(msc_return_and_new_lend),
    TEST_CASE(msc_lend_changed_unseen_is_announced),
    TEST_CASE(msc_writes_reach_the_blocks_addressed),
    TEST_CASE(msc_reads_return_the_blocks_addressed),
    TEST_CASE(msc_transfers_outside_the_medium_are_refused),
    TEST_CASE(msc_partial_blocks_are_refused),
    TEST_CASE(msc_read_failures),
    TEST_CASE(msc_write_failures),
    TEST_CASE(msc_unusable_geometry_is_a_target_failure),
    TEST_CASE(msc_capacity_beyond_32_bits_is_clamped),
    TEST_CASE(msc_removal_found_by_a_transfer),
    TEST_CASE(msc_removal_found_by_a_write_or_lookup),
    TEST_CASE(msc_card_swapped_unseen_is_announced),
    TEST_CASE(msc_eject_withholds_the_medium),
    TEST_CASE(msc_writable_follows_the_device),
};
const size_t msc_disk_test_count = sizeof msc_disk_tests / sizeof msc_disk_tests[0];
