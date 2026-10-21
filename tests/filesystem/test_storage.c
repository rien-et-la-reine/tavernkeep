//tests for src/storage/storage.c, the storage coordinator, through storage.h, with the real filesystem
//layer, adapter and FatFs underneath and the fake card at the bottom
//
//the coordinator is a boot-once singleton (storage_init succeeds once per boot), so these cases rely on
//the runner's default of one process per case. time is whatever each case passes to storage_step, so
//retry timing is exact and independent of the machine.
//
//what is asserted comes from storage.h: the owner/medium state each event leads to, when the epoch
//changes (and that it changes by exactly one), that a request is carried out by a later step, that the
//loss path never touches an absent card, that a new card waits STORAGE_INSERT_SETTLE_MS of continuous
//presence before its first attempt, that a faulty card is retried exactly STORAGE_BRING_UP_ATTEMPTS
//times at the waits in STORAGE_BRING_UP_DELAYS_MS and then left alone, and what an idle step costs. card contents are
//checked through the filesystem layer, and a "USB host" writes through the lent block device.

#include <string.h>

#include "fake_block_device.h"
#include "fixture.h"
#include "harness.h"
#include "storage/filesystem.h"
#include "storage/storage.h"

#define R(x) FILESYSTEM_RESULT_##x
#define LOCAL STORAGE_OWNER_LOCAL
#define USB STORAGE_OWNER_USB
#define NONE STORAGE_OWNER_NONE
#define ABSENT STORAGE_MEDIUM_ABSENT
#define PREPARING STORAGE_MEDIUM_PREPARING
#define FAULTY STORAGE_MEDIUM_FAULTY
#define UNFORMATTED STORAGE_MEDIUM_UNFORMATTED
#define READY STORAGE_MEDIUM_READY

/* ------------------------------------------------------------ helpers */

static uint32_t now;

static void step_at(uint32_t ms)
{
    now = ms;
    storage_step(now);
}

static void step_after(uint32_t ms)
{
    step_at(now + ms);
}

//step past the insertion settle wait of a card the socket has just started reporting: the step at ms
//sees it and only starts the wait, and the step STORAGE_INSERT_SETTLE_MS later makes the first attempt
static void settle_at(uint32_t ms)
{
    step_at(ms);
    step_at(ms + STORAGE_INSERT_SETTLE_MS);
}

static void settle_after(uint32_t ms)
{
    settle_at(now + ms);
}

//the wait before bring-up attempt number attempt (from 2), after the one before it failed (storage.h)
static uint32_t retry_wait(unsigned attempt)
{
    static const uint32_t waits[] = STORAGE_BRING_UP_DELAYS_MS;
    return waits[attempt - 2U];
}

