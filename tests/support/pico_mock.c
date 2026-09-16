#include "pico_mock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"
#include "platform/gpio_irq.h"
#include "sd_card_model.h"
#include "sim_clock.h"

enum {
    MOCK_PIN_COUNT = 64,
    MOCK_GPIO_INPUT_SEQUENCE_CAPACITY = 256,
};

static bool gpio_levels[MOCK_PIN_COUNT];
static pico_mock_sleep_hook_t sleep_hook;
static void *sleep_hook_context;
static bool gpio_pull_ups[MOCK_PIN_COUNT];
static bool gpio_pulled_up_at_init[MOCK_PIN_COUNT];
static bool gpio_initialized[MOCK_PIN_COUNT];
static bool gpio_deinitialized[MOCK_PIN_COUNT];
static size_t gpio_deinit_counts[MOCK_PIN_COUNT];
static bool gpio_directions[MOCK_PIN_COUNT];
static unsigned int gpio_functions[MOCK_PIN_COUNT];
static size_t gpio_read_counts[MOCK_PIN_COUNT];
static bool gpio_input_sequence[MOCK_GPIO_INPUT_SEQUENCE_CAPACITY];
static size_t gpio_input_sequence_size;
static size_t gpio_input_sequence_position;
static unsigned int gpio_input_sequence_pin;
static bool gpio_input_sequence_exhausted;

static bool spi_initialized;
static unsigned int spi_initial_baudrate;
static unsigned int spi_baudrate;
static size_t spi_init_count;
static size_t spi_deinit_count;
static uint8_t spi_tx_log[PICO_MOCK_TX_WINDOW];
static bool spi_tx_cs_high[PICO_MOCK_TX_WINDOW];
static size_t spi_tx_logged;
static size_t spi_tx_count;

static unsigned int chip_select_pin;
static unsigned int card_detect_pin;
static bool card_detect_active_high;

static bool scheduled_gpio_irq;
static size_t scheduled_gpio_irq_transfer;
static unsigned int scheduled_gpio_irq_pin;
static uint32_t scheduled_gpio_irq_events;

static bool valid_pin(unsigned int pin)
{
    return pin < MOCK_PIN_COUNT;
}

static void sync_chip_select(void)
{
    if (!valid_pin(chip_select_pin)) {
        /* No chip select registered: the historical fixtures ran with the
         * card permanently selected. Keep that so they keep meaning what
         * they meant, rather than silently changing their subject. */
        sd_card_set_chip_select(false);
        return;
    }
    sd_card_set_chip_select(gpio_levels[chip_select_pin]);
}

void pico_mock_reset(void)
{
    pico_mock_gpio_irq_reset();
    (void)platform_gpio_irq_init();
    /* The clock model is a property of the run, not of a single fixture, so
     * it survives a reset. Otherwise a suite launched in poll-tick mode would
     * silently fall back to bus time at the first pico_mock_reset(). */
    const sim_clock_mode_t mode = sim_clock_mode();
    sim_clock_reset();
    sim_clock_set_mode(mode);
    sd_card_reset(NULL);
    sleep_hook = NULL;
    sleep_hook_context = NULL;

    memset(gpio_levels, 0, sizeof(gpio_levels));
    memset(gpio_pull_ups, 0, sizeof(gpio_pull_ups));
    memset(gpio_pulled_up_at_init, 0, sizeof(gpio_pulled_up_at_init));
    memset(gpio_initialized, 0, sizeof(gpio_initialized));
    memset(gpio_deinitialized, 0, sizeof(gpio_deinitialized));
    memset(gpio_deinit_counts, 0, sizeof(gpio_deinit_counts));
    memset(gpio_directions, 0, sizeof(gpio_directions));
    memset(gpio_functions, 0, sizeof(gpio_functions));
    memset(gpio_read_counts, 0, sizeof(gpio_read_counts));
    memset(gpio_input_sequence, 0, sizeof(gpio_input_sequence));
    gpio_input_sequence_size = 0U;
    gpio_input_sequence_position = 0U;
    gpio_input_sequence_pin = MOCK_PIN_COUNT;
    gpio_input_sequence_exhausted = false;

    spi_initialized = false;
    spi_initial_baudrate = 0U;
    spi_baudrate = 0U;
    spi_init_count = 0U;
    spi_deinit_count = 0U;
    memset(spi_tx_log, 0, sizeof(spi_tx_log));
    memset(spi_tx_cs_high, 0, sizeof(spi_tx_cs_high));
    spi_tx_logged = 0U;
    spi_tx_count = 0U;

    chip_select_pin = MOCK_PIN_COUNT;
    card_detect_pin = MOCK_PIN_COUNT;
    card_detect_active_high = false;
    scheduled_gpio_irq = false;
    scheduled_gpio_irq_transfer = 0U;
    scheduled_gpio_irq_pin = 0U;
    scheduled_gpio_irq_events = 0U;
    sync_chip_select();
}

/* ------------------------------------------------------------------ SPI */

