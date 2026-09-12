#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico_mock.h"
#include "sd_card_model.h"
#include "storage/sd_spi.h"

enum {
    PIN_CLOCK = 2,
    PIN_CONTROLLER_OUT = 3,
    PIN_CONTROLLER_IN = 4,
    PIN_CHIP_SELECT = 5,
    PIN_CARD_AVAILABLE = 6,
    OPERATIONAL_BAUDRATE = 12000000,
    SD_BLOCK_SIZE = 512,
    SDHC_C_SIZE = 4095,
    SDHC_BLOCK_COUNT = (SDHC_C_SIZE + 1) * 1024,
    SDSC_C_SIZE = 1023,
    SDSC_C_SIZE_MULT = 7,
    SDSC_BLOCK_COUNT = (SDSC_C_SIZE + 1) * (1 << (SDSC_C_SIZE_MULT + 2)),
    /* Busy budgets the driver declares, in microseconds. sd_spi_command()
     * waits for a ready card before every frame it sends; every other wait
     * (teardown, the wait after CMD12's R1b, the read-path R1-error cleanup)
     * goes through sd_spi_wait_ready(). Neither value comes from the
     * specification, which bounds operations rather than the pre-command
     * wait; they are the driver's own contract and are pinned from both sides
     * below so a change to one cannot be mistaken for the other. */
    DRIVER_COMMAND_READY_WAIT_US = 500000,
    DRIVER_READY_WAIT_US = 250000,
};

static int failures;
static int tests_run;
static spi_inst_t test_spi = { .id = 0U };

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                __FILE__, __LINE__, #condition);                               \
            failures++;                                                        \
            return;                                                            \
        }                                                                      \
    } while (false)

#define CHECK_EQ(expected, actual) CHECK((expected) == (actual))

/* Upper bound on bytes the modelled bus can carry in `elapsed_us`, plus slack
 * for the surrounding command frames. Under SIM_CLOCK_BUS_TIME this is a tight
 * bound on how much work a timeout may cost; under SIM_CLOCK_POLL_TICK bytes
 * are free, so the elapsed-time assertion beside it carries the weight. */
static uint64_t bus_byte_budget(uint64_t elapsed_us, uint64_t slack)
{
    const uint64_t per_byte_ns = sim_clock_spi_byte_ns();
    return ((elapsed_us * UINT64_C(1000)) / (per_byte_ns == 0U ? 1U : per_byte_ns))
        + slack;
}

static sd_spi_config_t valid_config(void)
{
    const sd_spi_config_t config = {
        .spi = &test_spi,
        .baud_rate_hz = OPERATIONAL_BAUDRATE,
        .pin_clock = PIN_CLOCK,
        .pin_controller_out = PIN_CONTROLLER_OUT,
        .pin_controller_in = PIN_CONTROLLER_IN,
        .pin_chip_select = PIN_CHIP_SELECT,
        .pin_card_available = PIN_CARD_AVAILABLE,
    };
    return config;
}

static void configure_csd_v2_fields(uint32_t c_size)
{
    uint8_t payload[19] = { 0 };
    payload[0] = 0xfeU;
    payload[1] = 0x40U;
    payload[8] = (uint8_t)((c_size >> 16U) & 0x3fU);
    payload[9] = (uint8_t)(c_size >> 8U);
    payload[10] = (uint8_t)c_size;
    /* The driver validates the CRC16 after the CSD register, so the scripted
     * card must send the real one (MSB first), from the model's independent
     * implementation. */
    const uint16_t csd_crc = sd_crc16_ccitt(&payload[1], 16U);
    payload[17] = (uint8_t)(csd_crc >> 8U);
    payload[18] = (uint8_t)(csd_crc & 0xFFU);
    pico_mock_sd_set_command(9U, 0x00U, payload, sizeof(payload));
}

static void configure_csd_v2(void)
{
    configure_csd_v2_fields(SDHC_C_SIZE);
}

static void configure_csd_v1_fields(
    uint32_t c_size,
    uint8_t c_size_mult,
    uint8_t read_bl_len)
{
    uint8_t payload[19] = { 0 };
    payload[0] = 0xfeU;
    payload[6] = read_bl_len & 0x0fU;
    payload[7] = (uint8_t)((c_size >> 10U) & 0x03U);
    payload[8] = (uint8_t)(c_size >> 2U);
    payload[9] = (uint8_t)((c_size & 0x03U) << 6U);
    payload[10] = (uint8_t)((c_size_mult >> 1U) & 0x03U);
    payload[11] = (uint8_t)((c_size_mult & 0x01U) << 7U);
    /* The driver validates the CRC16 after the CSD register, so the scripted
     * card must send the real one (MSB first), from the model's independent
     * implementation. */
    const uint16_t csd_crc = sd_crc16_ccitt(&payload[1], 16U);
    payload[17] = (uint8_t)(csd_crc >> 8U);
    payload[18] = (uint8_t)(csd_crc & 0xFFU);
    pico_mock_sd_set_command(9U, 0x00U, payload, sizeof(payload));
}

static void configure_csd_v1(void)
{
    configure_csd_v1_fields(SDSC_C_SIZE, SDSC_C_SIZE_MULT, 9U);
}

/* CMD59 (CRC_ON_OFF) is issued on every successful bring-up, so a scripted
 * card must answer it. An unscripted command answers a lone 0xFF, which never
 * clears the R1 busy bit, so the poll exhausts its budget and initialisation
 * fails for a reason unrelated to the case under test. */
static void configure_crc_on_off(void)
{
    pico_mock_sd_set_command(59U, 0x01U, NULL, 0U);
}

