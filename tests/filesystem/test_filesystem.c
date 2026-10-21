//tests for src/storage/filesystem.c through its public interface (filesystem.h), with the real adapter
//and FatFs underneath and the fake block device at the bottom
//
//what is asserted, and where it comes from:
//  - the contract written in filesystem.h, including the documented removal behaviour
//  - FatFs behaviour that filesystem.h promises callers (dot entries never listed, seek clipping,
//    short writes on a full volume), checked against FatFs's own documentation
//what is deliberately not asserted, because nothing promises it:
//  - which slot a handle gets, or the numeric value of a generation (only that it is never 0 and that
//    a reused slot gets a different one)
//  - the order of directory entries (listings are compared as sets)
//  - the size of a directory entry, the bytes of a seek-extended gap, and the contents of a file whose
//    writer was unmounted mid-write
//  - which error wins when a call has several things wrong with it
//
//a few cases call FatFs directly, marked "white-box": to create a directory or a volume label (the
//interface has no call for either), to read a file's timestamp or first cluster, and to observe whether
//the volume is still registered with FatFs.

#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "fake_block_device.h"
#include "fake_platform.h"
#include "fixture.h"
#include "harness.h"
#include "storage/filesystem.h"
#include "storage/filesystem_diskio.h"

#define R(x) FILESYSTEM_RESULT_##x

/* ------------------------------------------------------------ helpers */

static bool mount_fresh(void)
{
    return fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK
        && fixture_format(&disk)
        && filesystem_mount(&disk.device) == R(OK);
}

//write `size` bytes of fixture_pattern, in deliberately uneven chunks so writes straddle sector and
//cluster boundaries
static bool write_pattern_file(const char *path, uint32_t size)
{
    static const uint32_t chunks[] = { 1, 511, 513, 4096, 7, 1024, 3000 };
    static uint8_t buffer[8192];
    filesystem_file_t file;
    if (filesystem_open(path, FILESYSTEM_OPEN_REPLACE, &file) != R(OK)) {
        return false;
    }
    bool ok = true;
    uint32_t done = 0;
    for (size_t c = 0; ok && done < size; c = (c + 1) % (sizeof chunks / sizeof chunks[0])) {
        uint32_t n = chunks[c] < size - done ? chunks[c] : size - done;
        for (uint32_t i = 0; i < n; i++) {
            buffer[i] = fixture_pattern(done + i);
        }
        size_t written = 0;
        ok = filesystem_write(file, buffer, n, &written) == R(OK) && written == n;
        done += n;
    }
    return filesystem_close(file) == R(OK) && ok;
}

//read the whole file back in a different chunk pattern and compare
static bool check_pattern_file(const char *path, uint32_t size)
{
    static const uint32_t chunks[] = { 700, 1, 512, 5000, 33 };
    static uint8_t buffer[8192];
    filesystem_file_t file;
    if (filesystem_open(path, FILESYSTEM_OPEN_READ, &file) != R(OK)) {
        return false;
    }
    uint32_t actual_size = 0;
    bool ok = filesystem_size(file, &actual_size) == R(OK) && actual_size == size;
    uint32_t done = 0;
    for (size_t c = 0; ok && done < size; c = (c + 1) % (sizeof chunks / sizeof chunks[0])) {
        const uint32_t want = chunks[c] < size - done ? chunks[c] : size - done;
        size_t got = 0;
        ok = filesystem_read(file, buffer, want, &got) == R(OK) && got == want;
        for (uint32_t i = 0; ok && i < got; i++) {
            ok = buffer[i] == fixture_pattern(done + i);
        }
        done += want;
    }
    size_t extra = 1;
    ok = ok && filesystem_read(file, buffer, sizeof buffer, &extra) == R(OK) && extra == 0;
    return filesystem_close(file) == R(OK) && ok;
}

static bool write_text(const char *path, const char *text)
{
    filesystem_file_t file;
    size_t written = 0;
    if (filesystem_open(path, FILESYSTEM_OPEN_REPLACE, &file) != R(OK)) {
        return false;
    }
    const bool ok = filesystem_write(file, text, strlen(text), &written) == R(OK)
        && written == strlen(text);
    return filesystem_close(file) == R(OK) && ok;
}

static bool text_is(const char *path, const char *text)
{
    filesystem_file_t file;
    char buffer[256];
    size_t got = 0;
    if (filesystem_open(path, FILESYSTEM_OPEN_READ, &file) != R(OK)) {
        return false;
    }
    const bool ok = filesystem_read(file, buffer, sizeof buffer, &got) == R(OK)
        && got == strlen(text) && memcmp(buffer, text, got) == 0;
    return filesystem_close(file) == R(OK) && ok;
}

//white-box fixture: the interface has no mkdir
static bool make_dir(const char *path)
{
    return f_mkdir(path) == FR_OK;
}

static bool build_tree(void)
{
    return write_text("/a.txt", "aa")
        && write_text("/b.txt", "bbb")
        && write_text("/c.bin", "c")
        && write_text("/A Long Book Title.epub", "epub!")
        && make_dir("/books")
        && write_text("/books/inner.txt", "inner");
}

#define MAX_LISTED 16

typedef struct {
    char names[MAX_LISTED][FILESYSTEM_NAME_MAX + 1];
    uint32_t sizes[MAX_LISTED];
    bool dirs[MAX_LISTED];
    size_t count;
    bool ok;            //every call succeeded, the listing ended, and it fit
} listing_t;

static listing_t list_dir(const char *path, const char *pattern)
{
    listing_t listing = { .ok = false };
    filesystem_dir_t dir;
    if (filesystem_opendir(path, pattern, &dir) != R(OK)) {
        return listing;
    }
    for (;;) {
        filesystem_dirent_t entry;
        bool end = false;
        if (filesystem_readdir(dir, &entry, &end) != R(OK)) {
            break;
        }
        if (end) {
            //and it stays at the end
            filesystem_dirent_t again;
            bool end_again = false;
            listing.ok = filesystem_readdir(dir, &again, &end_again) == R(OK) && end_again;
            break;
        }
        if (listing.count == MAX_LISTED) {
            break;
        }
        memcpy(listing.names[listing.count], entry.name, sizeof entry.name);
        listing.sizes[listing.count] = entry.size;
        listing.dirs[listing.count] = entry.is_directory;
        listing.count++;
    }
    if (filesystem_closedir(dir) != R(OK)) {
        listing.ok = false;
    }
    return listing;
}

//index of name in the listing, or -1
static int listed(const listing_t *listing, const char *name)
{
    int found = -1;
    for (size_t i = 0; i < listing->count; i++) {
        if (strcmp(listing->names[i], name) == 0) {
            if (found >= 0) {
                return -2;      //listed twice
            }
            found = (int)i;
        }
    }
    return found;
}

