/* Filesystem layer: the only code that calls FatFs.
 *
 * upper layers -> filesystem.c -> FatFs (ff.c) -> diskio.c -> block device
 *
 * All FatFs state lives here, privately: the one volume object and fixed pools
 * of open files and directories. Callers hold only slot/generation handles
 * (filesystem.h), which is what lets unmount invalidate every open handle at
 * once without any I/O, as the ownership transition requires
 * (docs/architecture.md, "Operating Modes and Storage Ownership").
 *
 * Cooperative and single-threaded: no locking, called from the storage
 * coordinator's core only, and never from an interrupt handler.
 * FF_FS_REENTRANT is 0 to match. (FF_FS_LOCK is FatFs's table of open
 * entries, not a lock between threads; see "Open entries" in filesystem.h.)
 *
 * This file also supplies get_fattime(), FatFs's one seam for time, so that no
 * code outside the filesystem layer includes ff.h; the time itself comes from
 * platform/clock.h.
 *
 * FRESULT mapping (map_result):
 *   FR_OK                                  -> OK
 *   FR_NOT_READY                           -> MEDIUM_UNAVAILABLE
 *   FR_NO_FILESYSTEM                       -> NO_FILESYSTEM
 *   FR_NO_FILE, FR_NO_PATH                 -> NOT_FOUND
 *   FR_EXIST                               -> EXISTS
 *   FR_DENIED, FR_WRITE_PROTECTED          -> DENIED
 *   FR_LOCKED                              -> IN_USE
 *   FR_INVALID_NAME, FR_INVALID_PARAMETER  -> INVALID_ARGUMENT
 *   FR_INVALID_OBJECT                      -> STALE_HANDLE
 *   FR_TOO_MANY_OPEN_FILES                 -> TOO_MANY_OPEN
 *   FR_DISK_ERR, FR_INT_ERR, and the rest  -> STORAGE_ERROR
 *     (FR_MKFS_ABORTED means no FAT type fits the card's size, which no
 *     usable card causes; FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_TIMEOUT and
 *     FR_NOT_ENOUGH_CORE indicate a configuration or internal error at the
 *     current ffconf.h. Neither is a condition callers can act on) */

#include "storage/filesystem.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ff.h"

#include "platform/clock.h"
#include "platform/debug.h"
#include "storage/filesystem_diskio.h"

/* Pool sizes. Each FIL carries its own sector buffer while FF_FS_TINY is 0
 * (about 550 bytes per slot). Sized for the expected worst case: the open EPUB
 * archive, an audio file, and one bookmark/settings file at a save point. */
#define FILESYSTEM_MAX_OPEN_FILES 4
#define FILESYSTEM_MAX_OPEN_DIRS 1

static_assert(FILESYSTEM_NAME_MAX == FF_LFN_BUF,
    "FILESYSTEM_NAME_MAX must match FF_LFN_BUF in ffconf.h");
static_assert(FILESYSTEM_MAX_OPEN_FILES <= UINT8_MAX
        && FILESYSTEM_MAX_OPEN_DIRS <= UINT8_MAX,
    "handle slot numbers are uint8_t");
static_assert(sizeof(FSIZE_t) == sizeof(uint32_t),
    "sizes and offsets are 32-bit while FF_FS_EXFAT is 0");
//FatFs's open-entry table needs an entry for every file and directory the pools can hold open; with
//fewer, an open the pools allow would fail with TOO_MANY_OPEN
static_assert(FF_FS_LOCK >= FILESYSTEM_MAX_OPEN_FILES + FILESYSTEM_MAX_OPEN_DIRS,
    "FF_FS_LOCK in ffconf.h must cover both handle pools");

//FatFs's flag for a file whose changes are not yet on the card (FA_MODIFIED in ff.c, R0.16). ff.h does
//not export it; filesystem_close clears it to release a file whose close failed (see there), and
//fs_failed_close_releases_the_entry in the host tests catches a FatFs that changes it
#define FATFS_FILE_MODIFIED 0x40U