static void configure_successful_sdhc_card(void)
{
    static const uint8_t r7[] = { 0x00U, 0x00U, 0x01U, 0xaaU };
    static const uint8_t ocr[] = { 0xc0U, 0xffU, 0x80U, 0x00U };

    pico_mock_sd_set_command(0U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(8U, 0x01U, r7, sizeof(r7));
    configure_crc_on_off();
    pico_mock_sd_set_command(55U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(41U, 0x00U, NULL, 0U);
    pico_mock_sd_set_command(58U, 0x00U, ocr, sizeof(ocr));
    configure_csd_v2();
}

static void configure_successful_sdsc_card(void)
{
    static const uint8_t ocr[] = { 0x80U, 0x00U, 0x00U, 0x00U };

    pico_mock_sd_set_command(0U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(8U, 0x05U, NULL, 0U);
    configure_crc_on_off();
    pico_mock_sd_set_command(55U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(41U, 0x00U, NULL, 0U);
    pico_mock_sd_set_command(58U, 0x00U, ocr, sizeof(ocr));
    configure_csd_v1();
    pico_mock_sd_set_command(16U, 0x00U, NULL, 0U);
}

static block_device_t *initialize_sdhc(sd_spi_t *sd)
{
    const sd_spi_config_t config = valid_config();
    if (sd_spi_configure(sd, &config) != BLOCK_DEVICE_RESULT_OK) {
        (void)fprintf(stderr, "%s:%d: SDHC configure failed\n",
            __FILE__, __LINE__);
        failures++;
        return NULL;
    }
    configure_successful_sdhc_card();
    block_device_t *const device = sd_spi_as_block_device(sd);
    if (device == NULL
            || block_device_init(device) != BLOCK_DEVICE_RESULT_OK) {
        (void)fprintf(stderr, "%s:%d: SDHC initialization failed\n",
            __FILE__, __LINE__);
        failures++;
        return NULL;
    }
    return device;
}

static block_device_t *initialize_legacy(sd_spi_t *sd)
{
    const sd_spi_config_t config = valid_config();

    if (sd_spi_configure(sd, &config) != BLOCK_DEVICE_RESULT_OK) {
        failures++;
        return NULL;
    }
    configure_successful_sdsc_card();

    block_device_t *const device = sd_spi_as_block_device(sd);
    if (device == NULL
            || block_device_init(device) != BLOCK_DEVICE_RESULT_OK) {
        failures++;
        return NULL;
    }
    return device;
}

static uint8_t expected_data_byte(
    uint8_t seed,
    size_t block,
    size_t offset)
{
    return (uint8_t)(seed + (uint8_t)(block * 17U) + (uint8_t)offset);
}

static size_t make_read_payload(
    uint8_t *payload,
    size_t block_count,
    uint8_t seed)
{
    size_t position = 0U;
    for (size_t block = 0U; block < block_count; ++block) {
        payload[position++] = 0xfeU;
        const uint8_t *const data = &payload[position];
        for (size_t offset = 0U; offset < SD_BLOCK_SIZE; ++offset) {
            payload[position++] = expected_data_byte(seed, block, offset);
        }
        /* The driver validates the data CRC16 (MSB first) after every block,
         * so the scripted card must send the real one. sd_crc16_ccitt() is
         * the card model's implementation, independent of the production
         * helper the driver folds the payload through. */
        const uint16_t crc = sd_crc16_ccitt(data, SD_BLOCK_SIZE);
        payload[position++] = (uint8_t)(crc >> 8U);
        payload[position++] = (uint8_t)(crc & 0xFFU);
    }
    return position;
}

static bool buffer_matches(
    const uint8_t *buffer,
    size_t block_count,
    uint8_t seed)
{
    for (size_t block = 0U; block < block_count; ++block) {
        for (size_t offset = 0U; offset < SD_BLOCK_SIZE; ++offset) {
            if (buffer[(block * SD_BLOCK_SIZE) + offset]
                    != expected_data_byte(seed, block, offset)) {
                return false;
            }
        }
    }
    return true;
}

typedef struct {
    block_device_result_t result;
    size_t transfer_count;
    size_t cmd9_count;
    size_t cmd12_count;
    size_t cmd17_count;
    size_t cmd18_count;
    bool chip_select_released;
    bool initialized;
    bool card_type_legacy;
    bool card_type_hcxc;
    uint64_t block_count;
    bool spi_initialized;
    bool all_gpio_deinitialized;
} error_token_observation_t;

static error_token_observation_t observe_cmd17_error_token(uint8_t token)
{
    uint8_t block[SD_BLOCK_SIZE];
    error_token_observation_t observation = { 0 };

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    if (device == NULL
            || !pico_mock_sd_set_command(17U, 0x00U, &token, 1U)) {
        failures++;
        observation.result = BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
        return observation;
    }

    const size_t transfers_before = pico_mock_spi_transfer_count();
    observation.result = block_device_read_blocks(device, 0U, block, 1U);
    observation.transfer_count =
        pico_mock_spi_transfer_count() - transfers_before;
    observation.cmd17_count = pico_mock_sd_command_count(17U);
    observation.chip_select_released =
        pico_mock_gpio_level(PIN_CHIP_SELECT);
    return observation;
}

static error_token_observation_t observe_cmd18_error_token(uint8_t token)
{
    uint8_t blocks[2U * SD_BLOCK_SIZE];
    error_token_observation_t observation = { 0 };

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    if (device == NULL
            || !pico_mock_sd_set_command(18U, 0x00U, &token, 1U)
            || !pico_mock_sd_set_command(12U, 0x00U, NULL, 0U)) {
        failures++;
        observation.result = BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
        return observation;
    }

    const size_t transfers_before = pico_mock_spi_transfer_count();
    observation.result = block_device_read_blocks(device, 0U, blocks, 2U);
    observation.transfer_count =
        pico_mock_spi_transfer_count() - transfers_before;
    observation.cmd12_count = pico_mock_sd_command_count(12U);
    observation.cmd18_count = pico_mock_sd_command_count(18U);
    observation.chip_select_released =
        pico_mock_gpio_level(PIN_CHIP_SELECT);
    return observation;
}

static error_token_observation_t observe_cmd9_error_token(uint8_t token)
{
    error_token_observation_t observation = { 0 };
    const sd_spi_config_t config = valid_config();

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    if (sd_spi_configure(&sd, &config) != BLOCK_DEVICE_RESULT_OK) {
        failures++;
        observation.result = BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
        return observation;
    }
    configure_successful_sdhc_card();
    if (!pico_mock_sd_set_command(9U, 0x00U, &token, 1U)) {
        failures++;
        observation.result = BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
        return observation;
    }

    observation.result = block_device_init(sd_spi_as_block_device(&sd));
    observation.transfer_count = pico_mock_spi_transfer_count();
    observation.cmd9_count = pico_mock_sd_command_count(9U);
    observation.chip_select_released =
        pico_mock_gpio_level(PIN_CHIP_SELECT);
    observation.initialized = sd.initialized;
    observation.card_type_legacy = sd.card_type_legacy;
    observation.card_type_hcxc = sd.card_type_hcxc;
    observation.block_count = sd.block_count;
    observation.spi_initialized = pico_mock_spi_is_initialized();
    observation.all_gpio_deinitialized =
        pico_mock_gpio_was_deinitialized(PIN_CLOCK)
        && pico_mock_gpio_was_deinitialized(PIN_CONTROLLER_OUT)
        && pico_mock_gpio_was_deinitialized(PIN_CONTROLLER_IN)
        && pico_mock_gpio_was_deinitialized(PIN_CHIP_SELECT)
        && pico_mock_gpio_was_deinitialized(PIN_CARD_AVAILABLE);
    return observation;
}

static void verify_data_error_token(uint8_t token)
{
    const error_token_observation_t cmd17_baseline =
        observe_cmd17_error_token(0x01U);
    const error_token_observation_t cmd17 = token == 0x01U
        ? cmd17_baseline : observe_cmd17_error_token(token);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR, cmd17.result);
    CHECK_EQ(cmd17_baseline.transfer_count, cmd17.transfer_count);
    CHECK_EQ(1U, cmd17.cmd17_count);
    CHECK(cmd17.chip_select_released);

    const error_token_observation_t cmd18_baseline =
        observe_cmd18_error_token(0x01U);
    const error_token_observation_t cmd18 = token == 0x01U
        ? cmd18_baseline : observe_cmd18_error_token(token);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR, cmd18.result);
    CHECK_EQ(cmd18_baseline.transfer_count, cmd18.transfer_count);
    CHECK_EQ(1U, cmd18.cmd18_count);
    CHECK_EQ(1U, cmd18.cmd12_count);
    CHECK(cmd18.chip_select_released);

    const error_token_observation_t cmd9_baseline =
        observe_cmd9_error_token(0x01U);
    const error_token_observation_t cmd9 = token == 0x01U
        ? cmd9_baseline : observe_cmd9_error_token(token);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR, cmd9.result);
    CHECK_EQ(cmd9_baseline.transfer_count, cmd9.transfer_count);
    CHECK_EQ(1U, cmd9.cmd9_count);
    CHECK(cmd9.chip_select_released);
    CHECK(!cmd9.initialized);
    CHECK(!cmd9.card_type_legacy);
    CHECK(!cmd9.card_type_hcxc);
    CHECK_EQ(0U, cmd9.block_count);
    CHECK(!cmd9.spi_initialized);
    CHECK(cmd9.all_gpio_deinitialized);
}

static void test_data_error_token_01_is_immediate(void)
{
    verify_data_error_token(0x01U);
}

static void test_data_error_token_02_is_immediate(void)
{
    verify_data_error_token(0x02U);
}

static void test_data_error_token_04_is_immediate(void)
{
    verify_data_error_token(0x04U);
}

static void test_data_error_token_08_is_immediate(void)
{
    verify_data_error_token(0x08U);
}

static void test_zero_is_not_a_data_error_token(void)
{
    const error_token_observation_t cmd17_error =
        observe_cmd17_error_token(0x01U);
    const error_token_observation_t cmd17_zero =
        observe_cmd17_error_token(0x00U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR, cmd17_zero.result);
    CHECK(cmd17_zero.transfer_count > cmd17_error.transfer_count);
    CHECK(cmd17_zero.chip_select_released);

    const error_token_observation_t cmd18_error =
        observe_cmd18_error_token(0x01U);
    const error_token_observation_t cmd18_zero =
        observe_cmd18_error_token(0x00U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR, cmd18_zero.result);
    CHECK(cmd18_zero.transfer_count > cmd18_error.transfer_count);
    CHECK_EQ(1U, cmd18_zero.cmd12_count);
    CHECK(cmd18_zero.chip_select_released);

    const error_token_observation_t cmd9_error =
        observe_cmd9_error_token(0x01U);
    const error_token_observation_t cmd9_zero =
        observe_cmd9_error_token(0x00U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR, cmd9_zero.result);
    CHECK(cmd9_zero.transfer_count > cmd9_error.transfer_count);
    CHECK(!cmd9_zero.initialized);
    CHECK(!cmd9_zero.spi_initialized);
    CHECK(cmd9_zero.all_gpio_deinitialized);
}

static void test_mock_command_configuration_validation(void)
{
    static uint8_t oversized_payload[PICO_MOCK_MAX_COMMAND_PAYLOAD + 1U];

    pico_mock_reset();
    CHECK(!pico_mock_sd_set_command(UINT8_MAX, 0x00U, NULL, 0U));
    CHECK(!pico_mock_sd_set_command(17U, 0x00U, NULL, 1U));
    CHECK(!pico_mock_sd_set_command(18U, 0x00U, oversized_payload,
        sizeof(oversized_payload)));
    CHECK(pico_mock_sd_set_command(18U, 0x00U, oversized_payload,
        PICO_MOCK_MAX_COMMAND_PAYLOAD));
}

static void test_configure_validation(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    sd_spi_config_t config = valid_config();

    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        sd_spi_configure(NULL, &config));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        sd_spi_configure(&sd, NULL));
    config.spi = NULL;
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        sd_spi_configure(&sd, &config));
    config = valid_config();
    config.baud_rate_hz = 0U;
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        sd_spi_configure(&sd, &config));
    config.baud_rate_hz = 25000001U;
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        sd_spi_configure(&sd, &config));
}

static void test_configure_exposes_uninitialized_device(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    uint8_t block[512] = { 0 };
    block_device_info_t info = { 0 };

    CHECK(sd_spi_as_block_device(NULL) == NULL);
    CHECK(sd_spi_as_block_device(&sd) == NULL);
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    block_device_t *const device = sd_spi_as_block_device(&sd);
    CHECK(device != NULL);
    CHECK(block_device_is_valid(device));
    CHECK_EQ(BLOCK_DEVICE_RESULT_NOT_INITIALIZED,
        block_device_read_blocks(device, 0U, block, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_NOT_INITIALIZED,
        block_device_write_blocks(device, 0U, block, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_NOT_INITIALIZED,
        block_device_get_info(device, &info));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        block_device_read_blocks(device, 0U, NULL, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        block_device_write_blocks(device, 0U, block, 0U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        block_device_get_info(device, NULL));
}

static void test_reconfiguration_lifecycle(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    sd_spi_config_t config = valid_config();
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);

    config.baud_rate_hz = 8000000U;
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        sd_spi_configure(&sd, &config));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        block_device_init(device));
    CHECK(sd.initialized);

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_deinit(device));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    CHECK_EQ(8000000U, sd.config.baud_rate_hz);
}

static void test_sdhc_initialization(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    (void)initialize_sdhc(&sd);

    CHECK(sd.initialized);
    CHECK(!sd.card_type_legacy);
    CHECK(sd.card_type_hcxc);
    CHECK_EQ((uint64_t)SDHC_BLOCK_COUNT, sd.block_count);
    CHECK(pico_mock_spi_is_initialized());
    CHECK_EQ(400000U, pico_mock_spi_initial_baudrate());
    CHECK_EQ(OPERATIONAL_BAUDRATE, pico_mock_spi_baudrate());
    CHECK(pico_mock_gpio_was_initialized(PIN_CHIP_SELECT));
    CHECK(pico_mock_gpio_was_initialized(PIN_CARD_AVAILABLE));
    CHECK(pico_mock_gpio_was_pulled_up_at_init(PIN_CARD_AVAILABLE));
    /* Ten stable debounce samples plus the post-registration race check. */
    CHECK_EQ(11U, pico_mock_gpio_read_count(PIN_CARD_AVAILABLE));
    CHECK_EQ(GPIO_FUNC_SPI, pico_mock_gpio_function(PIN_CLOCK));
    CHECK_EQ(GPIO_FUNC_SPI, pico_mock_gpio_function(PIN_CONTROLLER_OUT));
    CHECK_EQ(GPIO_FUNC_SPI, pico_mock_gpio_function(PIN_CONTROLLER_IN));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
    CHECK_EQ(1U, pico_mock_sd_command_count(0U));
    CHECK_EQ(1U, pico_mock_sd_command_count(8U));
    CHECK_EQ(1U, pico_mock_sd_command_count(55U));
    CHECK_EQ(1U, pico_mock_sd_command_count(41U));
    CHECK_EQ(0x40000000U, pico_mock_sd_last_argument(41U));
    CHECK_EQ(1U, pico_mock_sd_command_count(58U));
    CHECK_EQ(1U, pico_mock_sd_command_count(9U));
    CHECK_EQ(0U, pico_mock_sd_command_count(16U));
}

