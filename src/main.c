// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"

#include "platform/board.h"
#include "platform/debug.h"
#include "platform/gpio_irq.h"

//the storage stack: save points over the filesystem layer, the coordinator that owns the card, the sd driver
#include "storage/filesystem.h"
#include "storage/save_point.h"
#include "storage/sd_spi.h"
#include "storage/storage.h"

#if TAVERNKEEP_USB_MSC_ENABLED
#include "usb/usb_device.h"
#include "usb/usb_mode.h"
#endif

//spi pin numbers for hardware testing
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19
//these are on spi0
#define SPI      spi0
//write protect/card detect pin
#define PIN_WP   12
//data rate after init: 25 MHz, the SD default-speed maximum in SPI mode and 150 MHz clk_peri / 6 (VAL-H05).
//the hardware tests' rate sweep overrides it on the compiler command line (-DSD_BAUD_HZ=5000000U)
#ifndef SD_BAUD_HZ
#define SD_BAUD_HZ 25000000U
#endif

enum {
    STATUS_LED_TOGGLE_INTERVAL_MS = 500,
    MAIN_LOOP_IDLE_INTERVAL_MS = 10,
    //entries the demo lists from the root directory before stopping
    DEMO_LIST_LIMIT = 16,
};

//static so the removal interrupt's pointer outlives main()'s frame and the object starts zeroed
static sd_spi_t sd;

//one result line per step; "PASS "/"FAIL " prefixes are what the host test and a hardware log both key on
static bool demo_report(bool ok, const char *name, filesystem_result_t result)
{
    if (ok) {
        DEBUG_INFO("PASS %s", name);
    } else {
        DEBUG_ERROR("FAIL %s: result %d", name, (int)result);
    }
    return ok;
}

static const char *owner_name(storage_owner_t owner)
{
    switch (owner) {
    case STORAGE_OWNER_LOCAL: return "local";
    case STORAGE_OWNER_USB: return "usb";
    case STORAGE_OWNER_NONE: return "none";
    }
    return "?";
}

static const char *medium_name(storage_medium_t medium)
{
    switch (medium) {
    case STORAGE_MEDIUM_ABSENT: return "absent";
    case STORAGE_MEDIUM_PREPARING: return "preparing";
    case STORAGE_MEDIUM_FAULTY: return "faulty";
    case STORAGE_MEDIUM_UNFORMATTED: return "unformatted";
    case STORAGE_MEDIUM_READY: return "ready";
    }
    return "?";
}

/* ------------------------------------------------------ filesystem demo */

//the counter's save point, in the slot files /tavernkeep-demo.a and /tavernkeep-demo.b
#define DEMO_RECORD "/tavernkeep-demo"
#define DEMO_BULK "/tavernkeep-bulk.bin"

enum {
    //written in one call and read back in one call, from offset 0, so FatFs moves whole sectors straight
    //between the buffer and the card: multi-block CMD25 and CMD18 transfers, split only at cluster
    //boundaries. the demo's other files are a few bytes and only ever exercise single-block transfers
    DEMO_BULK_BYTES = 16384,
};

//static: too large for the 4 KB stack
static uint32_t demo_bulk_buffer[DEMO_BULK_BYTES / sizeof(uint32_t)];

//read the demo's counter from its save point. returns false only on a storage failure: no record yet
//(nothing saved, or the first save was interrupted) reads as counter 0
static bool demo_read_counter(uint32_t *counter)
{
    size_t size = 0;
    const filesystem_result_t result = save_point_read(DEMO_RECORD, counter, sizeof *counter, &size);
    if (result == FILESYSTEM_RESULT_NOT_FOUND) {
        DEBUG_INFO("no saved counter yet");
        *counter = 0;
        return true;
    }
    return demo_report(result == FILESYSTEM_RESULT_OK && size == sizeof *counter, "read counter", result);
}

//save the counter: one save_point_write, which either lands whole or leaves the previous value readable
static bool demo_write_counter(uint32_t counter)
{
    const filesystem_result_t result = save_point_write(DEMO_RECORD, &counter, sizeof counter);
    return demo_report(result == FILESYSTEM_RESULT_OK, "save point write", result);
}

//the word expected at index i of the bulk file: different for every word and for every run, so a block
//landing at the wrong offset, or one left over from an earlier run, does not verify
static uint32_t demo_bulk_word(size_t i, uint32_t seed)
{
    return ((uint32_t)i * 4U) ^ (seed * 0x9E3779B9U);
}

//log how long one bulk write or read call took and the rate that works out to. the open and the close are
//outside the timing, so it covers the data transfer and FatFs's cluster lookups, not the directory
//update and sync the close does
static void demo_report_rate(const char *what, uint64_t elapsed_us)
{
    const uint64_t kib_per_s = elapsed_us == 0U
        ? 0U
        : (uint64_t)DEMO_BULK_BYTES * 1000000U / 1024U / elapsed_us;
    DEBUG_INFO("bulk %s: %u bytes in %lu us, %lu KiB/s", what, (unsigned)DEMO_BULK_BYTES,
        (unsigned long)elapsed_us, (unsigned long)kib_per_s);
}

