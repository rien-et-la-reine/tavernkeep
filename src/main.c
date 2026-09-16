#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

#include "platform/board.h"
#include "platform/debug.h"
#include "platform/gpio_irq.h"

//include for spi driver testing
#include <string.h>
#include "storage/sd_spi.h"
//spi pin numbers for hardware testing
#define PIN_MISO 12
#define PIN_CS   13
#define PIN_SCK  14
#define PIN_MOSI 11
//these are on spi1
#define SPI      spi1
//write protect/card detect pin
#define PIN_WP   10
//data rate for the first hardware runs; protocol first, speed later
#define SD_BAUD_HZ 1000000U

enum {
    STATUS_LED_TOGGLE_INTERVAL_MS = 500,
    MAIN_LOOP_IDLE_INTERVAL_MS = 10,
};

//sd hardware demo parameters
enum {
    SD_BLOCK_SIZE = 512,
    //blocks per transfer; multi-block operations are chunked to this so the buffer stays small
    SD_CHUNK_BLOCKS = 10,
    //the demo writes base..base+50 and reads base+51 as an untouched neighbour, all below block_count
    SD_BASE_MARGIN = 64,
    SD_MULTI_BLOCKS = 50,
    //pattern tags so the single and multi-block writes are distinguishable from each other
    SD_TAG_SINGLE = 0xA5,
    SD_TAG_MULTI = 0x5A,
};

//static so the removal interrupt's pointer outlives main()'s frame and the object starts zeroed
static sd_spi_t sd;
//static so no block data lives on the 2 KB stack
static uint8_t sd_chunk[SD_CHUNK_BLOCKS * SD_BLOCK_SIZE];
static uint8_t sd_expected[SD_BLOCK_SIZE];
static uint8_t sd_neighbour_before[SD_BLOCK_SIZE];
static uint8_t sd_neighbour_after[SD_BLOCK_SIZE];

//one result line per step; "PASS "/"FAIL " prefixes are what the host test and a hardware log both key on
static bool sd_report(bool ok, const char *name, block_device_result_t result)
{
    if (ok) {
        DEBUG_INFO("PASS %s", name);
    } else {
        DEBUG_ERROR("FAIL %s: result %d", name, (int)result);
    }
    return ok;
}

//fill one block with a pattern derived from its lba and a tag, so a block landing at the wrong address or from the wrong write is detectable
static void sd_fill_pattern(uint8_t *block, uint64_t lba, uint8_t tag)
{
    block[0] = (uint8_t)(lba >> 24);
    block[1] = (uint8_t)(lba >> 16);
    block[2] = (uint8_t)(lba >> 8);
    block[3] = (uint8_t)lba;
    block[4] = tag;
    for (size_t i = 5; i < SD_BLOCK_SIZE; i++) {
        block[i] = (uint8_t)((i * 7U) ^ tag ^ (uint8_t)lba);
    }
}

//write blocks first_lba..first_lba+count-1 with the given tag, one chunk per transfer
static block_device_result_t sd_write_pattern(block_device_t *dev, uint64_t first_lba, size_t count, uint8_t tag)
{
    while (count > 0) {
        const size_t n = count < SD_CHUNK_BLOCKS ? count : SD_CHUNK_BLOCKS;
        for (size_t i = 0; i < n; i++) {
            sd_fill_pattern(&sd_chunk[i * SD_BLOCK_SIZE], first_lba + i, tag);
        }
        const block_device_result_t result = block_device_write_blocks(dev, first_lba, sd_chunk, n);
        if (result != BLOCK_DEVICE_RESULT_OK) {
            return result;
        }
        first_lba += n;
        count -= n;
    }
    return BLOCK_DEVICE_RESULT_OK;
}

//read blocks first_lba..first_lba+count-1 one chunk per transfer and compare each against the pattern it should hold
//*matches is false if any block differs; the return value is the first read error, if any
static block_device_result_t sd_verify_pattern(block_device_t *dev, uint64_t first_lba, size_t count, uint8_t tag, bool *matches)
{
    *matches = true;
    while (count > 0) {
        const size_t n = count < SD_CHUNK_BLOCKS ? count : SD_CHUNK_BLOCKS;
        const block_device_result_t result = block_device_read_blocks(dev, first_lba, sd_chunk, n);
        if (result != BLOCK_DEVICE_RESULT_OK) {
            return result;
        }
        for (size_t i = 0; i < n; i++) {
            sd_fill_pattern(sd_expected, first_lba + i, tag);
            if (memcmp(&sd_chunk[i * SD_BLOCK_SIZE], sd_expected, SD_BLOCK_SIZE) != 0) {
                DEBUG_WARN("block %lu does not hold the expected pattern", (unsigned long)(first_lba + i));
                *matches = false;
            }
        }
        first_lba += n;
        count -= n;
    }
    return BLOCK_DEVICE_RESULT_OK;
}