static void test_legacy_card_initialization(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    static const uint8_t ocr[] = { 0x80U, 0x00U, 0x00U, 0x00U };

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    pico_mock_sd_set_command(0U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(8U, 0x05U, NULL, 0U);
    configure_crc_on_off();
    pico_mock_sd_set_command(55U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(41U, 0x00U, NULL, 0U);
    pico_mock_sd_set_command(58U, 0x00U, ocr, sizeof(ocr));
    configure_csd_v1();
    pico_mock_sd_set_command(16U, 0x00U, NULL, 0U);

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(sd.initialized);
    CHECK(sd.card_type_legacy);
    CHECK(!sd.card_type_hcxc);
    CHECK_EQ((uint64_t)SDSC_BLOCK_COUNT, sd.block_count);
    CHECK_EQ(0U, pico_mock_sd_last_argument(41U));
    CHECK_EQ(1U, pico_mock_sd_command_count(16U));
    CHECK_EQ(512U, pico_mock_sd_last_argument(16U));
}

static void test_csd_capacity_encoding_limits(void)
{
    const sd_spi_config_t config = valid_config();
    sd_spi_t sd = { 0 };

    pico_mock_reset();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdsc_card();
    configure_csd_v1_fields(0U, 0U, 9U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK_EQ(4ULL, sd.block_count);

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdsc_card();
    configure_csd_v1_fields(0x0fffU, 7U, 11U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK_EQ(8388608ULL, sd.block_count);

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    configure_csd_v2_fields(0U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK_EQ(1024ULL, sd.block_count);

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    configure_csd_v2_fields(0x3fffffU);
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK_EQ(4294967296ULL, sd.block_count);
}

static void test_missing_card_fails_initialization(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    pico_mock_gpio_set_input(PIN_CARD_AVAILABLE, true);
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!sd.initialized);
    CHECK_EQ(0U, pico_mock_sd_command_count(0U));
    CHECK(!pico_mock_spi_is_initialized());
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CARD_AVAILABLE));
    CHECK_EQ(30U, pico_mock_gpio_read_count(PIN_CARD_AVAILABLE));
}

static void test_card_detect_debounce(void)
{
    static const bool settles_present[] = {
        false, false, true, false, true,
        false, false, false, false, false,
        false, false, false, false, false,
    };
    static const bool never_stable[] = {
        false, false, false, false, false, false, false, false, false, true,
        false, false, false, false, false, false, false, false, false, true,
        false, false, false, false, false, false, false, false, false, true,
    };
    const sd_spi_config_t config = valid_config();
    sd_spi_t sd = { 0 };

    pico_mock_reset();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    CHECK(pico_mock_gpio_set_input_sequence(PIN_CARD_AVAILABLE,
        settles_present, sizeof(settles_present) / sizeof(settles_present[0])));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(sd.initialized);
    /* Fifteen debounce samples plus the post-registration race check. */
    CHECK_EQ(16U, pico_mock_gpio_read_count(PIN_CARD_AVAILABLE));
    CHECK(pico_mock_spi_is_initialized());

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    CHECK(pico_mock_gpio_set_input_sequence(PIN_CARD_AVAILABLE,
        never_stable, sizeof(never_stable) / sizeof(never_stable[0])));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!sd.initialized);
    CHECK_EQ(30U, pico_mock_gpio_read_count(PIN_CARD_AVAILABLE));
    CHECK(!pico_mock_spi_is_initialized());
    CHECK_EQ(0U, pico_mock_sd_command_count(0U));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CARD_AVAILABLE));
}

static void test_cmd59_rejection_fails_initialization(void)
{
    /* CMD59 is issued while the card is still idle, so R1 must read 0x01. A
     * card that refuses it answers illegal-command (0x04) - the realistic
     * response from a card that does not implement CRC_ON_OFF - and command
     * CRC checking stays off.
     *
     * Continuing past that would be worse than not sending CMD59 at all: the
     * driver would compute a correct CRC for every later frame while the card
     * ignored all of them, so a corrupted command would be accepted silently
     * and nothing in the bring-up would report anything wrong. Bring-up must
     * fail rather than proceed believing it is protected. */
    static const uint8_t refusals[] = { 0x04U, 0x05U, 0x08U, 0x40U };

    for (size_t i = 0U; i < sizeof(refusals) / sizeof(refusals[0]); ++i) {
        pico_mock_reset();
        sd_spi_t sd = { 0 };
        const sd_spi_config_t config = valid_config();
        CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
        configure_successful_sdhc_card();
        CHECK(pico_mock_sd_set_command(59U, refusals[i], NULL, 0U));

        CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
            block_device_init(sd_spi_as_block_device(&sd)));
        CHECK(!sd.initialized);
        /* The ACMD41 loop must never be entered on a refused CMD59. */
        CHECK_EQ(0U, pico_mock_sd_command_count(41U));
        /* Rollback still has to leave the bus and pins released. */
        CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
        CHECK(!pico_mock_spi_is_initialized());
    }
}

static void test_card_initialization_timeout_is_bounded(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    static const uint8_t r7[] = { 0x00U, 0x00U, 0x01U, 0xaaU };

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    pico_mock_sd_set_command(0U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(8U, 0x01U, r7, sizeof(r7));
    configure_crc_on_off();
    pico_mock_sd_set_command(55U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(41U, 0x01U, NULL, 0U);

    const uint64_t start_us = pico_mock_now_us();
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_init(sd_spi_as_block_device(&sd)));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    CHECK(!sd.initialized);
    CHECK(pico_mock_sd_command_count(41U) > 1U);
    /* The ACMD41 loop owns a 1200 ms budget. It must neither give up early
     * nor overrun by more than one CMD55+ACMD41 iteration. Asserting elapsed
     * time rather than a call count means this still measures the timeout if
     * the loop's polling structure changes. */
    /* The span measured here also contains the card-detect debounce sleeps
     * (up to 29 ms) and the 1 ms settle before the idle clocks, so the upper
     * bound allows for those on top of the loop's own budget. */
    CHECK(elapsed_us >= UINT64_C(1200000));
    CHECK(elapsed_us < UINT64_C(1200000) + UINT64_C(40000));
    CHECK_EQ(pico_mock_sd_command_count(55U), pico_mock_sd_command_count(41U));
    CHECK(pico_mock_spi_transfer_count() <= bus_byte_budget(elapsed_us, 64U));
}

static void test_deinit_waits_and_releases_resources(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);

    pico_mock_sd_set_busy_cycles(3U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_deinit(device));
    CHECK(!sd.initialized);
    CHECK(!sd.card_type_legacy);
    CHECK(!sd.card_type_hcxc);
    CHECK_EQ(0U, sd.block_count);
    CHECK(!pico_mock_spi_is_initialized());
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CLOCK));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CONTROLLER_OUT));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CONTROLLER_IN));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CHIP_SELECT));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CARD_AVAILABLE));
}

static void test_deinit_timeout_preserves_resources(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);

    pico_mock_sd_set_busy_forever();
    const uint64_t start_us = pico_mock_now_us();
    CHECK_EQ(BLOCK_DEVICE_RESULT_BUSY_TIMEOUT, block_device_deinit(device));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    CHECK(elapsed_us >= DRIVER_READY_WAIT_US);
    CHECK(elapsed_us < 2U * DRIVER_READY_WAIT_US);
    CHECK(sd.initialized);
    CHECK(pico_mock_spi_is_initialized());
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
    CHECK(!pico_mock_gpio_was_deinitialized(PIN_CLOCK));
    CHECK(!pico_mock_gpio_was_deinitialized(PIN_CHIP_SELECT));
}

static void test_deinit_before_init_does_not_transfer(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_deinit(sd_spi_as_block_device(&sd)));
    CHECK_EQ(0U, pico_mock_spi_transfer_count());
    CHECK(!pico_mock_spi_is_initialized());
}

static void test_block_device_wrapper_validation(void)
{
    uint8_t block[SD_BLOCK_SIZE] = { 0 };
    block_device_info_t info = { 0 };

    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT, block_device_init(NULL));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT, block_device_deinit(NULL));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        block_device_read_blocks(NULL, 0U, block, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        block_device_write_blocks(NULL, 0U, block, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        block_device_get_info(NULL, &info));

    const block_device_t empty = { 0 };
    CHECK(!block_device_is_valid(&empty));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        block_device_init(&empty));
}