//bring-up attempt number attempt (from 1) on a card: the first attempt on a new card comes after its
//settle wait, and retries follow a settled card after their scheduled wait, without settling again
static void attempt_after(unsigned attempt)
{
    if (attempt == 1U) {
        settle_after(STORAGE_RETRY_INTERVAL_MS);
    } else {
        step_after(retry_wait(attempt));
    }
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

//give a card a FAT volume holding /marker.txt, using the filesystem layer directly (so only before
//storage_init), and leave it uninitialized, as a card freshly put in the socket would be
static bool prepare_card(fake_disk_t *card, const char *marker)
{
    filesystem_file_t file;
    size_t written = 0;
    bool ok = fake_disk_bring_up(card) == BLOCK_DEVICE_RESULT_OK
        && fixture_format(card)
        && filesystem_mount(&card->device) == R(OK)
        && filesystem_open("/marker.txt", FILESYSTEM_OPEN_REPLACE, &file) == R(OK);
    if (ok) {
        ok = filesystem_write(file, marker, strlen(marker), &written) == R(OK) && written == strlen(marker);
        ok = filesystem_close(file) == R(OK) && ok;
    }
    filesystem_unmount();
    ok = block_device_deinit(&card->device) == BLOCK_DEVICE_RESULT_OK && ok;
    reset_counters(card);
    return ok;
}

static bool marker_is(const char *text)
{
    filesystem_file_t file;
    char buffer[64];
    size_t got = 0;
    if (filesystem_open("/marker.txt", FILESYSTEM_OPEN_READ, &file) != R(OK)) {
        return false;
    }
    const bool ok = filesystem_read(file, buffer, sizeof buffer, &got) == R(OK)
        && got == strlen(text) && memcmp(buffer, text, got) == 0;
    return filesystem_close(file) == R(OK) && ok;
}

static bool state_is(storage_owner_t owner, storage_medium_t medium)
{
    return storage_owner() == owner && storage_medium() == medium;
}

//a ready, locally mounted card holding marker "card"
static bool ready_card(void)
{
    if (!prepare_card(&disk, "card") || !storage_init(&disk.device)) {
        return false;
    }
    settle_at(1000);
    return state_is(LOCAL, READY) && filesystem_is_mounted();
}

/* --------------------------------------------------------------- init */

static void storage_before_init_does_nothing(void)
{
    storage_step(0);
    storage_step(100000);
    EXPECT(!storage_request_owner(USB));
    EXPECT(!storage_request_pending());
    EXPECT(storage_usb_device() == NULL);
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT_EQ_U(storage_epoch(), 0);
    EXPECT_EQ_U(fake_disk_io_calls(&disk) + disk.presence_calls + disk.init_calls, 0);
}

static void storage_init_validation(void)
{
    block_device_t no_context = { .context = NULL, .operations = disk.device.operations };
    block_device_operations_t without_presence = *disk.device.operations;
    without_presence.is_present = NULL;
    block_device_t no_presence = { .context = &disk, .operations = &without_presence };
    block_device_operations_t without_sync = *disk.device.operations;
    without_sync.sync = NULL;
    block_device_t no_sync = { .context = &disk, .operations = &without_sync };

    EXPECT(!storage_init(NULL));
    EXPECT(!storage_init(&no_context));
    EXPECT(!storage_init(&no_presence));            //insertion would be invisible
    EXPECT(!storage_init(&no_sync));                //no file close could ever succeed
    EXPECT(storage_init(&disk.device));
    EXPECT(!storage_init(&disk.device));            //once per boot
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT_EQ_U(disk.init_calls, 0);                //nothing happens until the first step
}

/* ------------------------------------------------- insertion and removal */

static void storage_empty_socket_costs_one_presence_check_per_step(void)
{
    disk.present = false;
    REQUIRE(storage_init(&disk.device));
    for (uint32_t i = 0; i < 50; i++) {
        step_at(i * 10U);
    }
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT_EQ_U(disk.init_calls, 0);
    EXPECT_EQ_U(fake_disk_io_calls(&disk), 0);
    EXPECT_EQ_U(disk.presence_calls, 50);
    EXPECT_EQ_U(storage_epoch(), 0);
}

static void storage_card_present_at_boot_is_mounted(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    settle_at(0);
    EXPECT(state_is(LOCAL, READY));                 //init and mount in the one step after the wait
    EXPECT(filesystem_is_mounted());
    EXPECT(marker_is("card"));
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT_EQ_U(storage_epoch(), 0);                //becoming ready invalidates nothing
}

//a card is not touched until the socket has reported it for STORAGE_INSERT_SETTLE_MS: the detect switch
//closes before the contacts seat and before the card has powered up (VAL-H02)
static void storage_new_card_settles_before_bring_up(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    step_at(0);
    EXPECT(state_is(LOCAL, PREPARING));             //seen, and waiting
    EXPECT_EQ_U(disk.init_calls, 0);
    EXPECT_EQ_U(fake_disk_io_calls(&disk), 0);
    step_at(STORAGE_INSERT_SETTLE_MS - 1U);
    EXPECT(state_is(LOCAL, PREPARING));
    EXPECT_EQ_U(disk.init_calls, 0);                //not before the wait is over
    step_at(STORAGE_INSERT_SETTLE_MS);
    EXPECT(state_is(LOCAL, READY));
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT_EQ_U(storage_epoch(), 0);
}

//any empty reading during the wait ends it, and the next reading of a card starts it from the beginning
static void storage_settle_restarts_on_an_empty_reading(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    step_at(0);
    REQUIRE(state_is(LOCAL, PREPARING));
    disk.present = false;                           //contact bounce while sliding in
    step_at(STORAGE_INSERT_SETTLE_MS / 2U);
    EXPECT(state_is(LOCAL, ABSENT));
    disk.present = true;
    step_at(STORAGE_INSERT_SETTLE_MS / 2U + 10U);   //the wait starts again here
    EXPECT(state_is(LOCAL, PREPARING));
    step_at(STORAGE_INSERT_SETTLE_MS);              //due by the first start, not by the restart
    EXPECT_EQ_U(disk.init_calls, 0);
    step_at(STORAGE_INSERT_SETTLE_MS / 2U + 10U + STORAGE_INSERT_SETTLE_MS - 1U);
    EXPECT_EQ_U(disk.init_calls, 0);
    step_at(STORAGE_INSERT_SETTLE_MS / 2U + 10U + STORAGE_INSERT_SETTLE_MS);
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT(state_is(LOCAL, READY));
}

static void storage_settle_timing_survives_clock_wrap(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    step_at(UINT32_MAX - 100U);                     //the wait starts 100 ms before the wrap
    REQUIRE(state_is(LOCAL, PREPARING));
    step_at(UINT32_MAX);
    step_at(STORAGE_INSERT_SETTLE_MS - 102U);       //1 ms short, across the wrap
    EXPECT_EQ_U(disk.init_calls, 0);
    step_at(STORAGE_INSERT_SETTLE_MS - 101U);       //exactly due
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT(state_is(LOCAL, READY));
}

static void storage_insertion_after_boot_is_mounted(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    disk.present = false;
    REQUIRE(storage_init(&disk.device));
    step_at(0);
    step_after(10);
    EXPECT(state_is(LOCAL, ABSENT));
    fake_disk_insert(&disk);
    settle_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT(marker_is("card"));
}

//an idle, ready card costs one status check per step, and the status check does not touch the bus
static void storage_ready_card_costs_one_status_check_per_step(void)
{
    REQUIRE(ready_card());
    reset_counters(&disk);
    for (int i = 0; i < 100; i++) {
        step_after(10);
    }
    EXPECT(state_is(LOCAL, READY));
    EXPECT_EQ_U(disk.info_calls, 100);
    EXPECT_EQ_U(disk.read_calls + disk.write_calls + disk.init_calls + disk.deinit_calls, 0);
}

static void storage_removal_while_ready(void)
{
    REQUIRE(ready_card());
    filesystem_file_t file;
    REQUIRE_RESULT(filesystem_open("/marker.txt", FILESYSTEM_OPEN_READ, &file), R(OK));
    const uint32_t epoch = storage_epoch();

    fake_disk_remove(&disk);
    reset_counters(&disk);
    step_after(10);
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);
    EXPECT(!filesystem_is_mounted());
    EXPECT(!disk.initialized);                      //released, as the driver's removal teardown
    EXPECT_EQ_U(disk.deinit_calls, 1);
    EXPECT_EQ_U(disk.read_calls + disk.write_calls, 0);   //nothing was sent to the absent card
    uint8_t byte;
    size_t n;
    EXPECT_RESULT(filesystem_read(file, &byte, 1, &n), R(STALE_HANDLE));

    //and it stays absent, without trying to bring anything up
    for (int i = 0; i < 20; i++) {
        step_after(100);
    }
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT_EQ_U(disk.init_calls, 0);
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);
}

