// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//tests for src/storage/diskio.c, the FatFs disk I/O adapter, driven directly through the disk_*()
//functions FatFs calls. expected values come from the mapping and status tables in diskio.c's header
//comment and from FatFs's documented disk I/O contract (third_party/fatfs/documents/doc/dio*.html),
//not from the adapter's code.

#include <string.h>

#include "ff.h"
#include "diskio.h"

#include "fake_block_device.h"
#include "fixture.h"
#include "harness.h"
#include "storage/filesystem_diskio.h"

#define DRIVE FILESYSTEM_DRIVE_SD

//bind the fixture disk to drive 0 after bringing it up; the case must unbind before it ends (the
//runner checks)
static bool bind_ready_disk(void)
{
    return fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK
        && filesystem_diskio_bind(DRIVE, &disk.device)
        && disk_initialize(DRIVE) == 0;
}

static void diskio_bind_rules(void)
{
    block_device_t invalid = { .context = NULL, .operations = disk.device.operations };

    EXPECT(!filesystem_diskio_bind(1, &disk.device));           //FF_VOLUMES is 1
    EXPECT(!filesystem_diskio_bind(DRIVE, NULL));
    EXPECT(!filesystem_diskio_bind(DRIVE, &invalid));
    EXPECT(filesystem_diskio_device(DRIVE) == NULL);

    EXPECT(filesystem_diskio_bind(DRIVE, &disk.device));
    EXPECT(filesystem_diskio_device(DRIVE) == &disk.device);
    EXPECT(!filesystem_diskio_bind(DRIVE, &disk.device));       //already bound
    EXPECT(filesystem_diskio_device(DRIVE) == &disk.device);    //and the refusal changed nothing

    filesystem_diskio_unbind(DRIVE);
    EXPECT(filesystem_diskio_device(DRIVE) == NULL);
    filesystem_diskio_unbind(DRIVE);                            //safe when nothing is bound
    filesystem_diskio_unbind(1);                                //and for a drive that does not exist
    EXPECT(filesystem_diskio_device(1) == NULL);

    EXPECT(filesystem_diskio_bind(DRIVE, &disk.device));        //rebindable after unbind
    filesystem_diskio_unbind(DRIVE);
    EXPECT_EQ_U(fake_disk_io_calls(&disk), 0);                  //binding touches nothing
}

static void diskio_status_before_initialize(void)
{
    EXPECT_EQ_U(disk_status(DRIVE), STA_NOINIT);                //unbound
    EXPECT_EQ_U(disk_status(1), STA_NOINIT);                    //no such drive
    EXPECT_EQ_U(disk_initialize(DRIVE), STA_NOINIT);            //unbound
    EXPECT_EQ_U(disk_initialize(1), STA_NOINIT);

    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE(filesystem_diskio_bind(DRIVE, &disk.device));
    EXPECT_EQ_U(disk_status(DRIVE), STA_NOINIT);                //bound, not yet initialized
    EXPECT_EQ_U(disk_initialize(DRIVE), 0);
    EXPECT_EQ_U(disk_status(DRIVE), 0);

    filesystem_diskio_unbind(DRIVE);
    EXPECT_EQ_U(disk_status(DRIVE), STA_NOINIT);                //again after unbind

    REQUIRE(filesystem_diskio_bind(DRIVE, &disk.device));
    EXPECT_EQ_U(disk_status(DRIVE), STA_NOINIT);                //a fresh bind starts uninitialized
    filesystem_diskio_unbind(DRIVE);
}

//the owner initializes the device; the adapter must never call block_device_init (the SD driver
//refuses a second init, and FatFs calls disk_initialize on every remount)
static void diskio_initialize_never_initializes_the_device(void)
{
    REQUIRE(filesystem_diskio_bind(DRIVE, &disk.device));
    EXPECT_EQ_U(disk_initialize(DRIVE), STA_NOINIT);            //device not brought up by its owner
    EXPECT_EQ_U(disk.init_calls, 0);
    EXPECT(!disk.initialized);

    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT_EQ_U(disk_initialize(DRIVE), 0);
    EXPECT_EQ_U(disk_initialize(DRIVE), 0);                     //repeatable
    EXPECT_EQ_U(disk.init_calls, 1);
    EXPECT_EQ_U(disk.deinit_calls, 0);
    filesystem_diskio_unbind(DRIVE);
}

