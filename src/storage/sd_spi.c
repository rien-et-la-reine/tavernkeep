#include "storage/sd_spi.h"

#include <stddef.h>

#include "hardware/gpio.h"
#include "pico/time.h"
#include "platform/gpio_irq.h"
#include "sd_crc.h"

enum {
    SD_SPI_CARD_DETECT_STABLE_SAMPLES = 10,
    SD_SPI_CARD_DETECT_MAX_SAMPLES = 30,
    SD_SPI_CARD_DETECT_SAMPLE_INTERVAL_MS = 1,
};

static bool sd_spi_card_available(const sd_spi_t *sd);
static void sd_spi_card_available_irq(
    void *context,
    unsigned int gpio,
    uint32_t events
);
static bool sd_spi_removal_latched(const sd_spi_t *sd);
static block_device_result_t sd_spi_require_usable(const sd_spi_t *sd);
static bool sd_spi_wait_ready_timeout(const sd_spi_t *sd, int timeout_ms);
static bool sd_spi_wait_ready(const sd_spi_t *sd);
static uint8_t sd_spi_transfer(const sd_spi_t *sd, uint8_t tx);
static block_device_result_t sd_spi_command(
    const sd_spi_t *sd,
    uint8_t cmd,
    uint32_t arg,
    uint8_t *r1);
static block_device_result_t sd_spi_stop_transmission(const sd_spi_t *sd);
static block_device_result_t sd_spi_read_csd(
    const sd_spi_t *sd,
    uint64_t *block_count);
static block_device_result_t sd_spi_init_rollback(
    sd_spi_t *sd,
    block_device_result_t result);
static void sd_spi_capture_bus(const sd_spi_t *sd) {
    gpio_put(sd->config.pin_chip_select, false);
}
static void sd_spi_release_bus(const sd_spi_t *sd) {
    gpio_put(sd->config.pin_chip_select, true);
    if (!sd_spi_removal_latched(sd)) {
        sd_spi_transfer(sd, 0xFF);
    }
}
static bool sd_spi_is_data_error_token(uint8_t token) {
    return (token & 0xF0U) == 0U
        && (token & 0x0FU) != 0U;
}
static block_device_result_t sd_spi_device_init(void *context);
static block_device_result_t sd_spi_device_deinit(void *context);
static block_device_result_t sd_spi_device_read_blocks(
    void *context,
    uint64_t first_lba,
    void *buffer,
    size_t block_count);
static block_device_result_t sd_spi_device_write_blocks(
    void *context,
    uint64_t first_lba,
    const void *buffer,
    size_t block_count);
static block_device_result_t sd_spi_device_get_info(
    void *context,
    block_device_info_t *info);

static const block_device_operations_t sd_spi_operations = {
    .init = sd_spi_device_init,
    .deinit = sd_spi_device_deinit,
    .read_blocks = sd_spi_device_read_blocks,
    .write_blocks = sd_spi_device_write_blocks,
    .get_info = sd_spi_device_get_info,
};

block_device_result_t sd_spi_configure(
    sd_spi_t *sd,
    const sd_spi_config_t *config)
{
    if (sd == NULL || sd->initialized == true || config == NULL || config->spi == NULL
            || config->baud_rate_hz == 0U || config->baud_rate_hz > 25000000U) {
        return BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
    }

    sd->config = *config;
    sd->configured = true;
    sd->initialized = false;
    sd->block_count = 0U;
    sd->block_device.context = sd;
    sd->block_device.operations = &sd_spi_operations;

    //unlatch the removal flag
    atomic_init(&sd->removal_latched, false);

    return BLOCK_DEVICE_RESULT_OK;
}

block_device_t *sd_spi_as_block_device(sd_spi_t *sd)
{
    if (sd == NULL || !sd->configured) {
        return NULL;
    }
    return &sd->block_device;
}