typedef struct {
    FIL fil;
    bool in_use;
    uint16_t generation;    //0 only before the slot's first open; advanced on open, close and every unmount
} file_slot_t;

typedef struct {
    DIR dir;
    //the entry most recently read. f_findfirst reads the first match while opening, so after a filtered
    //opendir this holds that match until the first readdir hands it out (pending)
    FILINFO info;
    //f_findnext reads the pattern through a pointer FatFs keeps in DIR, so it must live as long as the
    //listing does; the caller's string need not
    char pattern[FILESYSTEM_PATTERN_MAX + 1];
    bool filtered;          //listing through f_findnext rather than f_readdir
    bool pending;           //info holds a match not yet returned
    bool in_use;
    uint16_t generation;    //as file_slot_t
} dir_slot_t;

static FATFS volume;
//the block device itself is held by the adapter (filesystem_diskio_device) while mounted, and by no
//second pointer here; the coordinator owns its init/deinit
static bool mounted;

static file_slot_t files[FILESYSTEM_MAX_OPEN_FILES];
static dir_slot_t dirs[FILESYSTEM_MAX_OPEN_DIRS];

/* ----------------------------------------------------------- helpers */

static filesystem_result_t map_result(FRESULT result)
{
    //every value listed and no default, so -Wswitch flags any FRESULT a later FatFs adds
    switch (result) {
    case FR_OK:
        return FILESYSTEM_RESULT_OK;
    case FR_NOT_READY:
        return FILESYSTEM_RESULT_MEDIUM_UNAVAILABLE;
    case FR_NO_FILESYSTEM:
        return FILESYSTEM_RESULT_NO_FILESYSTEM;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return FILESYSTEM_RESULT_NOT_FOUND;
    case FR_EXIST:
        return FILESYSTEM_RESULT_EXISTS;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
        return FILESYSTEM_RESULT_DENIED;
    case FR_LOCKED:
        return FILESYSTEM_RESULT_IN_USE;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    case FR_INVALID_OBJECT:
        return FILESYSTEM_RESULT_STALE_HANDLE;
    case FR_TOO_MANY_OPEN_FILES:
        return FILESYSTEM_RESULT_TOO_MANY_OPEN;
    case FR_DISK_ERR:
    case FR_INT_ERR:
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED:
    case FR_MKFS_ABORTED:
    case FR_TIMEOUT:
    case FR_NOT_ENOUGH_CORE:
        return FILESYSTEM_RESULT_STORAGE_ERROR;
    }
    return FILESYSTEM_RESULT_STORAGE_ERROR;
}

//advance a slot generation, skipping 0 so a zero-initialized handle never matches
static uint16_t next_generation(uint16_t generation)
{
    generation = (uint16_t)(generation + 1U);
    return generation == 0U ? 1U : generation;
}

//resolve a handle to its slot, or NULL if the slot number is out of range, the slot is free, or the
//generation is 0 or does not match (closed since, or unmounted since). rejecting 0 outright keeps a
//zero-initialized handle stale even against a slot that has never been opened
static file_slot_t *file_slot(filesystem_file_t file)
{
    if (file.slot >= FILESYSTEM_MAX_OPEN_FILES || file.generation == 0U) {
        return NULL;
    }
    file_slot_t *const slot = &files[file.slot];
    if (!slot->in_use || slot->generation != file.generation) {
        return NULL;
    }
    return slot;
}

static dir_slot_t *dir_slot(filesystem_dir_t dir)
{
    if (dir.slot >= FILESYSTEM_MAX_OPEN_DIRS || dir.generation == 0U) {
        return NULL;
    }
    dir_slot_t *const slot = &dirs[dir.slot];
    if (!slot->in_use || slot->generation != dir.generation) {
        return NULL;
    }
    return slot;
}

//free a slot; the generation advance is what turns every outstanding copy of its handle stale
static void release_file_slot(file_slot_t *slot)
{
    slot->in_use = false;
    slot->generation = next_generation(slot->generation);
}

static void release_dir_slot(dir_slot_t *slot)
{
    slot->in_use = false;
    slot->pending = false;
    slot->generation = next_generation(slot->generation);
}