static void storage_reinsertion_brings_the_card_back(void)
{
    REQUIRE(ready_card());
    fake_disk_remove(&disk);
    step_after(10);
    REQUIRE(state_is(LOCAL, ABSENT));
    const uint32_t epoch = storage_epoch();

    fake_disk_insert(&disk);
    settle_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT(marker_is("card"));
    EXPECT_EQ_U(storage_epoch(), epoch);            //only losing READY changes it
    EXPECT_EQ_U(disk.init_calls, 2);                //boot, and the reinsertion
}

//a different card in the socket after a removal is read afresh, nothing reused from the first
static void storage_swapped_card_is_read_fresh(void)
{
    fake_disk_t other;
    REQUIRE(fake_disk_create(&other, FIXTURE_DEFAULT_BLOCKS));
    const bool prepared = prepare_card(&other, "other");
    REQUIRE(ready_card());
    EXPECT(prepared);
    fake_disk_remove(&disk);
    step_after(10);
    memcpy(disk.data, other.data, (size_t)FIXTURE_DEFAULT_BLOCKS * FAKE_BLOCK_SIZE);
    fake_disk_destroy(&other);
    fake_disk_insert(&disk);
    settle_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT(marker_is("other"));
}

/* ------------------------------------------------ bring-up failures */

//the socket says a card is there but the debounced check refuses it (still being pushed in): never
//counted against the card, and since it is still arriving it settles again from the next step that sees
//it, a full STORAGE_INSERT_SETTLE_MS of presence before the next attempt
static void storage_unseated_card_settles_again_without_counting(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    fake_disk_fail_init(&disk, 0, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    settle_at(0);
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT(state_is(LOCAL, ABSENT));
    for (unsigned round = 1; round <= 10; round++) {
        if (round < 10) {
            fake_disk_fail_init(&disk, 0, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
        }
        step_after(10);                             //seen again: the wait starts here
        EXPECT(state_is(LOCAL, PREPARING));
        EXPECT_EQ_U(disk.init_calls, round);
        const uint32_t start = now;
        step_at(start + STORAGE_INSERT_SETTLE_MS - 1U);
        EXPECT_EQ_U(disk.init_calls, round);        //not before a full wait
        step_at(start + STORAGE_INSERT_SETTLE_MS);
        EXPECT_EQ_U(disk.init_calls, round + 1U);
        if (round < 10) {
            EXPECT(state_is(LOCAL, ABSENT));
        }
    }
    EXPECT(state_is(LOCAL, READY));                 //never declared faulty
    EXPECT(marker_is("card"));
}

static void storage_failing_init_is_retried_then_parked(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    fake_disk_fail_init(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    settle_at(0);
    EXPECT(state_is(LOCAL, PREPARING));
    EXPECT_EQ_U(disk.init_calls, 1);

    //retries follow a settled card, each after its scheduled wait to the millisecond, with no second
    //settle wait
    for (unsigned attempt = 2; attempt <= STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_init(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        step_after(retry_wait(attempt) - 1U);
        EXPECT_EQ_U(disk.init_calls, attempt - 1U); //not yet
        step_after(1);
        EXPECT_EQ_U(disk.init_calls, attempt);
    }
    EXPECT(state_is(LOCAL, FAULTY));

    //left alone while it stays in the socket
    for (int i = 0; i < 100; i++) {
        step_after(100);
    }
    EXPECT_EQ_U(disk.init_calls, STORAGE_BRING_UP_ATTEMPTS);
    EXPECT(state_is(LOCAL, FAULTY));
    EXPECT_EQ_U(storage_epoch(), 0);

    //taking it out and putting it back tries again, with a fresh count
    disk.present = false;
    step_after(10);
    EXPECT(state_is(LOCAL, ABSENT));
    disk.present = true;
    settle_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT(marker_is("card"));
}

//init works but mount fails: only the mount is retried, and a parked card is released
static void storage_failing_mount_is_retried_then_parked(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    for (unsigned attempt = 1; attempt <= STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        attempt_after(attempt);
        EXPECT(!filesystem_is_mounted());
    }
    EXPECT(state_is(LOCAL, FAULTY));
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT(!disk.initialized);
    EXPECT_EQ_U(disk.deinit_calls, 1);
}

//a card the adapter cannot use (here, 4096-byte sectors) makes mount report the medium unavailable,
//exactly as a removal does. it must be parked as faulty, not mistaken for a removal and brought up
//again for ever
static void storage_unusable_card_is_faulty_not_absent(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    disk.reported_block_size = 4096U;
    REQUIRE(storage_init(&disk.device));
    for (unsigned attempt = 0; attempt < 40; attempt++) {             //10 s, past every scheduled attempt
        step_at(attempt * STORAGE_RETRY_INTERVAL_MS);
    }
    EXPECT(state_is(LOCAL, FAULTY));
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT(!disk.initialized);
}

static void storage_removal_between_attempts(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));

    //device down between attempts (init failed): noticed through the socket
    fake_disk_fail_init(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    settle_at(0);
    REQUIRE(state_is(LOCAL, PREPARING));
    fake_disk_remove(&disk);
    step_after(STORAGE_RETRY_INTERVAL_MS);
    EXPECT(state_is(LOCAL, ABSENT));

    //device up between attempts (mount failed): noticed through the latch, at once
    fake_disk_insert(&disk);
    fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    settle_after(STORAGE_RETRY_INTERVAL_MS);
    REQUIRE(state_is(LOCAL, PREPARING));
    REQUIRE(disk.initialized);
    fake_disk_remove(&disk);
    step_after(1);
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT(!disk.initialized);

    //and a reinserted card starts with a fresh count of attempts
    fake_disk_insert(&disk);
    for (unsigned attempt = 1; attempt < STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        attempt_after(attempt);
        EXPECT(state_is(LOCAL, PREPARING));
    }
    step_after(retry_wait(STORAGE_BRING_UP_ATTEMPTS));
    EXPECT(state_is(LOCAL, READY));

    //losing a card that never became ready invalidates nothing
    EXPECT_EQ_U(storage_epoch(), 0);

    //a success clears the count: the two failures above do not count against the mount on the way back
    //from USB, which gets its full allowance again
    REQUIRE(storage_request_owner(USB));
    step_after(10);
    REQUIRE(storage_request_owner(LOCAL));
    for (unsigned attempt = 1; attempt < STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        step_after(attempt == 1U ? 10U : retry_wait(attempt));   //the first is the owner change's mount
        EXPECT(state_is(LOCAL, PREPARING));
    }
    step_after(retry_wait(STORAGE_BRING_UP_ATTEMPTS));
    EXPECT(state_is(LOCAL, READY));
}

//a card pulled and pushed back in between two steps is a new card: the failures counted against the one
//that was pulled do not carry over (nothing in between saw an empty socket to reset the count)
static void storage_quick_reinsertion_is_a_new_card(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    for (unsigned attempt = 1; attempt < STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        attempt_after(attempt);
        REQUIRE(state_is(LOCAL, PREPARING));
    }
    fake_disk_remove(&disk);
    fake_disk_insert(&disk);
    fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    settle_after(STORAGE_RETRY_INTERVAL_MS);        //a new card, so it settles first
    EXPECT(state_is(LOCAL, PREPARING));             //the first failure of the new card, not the last
    EXPECT_EQ_U(disk.init_calls, 2);
    step_after(retry_wait(2));
    EXPECT(state_is(LOCAL, READY));
}

//a card pulled during the mount itself is a removal, not a failed attempt
static void storage_removal_during_mount(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    //the step's own status check is not reached (the device is down); the mount's disk_initialize asks
    //for the card's geometry and finds it gone
    fake_disk_remove_at_info(&disk, 0);
    settle_at(0);
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT(!disk.initialized);
    EXPECT(!filesystem_is_mounted());

    //and it was not counted: STORAGE_BRING_UP_ATTEMPTS - 1 failures after reinsertion still end READY
    fake_disk_insert(&disk);
    for (unsigned attempt = 1; attempt < STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        attempt_after(attempt);
        EXPECT(state_is(LOCAL, PREPARING));
    }
    step_after(retry_wait(STORAGE_BRING_UP_ATTEMPTS));
    EXPECT(state_is(LOCAL, READY));
}

//a card pulled during the mount's sector reads, after disk_initialize found it: FatFs reports the failed
//read as a disk error (STORAGE_ERROR, not MEDIUM_UNAVAILABLE), and it is still a removal, not a failed
//attempt
static void storage_removal_during_mount_reads(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    fake_disk_remove_at_read(&disk, 0);
    settle_at(0);
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT(!disk.initialized);
    EXPECT(!filesystem_is_mounted());
    EXPECT(disk.read_calls > 0);                        //the mount got as far as reading

    //not counted: STORAGE_BRING_UP_ATTEMPTS - 1 failures after reinsertion still end READY
    fake_disk_insert(&disk);
    for (unsigned attempt = 1; attempt < STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        attempt_after(attempt);
        EXPECT(state_is(LOCAL, PREPARING));
    }
    step_after(retry_wait(STORAGE_BRING_UP_ATTEMPTS));
    EXPECT(state_is(LOCAL, READY));
    EXPECT(marker_is("card"));
}

//the device stops answering as initialized without a removal (deinitialized by something other than the
//coordinator): treated as a loss - the volume is dropped and the epoch moves on - and, since the card is
//still in the socket, brought up again. it is treated as a new card and settles first: nothing tells the
//coordinator it is the same one
static void storage_device_deinitialized_behind_its_back(void)
{
    REQUIRE(ready_card());
    filesystem_file_t file;
    REQUIRE_RESULT(filesystem_open("/marker.txt", FILESYSTEM_OPEN_READ, &file), R(OK));
    const uint32_t epoch = storage_epoch();
    REQUIRE(block_device_deinit(&disk.device) == BLOCK_DEVICE_RESULT_OK);
    reset_counters(&disk);
    step_after(10);
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);
    EXPECT(state_is(LOCAL, PREPARING));
    EXPECT_EQ_U(disk.init_calls, 0);
    step_after(STORAGE_INSERT_SETTLE_MS);
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT(state_is(LOCAL, READY));
    uint8_t byte;
    size_t n;
    EXPECT_RESULT(filesystem_read(file, &byte, 1, &n), R(STALE_HANDLE));   //from the old mount
    EXPECT(marker_is("card"));
}

//a parked card that could not be released is released later; a removal is still noticed meanwhile
static void storage_parked_card_release_is_retried(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    for (unsigned attempt = 1; attempt <= STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        if (attempt == STORAGE_BRING_UP_ATTEMPTS) {
            fake_disk_fail_deinit(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
        }
        attempt_after(attempt);
    }
    REQUIRE(state_is(LOCAL, FAULTY));
    EXPECT(disk.initialized);
    EXPECT_EQ_U(disk.deinit_calls, 1);
    step_after(STORAGE_RETRY_INTERVAL_MS - 1U);
    EXPECT_EQ_U(disk.deinit_calls, 1);
    step_after(1);
    EXPECT_EQ_U(disk.deinit_calls, 2);
    EXPECT(!disk.initialized);
    EXPECT(state_is(LOCAL, FAULTY));

    //the same, but the card is pulled while still held: the latch is noticed and the card released
    disk.present = false;
    step_after(10);
    REQUIRE(state_is(LOCAL, ABSENT));
    disk.present = true;
    for (unsigned attempt = 1; attempt <= STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        if (attempt == STORAGE_BRING_UP_ATTEMPTS) {
            fake_disk_fail_deinit(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
        }
        attempt_after(attempt);
    }
    REQUIRE(state_is(LOCAL, FAULTY));
    REQUIRE(disk.initialized);

    //a faulty card is not lent to USB, even while it is still held (the release retry is not yet due)
    REQUIRE(storage_request_owner(USB));
    step_after(1);
    EXPECT(state_is(USB, FAULTY));
    EXPECT(storage_usb_device() == NULL);

    fake_disk_remove(&disk);
    step_after(1);
    EXPECT(state_is(USB, ABSENT));
    EXPECT(!disk.initialized);
}

static void storage_retry_timing_survives_clock_wrap(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    REQUIRE(storage_init(&disk.device));
    fake_disk_fail_init(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    settle_at(UINT32_MAX - 100U - STORAGE_INSERT_SETTLE_MS);   //the first attempt, 100 ms before the wrap
    REQUIRE(state_is(LOCAL, PREPARING));
    REQUIRE(now == UINT32_MAX - 100U);
    step_at(UINT32_MAX);                            //across the wrap, the retry not yet due
    step_at(retry_wait(2) - 102U);
    EXPECT_EQ_U(disk.init_calls, 1);
    step_at(retry_wait(2) - 101U);                  //exactly due
    EXPECT_EQ_U(disk.init_calls, 2);
    EXPECT(state_is(LOCAL, READY));
}

/* --------------------------------------------------------- unformatted */

//the firmware does not format cards. a card with no FAT volume is UNFORMATTED and left alone - not
//retried, not written - until it leaves the socket; one formatted on a computer then mounts as any other
static void storage_blank_card_stays_unformatted(void)
{
    REQUIRE(storage_init(&disk.device));            //the fixture card is blank
    settle_at(0);
    EXPECT(state_is(LOCAL, UNFORMATTED));
    EXPECT(!filesystem_is_mounted());
    EXPECT(!storage_request_pending());
    for (int i = 0; i < 10; i++) {
        step_after(STORAGE_RETRY_INTERVAL_MS);      //not retried: it works, it is just empty
    }
    EXPECT(state_is(LOCAL, UNFORMATTED));
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT_EQ_U(disk.write_calls, 0);
    EXPECT_EQ_U(storage_epoch(), 0);                //it was never READY

    //out, formatted on a computer, back in
    fake_disk_remove(&disk);
    step_after(10);
    EXPECT(state_is(LOCAL, ABSENT));
    fake_disk_insert(&disk);
    REQUIRE(prepare_card(&disk, "computer"));
    settle_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT(marker_is("computer"));
}

/* ------------------------------------------------------- owner changes */

//write a whole card image through a block device, as a USB host would
static bool host_writes_image(block_device_t *device, const uint8_t *image)
{
    for (uint64_t lba = 0; lba < FIXTURE_DEFAULT_BLOCKS; lba += 128U) {
        if (block_device_write_blocks(device, lba, image + lba * FAKE_BLOCK_SIZE, 128U)
                != BLOCK_DEVICE_RESULT_OK) {
            return false;
        }
    }
    return true;
}

static void storage_handover_to_usb_and_back(void)
{
    fake_disk_t other;
    REQUIRE(fake_disk_create(&other, FIXTURE_DEFAULT_BLOCKS));
    const bool prepared = prepare_card(&other, "written by the host");
    REQUIRE(ready_card());
    EXPECT(prepared);
    filesystem_file_t file;
    REQUIRE_RESULT(filesystem_open("/marker.txt", FILESYSTEM_OPEN_READ, &file), R(OK));
    const uint32_t epoch = storage_epoch();

    EXPECT(storage_request_owner(USB));
    EXPECT(storage_request_pending());
    EXPECT(storage_usb_device() == NULL);           //not until the step has done it
    EXPECT(filesystem_is_mounted());
    reset_counters(&disk);
    step_after(10);
    EXPECT(!storage_request_pending());
    EXPECT(state_is(USB, READY));
    EXPECT(storage_usb_device() == &disk.device);
    EXPECT(!filesystem_is_mounted());
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);
    EXPECT_EQ_U(disk.read_calls + disk.write_calls + disk.init_calls + disk.deinit_calls, 0);
    uint8_t byte;
    size_t n;
    EXPECT_RESULT(filesystem_read(file, &byte, 1, &n), R(STALE_HANDLE));

    //the host rewrites the card through the lent device; the coordinator stays out of its way
    EXPECT(host_writes_image(storage_usb_device(), other.data));
    fake_disk_destroy(&other);
    for (int i = 0; i < 10; i++) {
        step_after(10);
    }
    EXPECT(state_is(USB, READY));

    EXPECT(storage_request_owner(LOCAL));
    step_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT(storage_usb_device() == NULL);
    EXPECT_EQ_U(storage_epoch(), epoch + 2U);
    EXPECT(marker_is("written by the host"));       //a fresh mount sees the host's work
    EXPECT_EQ_U(disk.init_calls, 0);                //the card itself was never re-initialized
}

static void storage_usb_owns_cards_in_every_condition(void)
{
    //a blank card is lent as it is (the host can format it) and is unformatted again on return
    REQUIRE(storage_init(&disk.device));
    settle_at(0);
    REQUIRE(state_is(LOCAL, UNFORMATTED));
    REQUIRE(storage_request_owner(USB));
    step_after(10);
    EXPECT(state_is(USB, READY));
    EXPECT(storage_usb_device() == &disk.device);
    REQUIRE(storage_request_owner(LOCAL));
    step_after(10);
    EXPECT(state_is(LOCAL, UNFORMATTED));

    //removal and insertion while USB owns the card: the card is brought up but never mounted
    REQUIRE(storage_request_owner(USB));
    step_after(10);
    const uint32_t epoch = storage_epoch();
    fake_disk_remove(&disk);
    step_after(10);
    EXPECT(state_is(USB, ABSENT));
    EXPECT(storage_usb_device() == NULL);
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);       //the host must be told the medium changed
    fake_disk_insert(&disk);
    settle_after(10);                               //a new card settles whoever owns it
    EXPECT(state_is(USB, READY));
    EXPECT(storage_usb_device() == &disk.device);
    EXPECT(!filesystem_is_mounted());
    EXPECT(disk.initialized);
}

static void storage_owner_changes_with_no_card(void)
{
    disk.present = false;
    REQUIRE(storage_init(&disk.device));
    step_at(0);
    const uint32_t epoch = storage_epoch();
    REQUIRE(storage_request_owner(USB));
    step_after(10);
    EXPECT(state_is(USB, ABSENT));
    EXPECT(storage_usb_device() == NULL);
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);
    REQUIRE(storage_request_owner(LOCAL));
    step_after(10);
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT_EQ_U(disk.init_calls, 0);
}

static void storage_owner_requests(void)
{
    REQUIRE(ready_card());
    const uint32_t epoch = storage_epoch();
    EXPECT(!storage_request_owner((storage_owner_t)7));
    EXPECT(!storage_request_pending());

    //asking for the current owner cancels a pending change
    EXPECT(storage_request_owner(USB));
    EXPECT(storage_request_owner(LOCAL));
    EXPECT(!storage_request_pending());
    step_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT_EQ_U(storage_epoch(), epoch);
    EXPECT(filesystem_is_mounted());

    //asking twice is the same as asking once
    EXPECT(storage_request_owner(USB));
    EXPECT(storage_request_owner(USB));
    step_after(10);
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);
    step_after(10);
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);
}