static block_device_result_t sd_spi_device_init(void *context)
{
    sd_spi_t *const sd = context;
    if (sd == NULL || !sd->configured || sd->initialized) {
        return BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
    }

    sd->initialized = false;
    sd->card_type_legacy = false;
    sd->card_type_hcxc = false;
    sd->block_count = 0U;

    //select the pull-up before gpio_init enables the input buffer on RP2350
    gpio_pull_up(sd->config.pin_card_available);
    //setup the card detect/write protect pin (again, validate hardware slot's truth table to ensure WP can serve both functions)
    gpio_init(sd->config.pin_card_available);
    gpio_set_dir(sd->config.pin_card_available, GPIO_IN);
    //check that a non write protected card is present, with debouncing
    if (!sd_spi_card_available(sd)) {
        gpio_deinit(sd->config.pin_card_available);
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }

    //clear the latch
    atomic_store_explicit(&sd->removal_latched, false, memory_order_relaxed);

    //register the rising edge interrupt
    if (!platform_gpio_irq_register(
            sd->config.pin_card_available,
            GPIO_IRQ_EDGE_RISE,
            sd_spi_card_available_irq,
            sd)) {
        gpio_deinit(sd->config.pin_card_available);
        return BLOCK_DEVICE_RESULT_IO_ERROR;
    }

    //handle removal after the debounced presence check but before registration;
    //the latch also catches an edge that bounced low before this level check
    const bool card_unavailable = gpio_get(sd->config.pin_card_available);
    if (card_unavailable) {
        atomic_store_explicit(&sd->removal_latched, true, memory_order_relaxed);
    }
    if (card_unavailable || sd_spi_removal_latched(sd)) {
        platform_gpio_irq_unregister(sd->config.pin_card_available);
        gpio_deinit(sd->config.pin_card_available);
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }

    //configure remaining gpio and spi
    spi_init(sd->config.spi, 400000); //init using slower baud rate, will switch to higher one later
    gpio_init(sd->config.pin_chip_select);
    gpio_put(sd->config.pin_chip_select, true); //for safety to avoid accidentally asserting cs
    gpio_set_dir(sd->config.pin_chip_select, GPIO_OUT);
    gpio_set_function(sd->config.pin_clock, GPIO_FUNC_SPI);
    gpio_set_function(sd->config.pin_controller_in, GPIO_FUNC_SPI);
    gpio_set_function(sd->config.pin_controller_out, GPIO_FUNC_SPI);

    //cs high
    gpio_put(sd->config.pin_chip_select, true);
    sleep_ms(1);
    if (sd_spi_removal_latched(sd)) {
        return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    }
    //mosi high, 80 clock cycles
    for (uint8_t i = 0; i < 10; i++)
    {
        sd_spi_transfer(sd, 0xFF);
        if (sd_spi_removal_latched(sd)) {
            return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
        }
    }
    //cs low
    sd_spi_capture_bus(sd);

    uint8_t r1 = 0xFF;
    block_device_result_t result;
    //cmd0
    result = sd_spi_command(sd, 0, 0, &r1);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return sd_spi_init_rollback(sd, result);
    }
    //check R1 = 0x01 (in idle state)
    if (r1 != 0x01) {
        return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
    }
    //cmd8 with arg 0x1AA
    result = sd_spi_command(sd, 8, 0x1AA, &r1);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return sd_spi_init_rollback(sd, result);
    }
    if (r1 == 0x01) {
        //read R7, card type is v2+
        sd_spi_transfer(sd, 0xFF);
        sd_spi_transfer(sd, 0xFF);
        if ((sd_spi_transfer(sd, 0xFF) & 0x0F) != 0x01) {
            return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
        }
        if (sd_spi_transfer(sd, 0xFF) != 0xAA) { 
            return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
        }
        if (sd_spi_removal_latched(sd)) {
            return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
        }
    } else if (r1 == 0x05) {
        //card type is v1
        sd->card_type_legacy = true;
    } else {
        return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
    }
    //omitting ocr check since we're operating at 3v3 anyway and supporting that's basically mandated by the spec for all cards
    //cmd59 to enable command crc checking, done here as per spec recommendation to protect ACMD41 as well as remaining init procedure, ensures entire init sequence is crc protected
    result = sd_spi_command(sd, 59, 1, &r1);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return sd_spi_init_rollback(sd, result);
    }
    if (r1 > 0x01) { 
            return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
    }
    //repeat ACMD41 until card not idle, make sure to set the HCS bit if card is v2
    absolute_time_t timeout = make_timeout_time_ms(1200); //slightly higher to account for starting timer before first attempt
    do {
        result = sd_spi_command(sd, 55, 0, &r1);
        if (result != BLOCK_DEVICE_RESULT_OK) {
            return sd_spi_init_rollback(sd, result);
        }
        if (r1 > 0x01) { 
            return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
        }
        if (sd->card_type_legacy) {
            result = sd_spi_command(sd, 41, 0, &r1);
        } else {
            result = sd_spi_command(sd, 41, 0x40000000, &r1);
        }
        if (result != BLOCK_DEVICE_RESULT_OK) {
            return sd_spi_init_rollback(sd, result);
        }
        if (r1 > 0x01) { 
            return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
        }
        //repeat ACMD41 until R1 response no longer reads IDLE_STATE
    } while (!time_reached(timeout) && (r1 == 0x01));
    if (r1 != 0x00) { 
        return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
    }
    //cmd58 to get ocr, check ccs to verify SDSC or SDHC card (byte addressing vs block addressing)
    result = sd_spi_command(sd, 58, 0, &r1);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return sd_spi_init_rollback(sd, result);
    }
    if (r1 != 0x00) { 
        return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
    }
    uint8_t ocr_msb = sd_spi_transfer(sd, 0xFF); //not R1 format, first byte of OCR
    sd_spi_transfer(sd, 0xFF);
    sd_spi_transfer(sd, 0xFF);
    sd_spi_transfer(sd, 0xFF);
    if (sd_spi_removal_latched(sd)) {
        return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    }
    if ((ocr_msb & 0x80) != 0x80) {
        //not powered up for some reason
        return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
    }
    if (((ocr_msb & 0x40) == 0x40) && (!sd->card_type_legacy)) {
        sd->card_type_hcxc = true;
    }

    result = sd_spi_read_csd(sd, &sd->block_count);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return sd_spi_init_rollback(sd, result);
    }

    if (!sd->card_type_hcxc) {
        //for sdsc card need to set block length to 512 with cmd16
        result = sd_spi_command(sd, 16, 512, &r1);
        if (result != BLOCK_DEVICE_RESULT_OK) {
            return sd_spi_init_rollback(sd, result);
        }
        if (r1 != 0x00) { 
            return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_IO_ERROR);
        }
    }

    if (sd_spi_removal_latched(sd)) {
        return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    }

    //cs deassert and trailing clocks
    sd_spi_release_bus(sd);
    if (sd_spi_removal_latched(sd)) {
        return sd_spi_init_rollback(sd, BLOCK_DEVICE_RESULT_INVALID_DEVICE);
    }
    
    //move to operational baud rate
    spi_set_baudrate(sd->config.spi, sd->config.baud_rate_hz);
    
    //card initalized, update status accordingly
    sd->initialized = true;
    
    return BLOCK_DEVICE_RESULT_OK;
}