//FatFs takes UINT byte counts. both are 32-bit on the RP2350; on a host with a 64-bit size_t a larger
//request is clipped, which still honours the read/write contracts because no FAT file reaches 4 GiB
static UINT clip_to_uint(size_t size)
{
#if SIZE_MAX > UINT_MAX
    if (size > UINT_MAX) {
        return UINT_MAX;
    }
#endif
    return (UINT)size;
}

static void copy_entry(filesystem_dirent_t *entry, const FILINFO *info)
{
    //both are FF_LFN_BUF + 1 bytes (static_assert above), and fname is always terminated
    memcpy(entry->name, info->fname, sizeof entry->name);
    entry->size = (uint32_t)info->fsize;
    entry->is_directory = (info->fattrib & AM_DIR) != 0U;
}

//FAT timestamp packing, per third_party/fatfs/documents/doc/fattime.html:
//  bits 31..25  year - 1980      (0..127)
//  bits 24..21  month            (1..12)
//  bits 20..16  day              (1..31)
//  bits 15..11  hour             (0..23)
//  bits 10..5   minute           (0..59)
//  bits  4..0   second / 2       (0..29)
//
//the sentinel is 1980-01-01 00:00:00, the earliest representable FAT time. hosts display it as the epoch
//or as "unknown", which is the honest statement while no valid clock exists (see platform/clock.h)
#define FAT_TIME_SENTINEL_1980 \
    (((DWORD)(1980 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16))

//called by FatFs whenever it stamps a file or directory; compiled because ffconf.h sets FF_FS_NORTC to 0
DWORD get_fattime(void)
{
    clock_datetime_t now;
    if (!clock_get_datetime(&now) || now.year < 1980U || now.year > 1980U + 127U) {
        return FAT_TIME_SENTINEL_1980;
    }
    return ((DWORD)(now.year - 1980U) << 25)
        | ((DWORD)now.month << 21)
        | ((DWORD)now.day << 16)
        | ((DWORD)now.hour << 11)
        | ((DWORD)now.minute << 5)
        | ((DWORD)now.second / 2U);
}

/* ------------------------------------------------------------ volume */

filesystem_result_t filesystem_mount(block_device_t *new_device)
{
    if (!block_device_is_valid(new_device)) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (mounted) {
        return FILESYSTEM_RESULT_ALREADY_MOUNTED;
    }
    //the device is known valid, so a refusal means the drive is still bound from a mount or format that
    //never unbound - an internal error. the binding is left alone
    if (!filesystem_diskio_bind(FILESYSTEM_DRIVE_SD, new_device)) {
        return FILESYSTEM_RESULT_STORAGE_ERROR;
    }

    //opt 1 mounts now, so an absent card or a missing filesystem is reported here rather than on the
    //first open. every slot is already free, from the last unmount or zero-initialization
    const FRESULT result = f_mount(&volume, "", 1);
    if (result != FR_OK) {
        //FatFs leaves the volume registered after a failed mount
        (void)f_mount(NULL, "", 0);
        filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);
        return map_result(result);
    }

    mounted = true;
    return FILESYSTEM_RESULT_OK;
}

filesystem_result_t filesystem_unmount(void)
{
    //no i/o anywhere below - the card may already be gone
    if (!mounted) {
        return FILESYSTEM_RESULT_OK;
    }

    for (size_t i = 0; i < FILESYSTEM_MAX_OPEN_FILES; i++) {
        //never f_close: it would try to sync to a card that may be absent
        if (files[i].in_use && (files[i].fil.flag & FA_WRITE) != 0U) {
            //a writer skipped its save point; its data is dropped. reported, never refused - removal has
            //to succeed
            DEBUG_WARN("filesystem: unmount dropped file slot %u, still open for writing", (unsigned)i);
        }
        release_file_slot(&files[i]);
    }
    for (size_t i = 0; i < FILESYSTEM_MAX_OPEN_DIRS; i++) {
        release_dir_slot(&dirs[i]);
    }

    //only unregisters the volume, which also empties FatFs's open-entry table for it: the files dropped
    //above were never closed, so their entries are released here
    (void)f_mount(NULL, "", 0);
    filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);
    mounted = false;
    return FILESYSTEM_RESULT_OK;
}

