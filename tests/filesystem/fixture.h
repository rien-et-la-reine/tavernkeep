#ifndef TAVERNKEEP_TESTS_FILESYSTEM_FIXTURE_H
#define TAVERNKEEP_TESTS_FILESYSTEM_FIXTURE_H

//per-case fixture shared by the test files. before every case the runner unmounts the layer, replaces
//`disk` with a fresh, zero-filled, present but uninitialized card of FIXTURE_DEFAULT_BLOCKS, makes the
//clock invalid and clears the captured log. after every case it unmounts and checks that the adapter
//was left unbound.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"
#include "fake_block_device.h"
#include "harness.h"
#include "storage/filesystem_diskio.h"

#define FIXTURE_DEFAULT_BLOCKS (64U * 1024U)    //32 MiB: FatFs formats it as FAT16

extern fake_disk_t disk;

//replace `disk` with a fresh card of a different size (call before bringing it up)
void fixture_resize_disk(uint64_t blocks);

//a new, empty FAT volume on a card already brought up, as a computer would make one: the firmware has no
//format, and f_mkfs is built only here (FF_USE_MKFS 1 in this build). FatFs picks FAT12/16 or FAT32 by
//size, behind an MBR partition table
static inline bool fixture_format(fake_disk_t *card)
{
    if (!filesystem_diskio_bind(FILESYSTEM_DRIVE_SD, &card->device)) {
        return false;
    }
    static const MKFS_PARM params = { .fmt = FM_FAT | FM_FAT32 };
    BYTE work[FF_MAX_SS];
    const FRESULT result = f_mkfs("", &params, work, sizeof work);
    filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);
    return result == FR_OK;
}

//deterministic content: byte i of a test file. 251 is prime, so the pattern does not repeat on any
//power-of-two boundary and a misplaced sector cannot compare equal
static inline uint8_t fixture_pattern(uint32_t i)
{
    return (uint8_t)((i * 131U + 7U) % 251U);
}

extern const test_case_t diskio_tests[];
extern const size_t diskio_test_count;
extern const test_case_t filesystem_tests[];
extern const size_t filesystem_test_count;
extern const test_case_t storage_tests[];
extern const size_t storage_test_count;
extern const test_case_t save_point_tests[];
extern const size_t save_point_test_count;

#endif