// On a busy timeout, SPI resources remain configured and the device remains initialized.
static block_device_result_t sd_spi_device_deinit(void *context)
{
    sd_spi_t *const sd = context;
    if (sd == NULL || !sd->configured) {
        return BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
    }

    /* Every initialization path that fails releases what it acquired, so an
     * uninitialized device owns no SPI or GPIO resources. Releasing them again
     * would take the peripheral away from whatever owns it now and would break
     * the exactly-once teardown the architecture requires of the removal path.
     * Teardown therefore stays idempotent by doing nothing here. */
    if (!sd->initialized) {
        return BLOCK_DEVICE_RESULT_OK;
    }

    if (!sd_spi_removal_latched(sd)) {
        sd_spi_capture_bus(sd);
        if (!sd_spi_wait_ready(sd) && !sd_spi_removal_latched(sd)) {
            sd_spi_release_bus(sd);
            return BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
        }
        sd_spi_release_bus(sd);
    } else {
        gpio_put(sd->config.pin_chip_select, true);
    }

    (void)platform_gpio_irq_unregister(sd->config.pin_card_available);
    spi_deinit(sd->config.spi);
    gpio_deinit(sd->config.pin_clock);
    gpio_deinit(sd->config.pin_controller_out);
    gpio_deinit(sd->config.pin_controller_in);
    gpio_deinit(sd->config.pin_chip_select);
    gpio_deinit(sd->config.pin_card_available);

    sd->initialized = false;
    sd->card_type_legacy = false;
    sd->card_type_hcxc = false;
    sd->block_count = 0U;

    return BLOCK_DEVICE_RESULT_OK;
}