static void test_backend_context_validation(void)
{
    uint8_t block[SD_BLOCK_SIZE] = { 0 };
    block_device_info_t info = { 0 };
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    const block_device_operations_t *const operations =
        sd.block_device.operations;
    sd.configured = false;

    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        operations->init(&sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        operations->deinit(&sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        operations->read_blocks(&sd, 0U, block, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        operations->write_blocks(&sd, 0U, block, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        operations->get_info(&sd, &info));
}

static void test_initialization_response_validation(void)
{
    sd_spi_config_t config = valid_config();
    sd_spi_t sd = { 0 };
    static const uint8_t bad_r7[] = { 0x00U, 0x00U, 0x00U, 0x00U };
    static const uint8_t good_r7[] = { 0x00U, 0x00U, 0x01U, 0xaaU };
    static const uint8_t unpowered_ocr[] = { 0x40U, 0x00U, 0x00U, 0x00U };
    static const uint8_t legacy_ocr[] = { 0x80U, 0x00U, 0x00U, 0x00U };

    pico_mock_reset();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!sd.initialized);
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
    CHECK(!pico_mock_spi_is_initialized());
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CLOCK));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CHIP_SELECT));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CARD_AVAILABLE));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    pico_mock_sd_set_command(0U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(8U, 0x01U, bad_r7, sizeof(bad_r7));
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!sd.initialized);
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    pico_mock_sd_set_command(0U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(8U, 0x01U, good_r7, sizeof(good_r7));
    configure_crc_on_off();
    pico_mock_sd_set_command(55U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(41U, 0x00U, NULL, 0U);
    pico_mock_sd_set_command(58U, 0x00U,
        unpowered_ocr, sizeof(unpowered_ocr));
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!sd.initialized);

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    pico_mock_sd_set_command(0U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(8U, 0x05U, NULL, 0U);
    configure_crc_on_off();
    pico_mock_sd_set_command(55U, 0x01U, NULL, 0U);
    pico_mock_sd_set_command(41U, 0x00U, NULL, 0U);
    pico_mock_sd_set_command(58U, 0x00U, legacy_ocr, sizeof(legacy_ocr));
    configure_csd_v1();
    pico_mock_sd_set_command(16U, 0x04U, NULL, 0U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!sd.initialized);
}

static void test_unsupported_csd_rolls_back_initialization(void)
{
    uint8_t payload[19] = { 0 };
    payload[0] = 0xfeU;
    payload[1] = 0x80U;

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    /* The driver validates the CRC16 after the CSD register, so the scripted
     * card must send the real one (MSB first), from the model's independent
     * implementation. */
    const uint16_t csd_crc = sd_crc16_ccitt(&payload[1], 16U);
    payload[17] = (uint8_t)(csd_crc >> 8U);
    payload[18] = (uint8_t)(csd_crc & 0xFFU);
    pico_mock_sd_set_command(9U, 0x00U, payload, sizeof(payload));

    CHECK_EQ(BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!sd.initialized);
    CHECK_EQ(0U, sd.block_count);
    CHECK(!pico_mock_spi_is_initialized());
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CLOCK));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CARD_AVAILABLE));
}

static void test_cmd9_response_and_data_token_failures(void)
{
    const sd_spi_config_t config = valid_config();
    static const uint8_t error_token[] = { 0x0bU };
    sd_spi_t sd = { 0 };

    pico_mock_reset();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    pico_mock_sd_set_command(9U, 0x04U, NULL, 0U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK_EQ(0U, sd.block_count);
    CHECK(!pico_mock_spi_is_initialized());

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    pico_mock_sd_set_command(9U, 0x00U, NULL, 0U);
    CHECK(pico_mock_sd_set_stall_after_r1(9U, true));
    {
        const uint64_t start_us = pico_mock_now_us();
        CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
            block_device_init(sd_spi_as_block_device(&sd)));
        const uint64_t elapsed_us = pico_mock_now_us() - start_us;
        CHECK_EQ(0U, sd.block_count);
        CHECK_EQ(1U, pico_mock_sd_command_count(9U));
        /* CMD9's data-token wait owns a 100 ms budget and must not retry. */
        CHECK(elapsed_us >= UINT64_C(100000));
        CHECK(elapsed_us < UINT64_C(100000) + UINT64_C(40000));
        CHECK(pico_mock_spi_transfer_count()
            <= bus_byte_budget(elapsed_us, 128U));
    }

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    pico_mock_sd_set_command(9U, 0x00U,
        error_token, sizeof(error_token));
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK_EQ(0U, sd.block_count);
    CHECK(!pico_mock_spi_is_initialized());
}

static void test_capacity_state_cleared_across_reinitialization(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    CHECK_EQ((uint64_t)SDHC_BLOCK_COUNT, sd.block_count);

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_deinit(device));
    CHECK_EQ(0U, sd.block_count);

    configure_successful_sdhc_card();
    pico_mock_sd_set_command(9U, 0x04U, NULL, 0U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR, block_device_init(device));
    CHECK(!sd.initialized);
    CHECK_EQ(0U, sd.block_count);
}

static void test_single_block_read_sdhc(void)
{
    enum { PAYLOAD_SIZE = SD_BLOCK_SIZE + 3 };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t storage[SD_BLOCK_SIZE + 2U];
    const uint8_t seed = 0x21U;

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    CHECK_EQ(PAYLOAD_SIZE, make_read_payload(payload, 1U, seed));
    pico_mock_sd_set_command(17U, 0x00U, payload, sizeof(payload));
    memset(storage, 0xa5, sizeof(storage));

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_read_blocks(device, 123U, &storage[1], 1U));
    CHECK(buffer_matches(&storage[1], 1U, seed));
    CHECK_EQ(0xa5U, storage[0]);
    CHECK_EQ(0xa5U, storage[SD_BLOCK_SIZE + 1U]);
    CHECK_EQ(1U, pico_mock_sd_command_count(17U));
    CHECK_EQ(123U, pico_mock_sd_last_argument(17U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
    CHECK_EQ(0U, pico_mock_sd_pending_response_count());
}

static void test_single_block_read_sdsc_uses_byte_address(void)
{
    enum { PAYLOAD_SIZE = SD_BLOCK_SIZE + 3 };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t block[SD_BLOCK_SIZE];

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_legacy(&sd);
    CHECK(device != NULL);
    (void)make_read_payload(payload, 1U, 0x42U);
    pico_mock_sd_set_command(17U, 0x00U, payload, sizeof(payload));

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_read_blocks(device, 7U, block, 1U));
    CHECK(buffer_matches(block, 1U, 0x42U));
    CHECK_EQ(7U * SD_BLOCK_SIZE, pico_mock_sd_last_argument(17U));
}

static void test_single_block_read_failures(void)
{
    uint8_t block[SD_BLOCK_SIZE];
    static const uint8_t error_token[] = { 0x0bU };

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    pico_mock_sd_set_command(17U, 0x04U, NULL, 0U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, block, 1U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    pico_mock_sd_set_command(17U, 0x00U,
        error_token, sizeof(error_token));
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, block, 1U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    pico_mock_sd_set_command(17U, 0x00U, NULL, 0U);
    CHECK(pico_mock_sd_set_stall_after_r1(17U, true));
    const size_t transfers_before = pico_mock_spi_transfer_count();
    const uint64_t start_us = pico_mock_now_us();
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, block, 1U));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    /* A card that answers R1 and then never produces a data token must cost
     * exactly one 100 ms wait and one CMD17, not a retry loop. */
    CHECK(elapsed_us >= UINT64_C(100000));
    CHECK(elapsed_us < UINT64_C(110000));
    CHECK_EQ(1U, pico_mock_sd_command_count(17U));
    CHECK(pico_mock_spi_transfer_count() - transfers_before
        <= bus_byte_budget(elapsed_us, 128U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_multiple_block_read(void)
{
    enum {
        BLOCK_COUNT = 4,
        PAYLOAD_SIZE = BLOCK_COUNT * (SD_BLOCK_SIZE + 3),
    };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t storage[(BLOCK_COUNT * SD_BLOCK_SIZE) + 2U];
    const uint8_t seed = 0x33U;

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    CHECK_EQ(PAYLOAD_SIZE,
        make_read_payload(payload, BLOCK_COUNT, seed));
    CHECK(pico_mock_sd_set_command(
        18U, 0x00U, payload, sizeof(payload)));
    CHECK(pico_mock_sd_set_command(12U, 0x00U, NULL, 0U));
    memset(storage, 0x5a, sizeof(storage));
    const size_t transfers_before = pico_mock_spi_transfer_count();

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_read_blocks(device, 456U, &storage[1], BLOCK_COUNT));
    CHECK(buffer_matches(&storage[1], BLOCK_COUNT, seed));
    CHECK_EQ(0x5aU, storage[0]);
    CHECK_EQ(0x5aU, storage[(BLOCK_COUNT * SD_BLOCK_SIZE) + 1U]);
    CHECK_EQ(1U, pico_mock_sd_command_count(18U));
    CHECK_EQ(456U, pico_mock_sd_last_argument(18U));
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK_EQ(0U, pico_mock_sd_last_argument(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
    CHECK_EQ(0U, pico_mock_sd_pending_response_count());

    const size_t stop_offset = transfers_before + 8U
        + (BLOCK_COUNT * (SD_BLOCK_SIZE + 3U));
    const uint8_t *const tx_log = pico_mock_spi_tx_log();
    CHECK_EQ(12U | 0x40U, tx_log[stop_offset]);
    CHECK_EQ(0U, tx_log[stop_offset + 1U]);
    CHECK_EQ(0U, tx_log[stop_offset + 2U]);
    CHECK_EQ(0U, tx_log[stop_offset + 3U]);
    CHECK_EQ(0U, tx_log[stop_offset + 4U]);
    /* CRC7 over 4C 00 00 00 00 is 0x30, so the frame byte is (0x30 << 1) | 1.
     * This was 0x01 - the placeholder - until SD-006 was closed. Recomputed
     * from the polynomial, not copied from the driver. */
    CHECK_EQ(0x61U, tx_log[stop_offset + 5U]);
    CHECK_EQ(2078U,
        pico_mock_spi_transfer_count() - transfers_before);
}

static void test_multiple_block_timeout_is_not_success(void)
{
    enum {
        WAIT_BYTES = 101,
        PAYLOAD_SIZE = WAIT_BYTES + SD_BLOCK_SIZE + 3,
    };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t blocks[2U * SD_BLOCK_SIZE];

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    memset(payload, 0xff, WAIT_BYTES);
    CHECK_EQ(SD_BLOCK_SIZE + 3,
        make_read_payload(&payload[WAIT_BYTES], 1U, 0x55U));
    pico_mock_sd_set_command(18U, 0x00U, payload, sizeof(payload));
    pico_mock_sd_set_command(12U, 0x00U, NULL, 0U);

    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, blocks, 2U));
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_multiple_block_error_cleanup(void)
{
    uint8_t blocks[2U * SD_BLOCK_SIZE];
    static const uint8_t error_token[] = { 0x0dU };

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    pico_mock_sd_set_command(18U, 0x00U,
        error_token, sizeof(error_token));
    pico_mock_sd_set_command(12U, 0x00U, NULL, 0U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, blocks, 2U));
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    pico_mock_sd_set_command(18U, 0x00U,
        error_token, sizeof(error_token));
    pico_mock_sd_set_command(12U, 0x04U, NULL, 0U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, blocks, 2U));
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    pico_mock_sd_set_command(18U, 0x04U, NULL, 0U);
    pico_mock_sd_set_command(12U, 0x00U, NULL, 0U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, blocks, 2U));
    CHECK_EQ(0U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_multiple_block_error_recovery(void)
{
    enum { PAYLOAD_SIZE = SD_BLOCK_SIZE + 3 };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t failed_blocks[2U * SD_BLOCK_SIZE];
    uint8_t recovered_block[SD_BLOCK_SIZE];
    static const uint8_t error_token[] = { 0x08U };
    const uint8_t seed = 0x91U;

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    CHECK(pico_mock_sd_set_command(18U, 0x00U,
        error_token, sizeof(error_token)));
    CHECK(pico_mock_sd_set_command(12U, 0x00U, NULL, 0U));

    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, failed_blocks, 2U));
    CHECK_EQ(1U, pico_mock_sd_command_count(18U));
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));

    CHECK_EQ(PAYLOAD_SIZE, make_read_payload(payload, 1U, seed));
    CHECK(pico_mock_sd_set_command(
        17U, 0x00U, payload, sizeof(payload)));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_read_blocks(device, 2U, recovered_block, 1U));
    CHECK(buffer_matches(recovered_block, 1U, seed));
    CHECK_EQ(1U, pico_mock_sd_command_count(17U));
    CHECK_EQ(1U, pico_mock_sd_command_count(18U));
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
    CHECK_EQ(0U, pico_mock_sd_pending_response_count());
}

