#include "sd_fixture.h"

#include <string.h>

#include "hardware/gpio.h"

enum { SD_FX_FILL = 0xA5 };

static bool card_detect_active_high;

void sd_fx_set_card_detect_active_high(bool active_high)
{
    card_detect_active_high = active_high;
}

bool sd_fx_card_detect_active_high(void)
{
    return card_detect_active_high;
}

void sd_fx_set_card_present(bool present)
{
    /* Active-low: present is low. Active-high: present is high. */
    pico_mock_gpio_set_input(SD_FX_PIN_CARD_DETECT,
        present == card_detect_active_high);
}

uint32_t sd_fx_removal_edge(void)
{
    return card_detect_active_high ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE;
}

bool sd_fx_remove_card(void)
{
    sd_fx_set_card_present(false);
    return pico_mock_gpio_irq_fire(SD_FX_PIN_CARD_DETECT, sd_fx_removal_edge());
}

bool sd_fx_parse_card_detect_arg(const char *arg)
{
    if (strcmp(arg, "--card-detect=active-low") == 0) {
        card_detect_active_high = false;
        return true;
    }
    if (strcmp(arg, "--card-detect=active-high") == 0) {
        card_detect_active_high = true;
        return true;
    }
    return false;
}

void sd_fx_begin(sd_fixture_t *fx, const sd_card_desc_t *desc)
{
    memset(fx, 0, sizeof(*fx));
    fx->spi.id = 0U;

    pico_mock_reset();
    sd_card_reset(desc);
    sd_card_set_response_policy(SD_RESPONSE_MODELLED);
    pico_mock_sd_use_chip_select(SD_FX_PIN_CS);
    pico_mock_sd_use_card_detect(SD_FX_PIN_CARD_DETECT);
    pico_mock_sd_set_card_detect_active_high(card_detect_active_high);
    /* Usable media is present at the level the configured sense expects. */
    sd_fx_set_card_present(true);

    fx->config.spi = &fx->spi;
    fx->config.baud_rate_hz = SD_FX_BAUD_HZ;
    fx->config.pin_clock = SD_FX_PIN_CLOCK;
    fx->config.pin_controller_out = SD_FX_PIN_MOSI;
    fx->config.pin_controller_in = SD_FX_PIN_MISO;
    fx->config.pin_chip_select = SD_FX_PIN_CS;
    fx->config.pin_card_available = SD_FX_PIN_CARD_DETECT;
    fx->config.card_detect_active_high = card_detect_active_high;
}

block_device_result_t sd_fx_init(sd_fixture_t *fx)
{
    const block_device_result_t configured =
        sd_spi_configure(&fx->sd, &fx->config);
    if (configured != BLOCK_DEVICE_RESULT_OK) {
        return configured;
    }
    fx->device = sd_spi_as_block_device(&fx->sd);
    if (fx->device == NULL) {
        return BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
    }
    return block_device_init(fx->device);
}

bool sd_fx_require_init(sd_fixture_t *fx, const sd_card_desc_t *desc)
{
    sd_fx_begin(fx, desc);
    return sd_fx_init(fx) == BLOCK_DEVICE_RESULT_OK;
}

/* ------------------------------------------------------ guarded buffers */

void sd_fx_guard_init(sd_guarded_buffer_t *buffer, size_t blocks)
{
    memset(buffer->bytes, SD_FX_FILL, sizeof(buffer->bytes));
    buffer->blocks = blocks;
}

uint8_t *sd_fx_guard_data(sd_guarded_buffer_t *buffer)
{
    return &buffer->bytes[SD_FX_GUARD];
}

bool sd_fx_guard_intact(const sd_guarded_buffer_t *buffer)
{
    for (size_t i = 0U; i < SD_FX_GUARD; ++i) {
        if (buffer->bytes[i] != SD_FX_FILL) {
            return false;
        }
    }
    const size_t tail = SD_FX_GUARD + (buffer->blocks * SD_FX_BLOCK);
    for (size_t i = 0U; i < SD_FX_GUARD; ++i) {
        if (buffer->bytes[tail + i] != SD_FX_FILL) {
            return false;
        }
    }
    return true;
}

size_t sd_fx_guard_prefix_from_card(
    const sd_guarded_buffer_t *buffer,
    uint64_t first_lba)
{
    uint8_t expected[SD_FX_BLOCK];
    size_t matched = 0U;
    for (size_t block = 0U; block < buffer->blocks; ++block) {
        sd_card_fill_expected_block(first_lba + block, expected);
        for (size_t i = 0U; i < SD_FX_BLOCK; ++i) {
            if (buffer->bytes[SD_FX_GUARD + (block * SD_FX_BLOCK) + i]
                    != expected[i]) {
                return matched;
            }
            matched++;
        }
    }
    return matched;
}

bool sd_fx_guard_suffix_is_fill(
    const sd_guarded_buffer_t *buffer,
    size_t offset)
{
    const size_t length = buffer->blocks * SD_FX_BLOCK;
    for (size_t i = offset; i < length; ++i) {
        if (buffer->bytes[SD_FX_GUARD + i] != SD_FX_FILL) {
            return false;
        }
    }
    return true;
}

bool sd_fx_guard_untouched(const sd_guarded_buffer_t *buffer)
{
    return sd_fx_guard_suffix_is_fill(buffer, 0U);
}