static void diskio_initialize_checks_geometry(void)
{
    static const struct {
        uint32_t block_size;
        uint64_t block_count;
        bool writable;
        DSTATUS expected;
    } rows[] = {
        { 512U, FIXTURE_DEFAULT_BLOCKS, true, 0 },
        { 512U, 0xFFFFFFFFULL, true, 0 },                       //largest 32-bit sector count
        { 512U, 0x100000000ULL, true, STA_NOINIT },             //needs FF_LBA64
        { 512U, 0U, true, STA_NOINIT },
        { 4096U, FIXTURE_DEFAULT_BLOCKS, true, STA_NOINIT },    //FF_MAX_SS is 512
        { 1024U, FIXTURE_DEFAULT_BLOCKS, true, STA_NOINIT },
        { 512U, FIXTURE_DEFAULT_BLOCKS, false, STA_NOINIT },    //read-only is not a Tome mode
    };

    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        disk.reported_block_size = rows[i].block_size;
        disk.reported_block_count = rows[i].block_count;
        disk.reported_writable = rows[i].writable;
        REQUIRE(filesystem_diskio_bind(DRIVE, &disk.device));
        EXPECT_EQ_U(disk_initialize(DRIVE), rows[i].expected);
        EXPECT_EQ_U(disk_status(DRIVE), rows[i].expected);      //a refusal is not a missing disk
        filesystem_diskio_unbind(DRIVE);
    }
}

static void diskio_read_write_roundtrip(void)
{
    REQUIRE(bind_ready_disk());

    uint8_t out[3 * 512];
    uint8_t in[3 * 512];
    for (uint32_t i = 0; i < sizeof out; i++) {
        out[i] = fixture_pattern(i);
    }
    EXPECT_EQ_U(disk_write(DRIVE, out, 10, 3), RES_OK);
    //the sectors landed where FatFs asked, checked against the backing store rather than read back
    //through the code under test
    EXPECT(memcmp(disk.data + 10U * 512U, out, sizeof out) == 0);
    EXPECT(disk.data[9U * 512U + 511U] == 0 && disk.data[13U * 512U] == 0);

    memset(in, 0, sizeof in);
    EXPECT_EQ_U(disk_read(DRIVE, in, 10, 3), RES_OK);
    EXPECT(memcmp(in, out, sizeof in) == 0);
    memset(in, 0, sizeof in);
    EXPECT_EQ_U(disk_read(DRIVE, in, 11, 1), RES_OK);
    EXPECT(memcmp(in, out + 512, 512) == 0);

    filesystem_diskio_unbind(DRIVE);
}

static void diskio_io_gating(void)
{
    uint8_t buffer[512] = { 0 };

    //no such drive, or a malformed request
    EXPECT_EQ_U(disk_read(1, buffer, 0, 1), RES_PARERR);
    EXPECT_EQ_U(disk_write(1, buffer, 0, 1), RES_PARERR);

    //unbound
    EXPECT_EQ_U(disk_read(DRIVE, buffer, 0, 1), RES_NOTRDY);
    EXPECT_EQ_U(disk_write(DRIVE, buffer, 0, 1), RES_NOTRDY);

    //bound and the device up, but disk_initialize not yet called: refused without touching the device
    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE(filesystem_diskio_bind(DRIVE, &disk.device));
    EXPECT_EQ_U(disk_read(DRIVE, buffer, 0, 1), RES_NOTRDY);
    EXPECT_EQ_U(disk_write(DRIVE, buffer, 0, 1), RES_NOTRDY);
    EXPECT_EQ_U(disk.read_calls + disk.write_calls, 0);

    REQUIRE(disk_initialize(DRIVE) == 0);
    EXPECT_EQ_U(disk_read(DRIVE, NULL, 0, 1), RES_PARERR);
    EXPECT_EQ_U(disk_write(DRIVE, NULL, 0, 1), RES_PARERR);
    EXPECT_EQ_U(disk_read(DRIVE, buffer, 0, 0), RES_PARERR);
    EXPECT_EQ_U(disk_write(DRIVE, buffer, 0, 0), RES_PARERR);
    EXPECT_EQ_U(disk.read_calls + disk.write_calls, 0);
    filesystem_diskio_unbind(DRIVE);
}