static void test_multiple_block_stop_response_timeout_is_bounded(void)
{
    enum { PAYLOAD_SIZE = 2 * (SD_BLOCK_SIZE + 3) };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t blocks[2U * SD_BLOCK_SIZE];

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    (void)make_read_payload(payload, 2U, 0x67U);
    pico_mock_sd_set_command(18U, 0x00U, payload, sizeof(payload));
    const size_t transfers_before = pico_mock_spi_transfer_count();

    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, blocks, 2U));
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_spi_transfer_count() - transfers_before < 1200U);
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_multiple_block_stop_waits_until_ready(void)
{
    enum { PAYLOAD_SIZE = 2 * (SD_BLOCK_SIZE + 3) };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t blocks[2U * SD_BLOCK_SIZE];
    static const uint8_t stop_busy[] = { 0x00U, 0x00U, 0xffU };

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    (void)make_read_payload(payload, 2U, 0x61U);
    pico_mock_sd_set_command(18U, 0x00U, payload, sizeof(payload));
    pico_mock_sd_set_command(12U, 0x00U,
        stop_busy, sizeof(stop_busy));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_read_blocks(device, 0U, blocks, 2U));
    CHECK_EQ(0U, pico_mock_sd_pending_response_count());
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
}

static void test_multiple_block_stop_failure_only_sent_once(void)
{
    enum { PAYLOAD_SIZE = 2 * (SD_BLOCK_SIZE + 3) };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t blocks[2U * SD_BLOCK_SIZE];

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    (void)make_read_payload(payload, 2U, 0x61U);
    pico_mock_sd_set_command(18U, 0x00U, payload, sizeof(payload));
    pico_mock_sd_set_command(12U, 0x04U, NULL, 0U);
    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(device, 0U, blocks, 2U));
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_read_range_uses_csd_capacity(void)
{
    enum { PAYLOAD_SIZE = SD_BLOCK_SIZE + 3 };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t block[SD_BLOCK_SIZE];
    (void)make_read_payload(payload, 1U, 0x72U);

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    pico_mock_sd_set_command(17U, 0x00U, payload, sizeof(payload));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_read_blocks(device, sd.block_count - 1U, block, 1U));
    CHECK_EQ((uint32_t)(sd.block_count - 1U),
        pico_mock_sd_last_argument(17U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OUT_OF_RANGE,
        block_device_read_blocks(device, sd.block_count - 1U, block, 2U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OUT_OF_RANGE,
        block_device_read_blocks(device, sd.block_count, block, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OUT_OF_RANGE,
        block_device_read_blocks(device, (uint64_t)UINT32_MAX + 1U,
            block, 1U));
    CHECK_EQ(1U, pico_mock_sd_command_count(17U));
    CHECK_EQ(0U, pico_mock_sd_command_count(18U));

    pico_mock_reset();
    memset(&sd, 0, sizeof(sd));
    device = initialize_legacy(&sd);
    CHECK(device != NULL);
    pico_mock_sd_set_command(17U, 0x00U, payload, sizeof(payload));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OUT_OF_RANGE,
        block_device_read_blocks(device,
            ((uint64_t)UINT32_MAX / SD_BLOCK_SIZE) + 1U, block, 1U));
    CHECK_EQ(0U, pico_mock_sd_command_count(17U));
}

static void test_write_blocks_is_implemented(void)
{
    /* This case used to pin write_blocks as a NOT_IMPLEMENTED stub that never
     * reached the bus. The write path exists now (its protocol behaviour is
     * covered in test_sd_writes.c); here the card model answers the one
     * command bring-up did not script, and the block must land. */
    uint8_t block[SD_BLOCK_SIZE];
    for (size_t i = 0U; i < sizeof(block); ++i) {
        block[i] = (uint8_t)(0x5AU ^ i);
    }

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    sd_card_set_response_policy(SD_RESPONSE_MODELLED);
    const size_t transfers_before = pico_mock_spi_transfer_count();

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(device, 9U, block, 1U));
    CHECK(pico_mock_spi_transfer_count() > transfers_before);
    CHECK_EQ(1U, pico_mock_sd_command_count(24U));
    CHECK_EQ(9U, pico_mock_sd_last_argument(24U));

    uint8_t stored[SD_BLOCK_SIZE];
    CHECK(sd_card_get_block(9U, stored));
    CHECK(memcmp(stored, block, sizeof(block)) == 0);
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

/*
 * get_info reports the geometry cached during initialization, so it must
 * answer from the decoded CSD without touching the bus, and the capacity it
 * advertises must agree with the range read_blocks actually enforces.
 */
static void test_get_info_reports_card_geometry(void)
{
    uint8_t block[SD_BLOCK_SIZE];

    pico_mock_reset();
    sd_spi_t sdhc = { 0 };
    block_device_t *device = initialize_sdhc(&sdhc);
    CHECK(device != NULL);

    block_device_info_t info = {
        .block_size_bytes = 0xaaaaU,
        .block_count = 0xbbbbU,
        .writable = false,
    };
    size_t transfers_before = pico_mock_spi_transfer_count();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_get_info(device, &info));
    CHECK_EQ(transfers_before, pico_mock_spi_transfer_count());
    CHECK_EQ((uint64_t)SDHC_BLOCK_COUNT, info.block_count);
    CHECK_EQ((uint32_t)SD_BLOCK_SIZE, info.block_size_bytes);
    /* Pins today's behaviour: the driver hardcodes writable = true; no
     * write-protect state is ever read. */
    CHECK(info.writable);

    /* One block past the advertised end must be rejected, and rejected before
     * any bus traffic. If this ever disagrees with block_count, a caller could
     * derive a valid-looking LBA that the driver then refuses. */
    transfers_before = pico_mock_spi_transfer_count();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OUT_OF_RANGE,
        block_device_read_blocks(device, info.block_count, block, 1U));
    CHECK_EQ(transfers_before, pico_mock_spi_transfer_count());

    /* A byte-addressed CSD v1 card must report its own geometry, derived from
     * C_SIZE/C_SIZE_MULT/READ_BL_LEN rather than the CSD v2 formula. */
    pico_mock_reset();
    sd_spi_t sdsc = { 0 };
    device = initialize_legacy(&sdsc);
    CHECK(device != NULL);

    memset(&info, 0, sizeof(info));
    transfers_before = pico_mock_spi_transfer_count();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_get_info(device, &info));
    CHECK_EQ(transfers_before, pico_mock_spi_transfer_count());
    CHECK_EQ((uint64_t)SDSC_BLOCK_COUNT, info.block_count);
    CHECK_EQ((uint32_t)SD_BLOCK_SIZE, info.block_size_bytes);
    CHECK(info.writable);
}

/*
 * Before initialization there is no geometry to report. The getter must say so
 * and leave the caller's struct untouched, so a caller that ignores the result
 * cannot go on to read a plausible-looking zero capacity.
 */
static void test_get_info_before_initialization_leaves_output_untouched(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));

    block_device_info_t info;
    block_device_info_t untouched;
    memset(&info, 0xEEU, sizeof(info));
    memset(&untouched, 0xEEU, sizeof(untouched));

    const size_t transfers_before = pico_mock_spi_transfer_count();
    CHECK_EQ(BLOCK_DEVICE_RESULT_NOT_INITIALIZED,
        block_device_get_info(sd_spi_as_block_device(&sd), &info));
    CHECK_EQ(transfers_before, pico_mock_spi_transfer_count());
    CHECK(memcmp(&info, &untouched, sizeof(info)) == 0);
}