bool sd_fx_guard_matches_card(
    const sd_guarded_buffer_t *buffer,
    uint64_t first_lba)
{
    uint8_t expected[SD_FX_BLOCK];
    for (size_t block = 0U; block < buffer->blocks; ++block) {
        sd_card_fill_expected_block(first_lba + block, expected);
        if (memcmp(&buffer->bytes[SD_FX_GUARD + (block * SD_FX_BLOCK)],
                expected, SD_FX_BLOCK) != 0) {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------- state assertions */

sd_fx_state_t sd_fx_state(sd_fixture_t *fx)
{
    sd_fx_state_t state;
    memset(&state, 0, sizeof(state));
    state.bus_released = pico_mock_gpio_level(SD_FX_PIN_CS)
        && sd_card_chip_select_released();
    state.spi_initialized = pico_mock_spi_is_initialized();
    state.driver_initialized = fx->sd.initialized;
    state.removal_latched = atomic_load(&fx->sd.removal_latched);
    state.card_irq_registered =
        pico_mock_gpio_irq_is_registered(SD_FX_PIN_CARD_DETECT);
    state.card_irq_enabled =
        pico_mock_gpio_irq_is_enabled(SD_FX_PIN_CARD_DETECT);
    state.spi_deinit_count = pico_mock_spi_deinit_count();
    state.pending_card_bytes = sd_card_pending_scripted_bytes();
    return state;
}

const char *sd_fx_check_bus_quiescent(const sd_fixture_t *fx)
{
    if (!pico_mock_gpio_level(SD_FX_PIN_CS)) {
        return "chip select left asserted after the transaction";
    }
    if (!sd_card_chip_select_released()) {
        return "card still selected after the transaction";
    }
    if (sd_card_pending_scripted_bytes() != 0U) {
        return "scripted card response left unread on the bus";
    }
    (void)fx;
    return NULL;
}

const char *sd_fx_check_recovers(sd_fixture_t *fx, uint64_t lba)
{
    /* The question is whether the driver is still usable, so the card is
     * first returned to a responsive state: a fault that left the model
     * permanently busy would otherwise fail the probe for the card's reason
     * rather than the driver's. */
    sd_card_clear_faults();
    sd_card_resume();
    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, 1U);
    const block_device_result_t result = block_device_read_blocks(
        fx->device, lba, sd_fx_guard_data(&buffer), 1U);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return "the device did not recover: the next read failed";
    }
    if (!sd_fx_guard_intact(&buffer)) {
        return "the recovery read wrote outside its destination";
    }
    if (!sd_fx_guard_matches_card(&buffer, lba)) {
        return "the recovery read returned the wrong data";
    }
    return NULL;
}

const char *sd_fx_check_removed_and_teardown_once(sd_fixture_t *fx)
{
    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, 1U);

    const size_t before = pico_mock_spi_transfer_count();
    if (block_device_read_blocks(fx->device, 0U, sd_fx_guard_data(&buffer), 1U)
            != BLOCK_DEVICE_RESULT_INVALID_DEVICE) {
        return "a read after removal was not refused with INVALID_DEVICE";
    }
    if (pico_mock_spi_transfer_count() != before) {
        return "a read after removal still touched the bus";
    }
    if (!sd_fx_guard_untouched(&buffer)) {
        return "a refused read still modified the destination";
    }

    const size_t deinits_before = pico_mock_spi_deinit_count();
    if (block_device_deinit(fx->device) != BLOCK_DEVICE_RESULT_OK) {
        return "removal teardown did not succeed";
    }
    if (pico_mock_spi_transfer_count() != before) {
        return "removal teardown sent commands to an absent card";
    }
    if (pico_mock_spi_deinit_count() != deinits_before + 1U) {
        return "removal teardown did not release SPI exactly once";
    }
    if (block_device_deinit(fx->device) != BLOCK_DEVICE_RESULT_OK) {
        return "repeated teardown was not idempotent";
    }
    if (pico_mock_spi_deinit_count() != deinits_before + 1U) {
        return "repeated teardown released SPI a second time";
    }
    return NULL;
}

/* ------------------------------------------------------------ utilities */

uint64_t sd_fx_byte_budget(uint64_t elapsed_us, uint64_t slack)
{
    const uint64_t per_byte_ns = sim_clock_spi_byte_ns();
    return ((elapsed_us * UINT64_C(1000)) / (per_byte_ns == 0U ? 1U : per_byte_ns))
        + slack;
}

sd_card_desc_t sd_fx_card_sdhc(void)
{
    /* 8 GB: a plain, in-range SDHC card. */
    return sd_card_desc(SD_CARD_SDHC, UINT64_C(8) * 1024U * 1024U * 1024U);
}

sd_card_desc_t sd_fx_card_sdxc(void)
{
    /* 64 GB: above the SDHC 32 GB boundary, still CSD v2. */
    return sd_card_desc(SD_CARD_SDXC, UINT64_C(64) * 1024U * 1024U * 1024U);
}

sd_card_desc_t sd_fx_card_v1_sdsc(void)
{
    /* 1 GB v1 card: CMD8 is illegal, CSD v1, byte addressing. */
    return sd_card_desc(SD_CARD_V1_SDSC, UINT64_C(1) * 1024U * 1024U * 1024U);
}

sd_card_desc_t sd_fx_card_v2_sdsc(void)
{
    /* 2 GB v2 standard-capacity card: CMD8 answers, CCS is 0.
     * A 2 GB CSD v1 register is only encodable with READ_BL_LEN 10 or 11:
     * with READ_BL_LEN 9 the largest C_SIZE/C_SIZE_MULT pair reaches 1 GiB.
     * Real 2 GB cards do exactly this, and it exercises the driver's
     * requirement to force a 512-byte block length with CMD16. */
    sd_card_desc_t desc =
        sd_card_desc(SD_CARD_V2_SDSC, UINT64_C(2) * 1024U * 1024U * 1024U);
    desc.read_bl_len = 10U;
    return desc;
}