//every block_device_result_t, through both disk_read and disk_write: the DRESULT FatFs sees, and the
//status it sees afterwards
static void diskio_result_mapping(void)
{
    static const struct {
        block_device_result_t injected;
        DRESULT expected;
        DSTATUS status_after;
    } rows[] = {
        { BLOCK_DEVICE_RESULT_INVALID_DEVICE, RES_NOTRDY, STA_NOINIT | STA_NODISK },
        { BLOCK_DEVICE_RESULT_NOT_INITIALIZED, RES_NOTRDY, STA_NOINIT },
        { BLOCK_DEVICE_RESULT_OUT_OF_RANGE, RES_PARERR, 0 },
        { BLOCK_DEVICE_RESULT_INVALID_ARGUMENT, RES_PARERR, 0 },
        { BLOCK_DEVICE_RESULT_IO_ERROR, RES_ERROR, 0 },
        { BLOCK_DEVICE_RESULT_BUSY_TIMEOUT, RES_ERROR, 0 },
        { BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED, RES_ERROR, 0 },
    };
    uint8_t buffer[512] = { 0 };

    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    for (int write = 0; write <= 1; write++) {
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
            REQUIRE(filesystem_diskio_bind(DRIVE, &disk.device));
            REQUIRE(disk_initialize(DRIVE) == 0);
            DRESULT result;
            if (write) {
                fake_disk_fail_write(&disk, 0, rows[i].injected);
                result = disk_write(DRIVE, buffer, 0, 1);
            } else {
                fake_disk_fail_read(&disk, 0, rows[i].injected);
                result = disk_read(DRIVE, buffer, 0, 1);
            }
            EXPECT_EQ_U(result, rows[i].expected);
            EXPECT_EQ_U(disk_status(DRIVE), rows[i].status_after);

            //a result that left the drive ready must not stop the next transfer; one that did must
            //refuse it without asking the device
            const unsigned calls_before = disk.read_calls;
            const DRESULT next = disk_read(DRIVE, buffer, 0, 1);
            if (rows[i].status_after == 0) {
                EXPECT_EQ_U(next, RES_OK);
            } else {
                EXPECT_EQ_U(next, RES_NOTRDY);
                EXPECT_EQ_U(disk.read_calls, calls_before);
            }
            filesystem_diskio_unbind(DRIVE);
        }
    }
}

//a removal latches no-disk; nothing clears it until the owner re-initializes the device and FatFs
//re-runs disk_initialize
static void diskio_removal_latches_until_reinitialized(void)
{
    uint8_t buffer[512];
    REQUIRE(bind_ready_disk());

    fake_disk_remove(&disk);
    EXPECT_EQ_U(disk_read(DRIVE, buffer, 0, 1), RES_NOTRDY);
    EXPECT_EQ_U(disk_status(DRIVE), STA_NOINIT | STA_NODISK);
    EXPECT_EQ_U(disk_initialize(DRIVE), STA_NOINIT | STA_NODISK);

    fake_disk_insert(&disk);                                    //back in, but the latch holds
    EXPECT_EQ_U(disk_initialize(DRIVE), STA_NOINIT | STA_NODISK);
    EXPECT_EQ_U(disk_read(DRIVE, buffer, 0, 1), RES_NOTRDY);

    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    EXPECT_EQ_U(disk_status(DRIVE), STA_NOINIT | STA_NODISK);   //not until FatFs asks again
    EXPECT_EQ_U(disk_initialize(DRIVE), 0);
    EXPECT_EQ_U(disk_status(DRIVE), 0);
    EXPECT_EQ_U(disk_read(DRIVE, buffer, 0, 1), RES_OK);
    filesystem_diskio_unbind(DRIVE);
}

//the owner deinitialized the device under a bound drive (not a removal)
static void diskio_deinit_under_bound_drive(void)
{
    uint8_t buffer[512];
    REQUIRE(bind_ready_disk());

    REQUIRE(block_device_deinit(&disk.device) == BLOCK_DEVICE_RESULT_OK);
    EXPECT_EQ_U(disk_read(DRIVE, buffer, 0, 1), RES_NOTRDY);
    EXPECT_EQ_U(disk_status(DRIVE), STA_NOINIT);                //not initialized, but not "no disk"
    EXPECT_EQ_U(disk_initialize(DRIVE), STA_NOINIT);
    EXPECT_EQ_U(disk.init_calls, 1);                            //still only the owner's

    REQUIRE(block_device_init(&disk.device) == BLOCK_DEVICE_RESULT_OK);
    EXPECT_EQ_U(disk_initialize(DRIVE), 0);
    EXPECT_EQ_U(disk_read(DRIVE, buffer, 0, 1), RES_OK);
    filesystem_diskio_unbind(DRIVE);
}