static block_device_result_t sd_spi_device_read_blocks(
    void *context,
    uint64_t first_lba,
    void *buffer,
    size_t block_count)
{
    sd_spi_t *const sd = context;

    if (sd == NULL || !sd->configured || buffer == NULL || block_count == 0U) {
        return BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
    }
    const block_device_result_t usability = sd_spi_require_usable(sd);
    if (usability != BLOCK_DEVICE_RESULT_OK) {
        return usability;
    }
    if (first_lba >= sd->block_count
            || (uint64_t)block_count > sd->block_count - first_lba) {
        return BLOCK_DEVICE_RESULT_OUT_OF_RANGE;
    }

    uint8_t r1, token;
    uint8_t *buf = buffer;
    block_device_result_t result;

    //adjust block address to byte address for sdsc cards
    if (!sd->card_type_hcxc) {
        first_lba *= 512U;
    }


    //assert chip select
    sd_spi_capture_bus(sd);

    //requested block count check to determine command
    if (block_count != 1) {
        //multiblock read
        //issue command and collect r1
        result = sd_spi_command(sd, 18, (uint32_t)first_lba, &r1);
        if (result != BLOCK_DEVICE_RESULT_OK) {
            sd_spi_release_bus(sd);
            return result;
        }
        //validate r1 response
        if (r1 == 0x00) {
            do {
                absolute_time_t timeout = make_timeout_time_ms(100);
                do {
                    token = sd_spi_transfer(sd, 0xFF);
                    if (sd_spi_removal_latched(sd)) {
                        sd_spi_release_bus(sd);
                        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                    }
                    if (token == 0xFE) {
                        //data response token, read in block to buffer and discard 2 byte crc
                        for (uint16_t i = 0; i < 512; i++) {
                            *buf++ = sd_spi_transfer(sd, 0xFF);
                            if (sd_spi_removal_latched(sd)) {
                                sd_spi_release_bus(sd);
                                return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                            }
                        }
                        //discard CRC TODO: don't discard CRC, check it
                        sd_spi_transfer(sd, 0xFF);
                        sd_spi_transfer(sd, 0xFF);
                        if (sd_spi_removal_latched(sd)) {
                            sd_spi_release_bus(sd);
                            return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                        }
                        break;
                    } else if (sd_spi_is_data_error_token(token)) {
                        //data error token, issue command 12 and release bus
                        result = sd_spi_stop_transmission(sd);
                        sd_spi_release_bus(sd);
                        if (result == BLOCK_DEVICE_RESULT_INVALID_DEVICE) {
                            return result;
                        }
                        if (result == BLOCK_DEVICE_RESULT_BUSY_TIMEOUT) {
                            return result;
                        }
                        return BLOCK_DEVICE_RESULT_IO_ERROR;
                    }
                } while (!time_reached(timeout));
                if (token != 0xFE) {
                    break;
                }
            } while (--block_count);

            if (sd_spi_removal_latched(sd)) {
                sd_spi_release_bus(sd);
                return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
            }
            result = sd_spi_stop_transmission(sd);
            sd_spi_release_bus(sd);
            //a removal edge during the release clocks still cancels the transfer
            if (sd_spi_removal_latched(sd)) {
                return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
            }
            if (result != BLOCK_DEVICE_RESULT_OK) {
                return result;
            }
            return token == 0xFE
                ? BLOCK_DEVICE_RESULT_OK
                : BLOCK_DEVICE_RESULT_IO_ERROR;
        }
        //an error occurred before CMD18 entered the data-transfer state
        if (!sd_spi_wait_ready(sd)) {
            sd_spi_release_bus(sd);
            return sd_spi_removal_latched(sd)
                ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
                : BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
        }
        sd_spi_release_bus(sd);
        return BLOCK_DEVICE_RESULT_IO_ERROR;

    } else {
        //single block read command 17
        result = sd_spi_command(sd, 17, (uint32_t)first_lba, &r1);
        if (result != BLOCK_DEVICE_RESULT_OK) {
            sd_spi_release_bus(sd);
            return result;
        }
        if (r1 == 0x00) {
            //no errors, wait for data start token, timeout 100ms
            absolute_time_t timeout = make_timeout_time_ms(100);
            do {
                token = sd_spi_transfer(sd, 0xFF);
                if (sd_spi_removal_latched(sd)) {
                    sd_spi_release_bus(sd);
                    return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                }
                if (token == 0xFE) {
                    //data response token, read in block to buffer and discard 2 byte crc
                    for (uint16_t i = 0; i < 512; i++) {
                        *buf++ = sd_spi_transfer(sd, 0xFF);
                        if (sd_spi_removal_latched(sd)) {
                            sd_spi_release_bus(sd);
                            return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                        }
                    }
                    //discard CRC, TODO: check and validate CRC instead of discarding
                    sd_spi_transfer(sd, 0xFF);
                    sd_spi_transfer(sd, 0xFF);
                    if (sd_spi_removal_latched(sd)) {
                        sd_spi_release_bus(sd);
                        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                    }
                    break;
                }
                if (sd_spi_is_data_error_token(token)) {
                    //data error token
                    sd_spi_release_bus(sd);
                    return BLOCK_DEVICE_RESULT_IO_ERROR;
                }
            } while (!time_reached(timeout));

            if (token == 0xFE) {
                //did not time out
                sd_spi_release_bus(sd);
                //a removal edge during the release clocks still cancels the transfer
                if (sd_spi_removal_latched(sd)) {
                    return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                }
                return BLOCK_DEVICE_RESULT_OK;
            }
        }
    }

    //timed out or r1 error
    sd_spi_release_bus(sd);
    return BLOCK_DEVICE_RESULT_IO_ERROR;
}