static uint8_t transfer_byte(uint8_t tx)
{
    spi_tx_count++;
    if (spi_tx_logged < PICO_MOCK_TX_WINDOW) {
        spi_tx_cs_high[spi_tx_logged] =
            valid_pin(chip_select_pin) && gpio_levels[chip_select_pin];
        spi_tx_log[spi_tx_logged++] = tx;
    }

    sim_clock_charge_spi_byte();
    const uint8_t rx = sd_card_transfer(tx);

    if (sd_card_eject_requested()) {
        sd_card_clear_eject_request();
        if (valid_pin(card_detect_pin)) {
            /* Removal takes the line to its "absent" level for the configured
             * sense and fires the matching edge. */
            gpio_levels[card_detect_pin] = !card_detect_active_high;
            (void)pico_mock_gpio_irq_fire(card_detect_pin,
                card_detect_active_high ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE);
        }
    }

    if (scheduled_gpio_irq && spi_tx_count >= scheduled_gpio_irq_transfer) {
        scheduled_gpio_irq = false;
        (void)pico_mock_gpio_irq_fire(
            scheduled_gpio_irq_pin, scheduled_gpio_irq_events);
    }
    return rx;
}

unsigned int spi_init(spi_inst_t *spi, unsigned int baudrate)
{
    (void)spi;
    spi_initialized = true;
    spi_init_count++;
    spi_initial_baudrate = baudrate;
    spi_baudrate = baudrate;
    sim_clock_set_baud(baudrate);
    return baudrate;
}

void spi_deinit(spi_inst_t *spi)
{
    (void)spi;
    spi_deinit_count++;
    spi_initialized = false;
}

unsigned int spi_set_baudrate(spi_inst_t *spi, unsigned int baudrate)
{
    (void)spi;
    spi_baudrate = baudrate;
    sim_clock_set_baud(baudrate);
    return baudrate;
}

int spi_write_read_blocking(
    spi_inst_t *spi,
    const uint8_t *source,
    uint8_t *destination,
    size_t length)
{
    (void)spi;
    for (size_t i = 0U; i < length; ++i) {
        destination[i] = transfer_byte(source[i]);
    }
    return (int)length;
}

/* ----------------------------------------------------------------- GPIO */

void gpio_init(unsigned int pin)
{
    if (valid_pin(pin)) {
        gpio_pulled_up_at_init[pin] = gpio_pull_ups[pin];
        gpio_initialized[pin] = true;
        gpio_deinitialized[pin] = false;
    }
}

void gpio_deinit(unsigned int pin)
{
    if (valid_pin(pin)) {
        gpio_deinitialized[pin] = true;
        gpio_deinit_counts[pin]++;
        gpio_initialized[pin] = false;
    }
}

void gpio_pull_up(unsigned int pin)
{
    if (valid_pin(pin)) {
        gpio_pull_ups[pin] = true;
    }
}

void gpio_put(unsigned int pin, bool value)
{
    if (valid_pin(pin)) {
        gpio_levels[pin] = value;
        if (pin == chip_select_pin) {
            sync_chip_select();
        }
    }
}

bool gpio_get(unsigned int pin)
{
    if (!valid_pin(pin)) {
        return false;
    }
    gpio_read_counts[pin]++;
    if (pin == gpio_input_sequence_pin) {
        if (gpio_input_sequence_position < gpio_input_sequence_size) {
            return gpio_input_sequence[gpio_input_sequence_position++];
        }
        gpio_input_sequence_exhausted = true;
    }
    return gpio_levels[pin];
}

void gpio_set_dir(unsigned int pin, bool out)
{
    if (valid_pin(pin)) {
        gpio_directions[pin] = out;
    }
}

void gpio_set_function(unsigned int pin, unsigned int function)
{
    if (valid_pin(pin)) {
        gpio_functions[pin] = function;
    }
}

/* ----------------------------------------------------------------- time */

absolute_time_t make_timeout_time_ms(uint32_t milliseconds)
{
    return sim_clock_timeout_us(milliseconds);
}

bool time_reached(absolute_time_t target)
{
    return sim_clock_time_reached(target);
}

void pico_mock_set_sleep_hook(pico_mock_sleep_hook_t hook, void *context)
{
    sleep_hook = hook;
    sleep_hook_context = context;
}

void sleep_ms(uint32_t milliseconds)
{
    sim_clock_sleep_ms(milliseconds);
    if (sleep_hook != NULL) {
        sleep_hook(milliseconds, sleep_hook_context);
    }
}

spi_inst_t pico_mock_spi0_instance = { 0U };
spi_inst_t pico_mock_spi1_instance = { 1U };

/* -------------------------------------------------------------- queries */

void pico_mock_gpio_irq_fire_after_spi_transfers(
    size_t additional_transfers,
    unsigned int gpio,
    uint32_t events)
{
    scheduled_gpio_irq = true;
    scheduled_gpio_irq_transfer = spi_tx_count + additional_transfers;
    scheduled_gpio_irq_pin = gpio;
    scheduled_gpio_irq_events = events;
}

void pico_mock_gpio_set_input(unsigned int pin, bool value)
{
    if (valid_pin(pin)) {
        gpio_levels[pin] = value;
        if (pin == chip_select_pin) {
            sync_chip_select();
        }
    }
}