//the demo; returns as soon as a step fails so main falls through to the heartbeat loop either way
static void sd_hardware_demo(void)
{
    block_device_result_t result;
    bool ok;

    //declare sd_spi_t object and sd_spi_config_t object
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
    //MOSI and SCK are driven push-pull by the spi peripheral and need no pull; card detect gets its own in init

    //configure sd_spi_t object
    result = sd_spi_configure(&sd, &config);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OK, "configure", result)) {
        return;
    }
    block_device_t *const dev = sd_spi_as_block_device(&sd);

    //init sd_spi_t object
    result = block_device_init(dev);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OK, "init", result)) {
        return;
    }

    //get info -> store base as block_count - 64
    block_device_info_t info;
    result = block_device_get_info(dev, &info);
    ok = result == BLOCK_DEVICE_RESULT_OK && info.block_count > SD_BASE_MARGIN
        && info.block_size_bytes == SD_BLOCK_SIZE;
    if (!sd_report(ok, "get_info", result)) {
        return;
    }
    DEBUG_INFO("card reports %lu blocks of %lu bytes, writable=%d",
        (unsigned long)info.block_count, (unsigned long)info.block_size_bytes, (int)info.writable);
    const uint64_t base = info.block_count - SD_BASE_MARGIN;

    //invalid parameters must be refused before any bus traffic
    result = block_device_read_blocks(dev, 0, NULL, 1);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT, "reject null buffer", result)) {
        return;
    }
    result = block_device_read_blocks(dev, 0, sd_chunk, 0);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_INVALID_ARGUMENT, "reject zero block count", result)) {
        return;
    }
    result = block_device_write_blocks(dev, info.block_count, sd_chunk, 1);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OUT_OF_RANGE, "reject write past end", result)) {
        return;
    }

    //read block 0, log bytes 510 and 511
    result = block_device_read_blocks(dev, 0, sd_chunk, 1);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OK, "read block 0", result)) {
        return;
    }
    DEBUG_INFO("block 0 bytes 510..511: %02x %02x (55 aa on a formatted card)", sd_chunk[510], sd_chunk[511]);

    //write base block
    result = sd_write_pattern(dev, base, 1, SD_TAG_SINGLE);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OK, "write single block", result)) {
        return;
    }

    //read base block back and compare
    result = sd_verify_pattern(dev, base, 1, SD_TAG_SINGLE, &ok);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OK && ok, "read back single block", result)) {
        return;
    }

    //read block base+51
    result = block_device_read_blocks(dev, base + SD_MULTI_BLOCKS + 1, sd_neighbour_before, 1);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OK, "read neighbour block", result)) {
        return;
    }

    //write blocks base+1->base+50
    result = sd_write_pattern(dev, base + 1, SD_MULTI_BLOCKS, SD_TAG_MULTI);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OK, "write multiple blocks", result)) {
        return;
    }

    //read blocks base+1->base+51 and verify the written blocks as well as unchanged base+51 block
    //base is checked again too: a multi-block write that started one block early would show up there
    result = sd_verify_pattern(dev, base, 1, SD_TAG_SINGLE, &ok);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OK && ok, "single block untouched by multi-block write", result)) {
        return;
    }
    result = sd_verify_pattern(dev, base + 1, SD_MULTI_BLOCKS, SD_TAG_MULTI, &ok);
    if (!sd_report(result == BLOCK_DEVICE_RESULT_OK && ok, "read back multiple blocks", result)) {
        return;
    }
    result = block_device_read_blocks(dev, base + SD_MULTI_BLOCKS + 1, sd_neighbour_after, 1);
    ok = result == BLOCK_DEVICE_RESULT_OK
        && memcmp(sd_neighbour_before, sd_neighbour_after, SD_BLOCK_SIZE) == 0;
    if (!sd_report(ok, "neighbour block untouched by multi-block write", result)) {
        return;
    }

    //deinit
    result = block_device_deinit(dev);
    (void)sd_report(result == BLOCK_DEVICE_RESULT_OK, "deinit", result);
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

    //sd driver hardware demo: each step in order, stopping at the first failure; the heartbeat runs regardless (breakpoint here and then 'monitor rtt start' before continuing)
    sd_hardware_demo();

    bool status_led_on = false;
    absolute_time_t next_led_toggle =
        make_timeout_time_ms(STATUS_LED_TOGGLE_INTERVAL_MS);

    /*
     * Cooperative foreground loop. Future subsystem state machines can be
     * serviced here; interrupts, DMA, and PIO should handle time-critical I/O.
     * TODO(storage): Add a foreground storage coordinator that observes
     * latched media removal, cancels storage consumers, waits for active
     * operations to unwind, and requests idempotent backend teardown.
     */
    while (true) {
        if (board_status_led_available() && time_reached(next_led_toggle)) {
            status_led_on = !status_led_on;
            (void)board_status_led_set(status_led_on);
            next_led_toggle =
                make_timeout_time_ms(STATUS_LED_TOGGLE_INTERVAL_MS);
        }

        sleep_ms(MAIN_LOOP_IDLE_INTERVAL_MS);
    }
}