static block_device_result_t sd_spi_device_write_blocks(
    void *context,
    uint64_t first_lba,
    const void *buffer,
    size_t block_count)
{
    sd_spi_t *const sd = context;

    if (sd == NULL || !sd->configured) {
        return BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
    }
    const block_device_result_t usability = sd_spi_require_usable(sd);
    if (usability != BLOCK_DEVICE_RESULT_OK) {
        return usability;
    }
    if (first_lba >= sd->block_count || (uint64_t)block_count > sd->block_count - first_lba) {
        return BLOCK_DEVICE_RESULT_OUT_OF_RANGE;
    }
    if (block_count == 0 || buffer == NULL) {
        return BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
    }

    uint8_t r1, token;
    const uint8_t *buf = buffer;
    block_device_result_t result;
    uint16_t byte_counter, crc;

    //adjust block address to byte address for sdsc cards
    if (!sd->card_type_hcxc) {
        first_lba *= 512U;
    }

    //assert chip select
    sd_spi_capture_bus(sd);

    //requested block count check to determine command
    if (block_count > 1) {
        //multiblock write cmd25
        //issue command and check block device result
        result = sd_spi_command(sd, 25, (uint32_t)first_lba, &r1);
        if (result != BLOCK_DEVICE_RESULT_OK) {
            sd_spi_release_bus(sd);
            return result;
        }
        //check r1
        if (r1 == 0x00) {
            size_t sent_blocks = 0;
            //spec mandated byte gap
            sd_spi_transfer(sd, 0xFF);
            do {
                //reset byte counter
                byte_counter = 0;
                //transmit start block token (0b11111100 for multi block write) + load data block into transmit buffer
                sd_spi_transfer(sd, 0b11111100);
                do {
                    //transmit byte
                    sd_spi_transfer(sd, buf[(sent_blocks * 512) + (byte_counter)]);
                    //increment byte counter
                    byte_counter++;
                    if (sd_spi_removal_latched(sd)) {
                        sd_spi_release_bus(sd);
                        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                    }
                } while (byte_counter < 512);
                //transmit CRC
                crc = crc_helper_16((buf + (sent_blocks * 512)), 512);
                sd_spi_transfer(sd, (uint8_t) (crc >> 8));
                sd_spi_transfer(sd, (uint8_t) crc);
                //read data response token (which is the byte immediately following last CRC byte transmission with no gap)
                token = sd_spi_transfer(sd, 0xFF) & 0x1F; //mask out upper three bits, from spec
                if (sd_spi_removal_latched(sd)) {
                    sd_spi_release_bus(sd);
                    return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                }
                if ((token == 0b00001011) || (token == 0b00001101)) { //data CRC or data write error
                    //ensure card not busy
                    if (!sd_spi_wait_ready_timeout(sd, 1000)) {
                        sd_spi_release_bus(sd);
                        return sd_spi_removal_latched(sd)
                            ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
                            : BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
                    }
                    //data error token, stop transmission with cmd12 (according to spec)
                    result = sd_spi_stop_transmission(sd);
                    //TODO: optional implement CMD13 or ACMD22 later if needed
                    //release chip select
                    sd_spi_release_bus(sd);
                    //check latch
                    if (sd_spi_removal_latched(sd)) {
                        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                    }
                    if (result == BLOCK_DEVICE_RESULT_INVALID_DEVICE || result == BLOCK_DEVICE_RESULT_BUSY_TIMEOUT) {
                        return result;
                    }
                    return BLOCK_DEVICE_RESULT_IO_ERROR;
                }
                if (!(token == 0b00000101)) {
                    //unknown token, TODO: robust cleanup procedure, card likely never recieved data start token
                    sd_spi_release_bus(sd);
                    return sd_spi_removal_latched(sd)
                        ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
                        : BLOCK_DEVICE_RESULT_IO_ERROR;
                }
                //wait ready (NOTE: this is longer than the standard wait period)
                if (!sd_spi_wait_ready_timeout(sd, 1000)) {
                    sd_spi_release_bus(sd);
                    return sd_spi_removal_latched(sd)
                        ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
                        : BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
                }
            } while(++sent_blocks < block_count); //repeat for block_count number of blocks
            //stop transmission token (0b11111101)
            sd_spi_transfer(sd, 0b11111101);
            //spec mandated byte gap
            sd_spi_transfer(sd, 0xFF);
            if (sd_spi_removal_latched(sd)) {
                sd_spi_release_bus(sd);
                return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
            }
        } else {
            //r1 error
            if (!sd_spi_wait_ready_timeout(sd, 1000)) {
                sd_spi_release_bus(sd);
                return sd_spi_removal_latched(sd)
                    ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
                    : BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
            }
            sd_spi_release_bus(sd);
            return BLOCK_DEVICE_RESULT_IO_ERROR;
        }
    } else {
        //single block write cmd24
        //issue command and check block device result
        result = sd_spi_command(sd, 24, (uint32_t)first_lba, &r1);
        if (result != BLOCK_DEVICE_RESULT_OK) {
            sd_spi_release_bus(sd);
            return result;
        }
        //check r1
        if (r1 == 0x00) {
            //spec mandated byte gap
            sd_spi_transfer(sd, 0xFF);
            //reset byte counter
            byte_counter = 0;
            //transmit start block token (0b11111110 for single block write)
            sd_spi_transfer(sd, 0b11111110);
            do {
                //transmit byte + increment counter
                sd_spi_transfer(sd, buf[byte_counter++]);
                //check latch
                if (sd_spi_removal_latched(sd)) {
                    sd_spi_release_bus(sd);
                    return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                }
            } while (byte_counter < 512);
            //transmit crc
            crc = crc_helper_16(buf, 512);
            sd_spi_transfer(sd, (uint8_t) (crc >> 8));
            sd_spi_transfer(sd, (uint8_t) crc);

            //data response token
            token = sd_spi_transfer(sd, 0xFF) & 0x1F; //mask out upper three bits, from spec
            if (sd_spi_removal_latched(sd)) {
                sd_spi_release_bus(sd);
                return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
            }
            if ((token == 0b00001011) || (token == 0b00001101)) { //data CRC or data write error
                //ensure card not busy
                if (!sd_spi_wait_ready_timeout(sd, 1000)) {
                    sd_spi_release_bus(sd);
                    return sd_spi_removal_latched(sd)
                        ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
                        : BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
                }
                //data error token
                //release chip select
                sd_spi_release_bus(sd);
                //check latch
                if (sd_spi_removal_latched(sd)) {
                    return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
                }
                return BLOCK_DEVICE_RESULT_IO_ERROR;
            }
            if (!(token == 0b00000101)) {
                //unknown token, TODO: robust cleanup procedure, card likely never recieved data start token
                sd_spi_release_bus(sd);
                return sd_spi_removal_latched(sd)
                    ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
                    : BLOCK_DEVICE_RESULT_IO_ERROR;
            }
        } else {
            //r1 error
            if (!sd_spi_wait_ready_timeout(sd, 1000)) {
                sd_spi_release_bus(sd);
                return sd_spi_removal_latched(sd)
                    ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
                    : BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
            }
            sd_spi_release_bus(sd);
            return BLOCK_DEVICE_RESULT_IO_ERROR;
        }
    }
    //wait ready (NOTE: this is longer than the standard wait period)
    if (!sd_spi_wait_ready_timeout(sd, 1000)) {
        sd_spi_release_bus(sd);
        return sd_spi_removal_latched(sd)
            ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
            : BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
    }
    //release chip select
    sd_spi_release_bus(sd);
    //check latch
    if (sd_spi_removal_latched(sd)) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }
    //return ok
    return BLOCK_DEVICE_RESULT_OK;
}

