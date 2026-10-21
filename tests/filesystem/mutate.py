#!/usr/bin/env python3
"""Mutation testing for the storage-layer tests.

Each mutation plants one deliberate bug in a copy of src/storage/filesystem.c, diskio.c or storage.c, or
src/usb/msc_disk.c, rebuilds fs_tests against the copy and runs it. A mutation the suite fails on is "killed"; one it still
passes is a SURVIVOR, meaning the suite cannot detect that bug. The sources in the repository are never
modified.

    python3 tests/filesystem/mutate.py [--build-dir DIR] [--only SUBSTRING] [--keep-going]

Exit status is 0 only if the unmutated suite passes and every mutation is killed. A mutation whose
target text no longer appears exactly once is reported as STALE (the code changed under it; update
the list), and one that no longer compiles as NOT-COMPILED; both also fail the run, since either way
the bug it describes is no longer being tested.

Mutations known to be equivalent (no observable behaviour changes, so no test could kill them) are not
in the list; they are recorded at the end of this file with the reason.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
FILESYSTEM = "filesystem.c"
DISKIO = "diskio.c"
STORAGE = "storage.c"
MSC_DISK = "msc_disk.c"
SOURCES = {
    FILESYSTEM: os.path.join(ROOT, "src", "storage", "filesystem.c"),
    DISKIO: os.path.join(ROOT, "src", "storage", "diskio.c"),
    STORAGE: os.path.join(ROOT, "src", "storage", "storage.c"),
    MSC_DISK: os.path.join(ROOT, "src", "usb", "msc_disk.c"),
}

# (file, description, old text, new text)
MUTATIONS = [
    # ------------------------------------------------------------------ diskio.c: result mapping
    (DISKIO, "INVALID_DEVICE does not mark the drive no-disk",
     "        drive->no_disk = true;\n        return RES_NOTRDY;",
     "        return RES_NOTRDY;"),
    (DISKIO, "INVALID_DEVICE maps to RES_ERROR",
     "        drive->no_disk = true;\n        return RES_NOTRDY;",
     "        drive->no_disk = true;\n        return RES_ERROR;"),
    (DISKIO, "NOT_INITIALIZED does not clear initialized",
     "    case BLOCK_DEVICE_RESULT_NOT_INITIALIZED:\n        drive->initialized = false;\n",
     "    case BLOCK_DEVICE_RESULT_NOT_INITIALIZED:\n"),
    (DISKIO, "OUT_OF_RANGE and INVALID_ARGUMENT map to RES_ERROR",
     "    case BLOCK_DEVICE_RESULT_INVALID_ARGUMENT:\n        return RES_PARERR;",
     "    case BLOCK_DEVICE_RESULT_INVALID_ARGUMENT:\n        return RES_ERROR;"),
    (DISKIO, "BUSY_TIMEOUT treated as a removal",
     "    case BLOCK_DEVICE_RESULT_IO_ERROR:\n    case BLOCK_DEVICE_RESULT_BUSY_TIMEOUT:\n",
     "    case BLOCK_DEVICE_RESULT_BUSY_TIMEOUT:\n        drive->no_disk = true;\n        return RES_NOTRDY;\n"
     "    case BLOCK_DEVICE_RESULT_IO_ERROR:\n"),
    (DISKIO, "IO_ERROR maps to RES_OK",
     "    case BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED:\n        return RES_ERROR;",
     "    case BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED:\n        return result == BLOCK_DEVICE_RESULT_IO_ERROR"
     " ? RES_OK : RES_ERROR;"),

    # ------------------------------------------------------------------ diskio.c: binding
    (DISKIO, "bind accepts an already-bound drive",
     " || drives[drive].device != NULL) {", ") {"),
    (DISKIO, "bind accepts an invalid device",
     " || !block_device_is_valid(device) ||", " ||"),
    (DISKIO, "bind accepts drive number FF_VOLUMES",
     "    if (drive >= FF_VOLUMES || !block_device_is_valid",
     "    if (drive > FF_VOLUMES || !block_device_is_valid"),
    (DISKIO, "unbind does nothing",
     "        drives[drive] = (diskio_drive_t){ .device = NULL };", "        (void)drives;"),
    (DISKIO, "device() ignores the binding",
     "    return drive < FF_VOLUMES ? drives[drive].device : NULL;", "    (void)drive;\n    return NULL;"),

    # ------------------------------------------------------------------ diskio.c: status and initialize
    (DISKIO, "no-disk reported without STA_NOINIT",
     "        return STA_NOINIT | STA_NODISK;", "        return STA_NODISK;"),
    (DISKIO, "status ignores initialized",
     "    return drive->initialized ? 0 : STA_NOINIT;", "    return 0;"),
    (DISKIO, "disk_initialize calls block_device_init",
     "    block_device_info_t info;\n    if (map_result(drive, block_device_get_info",
     "    (void)block_device_init(drive->device);\n    block_device_info_t info;\n"
     "    if (map_result(drive, block_device_get_info"),
    (DISKIO, "disk_initialize does not clear no_disk",
     "    drive->initialized = true;\n    drive->no_disk = false;", "    drive->initialized = true;"),
    (DISKIO, "disk_initialize skips the device check",
     "    if (map_result(drive, block_device_get_info(drive->device, &info)) != RES_OK) {",
     "    info = (block_device_info_t){ 512U, 1U << 20, true };\n    if (0) {"),
    (DISKIO, "block size not checked",
     "info.block_size_bytes != FF_MAX_SS || ", ""),
    (DISKIO, "zero block count accepted",
     " || info.block_count == 0U\n", "\n"),
    (DISKIO, "block count above 32 bits accepted at initialize",
     "            || info.block_count > UINT32_MAX || !info.writable) {",
     "            || !info.writable) {"),
    (DISKIO, "read-only device accepted",
     " || !info.writable) {", ") {"),

    # ------------------------------------------------------------------ diskio.c: transfers and ioctl
    (DISKIO, "drive_ready ignores no_disk",
     " && drive->initialized && !drive->no_disk;", " && drive->initialized;"),
    (DISKIO, "drive_ready ignores initialized",
     " && drive->initialized && !drive->no_disk;", " && !drive->no_disk;"),
    (DISKIO, "disk_read off by one sector",
     "block_device_read_blocks(drive->device, sector, buff, count)",
     "block_device_read_blocks(drive->device, sector + 1U, buff, count)"),
    (DISKIO, "disk_read reads one sector only",
     "block_device_read_blocks(drive->device, sector, buff, count)",
     "block_device_read_blocks(drive->device, sector, buff, 1U)"),
    (DISKIO, "disk_write off by one sector",
     "block_device_write_blocks(drive->device, sector, buff, count)",
     "block_device_write_blocks(drive->device, sector + 1U, buff, count)"),
    (DISKIO, "disk_write writes one sector only",
     "block_device_write_blocks(drive->device, sector, buff, count)",
     "block_device_write_blocks(drive->device, sector, buff, 1U)"),
    (DISKIO, "disk_write not gated by readiness",
     "    if (!drive_ready(drive)) {\n        return RES_NOTRDY;\n    }\n    //CMD24",
     "    //CMD24"),
    (DISKIO, "disk_ioctl not gated by readiness",
     "    if (!drive_ready(drive)) {\n        return RES_NOTRDY;\n    }\n\n    switch (cmd) {",
     "    switch (cmd) {"),
    (DISKIO, "CTRL_SYNC does not ask the device",
     "        return map_result(drive, block_device_sync(drive->device));",
     "        return RES_OK;"),
    (DISKIO, "CTRL_SYNC fails",
     "        return map_result(drive, block_device_sync(drive->device));",
     "        return RES_ERROR;"),
    (DISKIO, "GET_SECTOR_COUNT one short",
     "        *(LBA_t *)buff = (LBA_t)info.block_count;",
     "        *(LBA_t *)buff = (LBA_t)info.block_count - 1U;"),
    (DISKIO, "GET_SECTOR_COUNT does not check 32 bits",
     "        if (info.block_count > UINT32_MAX) {\n            return RES_ERROR;\n        }\n", ""),
    (DISKIO, "GET_BLOCK_SIZE reports 8",
     "        *(DWORD *)buff = 1U;", "        *(DWORD *)buff = 8U;"),
    (DISKIO, "unknown ioctl succeeds",
     "        //GET_SECTOR_COUNT and GET_BLOCK_SIZE need FF_USE_MKFS\n"
     "        return RES_PARERR;",
     "        return RES_OK;"),

    # ------------------------------------------------------------------ filesystem.c: result mapping
    (FILESYSTEM, "FR_NOT_READY maps to STORAGE_ERROR",
     "    case FR_NOT_READY:\n        return FILESYSTEM_RESULT_MEDIUM_UNAVAILABLE;",
     "    case FR_NOT_READY:\n        return FILESYSTEM_RESULT_STORAGE_ERROR;"),
    (FILESYSTEM, "FR_NO_FILESYSTEM maps to STORAGE_ERROR",
     "    case FR_NO_FILESYSTEM:\n        return FILESYSTEM_RESULT_NO_FILESYSTEM;",
     "    case FR_NO_FILESYSTEM:\n        return FILESYSTEM_RESULT_STORAGE_ERROR;"),
    (FILESYSTEM, "FR_NO_PATH maps to DENIED",
     "    case FR_NO_FILE:\n    case FR_NO_PATH:\n        return FILESYSTEM_RESULT_NOT_FOUND;",
     "    case FR_NO_FILE:\n        return FILESYSTEM_RESULT_NOT_FOUND;\n    case FR_NO_PATH:\n"
     "        return FILESYSTEM_RESULT_DENIED;"),
    (FILESYSTEM, "FR_EXIST maps to DENIED",
     "    case FR_EXIST:\n        return FILESYSTEM_RESULT_EXISTS;",
     "    case FR_EXIST:\n        return FILESYSTEM_RESULT_DENIED;"),
    (FILESYSTEM, "FR_DENIED maps to STORAGE_ERROR",
     "    case FR_DENIED:\n    case FR_WRITE_PROTECTED:\n        return FILESYSTEM_RESULT_DENIED;",
     "    case FR_WRITE_PROTECTED:\n        return FILESYSTEM_RESULT_DENIED;\n"
     "    case FR_DENIED:\n        return FILESYSTEM_RESULT_STORAGE_ERROR;"),
    (FILESYSTEM, "FR_LOCKED maps to DENIED",
     "    case FR_LOCKED:\n        return FILESYSTEM_RESULT_IN_USE;",
     "    case FR_LOCKED:\n        return FILESYSTEM_RESULT_DENIED;"),
    (FILESYSTEM, "a failed close keeps the file's open entry",
     "        slot->fil.flag &= (BYTE)~FATFS_FILE_MODIFIED;\n        (void)f_close(&slot->fil);\n",
     ""),
    (FILESYSTEM, "removing a directory being listed is allowed",
     "        const bool same = probe.obj.sclust == dirs[i].dir.obj.sclust;",
     "        const bool same = false;"),
    (FILESYSTEM, "a failed filtered opendir keeps the directory open",
     "            if (slot->dir.obj.fs != NULL) {\n                (void)f_closedir(&slot->dir);\n            }\n",
     ""),
    (FILESYSTEM, "FR_INVALID_NAME maps to NOT_FOUND",
     "    case FR_INVALID_NAME:\n    case FR_INVALID_PARAMETER:\n        return FILESYSTEM_RESULT_INVALID_ARGUMENT;",
     "    case FR_INVALID_PARAMETER:\n        return FILESYSTEM_RESULT_INVALID_ARGUMENT;\n"
     "    case FR_INVALID_NAME:\n        return FILESYSTEM_RESULT_NOT_FOUND;"),
    (FILESYSTEM, "FR_INVALID_OBJECT maps to STORAGE_ERROR",
     "    case FR_INVALID_OBJECT:\n        return FILESYSTEM_RESULT_STALE_HANDLE;",
     "    case FR_INVALID_OBJECT:\n        return FILESYSTEM_RESULT_STORAGE_ERROR;"),
    (FILESYSTEM, "FR_DISK_ERR maps to MEDIUM_UNAVAILABLE",
     "    case FR_DISK_ERR:\n    case FR_INT_ERR:\n",
     "    case FR_DISK_ERR:\n        return FILESYSTEM_RESULT_MEDIUM_UNAVAILABLE;\n    case FR_INT_ERR:\n"),
    (FILESYSTEM, "FR_OK maps to STORAGE_ERROR",
     "    case FR_OK:\n        return FILESYSTEM_RESULT_OK;",
     "    case FR_OK:\n        return FILESYSTEM_RESULT_STORAGE_ERROR;"),

    # ------------------------------------------------------------------ filesystem.c: handles
    (FILESYSTEM, "generation may wrap to 0",
     "    return generation == 0U ? 1U : generation;", "    return generation;"),
    (FILESYSTEM, "generation never changes",
     "    generation = (uint16_t)(generation + 1U);\n    return generation == 0U ? 1U : generation;",
     "    (void)generation;\n    return 1U;"),
    (FILESYSTEM, "file slot bound off by one",
     "    if (file.slot >= FILESYSTEM_MAX_OPEN_FILES || file.generation == 0U) {",
     "    if (file.slot > FILESYSTEM_MAX_OPEN_FILES || file.generation == 0U) {"),
    (FILESYSTEM, "file handle generation not compared",
     "    if (!slot->in_use || slot->generation != file.generation) {",
     "    if (!slot->in_use) {"),
    (FILESYSTEM, "dir handle generation not compared",
     "    if (!slot->in_use || slot->generation != dir.generation) {",
     "    if (!slot->in_use) {"),
    (FILESYSTEM, "file slot in_use not checked",
     "    if (!slot->in_use || slot->generation != file.generation) {",
     "    if (slot->generation != file.generation) {"),
    (FILESYSTEM, "closing does not free the file slot",
     "static void release_file_slot(file_slot_t *slot)\n{\n    slot->in_use = false;\n",
     "static void release_file_slot(file_slot_t *slot)\n{\n"),
    (FILESYSTEM, "closing does not free the dir slot",
     "static void release_dir_slot(dir_slot_t *slot)\n{\n    slot->in_use = false;\n",
     "static void release_dir_slot(dir_slot_t *slot)\n{\n"),
    (FILESYSTEM, "open does not advance the generation",
     "        slot->generation = next_generation(slot->generation);\n        slot->in_use = true;\n"
     "        *file = ",
     "        slot->in_use = true;\n        *file = "),
    (FILESYSTEM, "open does not mark the slot in use",
     "        slot->generation = next_generation(slot->generation);\n        slot->in_use = true;\n"
     "        *file = ",
     "        slot->generation = next_generation(slot->generation);\n        *file = "),
    (FILESYSTEM, "opendir does not mark the slot in use",
     "        slot->generation = next_generation(slot->generation);\n        slot->in_use = true;\n"
     "        *dir = ",
     "        slot->generation = next_generation(slot->generation);\n        *dir = "),
    (FILESYSTEM, "close keeps the slot when f_close fails",
     "    FRESULT result = f_close(&slot->fil);\n", "    FRESULT result = f_close(&slot->fil);\n"
     "    if (result != FR_OK) {\n        return map_result(result);\n    }\n"),
    (FILESYSTEM, "close does not retry a failed flush",
     "        slot->fil.flag |= (BYTE)FATFS_FILE_MODIFIED;\n        if (f_close(&slot->fil) == FR_OK) {\n"
     "            result = FR_OK;\n        }\n",
     "        (void)0;\n"),
    (FILESYSTEM, "a failed retry reports its own error instead of the first",
     "        if (f_close(&slot->fil) == FR_OK) {\n            result = FR_OK;\n        }\n",
     "        result = f_close(&slot->fil);\n"),
    (FILESYSTEM, "close retries without marking the file modified again",
     "        slot->fil.flag |= (BYTE)FATFS_FILE_MODIFIED;\n", ""),

    # ------------------------------------------------------------------ filesystem.c: volume
    (FILESYSTEM, "mount is lazy (f_mount opt 0)",
     "    const FRESULT result = f_mount(&volume, \"\", 1);", "    const FRESULT result = f_mount(&volume, \"\", 0);"),
    (FILESYSTEM, "failed mount stays bound",
     "        (void)f_mount(NULL, \"\", 0);\n        filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);\n"
     "        return map_result(result);",
     "        (void)f_mount(NULL, \"\", 0);\n        return map_result(result);"),
    (FILESYSTEM, "failed mount stays registered with FatFs",
     "        (void)f_mount(NULL, \"\", 0);\n        filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);\n"
     "        return map_result(result);",
     "        filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);\n        return map_result(result);"),
    (FILESYSTEM, "mount does not refuse while mounted",
     "    if (mounted) {\n        return FILESYSTEM_RESULT_ALREADY_MOUNTED;\n    }\n    //the device is known valid",
     "    //the device is known valid"),
    (FILESYSTEM, "mount does not validate the device",
     "filesystem_result_t filesystem_mount(block_device_t *new_device)\n{\n"
     "    if (!block_device_is_valid(new_device)) {",
     "filesystem_result_t filesystem_mount(block_device_t *new_device)\n{\n"
     "    if (new_device == NULL) {"),
    (FILESYSTEM, "mount never sets mounted",
     "    mounted = true;\n    return FILESYSTEM_RESULT_OK;", "    return FILESYSTEM_RESULT_OK;"),
    (FILESYSTEM, "unmount closes files (syncs)",
     "        release_file_slot(&files[i]);\n    }\n    for",
     "        if (files[i].in_use) {\n            (void)f_close(&files[i].fil);\n        }\n"
     "        release_file_slot(&files[i]);\n    }\n    for"),
    (FILESYSTEM, "unmount leaves file slots in use",
     "        release_file_slot(&files[i]);\n    }\n    for", "    }\n    for"),
    (FILESYSTEM, "unmount leaves dir slots in use",
     "        release_dir_slot(&dirs[i]);\n", ""),
    (FILESYSTEM, "unmount stays bound",
     "    (void)f_mount(NULL, \"\", 0);\n    filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);\n    mounted = false;",
     "    (void)f_mount(NULL, \"\", 0);\n    mounted = false;"),
    (FILESYSTEM, "unmount stays registered with FatFs",
     "    (void)f_mount(NULL, \"\", 0);\n    filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);\n    mounted = false;",
     "    filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);\n    mounted = false;"),
    (FILESYSTEM, "unmount leaves mounted set",
     "    filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);\n    mounted = false;",
     "    filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);"),
    (FILESYSTEM, "unmount warns about readers too",
     "        if (files[i].in_use && (files[i].fil.flag & FA_WRITE) != 0U) {",
     "        if (files[i].in_use) {"),
    (FILESYSTEM, "unmount logs the dropped writer below warning level",
     "            DEBUG_WARN(\"filesystem: unmount dropped", "            DEBUG_INFO(\"filesystem: unmount dropped"),
    (FILESYSTEM, "label buffer one byte short accepted",
     "    if (label == NULL || label_size < FILESYSTEM_LABEL_SIZE) {",
     "    if (label == NULL || label_size < FILESYSTEM_LABEL_SIZE - 1U) {"),

    # ------------------------------------------------------------------ filesystem.c: files
    (FILESYSTEM, "REPLACE does not truncate",
     "        fatfs_mode = FA_WRITE | FA_CREATE_ALWAYS;", "        fatfs_mode = FA_WRITE | FA_OPEN_ALWAYS;"),
    (FILESYSTEM, "REPLACE is also readable",
     "        fatfs_mode = FA_WRITE | FA_CREATE_ALWAYS;", "        fatfs_mode = FA_READ | FA_WRITE | FA_CREATE_ALWAYS;"),
    (FILESYSTEM, "READ is also writable",
     "        fatfs_mode = FA_READ;\n", "        fatfs_mode = FA_READ | FA_WRITE;\n"),
    (FILESYSTEM, "unknown open mode treated as READ",
     "    } else {\n        return FILESYSTEM_RESULT_INVALID_ARGUMENT;\n    }\n    if (path == NULL || file == NULL) {",
     "    } else {\n        fatfs_mode = FA_READ;\n    }\n    if (path == NULL || file == NULL) {"),
    (FILESYSTEM, "open does not check mounted",
     "    if (!mounted) {\n        return FILESYSTEM_RESULT_NOT_MOUNTED;\n    }\n\n"
     "    for (size_t i = 0; i < FILESYSTEM_MAX_OPEN_FILES; i++) {\n        file_slot_t *const slot",
     "    for (size_t i = 0; i < FILESYSTEM_MAX_OPEN_FILES; i++) {\n        file_slot_t *const slot"),
    (FILESYSTEM, "read reports the requested count",
     "    *bytes_read = count;", "    *bytes_read = size;"),
    (FILESYSTEM, "write reports the requested count",
     "    *bytes_written = count;", "    *bytes_written = size;"),
    (FILESYSTEM, "seek off by one",
     "f_lseek(&slot->fil, (FSIZE_t)offset)", "f_lseek(&slot->fil, (FSIZE_t)offset + 1U)"),
    (FILESYSTEM, "tell reports the size",
     "    *offset = (uint32_t)f_tell(&slot->fil);", "    *offset = (uint32_t)f_size(&slot->fil);"),
    (FILESYSTEM, "size reports the position",
     "    *size = (uint32_t)f_size(&slot->fil);", "    *size = (uint32_t)f_tell(&slot->fil);"),

    # ------------------------------------------------------------------ filesystem.c: directories
    (FILESYSTEM, "pattern not copied (FatFs keeps the caller's pointer)",
     "f_findfirst(&slot->dir, &slot->info, path, slot->pattern)",
     "f_findfirst(&slot->dir, &slot->info, path, pattern)"),
    (FILESYSTEM, "pattern limit one short",
     "memchr(pattern, '\\0', FILESYSTEM_PATTERN_MAX + 1U)", "memchr(pattern, '\\0', FILESYSTEM_PATTERN_MAX)"),
    (FILESYSTEM, "pattern limit one long",
     "memchr(pattern, '\\0', FILESYSTEM_PATTERN_MAX + 1U)", "memchr(pattern, '\\0', FILESYSTEM_PATTERN_MAX + 2U)"),
    (FILESYSTEM, "first match not held for readdir",
     "            slot->filtered = true;\n            slot->pending = true;",
     "            slot->filtered = true;\n            slot->pending = false;"),
    (FILESYSTEM, "filtered listing continues with f_readdir",
     "            ? f_findnext(&slot->dir, &slot->info)", "            ? f_readdir(&slot->dir, &slot->info)"),
    (FILESYSTEM, "end of directory not detected",
     "    if (slot->info.fname[0] == '\\0') {", "    if (slot->info.fname[0] == '\\x01') {"),
    (FILESYSTEM, "directory flag from the wrong attribute",
     "    entry->is_directory = (info->fattrib & AM_DIR) != 0U;",
     "    entry->is_directory = (info->fattrib & AM_ARC) != 0U;"),
    (FILESYSTEM, "entry size not copied",
     "    entry->size = (uint32_t)info->fsize;", "    entry->size = 0U;"),
    (FILESYSTEM, "entry name truncated",
     "    memcpy(entry->name, info->fname, sizeof entry->name);",
     "    memcpy(entry->name, info->fname, 64);\n    entry->name[63] = '\\0';"),

    # ------------------------------------------------------------------ filesystem.c: entries
    (FILESYSTEM, "stat does not check mounted",
     "    if (!mounted) {\n        return FILESYSTEM_RESULT_NOT_MOUNTED;\n    }\n    FILINFO info;",
     "    FILINFO info;"),
    (FILESYSTEM, "rename arguments swapped",
     "f_rename(old_path, new_path)", "f_rename(new_path, old_path)"),
    (FILESYSTEM, "remove does not check mounted",
     "    if (!mounted) {\n        return FILESYSTEM_RESULT_NOT_MOUNTED;\n    }\n    if (being_listed(path)) {",
     "    if (being_listed(path)) {"),

    # ------------------------------------------------------------------ filesystem.c: time
    (FILESYSTEM, "years after 2107 wrap",
     "now.year > 1980U + 127U", "now.year > 1980U + 128U"),
    (FILESYSTEM, "1980 refused",
     "now.year < 1980U ||", "now.year <= 1980U ||"),
    (FILESYSTEM, "month packed at the wrong bit",
     "        | ((DWORD)now.month << 21)", "        | ((DWORD)now.month << 20)"),
    (FILESYSTEM, "seconds not halved",
     "        | ((DWORD)now.second / 2U);", "        | ((DWORD)now.second & 0x1FU);"),
    (FILESYSTEM, "minute and hour swapped",
     "        | ((DWORD)now.hour << 11)\n        | ((DWORD)now.minute << 5)",
     "        | ((DWORD)now.minute << 11)\n        | ((DWORD)now.hour << 5)"),
    (FILESYSTEM, "sentinel wrong",
     "    (((DWORD)(1980 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16))",
     "    (((DWORD)(1980 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)0 << 16))"),

    # ------------------------------------------------------------------ storage.c: noticing events
    (STORAGE, "removal never noticed",
     "    if (device_up && device_gone()) {\n        lose_medium();",
     "    if (device_up && device_gone() && false) {\n        lose_medium();"),
    (STORAGE, "a deinitialized device is not a loss",
     "    return result == BLOCK_DEVICE_RESULT_INVALID_DEVICE\n        || result == BLOCK_DEVICE_RESULT_NOT_INITIALIZED;",
     "    return result == BLOCK_DEVICE_RESULT_INVALID_DEVICE;"),
    (STORAGE, "presence check ignores the answer",
     "== BLOCK_DEVICE_RESULT_OK && present;", "== BLOCK_DEVICE_RESULT_OK;"),

    # ------------------------------------------------------------------ storage.c: losing the medium
    (STORAGE, "loss does not unmount",
     "    filesystem_unmount();\n    release_device();\n    medium = STORAGE_MEDIUM_ABSENT;",
     "    release_device();\n    medium = STORAGE_MEDIUM_ABSENT;"),
    (STORAGE, "loss does not release the device",
     "    filesystem_unmount();\n    release_device();\n    medium = STORAGE_MEDIUM_ABSENT;",
     "    filesystem_unmount();\n    medium = STORAGE_MEDIUM_ABSENT;"),
    (STORAGE, "loss keeps the failure count",
     "    medium = STORAGE_MEDIUM_ABSENT;\n    clear_retry();\n    if (was_ready)",
     "    medium = STORAGE_MEDIUM_ABSENT;\n    retry_scheduled = false;\n    if (was_ready)"),
    (STORAGE, "loss never changes the epoch",
     "    if (was_ready) {\n        bump_epoch();\n    }", "    (void)was_ready;"),
    (STORAGE, "loss of any medium changes the epoch",
     "    if (was_ready) {\n        bump_epoch();\n    }", "    (void)was_ready;\n    bump_epoch();"),
    (STORAGE, "release ignores a failed deinit",
     "    if (device_up && block_device_deinit(device) == BLOCK_DEVICE_RESULT_OK) {",
     "    if (device_up) {\n        (void)block_device_deinit(device);"),

    # ------------------------------------------------------------------ storage.c: bring-up and retries
    (STORAGE, "one attempt too many",
     "    if (attempts < STORAGE_BRING_UP_ATTEMPTS) {", "    if (attempts <= STORAGE_BRING_UP_ATTEMPTS) {"),
    (STORAGE, "parking does not release the device",
     "    medium = STORAGE_MEDIUM_FAULTY;\n    retry_scheduled = false;\n    (void)let_go(now_ms);",
     "    medium = STORAGE_MEDIUM_FAULTY;\n    retry_scheduled = false;"),
    (STORAGE, "a failed release is never retried",
     "    if (device_up) {\n        schedule_retry(now_ms);\n    }\n    return device_up;",
     "    (void)now_ms;\n    return device_up;"),
    (STORAGE, "failed release retried at once",
     "        if (retry_scheduled && !reached(now_ms, retry_at_ms)) {\n            return;\n        }\n"
     "        retry_scheduled = false;\n        (void)let_go(now_ms);",
     "        retry_scheduled = false;\n        (void)let_go(now_ms);"),
    (STORAGE, "a faulty card is never let go of",
     "    if (!card_in_socket()) {\n        medium = STORAGE_MEDIUM_ABSENT;\n        clear_retry();\n    }\n}",
     "}"),
    (STORAGE, "retry timer ignored",
     "    if (retry_scheduled && !reached(now_ms, retry_at_ms)) {\n        return;\n    }\n    retry_scheduled = false;\n\n",
     "    retry_scheduled = false;\n\n"),
    (STORAGE, "retry deadline compared without wrap",
     "    return (int32_t)(now_ms - at_ms) >= 0;", "    return now_ms >= at_ms;"),
    (STORAGE, "retry interval halved",
     "    schedule_retry_after(now_ms, STORAGE_RETRY_INTERVAL_MS);",
     "    schedule_retry_after(now_ms, STORAGE_RETRY_INTERVAL_MS / 2U);"),
    (STORAGE, "bring-up retries ignore the schedule",
     "        schedule_retry_after(now_ms, bring_up_delays_ms[attempts - 1U]);",
     "        schedule_retry_after(now_ms, bring_up_delays_ms[0]);"),
    (STORAGE, "an unseated card counts as a failure",
     "            medium = STORAGE_MEDIUM_ABSENT;\n            settled = false;\n            return;",
     "            settled = false;\n            bring_up_failed(now_ms, \"block device\", (int)result);\n"
     "            return;"),
    (STORAGE, "an unseated card is not settled again",
     "            medium = STORAGE_MEDIUM_ABSENT;\n            settled = false;\n            return;",
     "            medium = STORAGE_MEDIUM_ABSENT;\n            return;"),
    (STORAGE, "init and mount in separate steps",
     "        device_up = true;\n    }\n",
     "        device_up = true;\n        return;\n    }\n"),
    (STORAGE, "USB owner gets a mounted card",
     "        device_up = true;\n    }\n\n    if (owner == STORAGE_OWNER_USB) {",
     "        device_up = true;\n    }\n\n    if (false) {"),
    (STORAGE, "no filesystem treated as a failure",
     "    case FILESYSTEM_RESULT_NO_FILESYSTEM:\n        medium = STORAGE_MEDIUM_UNFORMATTED;\n        clear_retry();\n        return;",
     "    case FILESYSTEM_RESULT_NO_FILESYSTEM:\n        bring_up_failed(now_ms, \"filesystem\", (int)result);\n"
     "        return;"),
    (STORAGE, "an unusable card is taken for a removal",
     "    if (device_gone()) {\n        lose_medium();", "    if (true) {\n        lose_medium();"),
    (STORAGE, "a removal during mount is taken for a failure",
     "    if (device_gone()) {\n        lose_medium();", "    if (false) {\n        lose_medium();"),
    (STORAGE, "a removal during the mount's reads is taken for a failure",
     "    if (device_gone()) {\n        lose_medium();",
     "    if (result == FILESYSTEM_RESULT_MEDIUM_UNAVAILABLE && device_gone()) {\n        lose_medium();"),

    # ------------------------------------------------------------------ storage.c: requests
    (STORAGE, "leaving the local owner does not unmount",
     "    if (previous == STORAGE_OWNER_LOCAL) {\n        filesystem_unmount();\n    }\n    owner = requested_owner;",
     "    owner = requested_owner;"),
    (STORAGE, "owner change keeps the epoch",
     "    owner = requested_owner;\n    bump_epoch();\n\n    if (owner == STORAGE_OWNER_NONE) {",
     "    owner = requested_owner;\n\n    if (owner == STORAGE_OWNER_NONE) {"),
    (STORAGE, "a faulty card is lent to USB",
     "    if (!device_up || medium == STORAGE_MEDIUM_FAULTY) {", "    if (!device_up) {"),
    (STORAGE, "an unformatted card is not lent to USB",
     "        //an unformatted card is lent too: formatting it is the host's business\n"
     "        medium = STORAGE_MEDIUM_READY;",
     "        //an unformatted card is lent too: formatting it is the host's business\n"
     "        medium = medium == STORAGE_MEDIUM_UNFORMATTED ? medium : STORAGE_MEDIUM_READY;"),
    (STORAGE, "returning to the local owner does not mount",
     "    medium = STORAGE_MEDIUM_PREPARING;\n    mount_now(now_ms);\n}\n\n//a faulty card",
     "    (void)now_ms;\n    medium = STORAGE_MEDIUM_READY;\n}\n\n//a faulty card"),
    (STORAGE, "requests not carried out at the top of the step",
     "    if (requested_owner != owner) {\n        change_owner(now_ms);\n        return;\n    }\n\n    bring_up(now_ms);",
     "    bring_up(now_ms);\n    if (requested_owner != owner) {\n        change_owner(now_ms);\n        return;\n    }"),
    (STORAGE, "owner request accepts anything",
     "    if (device == NULL || (new_owner != STORAGE_OWNER_LOCAL && new_owner != STORAGE_OWNER_USB\n"
     "            && new_owner != STORAGE_OWNER_NONE)) {",
     "    if (device == NULL) {"),

    # ------------------------------------------------------------------ storage.c: interface
    (STORAGE, "init accepts a device without a presence signal",
     "            || new_device->operations->is_present == NULL\n",
     ""),
    (STORAGE, "init accepts a device without sync",
     "            || new_device->operations->is_present == NULL\n            || new_device->operations->sync == NULL) {",
     "            || new_device->operations->is_present == NULL) {"),
    (STORAGE, "init twice accepted",
     "    if (device != NULL || !block_device_is_valid(new_device)", "    if (!block_device_is_valid(new_device)"),
    (STORAGE, "USB device lent to the local owner",
     "    return owner == STORAGE_OWNER_USB && medium == STORAGE_MEDIUM_READY ? device : NULL;",
     "    return medium == STORAGE_MEDIUM_READY ? device : NULL;"),
    (STORAGE, "USB device lent when not ready",
     "    return owner == STORAGE_OWNER_USB && medium == STORAGE_MEDIUM_READY ? device : NULL;",
     "    return owner == STORAGE_OWNER_USB ? device : NULL;"),

    # ------------------------------------------------------------------ storage.c: release for the rail
    (STORAGE, "NONE refused as an owner",
     "    if (device == NULL || (new_owner != STORAGE_OWNER_LOCAL && new_owner != STORAGE_OWNER_USB\n"
     "            && new_owner != STORAGE_OWNER_NONE)) {",
     "    if (device == NULL || (new_owner != STORAGE_OWNER_LOCAL && new_owner != STORAGE_OWNER_USB)) {"),
    (STORAGE, "release leaves the device initialized",
     "        clear_retry();\n        attempt_release(now_ms);\n        return;",
     "        clear_retry();\n        return;"),
    (STORAGE, "bring-up carries on while released",
     "    if (owner == STORAGE_OWNER_NONE) {\n        //released: nothing is brought up",
     "    if (false) {\n        //released: nothing is brought up"),
    (STORAGE, "a failed release is not retried",
     "    if (attempts < STORAGE_RELEASE_ATTEMPTS) {\n        schedule_retry(now_ms);\n        return;\n    }",
     "    if (attempts < STORAGE_RELEASE_ATTEMPTS) {\n        (void)now_ms;\n        return;\n    }"),
    (STORAGE, "a release is never given up",
     "    if (attempts < STORAGE_RELEASE_ATTEMPTS) {",
     "    if (attempts < STORAGE_RELEASE_ATTEMPTS + 100U) {"),
    (STORAGE, "a release under way is not pending",
     "    return requested_owner != owner || releasing;",
     "    (void)releasing;\n    return requested_owner != owner;"),
    (STORAGE, "a release given up stays pending",
     "owner == STORAGE_OWNER_NONE && device_up && medium != STORAGE_MEDIUM_FAULTY;",
     "owner == STORAGE_OWNER_NONE && device_up;"),
    (STORAGE, "reclaim keeps a card still held",
     "        medium = let_go(now_ms) ? STORAGE_MEDIUM_FAULTY : STORAGE_MEDIUM_ABSENT;",
     "        medium = device_up ? STORAGE_MEDIUM_FAULTY : STORAGE_MEDIUM_ABSENT;"),
    (STORAGE, "reclaim keeps the released medium",
     "        medium = let_go(now_ms) ? STORAGE_MEDIUM_FAULTY : STORAGE_MEDIUM_ABSENT;",
     "        if (let_go(now_ms)) {\n            medium = STORAGE_MEDIUM_FAULTY;\n        }"),
    # ------------------------------------------------------------------ msc_disk.c: what the host is offered
    (MSC_DISK, "a withheld medium is offered again",
     "    if (device == NULL || (withheld && epoch == withheld_epoch)) {", "    if (device == NULL) {"),
    (MSC_DISK, "a withheld medium stays withheld after a new lend",
     "    if (device == NULL || (withheld && epoch == withheld_epoch)) {",
     "    if (device == NULL || withheld) {"),
    (MSC_DISK, "a medium changed unseen is not announced",
     "    if (!presented || epoch != presented_epoch) {", "    if (!presented) {"),
    (MSC_DISK, "UNIT ATTENTION reported as NOT READY",
     "        set_sense(sense, SENSE_UNIT_ATTENTION, ASC_NOT_READY_TO_READY_CHANGE);",
     "        set_sense(sense, SENSE_NOT_READY, ASC_NOT_READY_TO_READY_CHANGE);"),
    (MSC_DISK, "the command that gets UNIT ATTENTION goes ahead",
     "        set_sense(sense, SENSE_UNIT_ATTENTION, ASC_NOT_READY_TO_READY_CHANGE);\n        return NULL;",
     "        set_sense(sense, SENSE_UNIT_ATTENTION, ASC_NOT_READY_TO_READY_CHANGE);"),
    (MSC_DISK, "the sense qualifier is left as it was",
     "    sense->qualifier = 0;\n", ""),

    # ------------------------------------------------------------------ msc_disk.c: failures
    (MSC_DISK, "a removal found by a transfer is offered until the coordinator notices",
     "        withhold();\n        no_medium(sense);\n        return;", "        no_medium(sense);\n        return;"),
    (MSC_DISK, "NOT_INITIALIZED is a target failure",
     "    case BLOCK_DEVICE_RESULT_NOT_INITIALIZED:\n",
     "    case BLOCK_DEVICE_RESULT_NOT_INITIALIZED:\n        if (result == BLOCK_DEVICE_RESULT_NOT_INITIALIZED) {\n"
     "            break;\n        }\n"),
    (MSC_DISK, "OUT_OF_RANGE from the device is a target failure",
     "        set_sense(sense, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE);\n        return;\n"
     "    case BLOCK_DEVICE_RESULT_IO_ERROR:",
     "        break;\n    case BLOCK_DEVICE_RESULT_IO_ERROR:"),
    (MSC_DISK, "BUSY_TIMEOUT is a target failure",
     "    case BLOCK_DEVICE_RESULT_BUSY_TIMEOUT:\n",
     "    case BLOCK_DEVICE_RESULT_BUSY_TIMEOUT:\n        if (result == BLOCK_DEVICE_RESULT_BUSY_TIMEOUT) {\n"
     "            break;\n        }\n"),
    (MSC_DISK, "a failed write is reported as a read error",
     "        report_failure(result, ASC_WRITE_ERROR, sense);",
     "        report_failure(result, ASC_UNRECOVERED_READ_ERROR, sense);"),
    (MSC_DISK, "a failed geometry lookup is a target failure",
     "        report_failure(result, medium_code, sense);",
     "        (void)medium_code;\n        set_sense(sense, SENSE_HARDWARE_ERROR, ASC_INTERNAL_TARGET_FAILURE);"),
    (MSC_DISK, "a block size over 16 bits accepted",
     " || info->block_size_bytes > UINT16_MAX", ""),
    (MSC_DISK, "a medium of no blocks accepted",
     " || info->block_count == 0U", ""),

    # ------------------------------------------------------------------ msc_disk.c: transfers
    (MSC_DISK, "writes not synced",
     "    if (result == BLOCK_DEVICE_RESULT_OK) {\n        result = block_device_sync(device);\n    }\n", ""),
    (MSC_DISK, "the offset is ignored",
     "    *first = (uint64_t)lba + offset / block_size;", "    *first = (uint64_t)lba;"),
    (MSC_DISK, "the first block is worked out in 32 bits",
     "    *first = (uint64_t)lba + offset / block_size;", "    *first = (uint32_t)(lba + offset / block_size);"),
    (MSC_DISK, "partial blocks accepted",
     " || offset % block_size != 0U || size % block_size != 0U", ""),
    (MSC_DISK, "a transfer of nothing accepted",
     "    if (size == 0U || size > INT32_MAX", "    if (size > INT32_MAX"),
    (MSC_DISK, "an address past the end reaches the device",
     "    if (*first >= info.block_count || *count > info.block_count - *first) {",
     "    if (*count > info.block_count - *first) {"),
    (MSC_DISK, "a transfer running off the end accepted",
     "    if (*first >= info.block_count || *count > info.block_count - *first) {",
     "    if (*first >= info.block_count) {"),

    # ------------------------------------------------------------------ msc_disk.c: capacity, writable, eject
    (MSC_DISK, "capacity not clamped to 32 bits",
     "    *block_count = info.block_count > UINT32_MAX ? UINT32_MAX : (uint32_t)info.block_count;",
     "    *block_count = (uint32_t)info.block_count;"),
    (MSC_DISK, "capacity left as it was on failure",
     "    *block_count = 0;\n    *block_size = 0;\n", ""),
    (MSC_DISK, "writable false with no medium",
     "    return device == NULL || block_device_get_info(device, &info) != BLOCK_DEVICE_RESULT_OK || info.writable;",
     "    return device != NULL && block_device_get_info(device, &info) == BLOCK_DEVICE_RESULT_OK && info.writable;"),
    (MSC_DISK, "writable ignores the device",
     " != BLOCK_DEVICE_RESULT_OK || info.writable;", " != BLOCK_DEVICE_RESULT_OK || !info.writable || true;"),
    (MSC_DISK, "a load ejects",
     "    if (!load_eject || start) {", "    if (!load_eject || (start && false)) {"),
    (MSC_DISK, "an eject does not withhold the medium",
     "    withhold();\n    presented = false;\n    eject_pending = true;", "    presented = false;\n    eject_pending = true;"),
    (MSC_DISK, "an eject is not reported",
     "    presented = false;\n    eject_pending = true;", "    presented = false;"),
    (MSC_DISK, "an eject is reported forever",
     "    eject_pending = false;\n    return ejected;", "    return ejected;"),
]


def run(command, cwd=None, timeout=None):
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", help="where to build (default: a temporary directory)")
    parser.add_argument("--only", help="run only mutations whose description contains this")
    parser.add_argument("--keep-going", action="store_true", help="do not stop at the first survivor")
    args = parser.parse_args()

    work = args.build_dir or tempfile.mkdtemp(prefix="fs-mutants-")
    os.makedirs(work, exist_ok=True)
    copies = {name: os.path.join(work, name) for name in SOURCES}
    originals = {name: open(path).read() for name, path in SOURCES.items()}
    for name, text in originals.items():
        with open(copies[name], "w") as f:
            f.write(text)

    build = os.path.join(work, "build")
    generator = ["-G", "Ninja"] if shutil.which("ninja") else []
    configure = run(["cmake", "-S", HERE, "-B", build, *generator,
                     "-DFS_TESTS_FILESYSTEM_SOURCE=" + copies[FILESYSTEM],
                     "-DFS_TESTS_DISKIO_SOURCE=" + copies[DISKIO],
                     "-DFS_TESTS_STORAGE_SOURCE=" + copies[STORAGE],
                     "-DFS_TESTS_MSC_DISK_SOURCE=" + copies[MSC_DISK]])
    if configure.returncode != 0:
        print(configure.stdout)
        return 2
    binary = os.path.join(build, "fs_tests")

    def build_and_test():
        made = run(["cmake", "--build", build])
        if made.returncode != 0:
            return "NOT-COMPILED", made.stdout
        try:
            tested = run([binary], timeout=300)
        except subprocess.TimeoutExpired:
            return "killed", "timed out"
        return ("killed" if tested.returncode != 0 else "SURVIVED"), tested.stdout

    status, output = build_and_test()
    if status != "SURVIVED":
        print("the unmutated suite does not pass; fix that first\n" + output[-3000:])
        return 2
    print("baseline: unmutated suite passes\n")

    selected = [m for m in MUTATIONS if not args.only or args.only in m[1]]
    tally = {"killed": 0, "SURVIVED": 0, "STALE": 0, "NOT-COMPILED": 0}
    for index, (name, description, old, new) in enumerate(selected, 1):
        for other, text in originals.items():
            with open(copies[other], "w") as f:
                f.write(text)
        occurrences = originals[name].count(old)
        if occurrences != 1:
            status, detail = "STALE", "target text found %d times" % occurrences
        else:
            with open(copies[name], "w") as f:
                f.write(originals[name].replace(old, new))
            status, output = build_and_test()
            detail = ""
            if status == "killed":
                failures = [line.strip() for line in output.splitlines() if "FAIL" in line or "ERROR" in line]
                detail = failures[0] if failures else output.strip().splitlines()[-1] if output.strip() else ""
            elif status == "NOT-COMPILED":
                errors = [line.strip() for line in output.splitlines() if "error" in line]
                detail = errors[0] if errors else ""
        tally[status] += 1
        print("[%3d/%d] %-12s %s: %s" % (index, len(selected), status, name, description))
        if detail and status != "killed":
            print("               " + detail[:200])
        elif detail:
            print("               by " + detail[:160])
        if status != "killed" and not args.keep_going:
            break

    for name, text in originals.items():
        with open(copies[name], "w") as f:
            f.write(text)
    print("\n%d killed, %d survived, %d stale, %d did not compile (of %d run)" % (
        tally["killed"], tally["SURVIVED"], tally["STALE"], tally["NOT-COMPILED"], sum(tally.values())))
    return 0 if tally["killed"] == sum(tally.values()) and sum(tally.values()) == len(selected) else 1


if __name__ == "__main__":
    sys.exit(main())

# Equivalent mutations, deliberately left out because no test can tell them apart from the real code:
#
# diskio.c
#   - bind assigning only .device instead of resetting the whole entry: unbind already resets the whole
#     entry, and bind only succeeds on an entry that is unbound, so there is never stale state to reset.
#   - removing the NULL-buffer or zero-count checks in disk_read/disk_write: block_device_read_blocks
#     and block_device_write_blocks (block_device.h) reject both before reaching the device, and map
#     to the same RES_PARERR through INVALID_ARGUMENT. the adapter's checks are defence in depth.
#
# filesystem.c
#   - not advancing a slot's generation on close or unmount: open advances it before issuing a handle,
#     and a free slot never resolves (in_use is checked), so an old handle is rejected either way. the
#     advance on close/unmount is defence in depth against a future change to either guard.
#   - not rejecting generation 0 in file_slot/dir_slot: open never issues 0, so a zero handle can only
#     name a slot whose generation is nonzero (stale by mismatch) or one never opened (stale by in_use).
#   - dir_slot's in_use check: a closed DIR is invalidated by f_closedir, so FatFs rejects it anyway
#     (FR_INVALID_OBJECT -> STALE_HANDLE). file_slot's is not equivalent: f_size/f_tell read FIL fields
#     without FatFs validating them, and the forged-handle test catches its removal.
#   - not unregistering FatFs's volume is NOT equivalent: tested white-box through f_stat.
#   - clip_to_uint: only differs for a single request above 4 GiB on a 64-bit host, which no FAT file
#     can satisfy anyway (see the comment on it).
#
# storage.c
#   - mount_now's success not clearing the attempt count: every way from a READY local card to another
#     counted attempt clears it first (a loss in lose_medium, a change to USB or NONE in change_owner,
#     and a return from USB mounts only after the lend cleared it). the format path went from READY
#     straight into a mount and made it observable; it was removed, and the clear stays as defence in depth.
#
# msc_disk.c
#   - not clearing presented in no_medium or msc_disk_start_stop: the coordinator bumps the epoch whenever
#     a lent medium stops being READY and on every owner change, so a medium the host was told was missing
#     always comes back at a new epoch and is announced by the epoch comparison alone. presented covers the
#     first lend and keeps the rule independent of that guarantee.
#   - >= weakened to > in the first-block bound: the count check after it refuses first == block_count as
#     well, since every transfer is at least one block. the first comparison is what keeps
#     block_count - first from wrapping, and dropping it is caught.
#
# Not equivalent, but not reliably detectable either:
#   - dir_slot's bound check off by one (> instead of >=): slot 1 of the one-slot pool reads one element
#     past the array. AddressSanitizer reports that only when the read lands in its guard zone; for this
#     pool's element size it lands beyond it, in whatever global follows, so the outcome depends on the
#     link layout. the same mutation in file_slot is caught, and the forged-handle test exercises both.