//a handover asked for in the moment a card arrives: the step that changes the owner does only that, and
//the card is then brought up for USB - never mounted, so never read
static void storage_handover_requested_as_a_card_arrives(void)
{
    REQUIRE(prepare_card(&disk, "card"));
    disk.present = false;
    REQUIRE(storage_init(&disk.device));
    step_at(0);
    REQUIRE(state_is(LOCAL, ABSENT));
    fake_disk_insert(&disk);
    REQUIRE(storage_request_owner(USB));
    step_after(10);
    EXPECT(state_is(USB, ABSENT));
    EXPECT_EQ_U(disk.init_calls, 0);                //one piece of card work per step
    settle_after(10);
    EXPECT(state_is(USB, READY));
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT_EQ_U(disk.read_calls + disk.write_calls, 0);
    EXPECT(!filesystem_is_mounted());
}

//returning to the local owner mounts afresh, and a mount that fails then is retried like any bring-up
static void storage_return_from_usb_retries_a_failing_mount(void)
{
    REQUIRE(ready_card());
    REQUIRE(storage_request_owner(USB));
    step_after(10);
    REQUIRE(storage_request_owner(LOCAL));
    fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    step_after(10);
    EXPECT(state_is(LOCAL, PREPARING));
    EXPECT(storage_usb_device() == NULL);
    step_after(retry_wait(2));
    EXPECT(state_is(LOCAL, READY));
    EXPECT(marker_is("card"));
}