static block_device_result_t sd_spi_device_get_info(
    void *context,
    block_device_info_t *info)
{
    sd_spi_t *const sd = context;
    (void)info;

    if (sd == NULL || !sd->configured || info == NULL) {
        return BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
    }
    const block_device_result_t usability = sd_spi_require_usable(sd);
    if (usability != BLOCK_DEVICE_RESULT_OK) {
        return usability;
    }

    info->block_count = sd->block_count;
    info->block_size_bytes = 512;
    info->writable = true;

    return BLOCK_DEVICE_RESULT_OK;
}

// The caller owns the captured bus and remains responsible for releasing it.
static block_device_result_t sd_spi_stop_transmission(const sd_spi_t *sd)
{
    if (sd_spi_removal_latched(sd)) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }
    uint8_t transmit_buffer[5], i;

    //populate transmit buffer
    transmit_buffer[0] = 12U | 0x40;
    transmit_buffer[1] = (0x00U);
    transmit_buffer[2] = (0x00U);
    transmit_buffer[3] = (0x00U);
    transmit_buffer[4] = (0x00U);
    //send command index and args
    for (i = 0; i < 5; i++) {
        sd_spi_transfer(sd, transmit_buffer[i]);
    }
    //send CRC7
    sd_spi_transfer(sd, (crc_helper_7(transmit_buffer, 5) << 1)| 0x01);

    if (sd_spi_removal_latched(sd)) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }

    // CMD12 has one positional stuff byte before its R1 response in SPI mode.
    sd_spi_transfer(sd, 0xFFU);
    if (sd_spi_removal_latched(sd)) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }

    uint8_t r1 = 0xFFU;
    for (uint8_t i = 0U; i < 8U; ++i) {
        r1 = sd_spi_transfer(sd, 0xFFU);
        if (sd_spi_removal_latched(sd)) {
            return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
        }
        if ((r1 & 0x80U) == 0U) {
            break;
        }
    }
    if (!sd_spi_wait_ready(sd)) {
        return sd_spi_removal_latched(sd)
            ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
            : BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
    }
    return r1 == 0x00U
        ? BLOCK_DEVICE_RESULT_OK
        : BLOCK_DEVICE_RESULT_IO_ERROR;
}