bool filesystem_is_mounted(void)
{
    return mounted;
}

filesystem_result_t filesystem_format(block_device_t *new_device)
{
    if (!block_device_is_valid(new_device)) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (mounted) {
        return FILESYSTEM_RESULT_ALREADY_MOUNTED;
    }
    if (!filesystem_diskio_bind(FILESYSTEM_DRIVE_SD, new_device)) {
        return FILESYSTEM_RESULT_STORAGE_ERROR;
    }

    //FatFs picks FAT12/16 or FAT32 by size. no FM_SFD, so the card keeps an MBR partition table as hosts
    //expect. align 0 takes the data-area alignment from disk_ioctl GET_BLOCK_SIZE; align to the card's
    //allocation unit there if format speed or card wear ever matters
    static const MKFS_PARM params = { .fmt = FM_FAT | FM_FAT32 };
    //the minimum f_mkfs accepts, and within the 4 KB stack. more would only mean fewer writes
    BYTE work[FF_MAX_SS];
    const FRESULT result = f_mkfs("", &params, work, sizeof work);

    //f_mkfs registers nothing, so unbinding is all the cleanup there is
    filesystem_diskio_unbind(FILESYSTEM_DRIVE_SD);
    return map_result(result);
}

filesystem_result_t filesystem_get_label(char *label, size_t label_size)
{
    if (label == NULL || label_size < FILESYSTEM_LABEL_SIZE) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (!mounted) {
        return FILESYSTEM_RESULT_NOT_MOUNTED;
    }
    return map_result(f_getlabel("", label, NULL));
}

/* ------------------------------------------------------------- files */