//write the bulk file in one call, read it back in one call, and check every word
static bool demo_bulk(uint32_t seed)
{
    const size_t words = sizeof demo_bulk_buffer / sizeof demo_bulk_buffer[0];
    for (size_t i = 0; i < words; i++) {
        demo_bulk_buffer[i] = demo_bulk_word(i, seed);
    }

    filesystem_file_t file;
    filesystem_result_t result = filesystem_open(DEMO_BULK, FILESYSTEM_OPEN_REPLACE, &file);
    if (!demo_report(result == FILESYSTEM_RESULT_OK, "bulk open for write", result)) {
        return false;
    }
    size_t done = 0;
    uint64_t started_us = time_us_64();
    result = filesystem_write(file, demo_bulk_buffer, sizeof demo_bulk_buffer, &done);
    const uint64_t write_us = time_us_64() - started_us;
    filesystem_result_t closed = filesystem_close(file);
    if (!demo_report(result == FILESYSTEM_RESULT_OK && done == sizeof demo_bulk_buffer
            && closed == FILESYSTEM_RESULT_OK, "bulk write", result != FILESYSTEM_RESULT_OK ? result : closed)) {
        return false;
    }
    demo_report_rate("write", write_us);

    memset(demo_bulk_buffer, 0, sizeof demo_bulk_buffer);
    result = filesystem_open(DEMO_BULK, FILESYSTEM_OPEN_READ, &file);
    if (!demo_report(result == FILESYSTEM_RESULT_OK, "bulk open for read", result)) {
        return false;
    }
    started_us = time_us_64();
    result = filesystem_read(file, demo_bulk_buffer, sizeof demo_bulk_buffer, &done);
    const uint64_t read_us = time_us_64() - started_us;
    closed = filesystem_close(file);
    if (!demo_report(result == FILESYSTEM_RESULT_OK && done == sizeof demo_bulk_buffer
            && closed == FILESYSTEM_RESULT_OK, "bulk read", result != FILESYSTEM_RESULT_OK ? result : closed)) {
        return false;
    }
    demo_report_rate("read", read_us);

    for (size_t i = 0; i < words; i++) {
        const uint32_t expected = demo_bulk_word(i, seed);
        if (demo_bulk_buffer[i] != expected) {
            DEBUG_ERROR("FAIL bulk verify: byte %lu holds 0x%08lx, expected 0x%08lx",
                (unsigned long)(i * sizeof demo_bulk_buffer[0]), (unsigned long)demo_bulk_buffer[i],
                (unsigned long)expected);
            return false;
        }
    }
    DEBUG_INFO("PASS bulk verify, %u bytes", (unsigned)DEMO_BULK_BYTES);
    return true;
}

//run once each time the card becomes ready for the local owner
static void filesystem_demo(void)
{
    char label[FILESYSTEM_LABEL_SIZE];
    filesystem_result_t result = filesystem_get_label(label, sizeof label);
    if (demo_report(result == FILESYSTEM_RESULT_OK, "label", result)) {
        DEBUG_INFO("volume label \"%s\"", label);
    }

    filesystem_dir_t dir;
    result = filesystem_opendir("/", NULL, &dir);
    if (demo_report(result == FILESYSTEM_RESULT_OK, "opendir", result)) {
        unsigned listed = 0;
        for (;;) {
            filesystem_dirent_t entry;
            bool end = false;
            result = filesystem_readdir(dir, &entry, &end);
            if (result != FILESYSTEM_RESULT_OK || end) {
                break;
            }
            if (listed++ < DEMO_LIST_LIMIT) {
                DEBUG_INFO("  %s%s %lu", entry.name, entry.is_directory ? "/" : "",
                    (unsigned long)entry.size);
            }
        }
        (void)demo_report(result == FILESYSTEM_RESULT_OK, "readdir", result);
        DEBUG_INFO("%u entries in /", listed);
        result = filesystem_closedir(dir);
        (void)demo_report(result == FILESYSTEM_RESULT_OK, "closedir", result);
    }

    uint32_t counter = 0;
    if (!demo_read_counter(&counter)) {
        return;
    }
    if (!demo_write_counter(counter + 1U)) {
        return;
    }
    uint32_t readback = 0;
    if (demo_read_counter(&readback)) {
        (void)demo_report(readback == counter + 1U, "counter readback", FILESYSTEM_RESULT_OK);
        DEBUG_INFO("this card has now been mounted by the demo %lu times", (unsigned long)readback);
    }

    //last, so a removal during it leaves the save point above untouched
    (void)demo_bulk(counter + 1U);
}

/* ----------------------------------------------------------- storage */