/* ------------------------------------------ release for the peripheral rail */

//the device calls an idle step made, of any kind
static unsigned device_calls(const fake_disk_t *card)
{
    return fake_disk_io_calls(card) + card->presence_calls + card->init_calls + card->deinit_calls;
}

//releasing a ready card unmounts it, deinitializes it cleanly and leaves the coordinator costing nothing;
//reclaiming it treats the card as just inserted: it settles, and is mounted afresh
static void storage_release_and_reclaim_a_ready_card(void)
{
    REQUIRE(ready_card());
    filesystem_file_t file;
    REQUIRE_RESULT(filesystem_open("/marker.txt", FILESYSTEM_OPEN_READ, &file), R(OK));
    const uint32_t epoch = storage_epoch();

    EXPECT(storage_request_owner(NONE));
    EXPECT(storage_request_pending());
    step_after(10);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT(!storage_request_pending());
    EXPECT(!filesystem_is_mounted());
    EXPECT(!disk.initialized);
    EXPECT_EQ_U(disk.deinit_calls, 1);
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);
    uint8_t byte;
    size_t n;
    EXPECT_RESULT(filesystem_read(file, &byte, 1, &n), R(STALE_HANDLE));

    //released: the socket is not watched and nothing touches the device
    reset_counters(&disk);
    for (int i = 0; i < 20; i++) {
        step_after(100);
    }
    EXPECT(state_is(NONE, ABSENT));
    EXPECT_EQ_U(device_calls(&disk), 0);

    //reclaimed: the step that changes the owner does only that, then the card settles and is mounted
    EXPECT(storage_request_owner(LOCAL));
    step_after(10);
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT_EQ_U(storage_epoch(), epoch + 2U);
    EXPECT_EQ_U(disk.init_calls, 0);
    step_after(10);
    EXPECT(state_is(LOCAL, PREPARING));                //the settle wait, which covers the card's power-up
    step_after(STORAGE_INSERT_SETTLE_MS);
    EXPECT(state_is(LOCAL, READY));
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT(marker_is("card"));
    EXPECT_EQ_U(storage_epoch(), epoch + 2U);          //becoming ready invalidates nothing
}