//the listing holds exactly these names, in any order
static bool listing_is(const listing_t *listing, const char *const *names, size_t count)
{
    if (!listing->ok || listing->count != count) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (listed(listing, names[i]) < 0) {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------- mount, unmount */

static void fs_mount_rejects_invalid_devices(void)
{
    block_device_t no_context = { .context = NULL, .operations = disk.device.operations };
    block_device_t no_operations = { .context = &disk, .operations = NULL };

    EXPECT_RESULT(filesystem_mount(NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_mount(&no_context), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_mount(&no_operations), R(INVALID_ARGUMENT));
    EXPECT(!filesystem_is_mounted());
    EXPECT(filesystem_diskio_device(FILESYSTEM_DRIVE_SD) == NULL);
    EXPECT_EQ_U(fake_disk_io_calls(&disk), 0);
}

static void fs_mount_blank_card_reports_no_filesystem(void)
{
    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    EXPECT_RESULT(filesystem_mount(&disk.device), R(NO_FILESYSTEM));
    EXPECT(!filesystem_is_mounted());
    EXPECT(filesystem_diskio_device(FILESYSTEM_DRIVE_SD) == NULL);  //a failed mount unbinds

    //white-box: and leaves nothing registered with FatFs
    FILINFO info;
    EXPECT(f_stat("/x", &info) == FR_NOT_ENABLED);

    //the binding really was released: making a volume and mounting it both need to bind
    EXPECT(fixture_format(&disk));
    EXPECT_RESULT(filesystem_mount(&disk.device), R(OK));
    EXPECT(filesystem_is_mounted());
}

static void fs_mount_needs_an_initialized_device(void)
{
    //the coordinator has not brought the card up
    EXPECT_RESULT(filesystem_mount(&disk.device), R(MEDIUM_UNAVAILABLE));
    EXPECT(filesystem_diskio_device(FILESYSTEM_DRIVE_SD) == NULL);
    EXPECT_EQ_U(disk.init_calls, 0);                            //and the layer did not do it itself
    EXPECT_EQ_U(disk.write_calls, 0);
}

static void fs_layer_never_initializes_or_deinitializes_the_device(void)
{
    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE(fixture_format(&disk));
    REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
    EXPECT(write_text("/a.txt", "a"));
    filesystem_unmount();
    EXPECT_RESULT(filesystem_mount(&disk.device), R(OK));
    filesystem_unmount();
    EXPECT_EQ_U(disk.init_calls, 1);                            //the bring-up above
    EXPECT_EQ_U(disk.deinit_calls, 0);
}

static void fs_mount_refused_while_mounted(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/keep.txt", "kept"));
    const unsigned writes = disk.write_calls;
    EXPECT_RESULT(filesystem_mount(&disk.device), R(ALREADY_MOUNTED));
    EXPECT_EQ_U(disk.write_calls, writes);                      //the refused mount wrote nothing
    EXPECT(filesystem_is_mounted());
    EXPECT(text_is("/keep.txt", "kept"));                       //and the volume still works
}

static void fs_unmount_is_idempotent(void)
{
    filesystem_unmount();                 //never mounted
    filesystem_unmount();
    REQUIRE(mount_fresh());
    filesystem_unmount();
    filesystem_unmount();
    EXPECT(!filesystem_is_mounted());

    //white-box: unmount unregistered the volume from FatFs
    FILINFO info;
    EXPECT(f_stat("/x", &info) == FR_NOT_ENABLED);
}

//unmount must be safe against an absent card: no call of any kind reaches the device
static void fs_unmount_performs_no_io(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/r.txt", "reader"));
    filesystem_file_t writer;
    filesystem_file_t reader;
    filesystem_dir_t dir;
    size_t written = 0;
    REQUIRE_RESULT(filesystem_open("/w.bin", FILESYSTEM_OPEN_REPLACE, &writer), R(OK));
    REQUIRE_RESULT(filesystem_write(writer, "unsaved", 7, &written), R(OK));    //dirty, unflushed
    REQUIRE_RESULT(filesystem_open("/r.txt", FILESYSTEM_OPEN_READ, &reader), R(OK));
    REQUIRE_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));

    const unsigned io = fake_disk_io_calls(&disk);
    const unsigned lifecycle = disk.init_calls + disk.deinit_calls;
    filesystem_unmount();
    EXPECT_EQ_U(fake_disk_io_calls(&disk), io);
    EXPECT_EQ_U(disk.init_calls + disk.deinit_calls, lifecycle);
    EXPECT(!filesystem_is_mounted());
    EXPECT(filesystem_diskio_device(FILESYSTEM_DRIVE_SD) == NULL);

    //and it succeeds with the card already gone
    REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
    REQUIRE_RESULT(filesystem_open("/w.bin", FILESYSTEM_OPEN_REPLACE, &writer), R(OK));
    REQUIRE_RESULT(filesystem_write(writer, "unsaved", 7, &written), R(OK));
    fake_disk_remove(&disk);
    const unsigned io_removed = fake_disk_io_calls(&disk);
    filesystem_unmount();
    EXPECT_EQ_U(fake_disk_io_calls(&disk), io_removed);
}

static void fs_unmount_invalidates_every_handle(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "a"));
    filesystem_file_t files[4];
    filesystem_dir_t dir;
    REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &files[0]), R(OK));
    REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &files[1]), R(OK));
    REQUIRE_RESULT(filesystem_open("/b.bin", FILESYSTEM_OPEN_REPLACE, &files[2]), R(OK));
    REQUIRE_RESULT(filesystem_open("/c.bin", FILESYSTEM_OPEN_REPLACE, &files[3]), R(OK));
    REQUIRE_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));

    filesystem_unmount();
    uint8_t buffer[4];
    size_t n;
    uint32_t value;
    bool end;
    filesystem_dirent_t entry;
    for (size_t i = 0; i < 4; i++) {
        EXPECT_RESULT(filesystem_read(files[i], buffer, 1, &n), R(STALE_HANDLE));
    }
    EXPECT_RESULT(filesystem_readdir(dir, &entry, &end), R(STALE_HANDLE));

    //unmount itself freed every slot: the whole pool is available before any old handle is closed
    //(closing a stale handle must not be what frees it)
    REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
    filesystem_file_t again[4];
    filesystem_dir_t dir_again;
    for (size_t i = 0; i < 4; i++) {
        REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &again[i]), R(OK));
    }
    REQUIRE_RESULT(filesystem_opendir("/", NULL, &dir_again), R(OK));

    //the old handles stay stale now that their slots are occupied again, through every call
    for (size_t i = 0; i < 4; i++) {
        EXPECT_RESULT(filesystem_read(files[i], buffer, 1, &n), R(STALE_HANDLE));
        EXPECT_RESULT(filesystem_write(files[i], buffer, 1, &n), R(STALE_HANDLE));
        EXPECT_RESULT(filesystem_seek(files[i], 0), R(STALE_HANDLE));
        EXPECT_RESULT(filesystem_tell(files[i], &value), R(STALE_HANDLE));
        EXPECT_RESULT(filesystem_size(files[i], &value), R(STALE_HANDLE));
        EXPECT_RESULT(filesystem_close(files[i]), R(STALE_HANDLE));
    }
    EXPECT_RESULT(filesystem_readdir(dir, &entry, &end), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_closedir(dir), R(STALE_HANDLE));

    //and closing them did not touch the new occupants
    for (size_t i = 0; i < 4; i++) {
        EXPECT_RESULT(filesystem_read(again[i], buffer, sizeof buffer, &n), R(OK));
        EXPECT(n == 1 && buffer[0] == 'a');
    }
    EXPECT_RESULT(filesystem_readdir(dir_again, &entry, &end), R(OK));
    EXPECT(!end);
}

static void fs_unmount_warns_only_about_open_writers(void)
{
    filesystem_file_t a, b;
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/r.txt", "r"));

    REQUIRE_RESULT(filesystem_open("/r.txt", FILESYSTEM_OPEN_READ, &a), R(OK));
    REQUIRE_RESULT(filesystem_open("/r.txt", FILESYSTEM_OPEN_READ, &b), R(OK));
    fake_log_reset();
    filesystem_unmount();
    EXPECT_EQ_U(fake_log_warnings(), 0);

    REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
    REQUIRE_RESULT(filesystem_open("/w1.bin", FILESYSTEM_OPEN_REPLACE, &a), R(OK));
    REQUIRE_RESULT(filesystem_close(a), R(OK));                 //a closed writer is not reported
    REQUIRE_RESULT(filesystem_open("/w2.bin", FILESYSTEM_OPEN_REPLACE, &a), R(OK));
    REQUIRE_RESULT(filesystem_open("/r.txt", FILESYSTEM_OPEN_READ, &b), R(OK));
    fake_log_reset();
    filesystem_unmount();
    EXPECT_EQ_U(fake_log_warnings(), 1);
}

//the medium may have changed while unmounted (USB mode, another card): the next mount must read it
//afresh. here a "host" rewrites the whole card behind the layer's back
static void fs_mount_assumes_a_changed_medium(void)
{
    fake_disk_t other;
    REQUIRE(fake_disk_create(&other, FIXTURE_DEFAULT_BLOCKS));
    bool ok = fake_disk_bring_up(&other) == BLOCK_DEVICE_RESULT_OK
        && fixture_format(&other)
        && filesystem_mount(&other.device) == R(OK)
        && write_text("/from-host.txt", "host");
    filesystem_unmount();
    EXPECT(ok);

    filesystem_dirent_t entry;
    EXPECT(mount_fresh());
    EXPECT(write_text("/local.txt", "local"));
    EXPECT_RESULT(filesystem_stat("/local.txt", &entry), R(OK));
    filesystem_unmount();

    memcpy(disk.data, other.data, (size_t)FIXTURE_DEFAULT_BLOCKS * FAKE_BLOCK_SIZE);
    fake_disk_destroy(&other);

    EXPECT_RESULT(filesystem_mount(&disk.device), R(OK));
    EXPECT_RESULT(filesystem_stat("/local.txt", &entry), R(NOT_FOUND));
    EXPECT(text_is("/from-host.txt", "host"));
}

/* ------------------------------------------------------------ handles */

static void fs_zero_handles_are_always_stale(void)
{
    const filesystem_file_t zero_file = { 0 };
    const filesystem_dir_t zero_dir = { 0 };
    uint8_t buffer[4];
    size_t n;
    uint32_t value;
    bool end;
    filesystem_dirent_t entry;

    //before anything was ever opened
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "abc"));
    EXPECT_RESULT(filesystem_read(zero_file, buffer, 1, &n), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_readdir(zero_dir, &entry, &end), R(STALE_HANDLE));

    //and with every slot in use, so whichever slot a zero handle names is occupied
    filesystem_file_t files[4];
    filesystem_dir_t dir;
    for (size_t i = 0; i < 4; i++) {
        REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &files[i]), R(OK));
        EXPECT(files[i].generation != 0);
    }
    REQUIRE_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));
    EXPECT(dir.generation != 0);

    EXPECT_RESULT(filesystem_read(zero_file, buffer, 1, &n), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_write(zero_file, buffer, 1, &n), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_seek(zero_file, 0), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_tell(zero_file, &value), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_size(zero_file, &value), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_close(zero_file), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_readdir(zero_dir, &entry, &end), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_closedir(zero_dir), R(STALE_HANDLE));

    //the real handles were untouched by those attempts
    for (size_t i = 0; i < 4; i++) {
        EXPECT_RESULT(filesystem_size(files[i], &value), R(OK));
        EXPECT_EQ_U(value, 3);
    }
}

