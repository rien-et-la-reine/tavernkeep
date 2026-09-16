/*
 * Pico SDK peripheral fakes.
 *
 * This layer fakes only what the SDK provides: GPIO pads, the SPI peripheral
 * and the time functions. Everything that behaves like an SD card lives in
 * sd_card_model.h, and everything that behaves like a clock lives in
 * sim_clock.h. The pico_mock_sd_* functions below are thin forwarders kept so
 * fixtures written against the original engine still compile; new tests should
 * drive the card model directly.
 */
#ifndef TAVERNKEEP_TEST_PICO_MOCK_H
#define TAVERNKEEP_TEST_PICO_MOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sd_card_model.h"
#include "sim_clock.h"

enum {
    PICO_MOCK_MAX_COMMAND_PAYLOAD = SD_MODEL_SCRIPT_PAYLOAD,
    /* Bytes of MOSI history kept for framing assertions. Beyond this the
     * counters keep counting but the log stops growing, so a long timeout
     * cannot abort the process the way the old fixed buffer did. */
    PICO_MOCK_TX_WINDOW = 65536,
};

void pico_mock_reset(void);

void pico_mock_gpio_irq_reset(void);
void pico_mock_gpio_irq_reject_next_registration(void);
void pico_mock_gpio_irq_fire_on_next_registration(uint32_t events);
bool pico_mock_gpio_irq_is_registered(unsigned int gpio);
bool pico_mock_gpio_irq_is_enabled(unsigned int gpio);
uint32_t pico_mock_gpio_irq_events(unsigned int gpio);
size_t pico_mock_gpio_irq_registration_count(unsigned int gpio);
size_t pico_mock_gpio_irq_unregistration_count(unsigned int gpio);
bool pico_mock_gpio_irq_fire(
    unsigned int gpio,
    uint32_t events);
void pico_mock_gpio_irq_fire_after_spi_transfers(
    size_t additional_transfers,
    unsigned int gpio,
    uint32_t events);

void pico_mock_gpio_set_input(unsigned int pin, bool value);
bool pico_mock_gpio_set_input_sequence(
    unsigned int pin,
    const bool *values,
    size_t value_count);
/* True once a test's input sequence has been consumed and reads have fallen
 * back to the static level. A test that meant its sequence to cover the whole
 * run can assert this is false instead of passing for the wrong reason. */
bool pico_mock_gpio_input_sequence_exhausted(void);
size_t pico_mock_gpio_read_count(unsigned int pin);
bool pico_mock_gpio_level(unsigned int pin);
bool pico_mock_gpio_was_pulled_up_at_init(unsigned int pin);
bool pico_mock_gpio_was_initialized(unsigned int pin);
bool pico_mock_gpio_was_deinitialized(unsigned int pin);
size_t pico_mock_gpio_deinit_count(unsigned int pin);
unsigned int pico_mock_gpio_function(unsigned int pin);
bool pico_mock_gpio_direction_is_output(unsigned int pin);

bool pico_mock_spi_is_initialized(void);
unsigned int pico_mock_spi_initial_baudrate(void);
unsigned int pico_mock_spi_baudrate(void);
size_t pico_mock_spi_transfer_count(void);
size_t pico_mock_spi_init_count(void);
size_t pico_mock_spi_deinit_count(void);
uint64_t pico_mock_now_ms(void);
uint64_t pico_mock_now_us(void);
void pico_mock_advance_us(uint64_t microseconds);
/* Called after every sleep_ms() with the requested duration, so a test can
 * observe a foreground loop or leave one that never returns (longjmp from the
 * hook is the intended escape). Cleared by pico_mock_reset(). */
typedef void (*pico_mock_sleep_hook_t)(uint32_t milliseconds, void *context);
void pico_mock_set_sleep_hook(pico_mock_sleep_hook_t hook, void *context);
void pico_mock_set_clock_mode(sim_clock_mode_t mode);
uint64_t pico_mock_poll_count(void);

void pico_mock_sd_use_chip_select(unsigned int pin);
/* Pin that SD_FAULT_EJECT drives to simulate removal mid-transaction. */
void pico_mock_sd_use_card_detect(unsigned int pin);
/* Sense of that pin. false (the default after reset): low means present and
 * an eject drives it high with a rising edge. true: high means present and an
 * eject drives it low with a falling edge. Mirrors
 * sd_spi_config_t.card_detect_active_high. */
void pico_mock_sd_set_card_detect_active_high(bool active_high);
bool pico_mock_sd_card_detect_active_high(void);
bool pico_mock_spi_tx_chip_select_high(size_t index);
const uint8_t *pico_mock_spi_tx_log(void);
size_t pico_mock_spi_tx_log_length(void);

bool pico_mock_sd_set_response_delay(uint8_t command, size_t bytes);
bool pico_mock_sd_set_idle_responses(uint8_t command, size_t responses);
bool pico_mock_sd_set_busy_after_payload(uint8_t command, bool busy);
bool pico_mock_sd_set_stall_after_r1(uint8_t command, bool stall);
size_t pico_mock_sd_pending_response_count(void);

void pico_mock_sd_set_busy_cycles(size_t cycles);
void pico_mock_sd_set_busy_forever(void);
bool pico_mock_sd_set_command(
    uint8_t command,
    uint8_t r1,
    const uint8_t *payload,
    size_t payload_size);
size_t pico_mock_sd_command_count(uint8_t command);
uint32_t pico_mock_sd_last_argument(uint8_t command);

#endif