//a card swapped, or inserted, while released is read fresh on reclaim; nothing notices it before
static void storage_card_changed_while_released_is_read_fresh(void)
{
    fake_disk_t other;
    REQUIRE(fake_disk_create(&other, FIXTURE_DEFAULT_BLOCKS));
    const bool prepared = prepare_card(&other, "other");
    REQUIRE(ready_card());
    EXPECT(prepared);
    REQUIRE(storage_request_owner(NONE));
    step_after(10);
    REQUIRE(state_is(NONE, ABSENT));

    fake_disk_remove(&disk);
    step_after(10);
    memcpy(disk.data, other.data, (size_t)FIXTURE_DEFAULT_BLOCKS * FAKE_BLOCK_SIZE);
    fake_disk_destroy(&other);
    fake_disk_insert(&disk);
    reset_counters(&disk);
    step_after(1000);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT_EQ_U(device_calls(&disk), 0);

    REQUIRE(storage_request_owner(LOCAL));
    step_after(10);
    settle_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT(marker_is("other"));
}

//whatever the card was doing, release stops it: no card, a card settling, a card between bring-up
//attempts with its device up, a card lent to USB, an unformatted card
static void storage_release_from_every_condition(void)
{
    //no card: nothing to release, and an insertion while released is not noticed
    REQUIRE(prepare_card(&disk, "card"));
    disk.present = false;
    REQUIRE(storage_init(&disk.device));
    step_at(0);
    REQUIRE(storage_request_owner(NONE));
    step_after(10);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT(!storage_request_pending());
    fake_disk_insert(&disk);
    reset_counters(&disk);
    step_after(1000);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT_EQ_U(device_calls(&disk), 0);

    //a card in its settle wait is dropped before it is ever initialized
    REQUIRE(storage_request_owner(LOCAL));
    step_after(10);
    step_after(10);
    REQUIRE(state_is(LOCAL, PREPARING));
    REQUIRE(storage_request_owner(NONE));
    step_after(10);
    EXPECT(state_is(NONE, ABSENT));
    step_after(STORAGE_INSERT_SETTLE_MS);
    EXPECT_EQ_U(disk.init_calls, 0);

    //a card whose mount failed is up between attempts: it is deinitialized, and its count forgotten
    REQUIRE(storage_request_owner(LOCAL));
    step_after(10);
    fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    settle_after(10);
    REQUIRE(state_is(LOCAL, PREPARING));
    REQUIRE(disk.initialized);
    REQUIRE(storage_request_owner(NONE));
    step_after(10);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT(!disk.initialized);
    REQUIRE(storage_request_owner(LOCAL));
    step_after(10);
    for (unsigned attempt = 1; attempt < STORAGE_BRING_UP_ATTEMPTS; attempt++) {
        fake_disk_fail_read(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        attempt_after(attempt);
    }
    step_after(retry_wait(STORAGE_BRING_UP_ATTEMPTS));
    EXPECT(state_is(LOCAL, READY));                    //a full set of attempts, not what was left of one

    //a card lent to USB is taken back from the host
    REQUIRE(storage_request_owner(USB));
    step_after(10);
    REQUIRE(state_is(USB, READY));
    const uint32_t epoch = storage_epoch();
    REQUIRE(storage_request_owner(NONE));
    step_after(10);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT(storage_usb_device() == NULL);
    EXPECT(!disk.initialized);
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);           //the host must be told the medium went away
}