static void fs_forged_handles_are_stale(void)
{
    uint32_t value;
    bool end;
    filesystem_dirent_t entry;
    filesystem_file_t file;
    filesystem_dir_t dir;

    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "abc"));
    REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &file), R(OK));
    REQUIRE_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));

    const filesystem_file_t wrong_generation = { .slot = file.slot, .generation = (uint16_t)(file.generation + 1U) };
    const filesystem_file_t out_of_range = { .slot = 4, .generation = file.generation };
    const filesystem_file_t far_out = { .slot = 255, .generation = file.generation };
    EXPECT_RESULT(filesystem_size(wrong_generation, &value), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_size(out_of_range, &value), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_size(far_out, &value), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_close(far_out), R(STALE_HANDLE));

    const filesystem_dir_t dir_wrong_generation = { .slot = dir.slot, .generation = (uint16_t)(dir.generation + 1U) };
    const filesystem_dir_t dir_out_of_range = { .slot = 1, .generation = dir.generation };
    EXPECT_RESULT(filesystem_readdir(dir_wrong_generation, &entry, &end), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_readdir(dir_out_of_range, &entry, &end), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_closedir(dir_out_of_range), R(STALE_HANDLE));

    EXPECT_RESULT(filesystem_size(file, &value), R(OK));        //the real ones still work
    EXPECT_RESULT(filesystem_readdir(dir, &entry, &end), R(OK));

    //a guessed handle naming a closed slot, with any generation near the one it had, never reaches
    //the closed file (size and tell read FatFs's object without FatFs validating it)
    REQUIRE_RESULT(filesystem_close(file), R(OK));
    REQUIRE_RESULT(filesystem_closedir(dir), R(OK));
    for (int delta = -2; delta <= 2; delta++) {
        const filesystem_file_t guess = { .slot = file.slot, .generation = (uint16_t)(file.generation + delta) };
        const filesystem_dir_t dir_guess = { .slot = dir.slot, .generation = (uint16_t)(dir.generation + delta) };
        EXPECT_RESULT(filesystem_size(guess, &value), R(STALE_HANDLE));
        EXPECT_RESULT(filesystem_tell(guess, &value), R(STALE_HANDLE));
        EXPECT_RESULT(filesystem_readdir(dir_guess, &entry, &end), R(STALE_HANDLE));
    }
}

static void fs_closed_handle_stays_stale_after_its_slot_is_reused(void)
{
    uint8_t buffer[4];
    size_t n;
    filesystem_file_t old;
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "abc"));
    REQUIRE(write_text("/b.txt", "xyz"));
    REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &old), R(OK));
    EXPECT_RESULT(filesystem_close(old), R(OK));
    EXPECT_RESULT(filesystem_close(old), R(STALE_HANDLE));     //double close

    //fill the pool, so old's slot is certainly reused
    filesystem_file_t files[4];
    int reused = -1;
    for (int i = 0; i < 4; i++) {
        REQUIRE_RESULT(filesystem_open("/b.txt", FILESYSTEM_OPEN_READ, &files[i]), R(OK));
        if (files[i].slot == old.slot) {
            reused = i;
        }
    }
    REQUIRE(reused >= 0);
    EXPECT(files[reused].generation != old.generation);
    EXPECT_RESULT(filesystem_read(old, buffer, sizeof buffer, &n), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_close(old), R(STALE_HANDLE));
    //the new occupant is unaffected and reads its own file
    EXPECT_RESULT(filesystem_read(files[reused], buffer, sizeof buffer, &n), R(OK));
    EXPECT(n == 3 && memcmp(buffer, "xyz", 3) == 0);
}

//a generation is 16 bits: more than 65535 reopenings of one slot must wrap past 0 without ever issuing
//it, and every reopening must invalidate the previous handle. an open/close cycle advances the
//generation twice, so one pass only ever issues values of one parity; a second pass, shifted by the
//single advance an unmount makes, covers the other, so that whichever advance reaches the wrap, some
//open lands on it
static void fs_generation_wraps_without_issuing_zero(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "abc"));
    unsigned zero_issued = 0;
    unsigned wrong_slot = 0;
    unsigned repeated = 0;
    unsigned stale_accepted = 0;
    uint16_t first_generation[2] = { 0, 0 };

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            filesystem_unmount();
            REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
        }
        //with three slots held, every open below lands in the same, last free slot
        filesystem_file_t holders[3];
        for (size_t i = 0; i < 3; i++) {
            REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &holders[i]), R(OK));
        }
        filesystem_file_t previous;
        REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &previous), R(OK));
        REQUIRE_RESULT(filesystem_close(previous), R(OK));
        first_generation[pass] = previous.generation;
        for (unsigned i = 0; i < 65536U + 16U; i++) {
            filesystem_file_t file;
            if (filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &file) != R(OK)) {
                EXPECT(false);
                return;
            }
            zero_issued += file.generation == 0;
            wrong_slot += file.slot != previous.slot;
            repeated += file.generation == previous.generation;
            uint32_t size;
            stale_accepted += filesystem_size(previous, &size) != R(STALE_HANDLE);
            if (filesystem_close(file) != R(OK)) {
                EXPECT(false);
                return;
            }
            previous = file;
        }
        for (size_t i = 0; i < 3; i++) {
            REQUIRE_RESULT(filesystem_close(holders[i]), R(OK));
        }
    }
    EXPECT_EQ_U(zero_issued, 0);
    EXPECT_EQ_U(wrong_slot, 0);
    EXPECT_EQ_U(repeated, 0);
    EXPECT_EQ_U(stale_accepted, 0);
    //the two passes really did start on opposite parities, or the second pass proved nothing
    EXPECT((first_generation[0] ^ first_generation[1]) & 1U);
}

static void fs_pools_are_limited_and_independent(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "abc"));
    filesystem_file_t files[4];
    filesystem_file_t extra;
    filesystem_dir_t dir;
    filesystem_dir_t extra_dir;
    for (size_t i = 0; i < 4; i++) {
        REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &files[i]), R(OK));
    }
    EXPECT_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &extra), R(TOO_MANY_OPEN));
    EXPECT_RESULT(filesystem_open("/new.bin", FILESYSTEM_OPEN_REPLACE, &extra), R(TOO_MANY_OPEN));
    filesystem_dirent_t entry;
    EXPECT_RESULT(filesystem_stat("/new.bin", &entry), R(NOT_FOUND));   //refused before creating it

    EXPECT_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));          //files do not use dir slots
    EXPECT_RESULT(filesystem_opendir("/", NULL, &extra_dir), R(TOO_MANY_OPEN));
    EXPECT_RESULT(filesystem_opendir("/", "*", &extra_dir), R(TOO_MANY_OPEN));

    EXPECT_RESULT(filesystem_close(files[2]), R(OK));
    EXPECT_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &extra), R(OK));
    EXPECT_RESULT(filesystem_closedir(dir), R(OK));
    EXPECT_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));
}

static void fs_failed_opens_do_not_consume_slots(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "abc"));
    filesystem_file_t file;
    filesystem_dir_t dir;
    for (int i = 0; i < 10; i++) {
        EXPECT_RESULT(filesystem_open("/missing.txt", FILESYSTEM_OPEN_READ, &file), R(NOT_FOUND));
        EXPECT_RESULT(filesystem_opendir("/missing", NULL, &dir), R(NOT_FOUND));
        EXPECT_RESULT(filesystem_opendir("/missing", "*", &dir), R(NOT_FOUND));
    }
    filesystem_file_t files[4];
    for (size_t i = 0; i < 4; i++) {
        EXPECT_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &files[i]), R(OK));
    }
    EXPECT_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));
}

//closing frees the slot even when the close itself fails: the handle is spent either way
static void fs_close_frees_the_slot_even_when_it_fails(void)
{
    REQUIRE(mount_fresh());
    filesystem_file_t file;
    size_t written;
    REQUIRE_RESULT(filesystem_open("/w.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
    REQUIRE_RESULT(filesystem_write(file, "dirty", 5, &written), R(OK));
    fake_disk_fail_write(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    disk.write_fault.repeat = 1;                                //the close's retry fails too
    EXPECT_RESULT(filesystem_close(file), R(STORAGE_ERROR));
    EXPECT_RESULT(filesystem_close(file), R(STALE_HANDLE));

    filesystem_file_t files[4];
    REQUIRE(write_text("/a.txt", "abc"));                       //the device itself is still usable
    for (size_t i = 0; i < 4; i++) {
        EXPECT_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &files[i]), R(OK));
    }
}

/* ---------------------------------------------------------- open entries */

//filesystem.h, "Open entries": what would corrupt the volume while an entry is open is refused with
//IN_USE and changes nothing; reading a file through several handles is not
static void fs_open_entries_in_use_are_refused(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(build_tree());
    REQUIRE(make_dir("/empty"));
    filesystem_file_t reader;
    filesystem_file_t second;
    filesystem_file_t writer;
    filesystem_dir_t dir;
    filesystem_dirent_t entry;
    size_t written = 0;

    //open for reading: more readers may open it; nothing may replace, remove or rename it
    REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &reader), R(OK));
    EXPECT_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &second), R(OK));
    EXPECT_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_REPLACE, &writer), R(IN_USE));
    EXPECT_RESULT(filesystem_remove("/a.txt"), R(IN_USE));
    EXPECT_RESULT(filesystem_rename("/a.txt", "/moved.txt"), R(IN_USE));
    EXPECT_RESULT(filesystem_close(second), R(OK));
    EXPECT_RESULT(filesystem_remove("/a.txt"), R(IN_USE));     //one reader is enough to hold it
    EXPECT_RESULT(filesystem_close(reader), R(OK));
    EXPECT(text_is("/a.txt", "aa"));                            //the refusals changed nothing
    EXPECT_RESULT(filesystem_rename("/a.txt", "/moved.txt"), R(OK));

    //open for replacement: nothing else may open, remove or rename it
    REQUIRE_RESULT(filesystem_open("/b.txt", FILESYSTEM_OPEN_REPLACE, &writer), R(OK));
    REQUIRE_RESULT(filesystem_write(writer, "new", 3, &written), R(OK));
    EXPECT_RESULT(filesystem_open("/b.txt", FILESYSTEM_OPEN_READ, &reader), R(IN_USE));
    EXPECT_RESULT(filesystem_open("/b.txt", FILESYSTEM_OPEN_REPLACE, &second), R(IN_USE));
    EXPECT_RESULT(filesystem_remove("/b.txt"), R(IN_USE));
    EXPECT_RESULT(filesystem_rename("/b.txt", "/other.txt"), R(IN_USE));
    EXPECT_RESULT(filesystem_close(writer), R(OK));
    EXPECT(text_is("/b.txt", "new"));
    EXPECT_RESULT(filesystem_stat("/other.txt", &entry), R(NOT_FOUND));

    //a directory being listed may not be removed, by either of its names, but may be renamed: the
    //listing follows the directory, so it carries on and ends normally
    REQUIRE(write_text("/books/second.txt", "2"));
    REQUIRE_RESULT(filesystem_opendir("/empty", NULL, &dir), R(OK));
    EXPECT_RESULT(filesystem_remove("/empty"), R(IN_USE));
    EXPECT_RESULT(filesystem_rename("/empty", "/renamed"), R(OK));
    EXPECT_RESULT(filesystem_remove("/renamed"), R(IN_USE));
    EXPECT_RESULT(filesystem_remove("/books/second.txt"), R(OK));    //another directory's contents are free
    filesystem_dirent_t listed;
    bool end = false;
    EXPECT_RESULT(filesystem_readdir(dir, &listed, &end), R(OK));
    EXPECT(end);
    EXPECT_RESULT(filesystem_closedir(dir), R(OK));
    EXPECT_RESULT(filesystem_remove("/renamed"), R(OK));
    EXPECT_RESULT(filesystem_stat("/renamed", &entry), R(NOT_FOUND));
}