static block_device_result_t sd_spi_read_csd(
    const sd_spi_t *sd,
    uint64_t *block_count)
{
    //issue command 9 and check result and r1
    uint8_t r1;
    block_device_result_t result = sd_spi_command(sd, 9, 0, &r1);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return result;
    }
    if (r1 != 0x00) {
        return BLOCK_DEVICE_RESULT_IO_ERROR;
    }

    //wait for data token
    uint8_t token;
    absolute_time_t timeout = make_timeout_time_ms(100);
    do {
        token = sd_spi_transfer(sd, 0xFF);
        if (sd_spi_removal_latched(sd)) {
            return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
        }
        if (token == 0xFE) {
            //data start token
            break;
        }
        if (sd_spi_is_data_error_token(token)) {
            //data error token
            return BLOCK_DEVICE_RESULT_IO_ERROR;
        }
    } while (!time_reached(timeout));
    if (token != 0xFE) {
        //timeout
        return BLOCK_DEVICE_RESULT_IO_ERROR;
    }

    //data start token recieved, read in 16 byte CSD register
    uint8_t csd[16];
    for (size_t i = 0U; i < sizeof(csd); ++i) {
        csd[i] = sd_spi_transfer(sd, 0xFF);
        if (sd_spi_removal_latched(sd)) {
            return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
        }
    }
    //discard crc
    sd_spi_transfer(sd, 0xFF);
    sd_spi_transfer(sd, 0xFF);
    if (sd_spi_removal_latched(sd)) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }

    //check csd structure to determine version (version 1.0 for sdsc and 2.0 for sdhc/sdxc. sduc not supported)
    const uint8_t csd_structure = (uint8_t)(csd[0] >> 6U);
    if (csd_structure == 0U) {
        //version 1.0, only for sdsc cards, doing a sanity check
        if (sd->card_type_hcxc) {
            return BLOCK_DEVICE_RESULT_IO_ERROR;
        }

        //check maximum read data block length
        const uint8_t read_bl_len = csd[5] & 0x0FU;
        if (read_bl_len < 9U || read_bl_len > 11U) {
            return BLOCK_DEVICE_RESULT_IO_ERROR;
        }
        //check device size and corresponding multiplier field
        const uint32_t c_size = ((uint32_t)(csd[6] & 0x03U) << 10U)
            | ((uint32_t)csd[7] << 2U)
            | ((uint32_t)(csd[8] & 0xC0U) >> 6U);
        const uint8_t c_size_mult = (uint8_t)(((csd[9] & 0x03U) << 1U)
            | ((csd[10] & 0x80U) >> 7U));
        //calculate capacity in bytes based on size and multiplier
        const uint64_t capacity_bytes = ((uint64_t)c_size + 1U)
            * (1ULL << (c_size_mult + 2U)) * (1ULL << read_bl_len);
        //double check that byte capacity is a valid value (non-zero and divisible into 512 byte blocks)
        if (capacity_bytes == 0U || (capacity_bytes % 512U) != 0U) {
            return BLOCK_DEVICE_RESULT_IO_ERROR;
        }
        //calculate block capacity from byte capacity and store it
        *block_count = capacity_bytes / 512U;
        return BLOCK_DEVICE_RESULT_OK;
    }

    if (csd_structure == 1U) {
        //version 2.0, for sdhc and sdxc cards, double check that the card is of the correct type
        if (!sd->card_type_hcxc) {
            return BLOCK_DEVICE_RESULT_IO_ERROR;
        }
        //check device size (longer field in the register than version 1.0)
        const uint32_t c_size = ((uint32_t)(csd[7] & 0x3FU) << 16U)
            | ((uint32_t)csd[8] << 8U)
            | (uint32_t)csd[9];
        //the spec's 512 KByte capacity unit equals 1024 logical 512-byte blocks
        *block_count = ((uint64_t)c_size + 1U) * 1024U;
        return BLOCK_DEVICE_RESULT_OK;
    }

    //version 3.0 for sduc not implemented
    return BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED;
}

static block_device_result_t sd_spi_init_rollback(
    sd_spi_t *sd,
    block_device_result_t result)
{
    (void)platform_gpio_irq_unregister(sd->config.pin_card_available);
    sd_spi_release_bus(sd);
    spi_deinit(sd->config.spi);
    gpio_deinit(sd->config.pin_clock);
    gpio_deinit(sd->config.pin_controller_out);
    gpio_deinit(sd->config.pin_controller_in);
    gpio_deinit(sd->config.pin_chip_select);
    gpio_deinit(sd->config.pin_card_available);
    sd->initialized = false;
    sd->card_type_legacy = false;
    sd->card_type_hcxc = false;
    sd->block_count = 0U;
    return result;
}