static void test_initialization_reports_busy_timeout(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    pico_mock_sd_set_busy_forever();

    const uint64_t start_us = pico_mock_now_us();
    CHECK_EQ(BLOCK_DEVICE_RESULT_BUSY_TIMEOUT,
        block_device_init(sd_spi_as_block_device(&sd)));
    /* The ready wait ahead of CMD0 is sd_spi_command()'s pre-frame wait. */
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    CHECK(elapsed_us >= DRIVER_COMMAND_READY_WAIT_US);
    CHECK(elapsed_us < 2U * DRIVER_COMMAND_READY_WAIT_US);
    CHECK(!sd.initialized);
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_read_command_reports_busy_timeout(void)
{
    uint8_t block[SD_BLOCK_SIZE] = { 0 };

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    pico_mock_sd_set_busy_forever();

    const uint64_t start_us = pico_mock_now_us();
    CHECK_EQ(BLOCK_DEVICE_RESULT_BUSY_TIMEOUT,
        block_device_read_blocks(device, 0U, block, 1U));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    CHECK(elapsed_us >= DRIVER_COMMAND_READY_WAIT_US);
    CHECK(elapsed_us < 2U * DRIVER_COMMAND_READY_WAIT_US);
    /* A busy card must block the command, not have it sent anyway. */
    CHECK_EQ(0U, pico_mock_sd_command_count(17U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_stop_transmission_reports_busy_timeout(void)
{
    enum { PAYLOAD_SIZE = 2 * (SD_BLOCK_SIZE + 3) };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t blocks[2U * SD_BLOCK_SIZE];

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    (void)make_read_payload(payload, 2U, 0x81U);
    pico_mock_sd_set_command(18U, 0x00U, payload, sizeof(payload));
    pico_mock_sd_set_command(12U, 0x00U, NULL, 0U);
    /* CMD12 answers R1 and then holds the line busy: R1b, never released. */
    CHECK(pico_mock_sd_set_busy_after_payload(12U, true));

    const uint64_t start_us = pico_mock_now_us();
    CHECK_EQ(BLOCK_DEVICE_RESULT_BUSY_TIMEOUT,
        block_device_read_blocks(device, 0U, blocks, 2U));
    /* The wait after CMD12's R1b is the generic ready wait, not the longer
     * pre-command one. */
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    CHECK(elapsed_us >= DRIVER_READY_WAIT_US);
    CHECK(elapsed_us < 2U * DRIVER_READY_WAIT_US);
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_error_cleanup_reports_busy_timeout(void)
{
    uint8_t blocks[2U * SD_BLOCK_SIZE];
    static const uint8_t error_token[] = { 0x08U };

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    CHECK(pico_mock_sd_set_command(18U, 0x00U,
        error_token, sizeof(error_token)));
    CHECK(pico_mock_sd_set_command(12U, 0x00U, NULL, 0U));
    CHECK(pico_mock_sd_set_busy_after_payload(12U, true));

    const uint64_t start_us = pico_mock_now_us();
    CHECK_EQ(BLOCK_DEVICE_RESULT_BUSY_TIMEOUT,
        block_device_read_blocks(device, 0U, blocks, 2U));
    /* The wait after CMD12's R1b is the generic ready wait, not the longer
     * pre-command one. */
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    CHECK(elapsed_us >= DRIVER_READY_WAIT_US);
    CHECK(elapsed_us < 2U * DRIVER_READY_WAIT_US);
    CHECK_EQ(1U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_card_detect_irq_registration_and_latch(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);

    CHECK(pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
    CHECK(pico_mock_gpio_irq_is_enabled(PIN_CARD_AVAILABLE));
    CHECK_EQ(GPIO_IRQ_EDGE_RISE,
        pico_mock_gpio_irq_events(PIN_CARD_AVAILABLE));
    CHECK_EQ(1U,
        pico_mock_gpio_irq_registration_count(PIN_CARD_AVAILABLE));
    CHECK(!atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));

    CHECK(!pico_mock_gpio_irq_fire(
        PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_FALL));
    CHECK(!atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));

    CHECK(pico_mock_gpio_irq_fire(
        PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE));
    CHECK(atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));
    CHECK(!pico_mock_gpio_irq_is_enabled(PIN_CARD_AVAILABLE));
    CHECK(!pico_mock_gpio_irq_fire(
        PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE));
}

static void test_card_detect_irq_registration_failure(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    pico_mock_gpio_irq_reject_next_registration();

    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CARD_AVAILABLE));
    CHECK(!pico_mock_spi_is_initialized());
}

static void test_card_detect_post_registration_race(void)
{
    static const bool card_detect_sequence[] = {
        false, false, false, false, false,
        false, false, false, false, false,
        true,
    };

    pico_mock_reset();
    CHECK(pico_mock_gpio_set_input_sequence(
        PIN_CARD_AVAILABLE,
        card_detect_sequence,
        sizeof(card_detect_sequence) / sizeof(card_detect_sequence[0])));

    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));

    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));
    CHECK(!pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
    CHECK_EQ(1U,
        pico_mock_gpio_irq_unregistration_count(PIN_CARD_AVAILABLE));
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CARD_AVAILABLE));
    CHECK(!pico_mock_spi_is_initialized());
}

static void test_card_detect_post_registration_bounce_is_latched(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    pico_mock_gpio_irq_fire_on_next_registration(GPIO_IRQ_EDGE_RISE);

    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));
    CHECK(!pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
    CHECK_EQ(1U,
        pico_mock_gpio_irq_unregistration_count(PIN_CARD_AVAILABLE));
    CHECK(!pico_mock_gpio_level(PIN_CARD_AVAILABLE));
    CHECK(!pico_mock_spi_is_initialized());
}

static void test_initialization_rollback_unregisters_card_irq(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    CHECK(pico_mock_sd_set_command(0U, 0x02U, NULL, 0U));

    CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
    CHECK_EQ(1U,
        pico_mock_gpio_irq_unregistration_count(PIN_CARD_AVAILABLE));
}

static void test_deinit_unregisters_card_irq(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    CHECK(pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));

    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_deinit(device));
    CHECK(!pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
    CHECK_EQ(1U,
        pico_mock_gpio_irq_unregistration_count(PIN_CARD_AVAILABLE));
}

static void test_latched_removal_rejects_new_operations(void)
{
    uint8_t block[SD_BLOCK_SIZE];
    block_device_info_t info;

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    CHECK(pico_mock_gpio_irq_fire(
        PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE));

    const size_t transfers_before = pico_mock_spi_transfer_count();
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_read_blocks(device, 0U, block, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_write_blocks(device, 0U, block, 1U));
    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_get_info(device, &info));
    CHECK_EQ(transfers_before, pico_mock_spi_transfer_count());
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_removal_interrupts_single_block_read(void)
{
    uint8_t payload[1U + SD_BLOCK_SIZE + 2U];
    uint8_t block[SD_BLOCK_SIZE];

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    const size_t payload_size = make_read_payload(payload, 1U, 0x2aU);
    CHECK(pico_mock_sd_set_command(
        17U, 0x00U, payload, payload_size));

    const size_t transfers_before = pico_mock_spi_transfer_count();
    pico_mock_gpio_irq_fire_after_spi_transfers(
        10U, PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE);

    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_read_blocks(device, 0U, block, 1U));
    CHECK(atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));
    CHECK(pico_mock_spi_transfer_count() - transfers_before < 64U);
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
    CHECK_EQ(0U, pico_mock_sd_command_count(12U));
}

static void test_removal_interrupts_multiple_block_read_without_cmd12(void)
{
    uint8_t payload[2U * (1U + SD_BLOCK_SIZE + 2U)];
    uint8_t blocks[2U * SD_BLOCK_SIZE];

    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    const size_t payload_size = make_read_payload(payload, 2U, 0x51U);
    CHECK(pico_mock_sd_set_command(
        18U, 0x00U, payload, payload_size));
    CHECK(pico_mock_sd_set_command(12U, 0x00U, NULL, 0U));

    const size_t transfers_before = pico_mock_spi_transfer_count();
    pico_mock_gpio_irq_fire_after_spi_transfers(
        10U, PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE);

    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_read_blocks(device, 0U, blocks, 2U));
    CHECK(atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));
    CHECK(pico_mock_spi_transfer_count() - transfers_before < 64U);
    CHECK_EQ(1U, pico_mock_sd_command_count(18U));
    CHECK_EQ(0U, pico_mock_sd_command_count(12U));
    CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
}

static void test_removal_during_initialization_rolls_back(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    const sd_spi_config_t config = valid_config();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    pico_mock_gpio_irq_fire_after_spi_transfers(
        1U, PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE);

    CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));
    CHECK(!pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
    CHECK(!pico_mock_spi_is_initialized());
    CHECK(pico_mock_gpio_was_deinitialized(PIN_CARD_AVAILABLE));
}

static void test_removal_cleanup_allows_fresh_initialization(void)
{
    pico_mock_reset();
    sd_spi_t sd = { 0 };
    block_device_t *const device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    CHECK(pico_mock_gpio_irq_fire(
        PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE));

    const size_t transfers_before = pico_mock_spi_transfer_count();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_deinit(device));
    CHECK_EQ(transfers_before, pico_mock_spi_transfer_count());
    CHECK(!pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
    CHECK(atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));

    configure_successful_sdhc_card();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_init(device));
    CHECK(pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
    CHECK(pico_mock_gpio_irq_is_enabled(PIN_CARD_AVAILABLE));
    CHECK(!atomic_load_explicit(
        &sd.removal_latched, memory_order_relaxed));
}

static void test_modern_sdsc_and_retry_success(void)
{
    pico_mock_reset();
    pico_mock_sd_use_chip_select(PIN_CHIP_SELECT);
    sd_spi_t sd = {0};
    sd_spi_config_t config = valid_config();
    config.baud_rate_hz = 25000000U;
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdsc_card();
    const uint8_t r7[] = {0, 0, 1, 0xaa};
    CHECK(pico_mock_sd_set_command(8, 1, r7, sizeof(r7)));
    CHECK(pico_mock_sd_set_idle_responses(41, 3));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_init(sd_spi_as_block_device(&sd)));
    CHECK(!sd.card_type_legacy && !sd.card_type_hcxc);
    CHECK_EQ(4U, pico_mock_sd_command_count(41));
    CHECK_EQ(4U, pico_mock_sd_command_count(55));
    CHECK_EQ(0x40000000U, pico_mock_sd_last_argument(41));
    CHECK_EQ(512U, pico_mock_sd_last_argument(16));
    CHECK_EQ(25000000U, pico_mock_spi_baudrate());

    uint8_t payload[2 * (SD_BLOCK_SIZE + 3)];
    uint8_t blocks[2 * SD_BLOCK_SIZE + 2];
    memset(blocks, 0x5a, sizeof(blocks));
    make_read_payload(payload, 2, 0x97);
    CHECK(pico_mock_sd_set_command(18, 0, payload, sizeof(payload)));
    CHECK(pico_mock_sd_set_command(12, 0, NULL, 0));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK,
        block_device_read_blocks(sd_spi_as_block_device(&sd), 0x12345, blocks + 1, 2));
    CHECK_EQ(0x02468a00U, pico_mock_sd_last_argument(18));
    CHECK(buffer_matches(blocks + 1, 2, 0x97));
    CHECK_EQ(0x5a, blocks[0]);
    CHECK_EQ(0x5a, blocks[sizeof(blocks) - 1]);
}