bool pico_mock_gpio_set_input_sequence(
    unsigned int pin,
    const bool *values,
    size_t value_count)
{
    if (!valid_pin(pin)
            || value_count > MOCK_GPIO_INPUT_SEQUENCE_CAPACITY
            || (value_count != 0U && values == NULL)) {
        return false;
    }
    gpio_input_sequence_pin = pin;
    gpio_input_sequence_size = value_count;
    gpio_input_sequence_position = 0U;
    gpio_input_sequence_exhausted = false;
    if (value_count != 0U) {
        memcpy(gpio_input_sequence, values, value_count * sizeof(values[0]));
    }
    return true;
}

bool pico_mock_gpio_input_sequence_exhausted(void)
{
    return gpio_input_sequence_exhausted;
}

size_t pico_mock_gpio_read_count(unsigned int pin)
{
    return valid_pin(pin) ? gpio_read_counts[pin] : 0U;
}

bool pico_mock_gpio_level(unsigned int pin)
{
    return valid_pin(pin) && gpio_levels[pin];
}

bool pico_mock_gpio_was_pulled_up_at_init(unsigned int pin)
{
    return valid_pin(pin) && gpio_pulled_up_at_init[pin];
}

bool pico_mock_gpio_was_initialized(unsigned int pin)
{
    return valid_pin(pin) && gpio_initialized[pin];
}

bool pico_mock_gpio_was_deinitialized(unsigned int pin)
{
    return valid_pin(pin) && gpio_deinitialized[pin];
}

size_t pico_mock_gpio_deinit_count(unsigned int pin)
{
    return valid_pin(pin) ? gpio_deinit_counts[pin] : 0U;
}

unsigned int pico_mock_gpio_function(unsigned int pin)
{
    return valid_pin(pin) ? gpio_functions[pin] : 0U;
}

bool pico_mock_gpio_direction_is_output(unsigned int pin)
{
    return valid_pin(pin) && gpio_directions[pin];
}

bool pico_mock_spi_is_initialized(void) { return spi_initialized; }
unsigned int pico_mock_spi_initial_baudrate(void) { return spi_initial_baudrate; }
unsigned int pico_mock_spi_baudrate(void) { return spi_baudrate; }
size_t pico_mock_spi_transfer_count(void) { return spi_tx_count; }
size_t pico_mock_spi_init_count(void) { return spi_init_count; }
size_t pico_mock_spi_deinit_count(void) { return spi_deinit_count; }

uint64_t pico_mock_now_ms(void) { return sim_clock_now_us() / 1000U; }
uint64_t pico_mock_now_us(void) { return sim_clock_now_us(); }
void pico_mock_advance_us(uint64_t us) { sim_clock_advance_us(us); }
void pico_mock_set_clock_mode(sim_clock_mode_t mode) { sim_clock_set_mode(mode); }
uint64_t pico_mock_poll_count(void) { return sim_clock_poll_count(); }

void pico_mock_sd_use_chip_select(unsigned int pin)
{
    chip_select_pin = pin;
    sync_chip_select();
}

void pico_mock_sd_use_card_detect(unsigned int pin)
{
    card_detect_pin = pin;
}

void pico_mock_sd_set_card_detect_active_high(bool active_high)
{
    card_detect_active_high = active_high;
}

bool pico_mock_sd_card_detect_active_high(void)
{
    return card_detect_active_high;
}

bool pico_mock_spi_tx_chip_select_high(size_t index)
{
    return index < spi_tx_logged && spi_tx_cs_high[index];
}

const uint8_t *pico_mock_spi_tx_log(void) { return spi_tx_log; }
size_t pico_mock_spi_tx_log_length(void) { return spi_tx_logged; }

bool pico_mock_sd_set_response_delay(uint8_t command, size_t bytes)
{
    return sd_card_script_response_delay(command, bytes);
}

bool pico_mock_sd_set_idle_responses(uint8_t command, size_t responses)
{
    return sd_card_script_idle_responses(command, responses);
}

bool pico_mock_sd_set_busy_after_payload(uint8_t command, bool busy)
{
    return sd_card_script_busy_after_payload(command, busy);
}

bool pico_mock_sd_set_stall_after_r1(uint8_t command, bool stall)
{
    return sd_card_script_stall_after_r1(command, stall);
}

size_t pico_mock_sd_pending_response_count(void)
{
    return sd_card_pending_scripted_bytes();
}

void pico_mock_sd_set_busy_cycles(size_t cycles)
{
    sd_card_set_busy_bytes(cycles);
}

void pico_mock_sd_set_busy_forever(void)
{
    sd_card_set_busy_forever();
}

bool pico_mock_sd_set_command(
    uint8_t command,
    uint8_t r1,
    const uint8_t *payload,
    size_t payload_size)
{
    return sd_card_script_command(command, r1, payload, payload_size);
}

size_t pico_mock_sd_command_count(uint8_t command)
{
    return sd_card_command_count(command);
}

uint32_t pico_mock_sd_last_argument(uint8_t command)
{
    return sd_card_last_argument(command);
}