static bool sd_spi_card_available(const sd_spi_t *sd)
{
    uint8_t stable_samples = 0U;

    for (uint8_t sample = 0U;
            sample < SD_SPI_CARD_DETECT_MAX_SAMPLES; ++sample) {
        if (!gpio_get(sd->config.pin_card_available)) {
            stable_samples++;
            if (stable_samples == SD_SPI_CARD_DETECT_STABLE_SAMPLES) {
                return true;
            }
        } else {
            stable_samples = 0U;
        }

        if (sample + 1U < SD_SPI_CARD_DETECT_MAX_SAMPLES) {
            sleep_ms(SD_SPI_CARD_DETECT_SAMPLE_INTERVAL_MS);
        }
    }

    return false;
}

static void sd_spi_card_available_irq(
    void *context,
    unsigned int gpio,
    uint32_t events)
{
    //pull the sd card object
    sd_spi_t *const sd = context;

    //if sd card object does not exist, the triggering gpio is not it's card detect/write protect pin, or the triggering event was not a rising edge, do nothing
    if (sd == NULL
            || gpio != sd->config.pin_card_available
            || (events & GPIO_IRQ_EDGE_RISE) == 0U) {
        return;
    }

    //set the card removal latch; a fresh initialization attempt may clear it
    atomic_store_explicit(&sd->removal_latched, true, memory_order_relaxed);

    //supress repeated triggers and bouncing, will be rearmed at fresh initialization
    platform_gpio_irq_set_enabled(gpio, false);
}

static bool sd_spi_removal_latched(const sd_spi_t *sd)
{
    return atomic_load_explicit(
        &sd->removal_latched, memory_order_relaxed);
}

static block_device_result_t sd_spi_require_usable(const sd_spi_t *sd) {
    if (sd_spi_removal_latched(sd)) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }

    if (!sd->initialized) {
        return BLOCK_DEVICE_RESULT_NOT_INITIALIZED;
    }

    return BLOCK_DEVICE_RESULT_OK;
}

static bool sd_spi_wait_ready(const sd_spi_t *sd) {
    return sd_spi_wait_ready_timeout(sd, 250); //this value is an estimate, not from spec
}

static bool sd_spi_wait_ready_timeout(const sd_spi_t *sd, int timeout_ms) {
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    while (!time_reached(timeout)) {
        if (sd_spi_removal_latched(sd)) {
            return false;
        }
        const uint8_t response = sd_spi_transfer(sd, 0xFF);
        if (sd_spi_removal_latched(sd)) {
            return false;
        }
        if (response == 0xFF) {
            return true;
        }
    }

    return false;
}

static uint8_t sd_spi_transfer(const sd_spi_t *sd, uint8_t tx) {
    uint8_t rx;
    spi_write_read_blocking(sd->config.spi, &tx, &rx, 1);
    return rx;
}

static block_device_result_t sd_spi_command(
    const sd_spi_t *sd,
    uint8_t cmd,
    uint32_t arg,
    uint8_t *r1)
{
    uint8_t response;
    uint8_t transmit_buffer[5];
    uint8_t i = 0;

    if (sd_spi_removal_latched(sd)) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }

    //ensure card is ready and preserve a distinct busy-timeout result
    if (!sd_spi_wait_ready_timeout(sd, 500)) {
        return sd_spi_removal_latched(sd)
            ? BLOCK_DEVICE_RESULT_INVALID_DEVICE
            : BLOCK_DEVICE_RESULT_BUSY_TIMEOUT;
    }
    //populate transmit buffer
    transmit_buffer[0] = cmd | 0x40;
    transmit_buffer[1] = (uint8_t) (arg >> 24);
    transmit_buffer[2] = (uint8_t) (arg >> 16);
    transmit_buffer[3] = (uint8_t) (arg >> 8);
    transmit_buffer[4] = (uint8_t) (arg);
    //send command index and args
    for (i = 0; i < 5; i++) {
        sd_spi_transfer(sd, transmit_buffer[i]);
    }
    //send CRC7
    sd_spi_transfer(sd, (crc_helper_7(transmit_buffer, 5) << 1)| 0x01);

    if (sd_spi_removal_latched(sd)) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }
    //wait for R1 response
    i = 0;
    while (((response = sd_spi_transfer(sd, 0xFF)) & 0x80) == 0x80) {
        if (sd_spi_removal_latched(sd)) {
            return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
        }
        i++;
        if (i > 7) {
            return BLOCK_DEVICE_RESULT_IO_ERROR;
        }
    }
    if (sd_spi_removal_latched(sd)) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }
    *r1 = response;
    return BLOCK_DEVICE_RESULT_OK;
}