static void test_initialization_error_matrix(void)
{
    for (unsigned int scenario = 0; scenario < 8; ++scenario) {
        pico_mock_reset();
        pico_mock_sd_use_chip_select(PIN_CHIP_SELECT);
        sd_spi_t sd = {0};
        sd_spi_config_t config = valid_config();
        CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
        configure_successful_sdhc_card();
        const uint8_t wrong_echo[] = {0, 0, 1, 0xab};
        switch (scenario) {
        case 0: CHECK(pico_mock_sd_set_command(8, 1, wrong_echo, sizeof(wrong_echo))); break;
        case 1: CHECK(pico_mock_sd_set_command(8, 0x09, NULL, 0)); break;
        case 2: CHECK(pico_mock_sd_set_command(55, 0x04, NULL, 0)); break;
        case 3: CHECK(pico_mock_sd_set_command(41, 0x04, NULL, 0)); break;
        case 4: CHECK(pico_mock_sd_set_command(58, 0x04, NULL, 0)); break;
        case 5: configure_successful_sdsc_card(); CHECK(pico_mock_sd_set_response_delay(16, 8)); break;
        case 6: configure_csd_v1(); break; /* CCS and CSD disagree. */
        case 7: configure_successful_sdsc_card(); configure_csd_v2(); break;
        }
        CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR, block_device_init(sd_spi_as_block_device(&sd)));
        CHECK(!sd.initialized && sd.block_count == 0);
        CHECK(!sd.card_type_legacy && !sd.card_type_hcxc);
        CHECK(!pico_mock_spi_is_initialized());
        CHECK_EQ(1U, pico_mock_spi_deinit_count());
        CHECK(!pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
        CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
        CHECK_EQ(400000U, pico_mock_spi_baudrate());
    }
    /* CSD v1 permits READ_BL_LEN 9..11; reject every other encoding. */
    for (uint8_t length = 0; length < 16; ++length) {
        if (length >= 9 && length <= 11) { continue; }
        pico_mock_reset();
        sd_spi_t sd = {0};
        sd_spi_config_t config = valid_config();
        CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
        configure_successful_sdsc_card();
        configure_csd_v1_fields(1023, 7, length);
        CHECK_EQ(BLOCK_DEVICE_RESULT_IO_ERROR, block_device_init(sd_spi_as_block_device(&sd)));
        CHECK(!sd.initialized && sd.block_count == 0);
        CHECK_EQ(0U, pico_mock_sd_command_count(16));
    }
}

static void test_spi_framing_and_response_boundary(void)
{
    /* Seven wait bytes put R1 in byte eight; eight wait bytes must time out. */
    for (size_t delay = 7; delay <= 8; ++delay) {
        pico_mock_reset();
        pico_mock_sd_use_chip_select(PIN_CHIP_SELECT);
        sd_spi_t sd = {0};
        block_device_t *device = initialize_sdhc(&sd);
        CHECK(device != NULL);
        const uint8_t *tx = pico_mock_spi_tx_log();
        for (size_t i = 0; i < 10; ++i) {
            CHECK_EQ(0xffU, tx[i]);
            CHECK(pico_mock_spi_tx_chip_select_high(i));
        }
        const uint8_t cmd0[] = {0x40, 0, 0, 0, 0, 0x95};
        const uint8_t cmd8[] = {0x48, 0, 0, 1, 0xaa, 0x87};
        CHECK(memcmp(tx + 11, cmd0, sizeof(cmd0)) == 0);
        CHECK(memcmp(tx + 19, cmd8, sizeof(cmd8)) == 0);
        size_t before = pico_mock_spi_transfer_count();
        for (size_t i = 10; i + 1 < before; ++i) {
            CHECK(!pico_mock_spi_tx_chip_select_high(i));
        }
        CHECK(pico_mock_spi_tx_chip_select_high(before - 1));
        uint8_t payload[SD_BLOCK_SIZE + 3], block[SD_BLOCK_SIZE];
        make_read_payload(payload, 1, 0x49);
        CHECK(pico_mock_sd_set_command(17, 0, payload, sizeof(payload)));
        CHECK(pico_mock_sd_set_response_delay(17, delay));
        CHECK_EQ(delay == 7 ? BLOCK_DEVICE_RESULT_OK : BLOCK_DEVICE_RESULT_IO_ERROR,
            block_device_read_blocks(device, 0, block, 1));
        if (delay == 7) { CHECK(buffer_matches(block, 1, 0x49)); }
        else { CHECK(pico_mock_spi_transfer_count() - before <= 16U); }
        CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
        CHECK_EQ(0U, pico_mock_sd_pending_response_count());
    }
}

static void test_all_data_error_token_combinations(void)
{
    for (uint8_t token = 1; token <= 15; ++token) {
        verify_data_error_token(token);
    }
}

static void test_maximum_sdhc_lba_and_overflow_requests(void)
{
    pico_mock_reset();
    sd_spi_t sd = {0};
    sd_spi_config_t config = valid_config();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
    configure_successful_sdhc_card();
    configure_csd_v2_fields(0x3fffff);
    block_device_t *device = sd_spi_as_block_device(&sd);
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_init(device));
    CHECK_EQ(UINT64_C(0x100000000), sd.block_count);
    uint8_t payload[SD_BLOCK_SIZE + 3], block[SD_BLOCK_SIZE];
    make_read_payload(payload, 1, 0x71);
    CHECK(pico_mock_sd_set_command(17, 0, payload, sizeof(payload)));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(device, UINT32_MAX, block, 1));
    CHECK_EQ(UINT32_MAX, pico_mock_sd_last_argument(17));
    CHECK(buffer_matches(block, 1, 0x71));
    size_t before = pico_mock_spi_transfer_count();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OUT_OF_RANGE, block_device_read_blocks(device, UINT64_MAX, block, 2));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OUT_OF_RANGE, block_device_read_blocks(device, UINT32_MAX, block, 2));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OUT_OF_RANGE, block_device_read_blocks(device, 1, block, SIZE_MAX));
    CHECK_EQ(before, pico_mock_spi_transfer_count());
}

static void test_initialization_removal_at_each_byte(void)
{
    pico_mock_reset();
    sd_spi_t baseline = {0};
    CHECK(initialize_sdhc(&baseline) != NULL);
    size_t total = pico_mock_spi_transfer_count();
    for (size_t offset = 1; offset <= total; ++offset) {
        pico_mock_reset();
        pico_mock_sd_use_chip_select(PIN_CHIP_SELECT);
        sd_spi_t sd = {0};
        sd_spi_config_t config = valid_config();
        CHECK_EQ(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&sd, &config));
        configure_successful_sdhc_card();
        pico_mock_gpio_irq_fire_after_spi_transfers(offset, PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE);
        block_device_result_t result = block_device_init(sd_spi_as_block_device(&sd));
        if (result != BLOCK_DEVICE_RESULT_INVALID_DEVICE) {
            fprintf(stderr, "initialization removal offset=%zu result=%d\n", offset, result);
        }
        CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE, result);
        CHECK(!sd.initialized && sd.block_count == 0);
        CHECK(atomic_load(&sd.removal_latched));
        CHECK(!pico_mock_gpio_irq_is_registered(PIN_CARD_AVAILABLE));
        CHECK(!pico_mock_spi_is_initialized());
        CHECK_EQ(1U, pico_mock_spi_deinit_count());
        CHECK(pico_mock_spi_transfer_count() <= offset + 5U);
    }
}

static void verify_read_removal_at_each_byte(size_t count, bool release_only)
{
    uint8_t payload[2 * (SD_BLOCK_SIZE + 3)];
    uint8_t guarded[2 * SD_BLOCK_SIZE + 2];
    const size_t payload_size = make_read_payload(payload, count, 0x37);
    pico_mock_reset();
    sd_spi_t baseline = {0};
    block_device_t *device = initialize_sdhc(&baseline);
    CHECK(device != NULL);
    CHECK(pico_mock_sd_set_command(count == 1 ? 17 : 18, 0, payload, payload_size));
    CHECK(pico_mock_sd_set_command(12, 0, NULL, 0));
    size_t start = pico_mock_spi_transfer_count();
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(device, 0, guarded + 1, count));
    const size_t total = pico_mock_spi_transfer_count() - start;
    /* The final release byte is a separately registered known-gap regression. */
    for (size_t offset = release_only ? total : 1; offset <= (release_only ? total : total - 1); ++offset) {
        pico_mock_reset();
        pico_mock_sd_use_chip_select(PIN_CHIP_SELECT);
        sd_spi_t sd = {0};
        device = initialize_sdhc(&sd);
        CHECK(device != NULL);
        CHECK(pico_mock_sd_set_command(count == 1 ? 17 : 18, 0, payload, payload_size));
        CHECK(pico_mock_sd_set_command(12, 0, NULL, 0));
        memset(guarded, 0xa5, sizeof(guarded));
        start = pico_mock_spi_transfer_count();
        pico_mock_gpio_irq_fire_after_spi_transfers(offset, PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE);
        block_device_result_t result = block_device_read_blocks(device, 0, guarded + 1, count);
        if (result != BLOCK_DEVICE_RESULT_INVALID_DEVICE) {
            fprintf(stderr, "read removal blocks=%zu offset=%zu/%zu result=%d latch=%d\n",
                count, offset, total, result, (int)atomic_load(&sd.removal_latched));
        }
        CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE, result);
        CHECK(atomic_load(&sd.removal_latched));
        CHECK_EQ(0xa5, guarded[0]);
        CHECK_EQ(0xa5, guarded[count * SD_BLOCK_SIZE + 1]);
        CHECK(pico_mock_gpio_level(PIN_CHIP_SELECT));
        CHECK(pico_mock_spi_is_initialized()); /* ISR does not tear down. */
        CHECK_EQ(0U, pico_mock_spi_deinit_count());
        CHECK(!pico_mock_gpio_irq_is_enabled(PIN_CARD_AVAILABLE));
        CHECK(pico_mock_spi_transfer_count() - start <= offset + 5U);
        if (offset <= 8U + payload_size) { CHECK_EQ(0U, pico_mock_sd_command_count(12)); }
        start = pico_mock_spi_transfer_count();
        CHECK_EQ(BLOCK_DEVICE_RESULT_INVALID_DEVICE, block_device_read_blocks(device, 0, guarded + 1, count));
        CHECK_EQ(start, pico_mock_spi_transfer_count());
        CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_deinit(device));
        CHECK_EQ(start, pico_mock_spi_transfer_count());
        CHECK_EQ(1U, pico_mock_spi_deinit_count());
    }
}
static void test_single_read_removal_boundaries(void) { verify_read_removal_at_each_byte(1, false); }
static void test_multi_read_removal_boundaries(void) { verify_read_removal_at_each_byte(2, false); }
static void test_single_read_release_removal(void) { verify_read_removal_at_each_byte(1, true); }
static void test_multi_read_release_removal(void) { verify_read_removal_at_each_byte(2, true); }