static bool storage_bring_up(void)
{
    const sd_spi_config_t config = {
        .spi = SPI,
        .baud_rate_hz = SD_BAUD_HZ,
        .pin_clock = PIN_SCK,
        .pin_controller_out = PIN_MOSI,
        .pin_controller_in = PIN_MISO,
        .pin_chip_select = PIN_CS,
        .pin_card_available = PIN_WP,
        //the adafruit microsd breakout's detect switch closes to ground when the socket is empty
        .card_detect_active_high = true,
    };

    //breadboard stand-ins for the discrete pull-ups the final board carries; pad pulls survive the driver
    //switching the pin's function, so enabling them here is enough
    //DO: the breakout has no pull-up and the card leaves DAT0 undriven until CMD0 moves it to spi mode,
    //so a floating line reads as never-ready and init fails with BUSY_TIMEOUT before CMD0 is ever sent
    gpio_pull_up(PIN_MISO);
    //CS: keeps the card deselected while the pin is not yet driven (spi mode is selected by CS low at CMD0)
    gpio_pull_up(PIN_CS);
    //MOSI and SCK are driven push-pull by the spi peripheral and need no pull; card detect gets its own
    //from the driver

    const block_device_result_t result = sd_spi_configure(&sd, &config);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        DEBUG_ERROR("FAIL configure: result %d", (int)result);
        return false;
    }
    if (!storage_init(sd_spi_as_block_device(&sd))) {
        DEBUG_ERROR("FAIL storage_init");
        return false;
    }
    DEBUG_INFO("PASS storage_init; insert or remove the card at any time");
    return true;
}

//log every change the coordinator reports, and run the filesystem demo each time the card becomes ready
static void storage_watch(void)
{
    static bool seen = false;
    static storage_owner_t last_owner;
    static storage_medium_t last_medium;
    static uint32_t last_epoch;

    const storage_owner_t owner = storage_owner();
    const storage_medium_t medium = storage_medium();
    const uint32_t epoch = storage_epoch();
    if (seen && owner == last_owner && medium == last_medium && epoch == last_epoch) {
        return;
    }
    const bool became_ready = medium == STORAGE_MEDIUM_READY
        && (!seen || last_medium != STORAGE_MEDIUM_READY || epoch != last_epoch);
    seen = true;
    last_owner = owner;
    last_medium = medium;
    last_epoch = epoch;

    DEBUG_INFO("storage: owner %s, medium %s, epoch %lu", owner_name(owner), medium_name(medium),
        (unsigned long)epoch);
    if (medium == STORAGE_MEDIUM_UNFORMATTED) {
        DEBUG_INFO("no FAT volume on this card; format it as FAT32 on a computer");
    }
    if (became_ready) {
        //the rate the card actually runs at: the nearest one at or below SD_BAUD_HZ that clk_peri divides to
        DEBUG_INFO("sd bus at %lu Hz (asked for %lu Hz)", (unsigned long)sd_spi_bus_rate_hz(&sd),
            (unsigned long)SD_BAUD_HZ);
    }
    if (became_ready && owner == STORAGE_OWNER_LOCAL) {
        filesystem_demo();
    }
}

int main(void)
{
    board_init();

    const bool debug_ready = debug_init();
    if (debug_ready) {
        DEBUG_INFO("Tavernkeep RP2350 firmware");
        DEBUG_INFO("Pico 2 Cortex-M33 bring-up complete; MCU is running");
    }

    //initialize gpio interrupt handling
    if (!platform_gpio_irq_init()) {
        //already initialized on other core
        DEBUG_INFO("a second core tried to initialize the gpio interrupt dispatcher, which already exists on another core");
    }

    //storage demo: the coordinator brings the card up, the filesystem demo runs each time it becomes
    //ready, and every removal, insertion and failure is logged (breakpoint here and then 'monitor rtt
    //start' before continuing)
    const bool storage_ready = storage_bring_up();

#if TAVERNKEEP_USB_MSC_ENABLED
    //the device joins the bus now and a host sees a reader with no card in it. the card is lent to USB when
    //the GDB harness asks (usb_mode_request) and a host has configured the device
    if (usb_device_init()) {
        DEBUG_INFO("PASS usb_device_init; vbus sense %s", board_vbus_sense_available() ? "on" : "absent");
    } else {
        DEBUG_ERROR("FAIL usb_device_init");
    }
#endif

    bool status_led_on = false;
    absolute_time_t next_led_toggle =
        make_timeout_time_ms(STATUS_LED_TOGGLE_INTERVAL_MS);

    //cooperative foreground loop: each subsystem gets one bounded step per pass
    while (true) {
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
#if TAVERNKEEP_USB_MSC_ENABLED
        usb_device_task();
        //the policy asks the coordinator for owner changes, which the storage step below carries out
        usb_device_attach(usb_mode_step(now_ms, board_vbus_present(), usb_device_configured()));
#endif
        if (storage_ready) {
            storage_step(now_ms);
            storage_watch();
        }

        if (board_status_led_available() && time_reached(next_led_toggle)) {
            status_led_on = !status_led_on;
            (void)board_status_led_set(status_led_on);
            next_led_toggle =
                make_timeout_time_ms(STATUS_LED_TOGGLE_INTERVAL_MS);
        }

#if TAVERNKEEP_USB_MSC_ENABLED
        //while a host is on the bus, each of its commands waits for the next pass, so the loop does not idle
        if (usb_device_host_present()) {
            continue;
        }
#endif
        sleep_ms(MAIN_LOOP_IDLE_INTERVAL_MS);
    }
}