//f_close releases a file's open entry only after a successful flush. the layer must release it anyway,
//or a file whose close failed with the card still in would be IN_USE until the next mount. both ways a
//close can fail: writing the buffered data, and the sync afterwards
static void fs_failed_close_releases_the_entry(void)
{
    for (int via_sync = 0; via_sync <= 1; via_sync++) {
        filesystem_unmount();
        REQUIRE(mount_fresh());
        filesystem_file_t file;
        size_t written = 0;
        REQUIRE_RESULT(filesystem_open("/w.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
        REQUIRE_RESULT(filesystem_write(file, "dirty", 5, &written), R(OK));
        if (via_sync) {
            fake_disk_fail_sync(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
            disk.sync_fault.repeat = 1;                         //the close's retry fails too
        } else {
            fake_disk_fail_write(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
            disk.write_fault.repeat = 1;
        }
        EXPECT_RESULT(filesystem_close(file), R(STORAGE_ERROR));

        EXPECT_RESULT(filesystem_open("/w.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
        EXPECT_RESULT(filesystem_write(file, "again", 5, &written), R(OK));
        EXPECT_RESULT(filesystem_close(file), R(OK));
        EXPECT(text_is("/w.bin", "again"));
        EXPECT_RESULT(filesystem_remove("/w.bin"), R(OK));
    }
}

//an unmount drops handles without closing them, so it must empty the open-entry table: after a remount,
//by the coordinator or after a removal, nothing is still IN_USE
static void fs_unmount_releases_every_entry(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "a"));
    REQUIRE(make_dir("/sub"));
    filesystem_file_t file;
    filesystem_dir_t dir;

    for (int removed = 0; removed <= 1; removed++) {
        REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
        REQUIRE_RESULT(filesystem_opendir("/sub", NULL, &dir), R(OK));
        if (removed) {
            fake_disk_remove(&disk);
        }
        filesystem_unmount();
        if (removed) {
            fake_disk_insert(&disk);
            REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
        }
        REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));

        EXPECT_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &file), R(OK));
        EXPECT_RESULT(filesystem_close(file), R(OK));
        EXPECT_RESULT(filesystem_rename("/sub", "/sub2"), R(OK));
        EXPECT_RESULT(filesystem_rename("/sub2", "/sub"), R(OK));
    }
}

//f_findfirst opens the directory before it reads the first entry. if that read fails, the layer must
//close the directory again, or its entry stays in FatFs's open-entry table. FatFs never refuses anything
//because of a listed directory, so the leak would show only as a full table: FF_FS_LOCK has exactly one
//entry per pool slot, and with one leaked, filling every pool fails at the last open. a read fault is
//swept across the listing's reads, so whichever read FatFs does not serve from its cache is the one
//that fails
static void fs_failed_filtered_opendir_releases_the_directory(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(make_dir("/sub"));
    REQUIRE(make_dir("/other"));
    char name[8];
    for (int i = 0; i < 4; i++) {
        snprintf(name, sizeof name, "/f%d", i);
        REQUIRE(write_text(name, "f"));
    }

    bool failed_once = false;
    for (int n = 0; n < 16; n++) {
        filesystem_dir_t dir;
        fake_disk_fail_read(&disk, n, BLOCK_DEVICE_RESULT_IO_ERROR);
        const filesystem_result_t result = filesystem_opendir("/sub", "*", &dir);
        const bool fired = disk.read_fault.calls_until < 0;
        fake_disk_fail_read(&disk, -1, BLOCK_DEVICE_RESULT_OK);     //disarm, if it never fired
        if (!fired) {
            EXPECT_RESULT(result, R(OK));
            EXPECT_RESULT(filesystem_closedir(dir), R(OK));
            break;
        }
        if (result == R(OK)) {
            EXPECT_RESULT(filesystem_closedir(dir), R(OK));
            continue;
        }
        failed_once = true;

        //every pool slot can still be filled, so the failed listing left no entry behind
        filesystem_file_t files[4];
        for (int i = 0; i < 4; i++) {
            snprintf(name, sizeof name, "/f%d", i);
            EXPECT_RESULT(filesystem_open(name, FILESYSTEM_OPEN_READ, &files[i]), R(OK));
        }
        EXPECT_RESULT(filesystem_opendir("/other", NULL, &dir), R(OK));
        (void)filesystem_closedir(dir);
        for (int i = 0; i < 4; i++) {
            (void)filesystem_close(files[i]);
        }
    }
    EXPECT(failed_once);
}