static void storage_release_of_an_unformatted_card(void)
{
    REQUIRE(storage_init(&disk.device));
    settle_at(0);
    REQUIRE(state_is(LOCAL, UNFORMATTED));
    REQUIRE(storage_request_owner(NONE));
    step_after(10);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT(!disk.initialized);
    REQUIRE(storage_request_owner(LOCAL));
    step_after(10);
    settle_after(10);
    EXPECT(state_is(LOCAL, UNFORMATTED));
}

//a card stuck busy is released once it finishes: the request stays pending until then, and the clean
//deinit is retried at STORAGE_RETRY_INTERVAL_MS
static void storage_release_waits_out_a_busy_card(void)
{
    REQUIRE(ready_card());
    fake_disk_fail_deinit(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
    REQUIRE(storage_request_owner(NONE));
    reset_counters(&disk);
    step_after(10);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT(storage_request_pending());                  //the rail must not be cut yet
    EXPECT(disk.initialized);
    step_after(STORAGE_RETRY_INTERVAL_MS - 1U);
    EXPECT_EQ_U(disk.deinit_calls, 1);
    step_after(1);
    EXPECT_EQ_U(disk.deinit_calls, 2);
    EXPECT(!disk.initialized);
    EXPECT(!storage_request_pending());
    EXPECT(state_is(NONE, ABSENT));
}

//a card that stays busy through STORAGE_RELEASE_ATTEMPTS clean deinits is given up: reported FAULTY and
//still held, the request no longer pending, and no more attempts. what happens next is outside it: a
//removal releases it, and a reclaim tries once more before treating it as any faulty card
static void storage_release_given_up_after_its_attempts(void)
{
    REQUIRE(ready_card());
    REQUIRE(storage_request_owner(NONE));
    reset_counters(&disk);
    for (unsigned attempt = 1; attempt <= STORAGE_RELEASE_ATTEMPTS; attempt++) {
        fake_disk_fail_deinit(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
        step_after(attempt == 1U ? 10U : STORAGE_RETRY_INTERVAL_MS);
        EXPECT_EQ_U(disk.deinit_calls, attempt);
        if (attempt < STORAGE_RELEASE_ATTEMPTS) {
            EXPECT(storage_request_pending());
            EXPECT(state_is(NONE, ABSENT));
        }
    }
    EXPECT(state_is(NONE, FAULTY));
    EXPECT(!storage_request_pending());
    EXPECT(disk.initialized);
    for (int i = 0; i < 10; i++) {
        step_after(STORAGE_RETRY_INTERVAL_MS);
    }
    EXPECT_EQ_U(disk.deinit_calls, STORAGE_RELEASE_ATTEMPTS);
    EXPECT_EQ_U(disk.presence_calls, 0);

    //reclaimed while still held: let go now that it is no longer busy, then brought up afresh
    REQUIRE(storage_request_owner(LOCAL));
    step_after(10);
    EXPECT(state_is(LOCAL, ABSENT));
    EXPECT(!disk.initialized);
    settle_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT(marker_is("card"));

    //given up again, then removed while held: the latch releases it, as with any removal
    REQUIRE(storage_request_owner(NONE));
    for (unsigned attempt = 1; attempt <= STORAGE_RELEASE_ATTEMPTS; attempt++) {
        fake_disk_fail_deinit(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
        step_after(attempt == 1U ? 10U : STORAGE_RETRY_INTERVAL_MS);
    }
    REQUIRE(state_is(NONE, FAULTY));
    fake_disk_remove(&disk);
    step_after(10);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT(!disk.initialized);
    EXPECT(!storage_request_pending());
}

//a card still held when reclaimed, and still busy: faulty for the new owner, released when it can be
static void storage_reclaim_of_a_card_still_busy_is_faulty(void)
{
    REQUIRE(ready_card());
    REQUIRE(storage_request_owner(NONE));
    for (unsigned attempt = 1; attempt <= STORAGE_RELEASE_ATTEMPTS; attempt++) {
        fake_disk_fail_deinit(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
        step_after(attempt == 1U ? 10U : STORAGE_RETRY_INTERVAL_MS);
    }
    REQUIRE(state_is(NONE, FAULTY));
    fake_disk_fail_deinit(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
    REQUIRE(storage_request_owner(LOCAL));
    step_after(10);
    EXPECT(state_is(LOCAL, FAULTY));
    EXPECT(disk.initialized);
    step_after(STORAGE_RETRY_INTERVAL_MS);
    EXPECT(!disk.initialized);                          //handle_faulty's retry let go of it
    EXPECT(state_is(LOCAL, FAULTY));                    //and, as any faulty card, it waits to be removed
}

//while released nothing else may use the card, and requests behave as for any owner
static void storage_requests_while_released(void)
{
    REQUIRE(ready_card());
    const uint32_t epoch = storage_epoch();

    //asking for NONE and back before a step has run changes nothing
    EXPECT(storage_request_owner(NONE));
    EXPECT(storage_request_owner(LOCAL));
    EXPECT(!storage_request_pending());
    step_after(10);
    EXPECT(state_is(LOCAL, READY));
    EXPECT_EQ_U(storage_epoch(), epoch);

    REQUIRE(storage_request_owner(NONE));
    step_after(10);
    EXPECT(state_is(NONE, ABSENT));
    EXPECT(!storage_request_pending());

    //no USB device, and asking for NONE again is no change
    EXPECT(storage_usb_device() == NULL);
    EXPECT(storage_request_owner(NONE));
    EXPECT(!storage_request_pending());
    step_after(10);
    EXPECT_EQ_U(storage_epoch(), epoch + 1U);
}

const test_case_t storage_tests[] = {
    TEST_CASE(storage_before_init_does_nothing),
    TEST_CASE(storage_init_validation),
    TEST_CASE(storage_empty_socket_costs_one_presence_check_per_step),
    TEST_CASE(storage_card_present_at_boot_is_mounted),
    TEST_CASE(storage_new_card_settles_before_bring_up),
    TEST_CASE(storage_settle_restarts_on_an_empty_reading),
    TEST_CASE(storage_settle_timing_survives_clock_wrap),
    TEST_CASE(storage_insertion_after_boot_is_mounted),
    TEST_CASE(storage_ready_card_costs_one_status_check_per_step),
    TEST_CASE(storage_removal_while_ready),
    TEST_CASE(storage_reinsertion_brings_the_card_back),
    TEST_CASE(storage_swapped_card_is_read_fresh),
    TEST_CASE(storage_unseated_card_settles_again_without_counting),
    TEST_CASE(storage_failing_init_is_retried_then_parked),
    TEST_CASE(storage_failing_mount_is_retried_then_parked),
    TEST_CASE(storage_unusable_card_is_faulty_not_absent),
    TEST_CASE(storage_removal_between_attempts),
    TEST_CASE(storage_quick_reinsertion_is_a_new_card),
    TEST_CASE(storage_removal_during_mount),
    TEST_CASE(storage_removal_during_mount_reads),
    TEST_CASE(storage_device_deinitialized_behind_its_back),
    TEST_CASE(storage_parked_card_release_is_retried),
    TEST_CASE(storage_retry_timing_survives_clock_wrap),
    TEST_CASE(storage_blank_card_stays_unformatted),
    TEST_CASE(storage_handover_to_usb_and_back),
    TEST_CASE(storage_usb_owns_cards_in_every_condition),
    TEST_CASE(storage_owner_changes_with_no_card),
    TEST_CASE(storage_owner_requests),
    TEST_CASE(storage_handover_requested_as_a_card_arrives),
    TEST_CASE(storage_return_from_usb_retries_a_failing_mount),
    TEST_CASE(storage_release_and_reclaim_a_ready_card),
    TEST_CASE(storage_card_changed_while_released_is_read_fresh),
    TEST_CASE(storage_release_from_every_condition),
    TEST_CASE(storage_release_of_an_unformatted_card),
    TEST_CASE(storage_release_waits_out_a_busy_card),
    TEST_CASE(storage_release_given_up_after_its_attempts),
    TEST_CASE(storage_reclaim_of_a_card_still_busy_is_faulty),
    TEST_CASE(storage_requests_while_released),
};
const size_t storage_test_count = sizeof storage_tests / sizeof storage_tests[0];