static void test_removal_during_busy_waits(void)
{
    for (unsigned int deinit = 0; deinit < 2; ++deinit) {
        pico_mock_reset();
        sd_spi_t sd = {0};
        block_device_t *device = initialize_sdhc(&sd);
        CHECK(device != NULL);
        pico_mock_sd_set_busy_cycles(2000);
        pico_mock_gpio_irq_fire_after_spi_transfers(3, PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE);
        uint64_t before_time = pico_mock_now_ms();
        size_t before = pico_mock_spi_transfer_count();
        uint8_t block[SD_BLOCK_SIZE];
        CHECK_EQ(deinit ? BLOCK_DEVICE_RESULT_OK : BLOCK_DEVICE_RESULT_INVALID_DEVICE,
            deinit ? block_device_deinit(device) : block_device_read_blocks(device, 0, block, 1));
        CHECK_EQ(3U, pico_mock_spi_transfer_count() - before);
        CHECK(pico_mock_now_ms() - before_time < 10U);
        CHECK_EQ(0U, pico_mock_sd_command_count(17));
        CHECK_EQ(deinit ? 1U : 0U, pico_mock_spi_deinit_count());
    }
}

static void test_repeated_removal_teardown(void)
{
    pico_mock_reset();
    sd_spi_t sd = {0};
    block_device_t *device = initialize_sdhc(&sd);
    CHECK(device != NULL);
    CHECK(pico_mock_gpio_irq_fire(PIN_CARD_AVAILABLE, GPIO_IRQ_EDGE_RISE));
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_deinit(device));
    CHECK_EQ(1U, pico_mock_spi_deinit_count());
    CHECK_EQ(BLOCK_DEVICE_RESULT_OK, block_device_deinit(device));
    CHECK_EQ(1U, pico_mock_spi_deinit_count());
}

static void run_test(void (*test)(void), const char *name)
{
    const int failures_before = failures;
    tests_run++;
    test();
    if (failures == failures_before) {
        (void)printf("PASS %s\n", name);
    } else {
        (void)printf("FAIL %s\n", name);
    }
}

/* The whole suite can be run under either clock model. Anything that only
 * passes with SIM_CLOCK_POLL_TICK is relying on the legacy artifact where
 * fake time advanced once per time_reached() call rather than with bus work;
 * CTest runs both so such a case cannot hide. */
static bool select_clock_mode(const char *argument)
{
    if (strcmp(argument, "--clock=bus-time") == 0) {
        pico_mock_set_clock_mode(SIM_CLOCK_BUS_TIME);
        return true;
    }
    if (strcmp(argument, "--clock=poll-tick") == 0) {
        pico_mock_set_clock_mode(SIM_CLOCK_POLL_TICK);
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    int positional = argc;
    if (argc >= 2 && select_clock_mode(argv[argc - 1])) {
        positional = argc - 1;
    }
    argc = positional;
    if (argc == 2) {
        if (strcmp(argv[1], "--gap-single-release") == 0) {
            run_test(test_single_read_release_removal, "removal on CMD17 release must cancel success");
        } else if (strcmp(argv[1], "--gap-multi-release") == 0) {
            run_test(test_multi_read_release_removal, "removal on CMD18 release must cancel success");
        } else if (strcmp(argv[1], "--gap-repeated-teardown") == 0) {
            run_test(test_repeated_removal_teardown, "removal teardown releases hardware exactly once");
        } else {
            fprintf(stderr, "Unknown test selector: %s\n", argv[1]);
            return 2;
        }
        return failures != 0;
    }
    if (argc != 1) { return 2; }
    run_test(test_modern_sdsc_and_retry_success, "modern SDSC, ACMD41 retries and multiblock byte addressing");
    run_test(test_initialization_error_matrix, "initialization rejection matrix and invalid CSD encodings");
    run_test(test_cmd59_rejection_fails_initialization,
        "a refused CMD59 fails initialization instead of proceeding unprotected");
    run_test(test_spi_framing_and_response_boundary, "SPI selection, command CRC and R1 byte boundary");
    run_test(test_all_data_error_token_combinations, "all fifteen data-error token combinations");
    run_test(test_maximum_sdhc_lba_and_overflow_requests, "maximum SDHC address and overflow rejection");
    run_test(test_initialization_removal_at_each_byte, "initialization removal at each SPI byte");
    run_test(test_single_read_removal_boundaries, "single read removal at each active SPI byte");
    run_test(test_multi_read_removal_boundaries, "multi read removal at each active SPI byte");
    run_test(test_removal_during_busy_waits, "read and deinit busy waits cancel promptly");
    run_test(test_block_device_wrapper_validation,
        "block device wrapper validation");
    run_test(test_backend_context_validation,
        "backend context validation");
    run_test(test_configure_validation, "configure validation");
    run_test(test_mock_command_configuration_validation,
        "mock command configuration validation");
    run_test(test_configure_exposes_uninitialized_device,
        "configured device preconditions");
    run_test(test_reconfiguration_lifecycle,
        "reconfiguration lifecycle");
    run_test(test_sdhc_initialization, "SDHC initialization");
    run_test(test_legacy_card_initialization, "legacy card initialization");
    run_test(test_csd_capacity_encoding_limits,
        "CSD capacity encoding limits");
    run_test(test_missing_card_fails_initialization,
        "missing card initialization failure");
    run_test(test_card_detect_debounce, "card-detect debounce");
    run_test(test_card_initialization_timeout_is_bounded,
        "card initialization timeout");
    run_test(test_initialization_response_validation,
        "initialization response validation");
    run_test(test_unsupported_csd_rolls_back_initialization,
        "unsupported CSD initialization rollback");
    run_test(test_cmd9_response_and_data_token_failures,
        "CMD9 response and data-token failures");
    run_test(test_data_error_token_01_is_immediate,
        "data-error token 0x01 is immediate");
    run_test(test_data_error_token_02_is_immediate,
        "data-error token 0x02 is immediate");
    run_test(test_data_error_token_04_is_immediate,
        "data-error token 0x04 is immediate");
    run_test(test_data_error_token_08_is_immediate,
        "data-error token 0x08 is immediate");
    run_test(test_zero_is_not_a_data_error_token,
        "zero is not a data-error token");
    run_test(test_capacity_state_cleared_across_reinitialization,
        "capacity state reinitialization lifecycle");
    run_test(test_deinit_waits_and_releases_resources,
        "deinit waits and releases resources");
    run_test(test_deinit_timeout_preserves_resources,
        "deinit timeout preserves resources");
    run_test(test_deinit_before_init_does_not_transfer,
        "deinit before init");
    run_test(test_single_block_read_sdhc, "single-block SDHC read");
    run_test(test_single_block_read_sdsc_uses_byte_address,
        "single-block SDSC byte address");
    run_test(test_single_block_read_failures,
        "single-block read failures");
    run_test(test_multiple_block_read, "multiple-block read");
    run_test(test_multiple_block_timeout_is_not_success,
        "multiple-block timeout");
    run_test(test_multiple_block_error_cleanup,
        "multiple-block error cleanup");
    run_test(test_multiple_block_error_recovery,
        "multiple-block error recovery");
    run_test(test_multiple_block_stop_response_timeout_is_bounded,
        "multiple-block stop response timeout");
    run_test(test_multiple_block_stop_waits_until_ready,
        "multiple-block stop waits until ready");
    run_test(test_multiple_block_stop_failure_only_sent_once,
        "multiple-block stop failure sent once");
    run_test(test_read_range_uses_csd_capacity,
        "CSD capacity read range");
    run_test(test_write_blocks_is_implemented,
        "write_blocks is implemented");
    run_test(test_get_info_reports_card_geometry,
        "get_info reports CSD geometry without bus traffic");
    run_test(test_get_info_before_initialization_leaves_output_untouched,
        "get_info before init reports NOT_INITIALIZED and writes nothing");
    run_test(test_initialization_reports_busy_timeout,
        "initialization busy timeout");
    run_test(test_read_command_reports_busy_timeout,
        "read command busy timeout");
    run_test(test_stop_transmission_reports_busy_timeout,
        "stop transmission busy timeout");
    run_test(test_error_cleanup_reports_busy_timeout,
        "error cleanup busy timeout");
    run_test(test_card_detect_irq_registration_and_latch,
        "card-detect IRQ registration and latch");
    run_test(test_card_detect_irq_registration_failure,
        "card-detect IRQ registration failure");
    run_test(test_card_detect_post_registration_race,
        "card-detect post-registration race");
    run_test(test_card_detect_post_registration_bounce_is_latched,
        "card-detect post-registration bounce latch");
    run_test(test_initialization_rollback_unregisters_card_irq,
        "initialization rollback unregisters card IRQ");
    run_test(test_deinit_unregisters_card_irq,
        "deinit unregisters card IRQ");
    run_test(test_latched_removal_rejects_new_operations,
        "latched removal rejects new operations");
    run_test(test_removal_interrupts_single_block_read,
        "removal interrupts single-block read");
    run_test(test_removal_interrupts_multiple_block_read_without_cmd12,
        "removal interrupts multi-block read without CMD12");
    run_test(test_removal_during_initialization_rolls_back,
        "removal during initialization rolls back");
    run_test(test_removal_cleanup_allows_fresh_initialization,
        "removal cleanup allows fresh initialization");

    if (failures != 0) {
        (void)fprintf(stderr, "%d of %d tests failed\n", failures, tests_run);
        return 1;
    }

    (void)printf("All %d tests passed\n", tests_run);
    return 0;
}