//close is where a writer learns its data is on the card (filesystem.h), so it must ask the device to
//sync, and a sync the device fails must fail the close
static void fs_close_reports_a_failed_sync(void)
{
    REQUIRE(mount_fresh());
    filesystem_file_t file;
    size_t written;
    unsigned syncs = disk.sync_calls;
    REQUIRE(write_text("/ok.txt", "synced"));
    EXPECT(disk.sync_calls > syncs);

    REQUIRE_RESULT(filesystem_open("/w.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
    REQUIRE_RESULT(filesystem_write(file, "dirty", 5, &written), R(OK));
    syncs = disk.sync_calls;
    fake_disk_fail_sync(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
    disk.sync_fault.repeat = 1;                                 //the close's retry fails too
    EXPECT_RESULT(filesystem_close(file), R(STORAGE_ERROR));
    EXPECT(disk.sync_calls > syncs);
}

//a flush that fails once is retried before the close reports it (filesystem.h), or a transient error
//would leave the file's new clusters allocated with no entry pointing at them. each of the close's
//writes is failed in turn - the file's buffered sector, the FAT sector in the volume's window, the
//directory entry - and then its device sync. the close succeeds, and after a remount the file reads
//back whole: its entry reached the card. the directory-entry write is the case where FatFs has already
//marked the file clean when it fails, so the retry must mark it modified again to rewrite anything
//a card pulled during the close's flush fails the retry too, but differently (FatFs finds the drive not
//initialized): the close must report the removal as the storage error it is, not a stale handle
static void fs_close_reports_a_removal_as_a_storage_error(void)
{
    REQUIRE(mount_fresh());
    filesystem_file_t file;
    size_t written = 0;
    REQUIRE_RESULT(filesystem_open("/w.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
    REQUIRE_RESULT(filesystem_write(file, "dirty", 5, &written), R(OK));
    fake_disk_remove(&disk);
    EXPECT_RESULT(filesystem_close(file), R(STORAGE_ERROR));
    EXPECT_RESULT(filesystem_close(file), R(STALE_HANDLE));
}

static void fs_close_retries_a_failed_flush(void)
{
    for (int failing = 0; failing <= 3; failing++) {
        filesystem_unmount();
        REQUIRE(mount_fresh());
        filesystem_file_t file;
        size_t written = 0;
        REQUIRE_RESULT(filesystem_open("/w.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
        REQUIRE_RESULT(filesystem_write(file, "dirty", 5, &written), R(OK));
        if (failing < 3) {
            fake_disk_fail_write(&disk, failing, BLOCK_DEVICE_RESULT_IO_ERROR);
        } else {
            fake_disk_fail_sync(&disk, 0, BLOCK_DEVICE_RESULT_IO_ERROR);
        }
        EXPECT_RESULT(filesystem_close(file), R(OK));
        //the fault fired during the close (it disarms once it has)
        EXPECT(disk.write_fault.calls_until == -1 && disk.sync_fault.calls_until == -1);
        filesystem_unmount();
        REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
        EXPECT(text_is("/w.bin", "dirty"));
    }
}

/* -------------------------------------------------------------- files */

static void fs_calls_refused_while_unmounted(void)
{
    filesystem_file_t file;
    filesystem_dir_t dir;
    filesystem_dirent_t entry;
    char label[FILESYSTEM_LABEL_SIZE];
    EXPECT_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &file), R(NOT_MOUNTED));
    EXPECT_RESULT(filesystem_opendir("/", NULL, &dir), R(NOT_MOUNTED));
    EXPECT_RESULT(filesystem_stat("/a.txt", &entry), R(NOT_MOUNTED));
    EXPECT_RESULT(filesystem_rename("/a.txt", "/b.txt"), R(NOT_MOUNTED));
    EXPECT_RESULT(filesystem_remove("/a.txt"), R(NOT_MOUNTED));
    EXPECT_RESULT(filesystem_get_label(label, sizeof label), R(NOT_MOUNTED));
    EXPECT_EQ_U(fake_disk_io_calls(&disk), 0);
}

static void fs_null_arguments_rejected(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "abc"));
    filesystem_file_t file;
    filesystem_dir_t dir;
    filesystem_dirent_t entry;
    uint8_t buffer[4];
    size_t n;
    bool end;
    REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &file), R(OK));
    REQUIRE_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));

    EXPECT_RESULT(filesystem_open(NULL, FILESYSTEM_OPEN_READ, &file), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_read(file, NULL, 1, &n), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_read(file, buffer, 1, NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_write(file, NULL, 1, &n), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_write(file, buffer, 1, NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_tell(file, NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_size(file, NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_opendir(NULL, NULL, &dir), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_opendir("/", NULL, NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_readdir(dir, NULL, &end), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_readdir(dir, &entry, NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_stat(NULL, &entry), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_stat("/a.txt", NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_rename(NULL, "/b.txt"), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_rename("/a.txt", NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_remove(NULL), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_get_label(NULL, FILESYSTEM_LABEL_SIZE), R(INVALID_ARGUMENT));

    //none of that disturbed the open handles
    EXPECT_RESULT(filesystem_read(file, buffer, 3, &n), R(OK));
    EXPECT(n == 3 && memcmp(buffer, "abc", 3) == 0);
}

static void fs_open_rejects_unknown_modes(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "abc"));
    filesystem_file_t file;
    EXPECT_RESULT(filesystem_open("/a.txt", (filesystem_open_mode_t)2, &file), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_open("/a.txt", (filesystem_open_mode_t)-1, &file), R(INVALID_ARGUMENT));
    EXPECT(text_is("/a.txt", "abc"));                           //not truncated by a bad mode
}

static void fs_open_reports_missing_paths(void)
{
    REQUIRE(mount_fresh());
    filesystem_file_t file;
    EXPECT_RESULT(filesystem_open("/missing.txt", FILESYSTEM_OPEN_READ, &file), R(NOT_FOUND));
    EXPECT_RESULT(filesystem_open("/no/such/dir.txt", FILESYSTEM_OPEN_READ, &file), R(NOT_FOUND));
    EXPECT_RESULT(filesystem_open("/no/such/dir.txt", FILESYSTEM_OPEN_REPLACE, &file), R(NOT_FOUND));
}

static void fs_names_are_validated(void)
{
    REQUIRE(mount_fresh());
    filesystem_file_t file;
    char path[2 + 256 + 1];

    EXPECT_RESULT(filesystem_open("/bad*name.txt", FILESYSTEM_OPEN_REPLACE, &file), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_open("/bad\"name.txt", FILESYSTEM_OPEN_REPLACE, &file), R(INVALID_ARGUMENT));

    //255 characters is the longest FAT name; FILESYSTEM_NAME_MAX bytes hold it in a listing
    path[0] = '/';
    memset(path + 1, 'n', 255);
    path[256] = '\0';
    EXPECT(write_text(path, "longest"));
    const listing_t root = list_dir("/", NULL);
    EXPECT(root.ok && root.count == 1 && strlen(root.names[0]) == 255 && strcmp(root.names[0], path + 1) == 0);
    EXPECT(text_is(path, "longest"));

    path[256] = 'n';
    path[257] = '\0';
    EXPECT_RESULT(filesystem_open(path, FILESYSTEM_OPEN_REPLACE, &file), R(INVALID_ARGUMENT));
}

static void fs_replace_creates_and_truncates(void)
{
    REQUIRE(mount_fresh());
    filesystem_file_t file;
    filesystem_dirent_t entry;
    REQUIRE(write_pattern_file("/f.bin", 5000));
    REQUIRE_RESULT(filesystem_open("/f.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
    uint32_t size = 1;
    EXPECT_RESULT(filesystem_size(file, &size), R(OK));
    EXPECT_EQ_U(size, 0);
    EXPECT_RESULT(filesystem_close(file), R(OK));
    EXPECT_RESULT(filesystem_stat("/f.bin", &entry), R(OK));
    EXPECT_EQ_U(entry.size, 0);

    EXPECT_RESULT(filesystem_stat("/new.bin", &entry), R(NOT_FOUND));
    REQUIRE_RESULT(filesystem_open("/new.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
    EXPECT_RESULT(filesystem_close(file), R(OK));
    EXPECT_RESULT(filesystem_stat("/new.bin", &entry), R(OK));
}

//READ is read-only and REPLACE is write-only (filesystem.h)
static void fs_access_modes_are_enforced(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "abc"));
    filesystem_file_t reader;
    filesystem_file_t writer;
    uint8_t buffer[4] = { 0 };
    size_t n = 99;
    REQUIRE_RESULT(filesystem_open("/a.txt", FILESYSTEM_OPEN_READ, &reader), R(OK));
    EXPECT_RESULT(filesystem_write(reader, "x", 1, &n), R(DENIED));
    EXPECT_EQ_U(n, 0);
    REQUIRE_RESULT(filesystem_close(reader), R(OK));
    EXPECT(text_is("/a.txt", "abc"));

    REQUIRE_RESULT(filesystem_open("/b.bin", FILESYSTEM_OPEN_REPLACE, &writer), R(OK));
    n = 99;
    EXPECT_RESULT(filesystem_read(writer, buffer, 1, &n), R(DENIED));
    EXPECT_EQ_U(n, 0);
    EXPECT_RESULT(filesystem_close(writer), R(OK));
}

static void fs_large_file_roundtrip(void)
{
    REQUIRE(mount_fresh());
    EXPECT(write_pattern_file("/big.bin", 100003));             //prime: ends mid-sector, mid-cluster
    EXPECT(check_pattern_file("/big.bin", 100003));
    EXPECT(write_pattern_file("/empty.bin", 0));
    EXPECT(check_pattern_file("/empty.bin", 0));
    //and it survives a remount, so it reached the card rather than a cache
    filesystem_unmount();
    REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
    EXPECT(check_pattern_file("/big.bin", 100003));
}

static void fs_reads_are_short_only_at_end_of_file(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_pattern_file("/f.bin", 1000));
    filesystem_file_t file;
    uint8_t buffer[600];
    size_t n = 0;
    REQUIRE_RESULT(filesystem_open("/f.bin", FILESYSTEM_OPEN_READ, &file), R(OK));
    EXPECT_RESULT(filesystem_read(file, buffer, 600, &n), R(OK));
    EXPECT_EQ_U(n, 600);
    EXPECT_RESULT(filesystem_read(file, buffer, 600, &n), R(OK));
    EXPECT_EQ_U(n, 400);
    EXPECT(buffer[0] == fixture_pattern(600) && buffer[399] == fixture_pattern(999));
    EXPECT_RESULT(filesystem_read(file, buffer, 600, &n), R(OK));
    EXPECT_EQ_U(n, 0);
    EXPECT_RESULT(filesystem_read(file, buffer, 0, &n), R(OK));
    EXPECT_EQ_U(n, 0);
}

static void fs_seek_tell_and_size(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_pattern_file("/f.bin", 500));
    filesystem_file_t file;
    uint32_t value = 0;
    uint8_t byte = 0;
    size_t n = 0;

    //read-only: an offset past the end stops at the end
    REQUIRE_RESULT(filesystem_open("/f.bin", FILESYSTEM_OPEN_READ, &file), R(OK));
    EXPECT_RESULT(filesystem_tell(file, &value), R(OK));
    EXPECT_EQ_U(value, 0);
    EXPECT_RESULT(filesystem_seek(file, 100), R(OK));
    EXPECT_RESULT(filesystem_read(file, &byte, 1, &n), R(OK));
    EXPECT(n == 1 && byte == fixture_pattern(100));
    EXPECT_RESULT(filesystem_tell(file, &value), R(OK));
    EXPECT_EQ_U(value, 101);
    EXPECT_RESULT(filesystem_seek(file, 100000), R(OK));
    EXPECT_RESULT(filesystem_tell(file, &value), R(OK));
    EXPECT_EQ_U(value, 500);
    EXPECT_RESULT(filesystem_size(file, &value), R(OK));
    EXPECT_EQ_U(value, 500);
    EXPECT_RESULT(filesystem_close(file), R(OK));

    //REPLACE: an offset past the end extends the file
    REQUIRE_RESULT(filesystem_open("/g.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
    EXPECT_RESULT(filesystem_seek(file, 3000), R(OK));
    EXPECT_RESULT(filesystem_tell(file, &value), R(OK));
    EXPECT_EQ_U(value, 3000);
    EXPECT_RESULT(filesystem_size(file, &value), R(OK));
    EXPECT_EQ_U(value, 3000);
    EXPECT_RESULT(filesystem_write(file, "tail", 4, &n), R(OK));
    EXPECT_RESULT(filesystem_seek(file, 0), R(OK));
    EXPECT_RESULT(filesystem_write(file, "head", 4, &n), R(OK));
    EXPECT_RESULT(filesystem_size(file, &value), R(OK));
    EXPECT_EQ_U(value, 3004);
    EXPECT_RESULT(filesystem_close(file), R(OK));

    char buffer[4];
    REQUIRE_RESULT(filesystem_open("/g.bin", FILESYSTEM_OPEN_READ, &file), R(OK));
    EXPECT_RESULT(filesystem_read(file, buffer, 4, &n), R(OK));
    EXPECT(n == 4 && memcmp(buffer, "head", 4) == 0);
    EXPECT_RESULT(filesystem_seek(file, 3000), R(OK));
    EXPECT_RESULT(filesystem_read(file, buffer, 4, &n), R(OK));
    EXPECT(n == 4 && memcmp(buffer, "tail", 4) == 0);
}

//a full volume: writes come up short with OK, and a seek that would extend stops short with OK
static void fs_full_volume_gives_short_writes(void)
{
    fixture_resize_disk(4096);                                  //2 MiB
    REQUIRE(mount_fresh());
    filesystem_file_t file;
    static uint8_t chunk[64 * 1024];
    for (uint32_t i = 0; i < sizeof chunk; i++) {
        chunk[i] = fixture_pattern(i);
    }

    REQUIRE_RESULT(filesystem_open("/fill.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
    uint32_t total = 0;
    size_t n = sizeof chunk;
    int rounds = 0;
    while (n == sizeof chunk && rounds++ < 64) {                //64 x 64 KiB is twice the card
        EXPECT_RESULT(filesystem_write(file, chunk, sizeof chunk, &n), R(OK));
        total += (uint32_t)n;
    }
    EXPECT(n < sizeof chunk);                                   //it came up short, with OK
    EXPECT(total > 1024U * 1024U && total < 2U * 1024U * 1024U);
    EXPECT_RESULT(filesystem_write(file, chunk, sizeof chunk, &n), R(OK));
    EXPECT_EQ_U(n, 0);
    EXPECT_RESULT(filesystem_close(file), R(OK));

    filesystem_dirent_t entry;
    EXPECT_RESULT(filesystem_stat("/fill.bin", &entry), R(OK));
    EXPECT_EQ_U(entry.size, total);

    uint32_t position = 0;
    REQUIRE_RESULT(filesystem_open("/extend.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
    EXPECT_RESULT(filesystem_seek(file, 10U * 1024U * 1024U), R(OK));
    EXPECT_RESULT(filesystem_tell(file, &position), R(OK));
    EXPECT(position < 10U * 1024U * 1024U);
    EXPECT_RESULT(filesystem_close(file), R(OK));
}

/* -------------------------------------------------------- directories */

static void fs_listing_returns_every_entry_once(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(build_tree());

    const listing_t root = list_dir("/", NULL);
    static const char *const root_names[] = { "a.txt", "b.txt", "c.bin", "A Long Book Title.epub", "books" };
    EXPECT(listing_is(&root, root_names, 5));
    const int a = listed(&root, "a.txt");
    const int epub = listed(&root, "A Long Book Title.epub");
    const int books = listed(&root, "books");
    REQUIRE(a >= 0 && epub >= 0 && books >= 0);
    EXPECT(root.sizes[a] == 2 && !root.dirs[a]);
    EXPECT(root.sizes[epub] == 5 && !root.dirs[epub]);
    EXPECT(root.dirs[books]);

    //no "." or ".." in a subdirectory
    const listing_t sub = list_dir("/books", NULL);
    static const char *const sub_names[] = { "inner.txt" };
    EXPECT(listing_is(&sub, sub_names, 1));

    //an empty directory ends at once
    REQUIRE(make_dir("/empty"));
    const listing_t empty = list_dir("/empty", NULL);
    EXPECT(empty.ok && empty.count == 0);
}

static void fs_listing_with_patterns(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(build_tree());

    const listing_t txt = list_dir("/", "*.txt");
    static const char *const txt_names[] = { "a.txt", "b.txt" };
    EXPECT(listing_is(&txt, txt_names, 2));

    const listing_t epub = list_dir("/", "*.epub");
    static const char *const epub_names[] = { "A Long Book Title.epub" };
    EXPECT(listing_is(&epub, epub_names, 1));

    const listing_t one = list_dir("/", "?.bin");
    static const char *const one_names[] = { "c.bin" };
    EXPECT(listing_is(&one, one_names, 1));

    const listing_t none = list_dir("/", "*.none");
    EXPECT(none.ok && none.count == 0);

    const listing_t sub = list_dir("/books", "*.txt");
    static const char *const sub_names[] = { "inner.txt" };
    EXPECT(listing_is(&sub, sub_names, 1));

    //a filtered listing of an empty directory ends at once, like an unfiltered one
    REQUIRE(make_dir("/empty"));
    const listing_t empty = list_dir("/empty", "*");
    EXPECT(empty.ok && empty.count == 0);
}

//FatFs allows at most four wildcards in a pattern (documents/doc/findfirst.html). one with more matches
//nothing, even a name it would otherwise match, and the listing simply ends rather than failing
static void fs_listing_pattern_wildcard_limit(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(build_tree());

    const listing_t four = list_dir("/", "*L*B*T*.epub");
    static const char *const four_names[] = { "A Long Book Title.epub" };
    EXPECT(listing_is(&four, four_names, 1));

    const listing_t five = list_dir("/", "*L*B*T*.e*");
    EXPECT(five.ok && five.count == 0);
}

//FatFs keeps only a pointer to the pattern; the layer must keep its own copy
static void fs_listing_pattern_is_copied(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(build_tree());
    char pattern[FILESYSTEM_PATTERN_MAX + 1];
    strcpy(pattern, "*.txt");
    filesystem_dir_t dir;
    REQUIRE_RESULT(filesystem_opendir("/", pattern, &dir), R(OK));
    strcpy(pattern, "*");                                       //the caller reuses its buffer

    filesystem_dirent_t entry;
    bool end = false;
    size_t count = 0;
    size_t non_txt = 0;
    while (filesystem_readdir(dir, &entry, &end) == R(OK) && !end && count < MAX_LISTED) {
        count++;
        non_txt += strstr(entry.name, ".txt") == NULL;
    }
    EXPECT(end);
    EXPECT_EQ_U(count, 2);
    EXPECT_EQ_U(non_txt, 0);
    EXPECT_RESULT(filesystem_closedir(dir), R(OK));
}

static void fs_listing_pattern_length_limit(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "a"));
    char pattern[FILESYSTEM_PATTERN_MAX + 2];
    filesystem_dir_t dir;

    //exactly FILESYSTEM_PATTERN_MAX bytes: accepted and used whole ("*" then 30 'a's matches nothing)
    pattern[0] = '*';
    memset(pattern + 1, 'a', FILESYSTEM_PATTERN_MAX - 1);
    pattern[FILESYSTEM_PATTERN_MAX] = '\0';
    REQUIRE_RESULT(filesystem_opendir("/", pattern, &dir), R(OK));
    filesystem_dirent_t entry;
    bool end = false;
    EXPECT_RESULT(filesystem_readdir(dir, &entry, &end), R(OK));
    EXPECT(end);
    EXPECT_RESULT(filesystem_closedir(dir), R(OK));

    //one more is refused, and refused without taking the slot
    pattern[FILESYSTEM_PATTERN_MAX] = 'a';
    pattern[FILESYSTEM_PATTERN_MAX + 1] = '\0';
    EXPECT_RESULT(filesystem_opendir("/", pattern, &dir), R(INVALID_ARGUMENT));
    EXPECT_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));
}

static void fs_opendir_errors(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_text("/a.txt", "a"));
    filesystem_dir_t dir;
    EXPECT_RESULT(filesystem_opendir("/missing", NULL, &dir), R(NOT_FOUND));
    EXPECT_RESULT(filesystem_opendir("/a.txt", NULL, &dir), R(NOT_FOUND));     //a file is not a directory
    EXPECT_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));
    EXPECT_RESULT(filesystem_closedir(dir), R(OK));
    EXPECT_RESULT(filesystem_closedir(dir), R(STALE_HANDLE));
    filesystem_dirent_t entry;
    bool end;
    EXPECT_RESULT(filesystem_readdir(dir, &entry, &end), R(STALE_HANDLE));
}

static void fs_utf8_names_roundtrip(void)
{
    REQUIRE(mount_fresh());
    static const char name[] = "\xC3\x9C" "n\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 \xE6\x9B\xB8.epub";  //Ünïcödé 書.epub
    char path[64] = "/";
    strcat(path, name);
    REQUIRE(write_text(path, "unicode"));
    const listing_t root = list_dir("/", NULL);
    static const char *const names[] = { name };
    EXPECT(listing_is(&root, names, 1));
    EXPECT(text_is(path, "unicode"));
    filesystem_dirent_t entry;
    EXPECT_RESULT(filesystem_stat(path, &entry), R(OK));
    EXPECT_EQ_U(entry.size, 7);
}

//a legal FAT name (105 UTF-16 units) whose UTF-8 form (305 bytes) does not fit FILESYSTEM_NAME_MAX.
//nothing promises which name the listing returns, only that it fits and that it opens the file
static void fs_overlong_utf8_name_is_listed_by_an_openable_name(void)
{
    REQUIRE(mount_fresh());
    char path[1 + 100 * 3 + 5 + 1] = "/";
    for (int i = 0; i < 100; i++) {
        strcat(path, "\xE6\x9B\xB8");                           //書
    }
    strcat(path, ".epub");
    REQUIRE(write_text(path, "long"));

    const listing_t root = list_dir("/", NULL);
    REQUIRE(root.ok && root.count == 1);
    const size_t length = strlen(root.names[0]);
    EXPECT(length > 0 && length <= FILESYSTEM_NAME_MAX);
    char listed_path[FILESYSTEM_NAME_MAX + 2] = "/";
    strcat(listed_path, root.names[0]);
    EXPECT(text_is(listed_path, "long"));
    EXPECT(text_is(path, "long"));
}

/* ------------------------------------------------------------ entries */

static void fs_stat_describes_entries(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(build_tree());
    filesystem_dirent_t entry;
    memset(&entry, 0xFF, sizeof entry);
    EXPECT_RESULT(filesystem_stat("/b.txt", &entry), R(OK));
    EXPECT(strcmp(entry.name, "b.txt") == 0 && entry.size == 3 && !entry.is_directory);
    EXPECT_RESULT(filesystem_stat("/books", &entry), R(OK));
    EXPECT(strcmp(entry.name, "books") == 0 && entry.is_directory);
    EXPECT_RESULT(filesystem_stat("/books/inner.txt", &entry), R(OK));
    EXPECT_EQ_U(entry.size, 5);
    EXPECT_RESULT(filesystem_stat("/missing", &entry), R(NOT_FOUND));
    EXPECT_RESULT(filesystem_stat("/missing/x.txt", &entry), R(NOT_FOUND));
    EXPECT_RESULT(filesystem_stat("/", &entry), R(INVALID_ARGUMENT));   //documented: root has no entry
}

static void fs_rename_semantics(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(build_tree());
    filesystem_dirent_t entry;

    EXPECT_RESULT(filesystem_rename("/a.txt", "/z.txt"), R(OK));
    EXPECT_RESULT(filesystem_stat("/a.txt", &entry), R(NOT_FOUND));
    EXPECT(text_is("/z.txt", "aa"));

    //never overwrites: both files survive unchanged
    EXPECT_RESULT(filesystem_rename("/z.txt", "/b.txt"), R(EXISTS));
    EXPECT(text_is("/z.txt", "aa"));
    EXPECT(text_is("/b.txt", "bbb"));

    EXPECT_RESULT(filesystem_rename("/missing", "/q.txt"), R(NOT_FOUND));
    EXPECT_RESULT(filesystem_rename("/z.txt", "/books/z.txt"), R(OK));  //moves between directories
    EXPECT(text_is("/books/z.txt", "aa"));
    EXPECT_RESULT(filesystem_stat("/z.txt", &entry), R(NOT_FOUND));
}

static void fs_remove_semantics(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(build_tree());
    filesystem_dirent_t entry;
    EXPECT_RESULT(filesystem_remove("/a.txt"), R(OK));
    EXPECT_RESULT(filesystem_stat("/a.txt", &entry), R(NOT_FOUND));
    EXPECT_RESULT(filesystem_remove("/a.txt"), R(NOT_FOUND));
    EXPECT_RESULT(filesystem_remove("/books"), R(DENIED));              //not empty
    EXPECT(text_is("/books/inner.txt", "inner"));
    EXPECT_RESULT(filesystem_remove("/books/inner.txt"), R(OK));
    EXPECT_RESULT(filesystem_remove("/books"), R(OK));                  //empty now
    EXPECT_RESULT(filesystem_stat("/books", &entry), R(NOT_FOUND));
}

//white-box: the cluster a file starts at, from FatFs's own file object; 0 if it cannot be opened
static DWORD start_cluster(const char *path)
{
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) {
        return 0;
    }
    const DWORD cluster = fil.obj.sclust;
    (void)f_close(&fil);
    return cluster;
}

//the hazard filesystem_rename documents, and the reason save points never rename. f_rename registers the
//new name before it deletes the old one, so when the two entries sit in different directory sectors, a
//removal between the two sector writes leaves both names on the card, pointing at the same clusters.
//removing either name then frees clusters the other still uses, and the next allocation takes them
static void fs_rename_interrupted_across_sectors_shares_clusters(void)
{
    REQUIRE(mount_fresh());
    filesystem_dirent_t entry;

    //lay the root directory out so the rename's two entries fall in different sectors. a sector of the
    //FAT16 root directory holds 16 entries, and these short lower-case names take one entry each: the
    //file being renamed first (sector 0), then 15 fillers to finish the sector, so the first free entry,
    //where f_rename registers the new name, is the first of sector 1
    REQUIRE(write_text("/old.txt", "v2"));
    char name[8];
    for (int i = 0; i < 15; i++) {
        snprintf(name, sizeof name, "/f%02d", i);
        REQUIRE(write_text(name, "filler"));
    }

    //the rename's first device write flushes sector 1, holding the new entry, as FatFs moves to sector 0
    //to delete the old one; the card is pulled before the second write, which would carry the deletion
    fake_disk_fail_write(&disk, 1, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    EXPECT_RESULT(filesystem_rename("/old.txt", "/new.txt"), R(STORAGE_ERROR));
    fake_disk_remove(&disk);

    //the coordinator's transition, then the card back in
    filesystem_unmount();
    fake_disk_insert(&disk);
    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));

    //both names are on the card, and they are one file. a failure here means the layout above no longer
    //splits the entries, not that the hazard is gone
    REQUIRE_RESULT(filesystem_stat("/new.txt", &entry), R(OK));
    REQUIRE_RESULT(filesystem_stat("/old.txt", &entry), R(OK));
    const DWORD shared = start_cluster("/new.txt");
    REQUIRE(shared != 0);
    EXPECT_EQ_U(start_cluster("/old.txt"), shared);
    EXPECT(text_is("/new.txt", "v2"));

    //removing one name frees the clusters the other still uses: its data survives only until the next
    //file is allocated, which takes them
    EXPECT_RESULT(filesystem_remove("/old.txt"), R(OK));
    EXPECT(text_is("/new.txt", "v2"));
    REQUIRE(write_text("/next.txt", "XY"));
    EXPECT_EQ_U(start_cluster("/next.txt"), shared);
    EXPECT(text_is("/new.txt", "XY"));
}

static void fs_volume_label(void)
{
    char label[FILESYSTEM_LABEL_SIZE];
    REQUIRE(mount_fresh());
    EXPECT_RESULT(filesystem_get_label(label, FILESYSTEM_LABEL_SIZE - 1), R(INVALID_ARGUMENT));
    memset(label, 'x', sizeof label);
    EXPECT_RESULT(filesystem_get_label(label, sizeof label), R(OK));
    EXPECT(label[0] == '\0');                                   //f_mkfs sets no label
    REQUIRE(f_setlabel("TOME") == FR_OK);                       //white-box: no setter in the interface
    EXPECT_RESULT(filesystem_get_label(label, sizeof label), R(OK));
    EXPECT(strcmp(label, "TOME") == 0);
}

/* --------------------------------------------- removal and device faults */

//filesystem.h documents three results for a removal the coordinator has not yet acted on
static void fs_removal_before_unmount(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_pattern_file("/big.bin", 8192));
    REQUIRE(write_text("/other.txt", "other"));
    filesystem_file_t reading;
    filesystem_file_t idle;
    filesystem_dir_t dir;
    uint8_t buffer[4096];
    size_t n;
    REQUIRE_RESULT(filesystem_open("/big.bin", FILESYSTEM_OPEN_READ, &reading), R(OK));
    REQUIRE_RESULT(filesystem_open("/other.txt", FILESYSTEM_OPEN_READ, &idle), R(OK));
    REQUIRE_RESULT(filesystem_opendir("/", NULL, &dir), R(OK));
    REQUIRE_RESULT(filesystem_read(reading, buffer, 100, &n), R(OK));

    fake_disk_remove(&disk);
    const unsigned writes = disk.write_calls;

    //a call that needs a sector it does not have
    EXPECT_RESULT(filesystem_read(reading, buffer, sizeof buffer, &n), R(STORAGE_ERROR));
    //later calls on handles opened before the removal
    EXPECT_RESULT(filesystem_read(reading, buffer, 1, &n), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_read(idle, buffer, 1, &n), R(STALE_HANDLE));
    filesystem_dirent_t entry;
    bool end;
    EXPECT_RESULT(filesystem_readdir(dir, &entry, &end), R(STALE_HANDLE));
    //calls naming a path
    EXPECT_RESULT(filesystem_stat("/other.txt", &entry), R(MEDIUM_UNAVAILABLE));
    filesystem_file_t file;
    EXPECT_RESULT(filesystem_open("/other.txt", FILESYSTEM_OPEN_READ, &file), R(MEDIUM_UNAVAILABLE));
    EXPECT_EQ_U(disk.write_calls, writes);                      //nothing was written to the absent card

    //the coordinator's transition
    const unsigned io = fake_disk_io_calls(&disk);
    filesystem_unmount();
    EXPECT_EQ_U(fake_disk_io_calls(&disk), io);
    EXPECT_RESULT(filesystem_mount(&disk.device), R(MEDIUM_UNAVAILABLE));
    fake_disk_insert(&disk);
    EXPECT_RESULT(filesystem_mount(&disk.device), R(MEDIUM_UNAVAILABLE));  //latched until re-init
    EXPECT(!filesystem_is_mounted());

    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
    EXPECT(check_pattern_file("/big.bin", 8192));
    EXPECT_RESULT(filesystem_read(reading, buffer, 1, &n), R(STALE_HANDLE));   //old handles stay dead
}

static void fs_removal_during_write(void)
{
    REQUIRE(mount_fresh());
    filesystem_file_t file;
    static uint8_t buffer[4096];
    size_t n;
    REQUIRE_RESULT(filesystem_open("/w.bin", FILESYSTEM_OPEN_REPLACE, &file), R(OK));
    REQUIRE_RESULT(filesystem_write(file, buffer, sizeof buffer, &n), R(OK));
    fake_disk_remove(&disk);
    EXPECT_RESULT(filesystem_write(file, buffer, sizeof buffer, &n), R(STORAGE_ERROR));
    EXPECT_RESULT(filesystem_write(file, buffer, sizeof buffer, &n), R(STALE_HANDLE));
    EXPECT_RESULT(filesystem_close(file), R(STALE_HANDLE));
    filesystem_unmount();
}

//a transient error (the device stays usable) is not a removal: it fails the call, but the volume and
//other files carry on without a remount
static void fs_transient_errors_do_not_take_the_volume_down(void)
{
    static const block_device_result_t transient[] = {
        BLOCK_DEVICE_RESULT_IO_ERROR,
        BLOCK_DEVICE_RESULT_BUSY_TIMEOUT,
    };
    REQUIRE(mount_fresh());
    REQUIRE(write_pattern_file("/big.bin", 8192));
    REQUIRE(write_text("/other.txt", "other"));

    for (size_t i = 0; i < sizeof transient / sizeof transient[0]; i++) {
        filesystem_file_t file;
        uint8_t buffer[4096];
        size_t n;
        REQUIRE_RESULT(filesystem_open("/big.bin", FILESYSTEM_OPEN_READ, &file), R(OK));
        REQUIRE_RESULT(filesystem_read(file, buffer, 100, &n), R(OK));
        fake_disk_fail_read(&disk, 0, transient[i]);
        EXPECT_RESULT(filesystem_read(file, buffer, sizeof buffer, &n), R(STORAGE_ERROR));
        EXPECT_RESULT(filesystem_close(file), R(OK));

        EXPECT(filesystem_is_mounted());
        EXPECT(text_is("/other.txt", "other"));
        EXPECT(check_pattern_file("/big.bin", 8192));
    }
    EXPECT_EQ_U(disk.init_calls, 1);
}

//the coordinator deinitialized the device without unmounting first: treated like a removal
static void fs_device_deinitialized_under_mounted_volume(void)
{
    REQUIRE(mount_fresh());
    REQUIRE(write_pattern_file("/big.bin", 8192));
    filesystem_file_t file;
    uint8_t buffer[4096];
    size_t n;
    REQUIRE_RESULT(filesystem_open("/big.bin", FILESYSTEM_OPEN_READ, &file), R(OK));
    REQUIRE_RESULT(filesystem_read(file, buffer, 100, &n), R(OK));

    REQUIRE(block_device_deinit(&disk.device) == BLOCK_DEVICE_RESULT_OK);
    EXPECT_RESULT(filesystem_read(file, buffer, sizeof buffer, &n), R(STORAGE_ERROR));
    EXPECT_RESULT(filesystem_read(file, buffer, 1, &n), R(STALE_HANDLE));
    filesystem_dirent_t entry;
    EXPECT_RESULT(filesystem_stat("/big.bin", &entry), R(MEDIUM_UNAVAILABLE));
    EXPECT_EQ_U(disk.init_calls, 1);                            //the layer did not re-init it

    filesystem_unmount();
    REQUIRE(fake_disk_bring_up(&disk) == BLOCK_DEVICE_RESULT_OK);
    REQUIRE_RESULT(filesystem_mount(&disk.device), R(OK));
    EXPECT(check_pattern_file("/big.bin", 8192));
}

/* --------------------------------------------------------------- time */

//expected values are packed by hand from the table in fattime.html, not computed by the code under test:
//  2026-10-16 13:45:58 -> year 46, month 10, day 16, hour 13, minute 45, second/2 29
//    date (46 << 9) | (10 << 5) | 16 = 0x5D50
//    time (13 << 11) | (45 << 5) | 29 = 0x6DBD
//  2107-12-31 23:59:59 -> date (127 << 9) | (12 << 5) | 31 = 0xFF9F, time (23 << 11) | (59 << 5) | 29 = 0xBF7D
//  1980-01-01 00:00:00 -> date (0 << 9) | (1 << 5) | 1 = 0x0021, time 0 (equal to the sentinel)
//  1980-06-15 12:30:10 -> date (0 << 9) | (6 << 5) | 15 = 0x00CF, time (12 << 11) | (30 << 5) | 5 = 0x63C5
//    (the first year must be accepted: this differs from the sentinel, unlike 1980-01-01)
static void fs_fattime_packing(void)
{
    EXPECT_EQ_U(get_fattime(), 0x00210000U);                    //clock not valid: the 1980 sentinel

    fake_clock_set((clock_datetime_t){ 2026, 10, 16, 13, 45, 58 });
    EXPECT_EQ_U(get_fattime(), 0x5D506DBDU);
    fake_clock_set((clock_datetime_t){ 2026, 10, 16, 13, 45, 59 });
    EXPECT_EQ_U(get_fattime(), 0x5D506DBDU);                    //two-second resolution, truncated
    fake_clock_set((clock_datetime_t){ 2107, 12, 31, 23, 59, 59 });
    EXPECT_EQ_U(get_fattime(), 0xFF9FBF7DU);                    //the last representable time
    fake_clock_set((clock_datetime_t){ 1980, 1, 1, 0, 0, 0 });
    EXPECT_EQ_U(get_fattime(), 0x00210000U);
    fake_clock_set((clock_datetime_t){ 1980, 6, 15, 12, 30, 10 });
    EXPECT_EQ_U(get_fattime(), 0x00CF63C5U);
    //unrepresentable: the sentinel, not a wrapped year. not 2108-01-01 00:00:00, whose wrapped packing
    //(year bits 0, month 1, day 1) happens to equal the sentinel and so could not tell the two apart
    fake_clock_set((clock_datetime_t){ 2108, 6, 15, 12, 30, 10 });
    EXPECT_EQ_U(get_fattime(), 0x00210000U);
    fake_clock_set((clock_datetime_t){ 1979, 12, 31, 23, 59, 58 });
    EXPECT_EQ_U(get_fattime(), 0x00210000U);
    fake_clock_invalidate();
    EXPECT_EQ_U(get_fattime(), 0x00210000U);
}

//the seam is wired: FatFs stamps files with the clock's time
static void fs_files_are_stamped_from_the_clock(void)
{
    REQUIRE(mount_fresh());
    FILINFO info;

    REQUIRE(write_text("/undated.txt", "u"));
    REQUIRE(f_stat("/undated.txt", &info) == FR_OK);            //white-box: no timestamps in the interface
    EXPECT_EQ_U(info.fdate, 0x0021);
    EXPECT_EQ_U(info.ftime, 0x0000);

    fake_clock_set((clock_datetime_t){ 2026, 10, 16, 13, 45, 58 });
    REQUIRE(write_text("/dated.txt", "d"));
    REQUIRE(f_stat("/dated.txt", &info) == FR_OK);
    EXPECT_EQ_U(info.fdate, 0x5D50);
    EXPECT_EQ_U(info.ftime, 0x6DBD);
}

const test_case_t filesystem_tests[] = {
    TEST_CASE(fs_mount_rejects_invalid_devices),
    TEST_CASE(fs_mount_blank_card_reports_no_filesystem),
    TEST_CASE(fs_mount_needs_an_initialized_device),
    TEST_CASE(fs_layer_never_initializes_or_deinitializes_the_device),
    TEST_CASE(fs_mount_refused_while_mounted),
    TEST_CASE(fs_unmount_is_idempotent),
    TEST_CASE(fs_unmount_performs_no_io),
    TEST_CASE(fs_unmount_invalidates_every_handle),
    TEST_CASE(fs_unmount_warns_only_about_open_writers),
    TEST_CASE(fs_mount_assumes_a_changed_medium),
    TEST_CASE(fs_zero_handles_are_always_stale),
    TEST_CASE(fs_forged_handles_are_stale),
    TEST_CASE(fs_closed_handle_stays_stale_after_its_slot_is_reused),
    TEST_CASE(fs_generation_wraps_without_issuing_zero),
    TEST_CASE(fs_pools_are_limited_and_independent),
    TEST_CASE(fs_failed_opens_do_not_consume_slots),
    TEST_CASE(fs_close_frees_the_slot_even_when_it_fails),
    TEST_CASE(fs_open_entries_in_use_are_refused),
    TEST_CASE(fs_failed_close_releases_the_entry),
    TEST_CASE(fs_unmount_releases_every_entry),
    TEST_CASE(fs_failed_filtered_opendir_releases_the_directory),
    TEST_CASE(fs_close_reports_a_failed_sync),
    TEST_CASE(fs_close_retries_a_failed_flush),
    TEST_CASE(fs_close_reports_a_removal_as_a_storage_error),
    TEST_CASE(fs_calls_refused_while_unmounted),
    TEST_CASE(fs_null_arguments_rejected),
    TEST_CASE(fs_open_rejects_unknown_modes),
    TEST_CASE(fs_open_reports_missing_paths),
    TEST_CASE(fs_names_are_validated),
    TEST_CASE(fs_replace_creates_and_truncates),
    TEST_CASE(fs_access_modes_are_enforced),
    TEST_CASE(fs_large_file_roundtrip),
    TEST_CASE(fs_reads_are_short_only_at_end_of_file),
    TEST_CASE(fs_seek_tell_and_size),
    TEST_CASE(fs_full_volume_gives_short_writes),
    TEST_CASE(fs_listing_returns_every_entry_once),
    TEST_CASE(fs_listing_with_patterns),
    TEST_CASE(fs_listing_pattern_is_copied),
    TEST_CASE(fs_listing_pattern_length_limit),
    TEST_CASE(fs_listing_pattern_wildcard_limit),
    TEST_CASE(fs_opendir_errors),
    TEST_CASE(fs_utf8_names_roundtrip),
    TEST_CASE(fs_overlong_utf8_name_is_listed_by_an_openable_name),
    TEST_CASE(fs_stat_describes_entries),
    TEST_CASE(fs_rename_semantics),
    TEST_CASE(fs_remove_semantics),
    TEST_CASE(fs_rename_interrupted_across_sectors_shares_clusters),
    TEST_CASE(fs_volume_label),
    TEST_CASE(fs_removal_before_unmount),
    TEST_CASE(fs_removal_during_write),
    TEST_CASE(fs_transient_errors_do_not_take_the_volume_down),
    TEST_CASE(fs_device_deinitialized_under_mounted_volume),
    TEST_CASE(fs_fattime_packing),
    TEST_CASE(fs_files_are_stamped_from_the_clock),
};
const size_t filesystem_test_count = sizeof filesystem_tests / sizeof filesystem_tests[0];
