/*
 * Reusable SD driver fixture and state/recovery assertions.
 *
 * The point of this layer is that an adversarial case should be cheap to
 * write. Bringing a card up, checking that a failure left the bus released and
 * the driver reusable, and confirming that a destination buffer was not
 * partially updated are all one call each, so a new test spends its lines on
 * the interesting question rather than on setup.
 */
#ifndef TAVERNKEEP_TEST_SD_FIXTURE_H
#define TAVERNKEEP_TEST_SD_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/spi.h"
#include "pico_mock.h"
#include "sd_card_model.h"
#include "storage/sd_spi.h"

enum {
    SD_FX_PIN_CLOCK = 2,
    SD_FX_PIN_MOSI = 3,
    SD_FX_PIN_MISO = 4,
    SD_FX_PIN_CS = 5,
    SD_FX_PIN_CARD_DETECT = 6,
    SD_FX_BAUD_HZ = 12000000,
    SD_FX_BLOCK = 512,
    /* Canary bytes placed either side of every read destination. */
    SD_FX_GUARD = 8,
};

typedef struct {
    sd_spi_t sd;
    sd_spi_config_t config;
    spi_inst_t spi;
    block_device_t *device;
} sd_fixture_t;

/* Bring the harness to a known state with the card model answering as a card
 * (SD_RESPONSE_MODELLED) and chip select and card detect wired up. Does not
 * initialise the driver. */
void sd_fx_begin(sd_fixture_t *fx, const sd_card_desc_t *desc);

/* sd_fx_begin plus a successful block_device_init(). Returns the result so a
 * caller can assert on it; the common case is sd_fx_require_init. */
block_device_result_t sd_fx_init(sd_fixture_t *fx);
bool sd_fx_require_init(sd_fixture_t *fx, const sd_card_desc_t *desc);

/* ------------------------------------------------- card-detect polarity */

/* Sense of the card-detect line for every fixture begun after this call:
 * false (the default) is the active-low switch the driver assumes for a
 * zeroed config, true is a switch that closes to ground when the socket is
 * empty. sd_fx_begin() wires the driver config, the SPI fake's eject path and
 * the initial "present" level to match, so a suite can be run under both
 * senses from one source. Suites that take --card-detect=active-high call
 * this from main(). */
void sd_fx_set_card_detect_active_high(bool active_high);
bool sd_fx_card_detect_active_high(void);
/* Drive the line to the level that means present/absent for the current
 * sense. Does not fire an edge. */
void sd_fx_set_card_present(bool present);
/* The edge that means removal for the current sense. */
uint32_t sd_fx_removal_edge(void);
/* Drive the line to absent and fire the removal edge, as a real removal
 * would. Returns whether the edge was delivered (false when the driver has
 * no handler armed). */
bool sd_fx_remove_card(void);
/* Parse one --card-detect=<active-low|active-high> argument for suites that
 * run under both senses. Returns false for an unrecognised argument. */
bool sd_fx_parse_card_detect_arg(const char *arg);

/* ------------------------------------------------------ guarded buffers */

typedef struct {
    uint8_t bytes[SD_FX_GUARD + (16 * SD_FX_BLOCK) + SD_FX_GUARD];
    size_t blocks;
} sd_guarded_buffer_t;

void sd_fx_guard_init(sd_guarded_buffer_t *buffer, size_t blocks);
uint8_t *sd_fx_guard_data(sd_guarded_buffer_t *buffer);
bool sd_fx_guard_intact(const sd_guarded_buffer_t *buffer);
/* True when every data byte still holds the fill pattern, i.e. the driver
 * wrote nothing at all into the destination. */
bool sd_fx_guard_untouched(const sd_guarded_buffer_t *buffer);
/* Number of leading destination bytes that already hold the card's content
 * for `first_lba` onwards. This is the precise way to say "the driver
 * populated exactly N blocks before it failed": counting bytes that differ
 * from the fill pattern under-counts, because real block data contains the
 * fill byte by coincidence. */
size_t sd_fx_guard_prefix_from_card(
    const sd_guarded_buffer_t *buffer,
    uint64_t first_lba);
/* True when every destination byte from `offset` onwards is still the fill
 * pattern, i.e. the driver wrote nothing there. */
bool sd_fx_guard_suffix_is_fill(
    const sd_guarded_buffer_t *buffer,
    size_t offset);
/* Compare against what the card model says those blocks contain. */
bool sd_fx_guard_matches_card(
    const sd_guarded_buffer_t *buffer,
    uint64_t first_lba);

/* --------------------------------------------------- state assertions */

typedef struct {
    bool bus_released;      /* chip select high and the card deselected */
    bool spi_initialized;
    bool driver_initialized;
    bool removal_latched;
    bool card_irq_registered;
    bool card_irq_enabled;
    size_t spi_deinit_count;
    size_t pending_card_bytes; /* scripted bytes the driver left unread */
} sd_fx_state_t;

sd_fx_state_t sd_fx_state(sd_fixture_t *fx);

/* The invariant every completed transaction owes the caller regardless of
 * whether it succeeded: chip select is high and the card is not mid-response.
 * Returns a static description of the first violation, or NULL. */
const char *sd_fx_check_bus_quiescent(const sd_fixture_t *fx);

/* After a failure that is not a removal, the device must still be usable.
 * Performs a single-block read of `lba` and verifies the data. Returns NULL
 * on success or a description of what went wrong. */
const char *sd_fx_check_recovers(sd_fixture_t *fx, uint64_t lba);

/* After removal, every new operation must be refused without touching the
 * bus, and teardown must release hardware exactly once. */
const char *sd_fx_check_removed_and_teardown_once(sd_fixture_t *fx);

/* ------------------------------------------------------------ utilities */

/* Number of bus bytes the given simulated duration can carry, plus slack. */
uint64_t sd_fx_byte_budget(uint64_t elapsed_us, uint64_t slack);

/* Convenience card descriptions used across the suites. */
sd_card_desc_t sd_fx_card_sdhc(void);
sd_card_desc_t sd_fx_card_sdxc(void);
sd_card_desc_t sd_fx_card_v1_sdsc(void);
sd_card_desc_t sd_fx_card_v2_sdsc(void);

#endif