static void diskio_ioctl(void)
{
    LBA_t sectors = 0;
    DWORD erase_block = 0;
    WORD sector_size = 0;

    EXPECT_EQ_U(disk_ioctl(1, CTRL_SYNC, NULL), RES_PARERR);
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_NOTRDY);         //unbound
    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE(filesystem_diskio_bind(DRIVE, &disk.device));
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_NOTRDY);         //not initialized
    EXPECT_EQ_U(disk_ioctl(DRIVE, GET_SECTOR_COUNT, &sectors), RES_NOTRDY);
    REQUIRE(disk_initialize(DRIVE) == 0);

    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_OK);
    EXPECT_EQ_U(disk_ioctl(DRIVE, GET_SECTOR_COUNT, &sectors), RES_OK);
    EXPECT_EQ_U(sectors, FIXTURE_DEFAULT_BLOCKS);
    EXPECT_EQ_U(disk_ioctl(DRIVE, GET_BLOCK_SIZE, &erase_block), RES_OK);
    EXPECT_EQ_U(erase_block, 1);
    EXPECT_EQ_U(disk_ioctl(DRIVE, GET_SECTOR_SIZE, &sector_size), RES_PARERR);
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_TRIM, NULL), RES_PARERR);
    EXPECT_EQ_U(disk_ioctl(DRIVE, 0xEE, NULL), RES_PARERR);
    EXPECT_EQ_U(disk_ioctl(DRIVE, GET_SECTOR_COUNT, NULL), RES_PARERR);
    EXPECT_EQ_U(disk_ioctl(DRIVE, GET_BLOCK_SIZE, NULL), RES_PARERR);

    //the sector count comes from the device at the time of the call, through the result mapping
    fake_disk_fail_info(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    EXPECT_EQ_U(disk_ioctl(DRIVE, GET_SECTOR_COUNT, &sectors), RES_ERROR);
    disk.reported_block_count = 0x100000000ULL;
    EXPECT_EQ_U(disk_ioctl(DRIVE, GET_SECTOR_COUNT, &sectors), RES_ERROR);
    disk.reported_block_count = FIXTURE_DEFAULT_BLOCKS;

    //a sync after a removal must not report success
    fake_disk_remove(&disk);
    EXPECT_EQ_U(disk_ioctl(DRIVE, GET_SECTOR_COUNT, &sectors), RES_NOTRDY);
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_NOTRDY);
    filesystem_diskio_unbind(DRIVE);
}

//CTRL_SYNC asks the device (block_device_sync) and maps its result like a transfer's, so a write the
//device accepted but failed afterwards is never confirmed to FatFs
static void diskio_sync_reaches_the_device(void)
{
    REQUIRE(bind_ready_disk());
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_OK);
    EXPECT_EQ_U(disk.sync_calls, 1);

    fake_disk_fail_sync(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_ERROR);
    EXPECT_EQ_U(disk_status(DRIVE), 0);                         //a failed sync is not a removal
    fake_disk_fail_sync(&disk, 0, BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_ERROR);
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_OK);
    fake_disk_fail_sync(&disk, 0, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_NOTRDY);
    EXPECT_EQ_U(disk_status(DRIVE), STA_NOINIT | STA_NODISK);
    filesystem_diskio_unbind(DRIVE);

    //a device without sync cannot confirm anything
    block_device_operations_t without_sync = *disk.device.operations;
    without_sync.sync = NULL;
    block_device_t no_sync = { .context = &disk, .operations = &without_sync };
    REQUIRE(filesystem_diskio_bind(DRIVE, &no_sync));
    REQUIRE(disk_initialize(DRIVE) == 0);
    EXPECT_EQ_U(disk_ioctl(DRIVE, CTRL_SYNC, NULL), RES_PARERR);
    filesystem_diskio_unbind(DRIVE);
}

const test_case_t diskio_tests[] = {
    TEST_CASE(diskio_bind_rules),
    TEST_CASE(diskio_status_before_initialize),
    TEST_CASE(diskio_initialize_never_initializes_the_device),
    TEST_CASE(diskio_initialize_checks_geometry),
    TEST_CASE(diskio_read_write_roundtrip),
    TEST_CASE(diskio_io_gating),
    TEST_CASE(diskio_result_mapping),
    TEST_CASE(diskio_removal_latches_until_reinitialized),
    TEST_CASE(diskio_deinit_under_bound_drive),
    TEST_CASE(diskio_ioctl),
    TEST_CASE(diskio_sync_reaches_the_device),
};
const size_t diskio_test_count = sizeof diskio_tests / sizeof diskio_tests[0];