filesystem_result_t filesystem_open(
    const char *path,
    filesystem_open_mode_t mode,
    filesystem_file_t *file)
{
    BYTE fatfs_mode;
    if (mode == FILESYSTEM_OPEN_READ) {
        fatfs_mode = FA_READ;
    } else if (mode == FILESYSTEM_OPEN_REPLACE) {
        fatfs_mode = FA_WRITE | FA_CREATE_ALWAYS;
    } else {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (path == NULL || file == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (!mounted) {
        return FILESYSTEM_RESULT_NOT_MOUNTED;
    }

    for (size_t i = 0; i < FILESYSTEM_MAX_OPEN_FILES; i++) {
        file_slot_t *const slot = &files[i];
        if (slot->in_use) {
            continue;
        }
        const FRESULT result = f_open(&slot->fil, path, fatfs_mode);
        if (result != FR_OK) {
            return map_result(result);
        }
        //advancing here is what keeps generation 0 from ever being issued: a slot not yet opened since
        //boot still holds the zero it was initialized with
        slot->generation = next_generation(slot->generation);
        slot->in_use = true;
        *file = (filesystem_file_t){ .slot = (uint8_t)i, .generation = slot->generation };
        return FILESYSTEM_RESULT_OK;
    }
    return FILESYSTEM_RESULT_TOO_MANY_OPEN;
}

filesystem_result_t filesystem_close(filesystem_file_t file)
{
    file_slot_t *const slot = file_slot(file);
    if (slot == NULL) {
        return FILESYSTEM_RESULT_STALE_HANDLE;
    }
    const FRESULT result = f_close(&slot->fil);
    //the handle is spent whatever f_close returned: after a disk error FatFs has already marked the FIL
    //failed, so there is nothing a retry could flush. but f_close gives up the file's entry in FatFs's
    //open-entry table only after a successful flush, so a failed close would leave the file IN_USE until
    //the next mount. FatFs left the FIL open in that case: drop its unflushed changes and close it again,
    //which now has nothing to flush and so releases the entry. after a removal that second close fails
    //too (the drive reports not ready), and the coordinator's unmount clears the table instead
    if (result != FR_OK && slot->fil.obj.fs != NULL) {
        slot->fil.flag &= (BYTE)~FATFS_FILE_MODIFIED;
        (void)f_close(&slot->fil);
    }
    release_file_slot(slot);
    return map_result(result);
}

filesystem_result_t filesystem_read(
    filesystem_file_t file,
    void *buffer,
    size_t size,
    size_t *bytes_read)
{
    if (buffer == NULL || bytes_read == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    *bytes_read = 0U;
    file_slot_t *const slot = file_slot(file);
    if (slot == NULL) {
        return FILESYSTEM_RESULT_STALE_HANDLE;
    }
    UINT count = 0U;
    const FRESULT result = f_read(&slot->fil, buffer, clip_to_uint(size), &count);
    *bytes_read = count;
    return map_result(result);
}

filesystem_result_t filesystem_write(
    filesystem_file_t file,
    const void *buffer,
    size_t size,
    size_t *bytes_written)
{
    if (buffer == NULL || bytes_written == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    *bytes_written = 0U;
    file_slot_t *const slot = file_slot(file);
    if (slot == NULL) {
        return FILESYSTEM_RESULT_STALE_HANDLE;
    }
    UINT count = 0U;
    const FRESULT result = f_write(&slot->fil, buffer, clip_to_uint(size), &count);
    *bytes_written = count;
    return map_result(result);
}

filesystem_result_t filesystem_seek(filesystem_file_t file, uint32_t offset)
{
    file_slot_t *const slot = file_slot(file);
    if (slot == NULL) {
        return FILESYSTEM_RESULT_STALE_HANDLE;
    }
    //FatFs itself stops at the end on a read-only file, and extends a writable one (stopping short if
    //the volume fills), as filesystem.h documents
    return map_result(f_lseek(&slot->fil, (FSIZE_t)offset));
}

filesystem_result_t filesystem_tell(filesystem_file_t file, uint32_t *offset)
{
    if (offset == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    const file_slot_t *const slot = file_slot(file);
    if (slot == NULL) {
        return FILESYSTEM_RESULT_STALE_HANDLE;
    }
    *offset = (uint32_t)f_tell(&slot->fil);
    return FILESYSTEM_RESULT_OK;
}

filesystem_result_t filesystem_size(filesystem_file_t file, uint32_t *size)
{
    if (size == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    const file_slot_t *const slot = file_slot(file);
    if (slot == NULL) {
        return FILESYSTEM_RESULT_STALE_HANDLE;
    }
    *size = (uint32_t)f_size(&slot->fil);
    return FILESYSTEM_RESULT_OK;
}

/* ------------------------------------------------------- directories */

filesystem_result_t filesystem_opendir(
    const char *path,
    const char *pattern,
    filesystem_dir_t *dir)
{
    if (path == NULL || dir == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    //reads at most FILESYSTEM_PATTERN_MAX + 1 bytes, stopping at the terminator
    if (pattern != NULL && memchr(pattern, '\0', FILESYSTEM_PATTERN_MAX + 1U) == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (!mounted) {
        return FILESYSTEM_RESULT_NOT_MOUNTED;
    }

    for (size_t i = 0; i < FILESYSTEM_MAX_OPEN_DIRS; i++) {
        dir_slot_t *const slot = &dirs[i];
        if (slot->in_use) {
            continue;
        }
        FRESULT result;
        if (pattern == NULL) {
            result = f_opendir(&slot->dir, path);
            slot->filtered = false;
            slot->pending = false;
        } else {
            strcpy(slot->pattern, pattern);
            result = f_findfirst(&slot->dir, &slot->info, path, slot->pattern);
            slot->filtered = true;
            slot->pending = true;
        }
        if (result != FR_OK) {
            //a failed f_opendir leaves nothing open, but f_findfirst opens the directory before it reads
            //the first entry, so if that read failed the directory holds an entry in FatFs's open-entry
            //table. FatFs marks an object it left open by keeping obj.fs set
            if (slot->dir.obj.fs != NULL) {
                (void)f_closedir(&slot->dir);
            }
            slot->pending = false;
            return map_result(result);
        }
        slot->generation = next_generation(slot->generation);
        slot->in_use = true;
        *dir = (filesystem_dir_t){ .slot = (uint8_t)i, .generation = slot->generation };
        return FILESYSTEM_RESULT_OK;
    }
    return FILESYSTEM_RESULT_TOO_MANY_OPEN;
}

filesystem_result_t filesystem_readdir(
    filesystem_dir_t dir,
    filesystem_dirent_t *entry,
    bool *end)
{
    if (entry == NULL || end == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    dir_slot_t *const slot = dir_slot(dir);
    if (slot == NULL) {
        return FILESYSTEM_RESULT_STALE_HANDLE;
    }

    if (slot->pending) {
        slot->pending = false;
    } else {
        const FRESULT result = slot->filtered
            ? f_findnext(&slot->dir, &slot->info)
            : f_readdir(&slot->dir, &slot->info);
        if (result != FR_OK) {
            return map_result(result);
        }
    }

    //an empty name is FatFs's end of directory. "." and ".." never appear: FatFs skips them
    if (slot->info.fname[0] == '\0') {
        *end = true;
        return FILESYSTEM_RESULT_OK;
    }
    *end = false;
    copy_entry(entry, &slot->info);
    return FILESYSTEM_RESULT_OK;
}

filesystem_result_t filesystem_closedir(filesystem_dir_t dir)
{
    dir_slot_t *const slot = dir_slot(dir);
    if (slot == NULL) {
        return FILESYSTEM_RESULT_STALE_HANDLE;
    }
    const FRESULT result = f_closedir(&slot->dir);
    release_dir_slot(slot);
    return map_result(result);
}

/* ----------------------------------------------------------- entries */

filesystem_result_t filesystem_stat(const char *path, filesystem_dirent_t *entry)
{
    if (path == NULL || entry == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (!mounted) {
        return FILESYSTEM_RESULT_NOT_MOUNTED;
    }
    FILINFO info;   //~270 bytes of stack, on top of FatFs's own LFN buffer
    const FRESULT result = f_stat(path, &info);
    if (result != FR_OK) {
        return map_result(result);
    }
    copy_entry(entry, &info);
    return FILESYSTEM_RESULT_OK;
}

//both of these refuse a file that is open (IN_USE), through FatFs's open-entry table (FF_FS_LOCK). that
//table does not cover a directory being listed: f_opendir records a listing under the directory's own
//first cluster, while f_unlink and f_rename look the directory up by its entry in its parent, so the two
//never match. renaming a listed directory is harmless, as the listing follows its clusters, not its
//name; removing one would leave the listing reading freed clusters, so remove checks for it here

//true if path names the directory a listing has open. probing with f_opendir costs a path walk, and only
//happens while a listing of a subdirectory is open
static bool being_listed(const char *path)
{
    for (size_t i = 0; i < FILESYSTEM_MAX_OPEN_DIRS; i++) {
        //the root directory has no cluster of its own here (0), and cannot be removed anyway
        if (!dirs[i].in_use || dirs[i].dir.obj.sclust == 0U) {
            continue;
        }
        DIR probe;
        if (f_opendir(&probe, path) != FR_OK) {
            //not a directory, or not there: f_unlink reports that itself
            return false;
        }
        const bool same = probe.obj.sclust == dirs[i].dir.obj.sclust;
        (void)f_closedir(&probe);
        if (same) {
            return true;
        }
    }
    return false;
}

filesystem_result_t filesystem_rename(const char *old_path, const char *new_path)
{
    if (old_path == NULL || new_path == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (!mounted) {
        return FILESYSTEM_RESULT_NOT_MOUNTED;
    }
    return map_result(f_rename(old_path, new_path));
}

filesystem_result_t filesystem_remove(const char *path)
{
    if (path == NULL) {
        return FILESYSTEM_RESULT_INVALID_ARGUMENT;
    }
    if (!mounted) {
        return FILESYSTEM_RESULT_NOT_MOUNTED;
    }
    if (being_listed(path)) {
        return FILESYSTEM_RESULT_IN_USE;
    }
    return map_result(f_unlink(path));
}
