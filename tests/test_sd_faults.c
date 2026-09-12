/*
 * Fault injection, partial-operation semantics and recovery.
 *
 * The question every case here asks is not "does an error come back" but
 * "what state is the driver, the bus and the caller's buffer in afterwards,
 * and can the device be used again". A driver that returns the right code
 * while leaving chip select asserted, half-filling a destination, or becoming
 * permanently unusable would pass a return-code-only suite.
 */
#include <inttypes.h>
#include <string.h>

#include "hardware/gpio.h"
#include "sd_card_model.h"
#include "sd_fixture.h"
#include "test_harness.h"

/* Budgets the driver declares for itself, in microseconds. Any single read
 * must finish inside the sum of the waits it can legitimately perform.
 * sd_spi_command() waits for a ready card before every frame; teardown, the
 * wait after CMD12's R1b and the R1-error cleanup use the shorter generic
 * wait. Neither is a specification value; see PROTOCOL.md P-16. */
enum {
    DRIVER_COMMAND_READY_WAIT_US = 500000,
    DRIVER_READY_WAIT_US = 250000,
    DRIVER_DATA_WAIT_US = 100000,
};

static const char *phase_label(sd_phase_t phase)
{
    return sd_phase_name(phase);
}

static const char *fault_label(sd_fault_kind_t kind)
{
    switch (kind) {
    case SD_FAULT_NONE: return "none";
    case SD_FAULT_STALL: return "stall(0xFF forever)";
    case SD_FAULT_BUSY_FOREVER: return "busy(0x00 forever)";
    case SD_FAULT_DATA_ERROR_TOKEN: return "data-error-token";
    case SD_FAULT_SUBSTITUTE_R1: return "substitute-r1";
    case SD_FAULT_GARBAGE: return "garbage-byte";
    case SD_FAULT_FLIP_BITS: return "flip-bits";
    case SD_FAULT_TRUNCATE: return "truncate";
    case SD_FAULT_BAD_DATA_CRC: return "bad-data-crc";
    case SD_FAULT_EJECT: return "eject";
    case SD_FAULT_EXTRA_BUSY_US: return "extra-busy";
    default: return "?";
    }
}

/* ------------------------------------------------ the invariant sweep */

/*
 * Every fault at every meaningful phase of a read, checked against the
 * invariants that hold no matter what the card did. This is the reusable
 * core: adding a new phase or a new fault kind to the model extends the
 * sweep automatically, so future work inherits the coverage.
 */
static void sweep_read_faults(size_t blocks)
{
    static const sd_phase_t phases[] = {
        SD_PHASE_RESPONSE_WAIT,
        SD_PHASE_R1,
        SD_PHASE_DATA_WAIT,
        SD_PHASE_DATA_TOKEN,
        SD_PHASE_DATA_PAYLOAD,
        SD_PHASE_DATA_CRC,
        SD_PHASE_BETWEEN_BLOCKS,
        SD_PHASE_STOP_STUFF,
    };
    static const struct { sd_fault_kind_t kind; uint32_t param; } faults[] = {
        { SD_FAULT_STALL, 0U },
        { SD_FAULT_BUSY_FOREVER, 0U },
        { SD_FAULT_GARBAGE, 0x5AU },
        { SD_FAULT_TRUNCATE, 0U },
        { SD_FAULT_FLIP_BITS, 0xFFU },
    };
    static const uint32_t offsets[] = { 0U, 1U, 200U };

    const uint8_t command = blocks == 1U ? 17U : 18U;

    for (size_t p = 0U; p < sizeof(phases) / sizeof(phases[0]); ++p) {
        for (size_t f = 0U; f < sizeof(faults) / sizeof(faults[0]); ++f) {
            for (size_t o = 0U; o < sizeof(offsets) / sizeof(offsets[0]); ++o) {
                /* Only the payload phase is long enough for a deep offset. */
                if (offsets[o] > 1U && phases[p] != SD_PHASE_DATA_PAYLOAD) {
                    continue;
                }
                sd_fixture_t fx;
                sd_card_desc_t desc = sd_fx_card_sdhc();
                t_context("blocks=%zu phase=%s fault=%s offset=%u",
                    blocks, phase_label(phases[p]),
                    fault_label(faults[f].kind), (unsigned)offsets[o]);
                T_CHECK(sd_fx_require_init(&fx, &desc));

                sd_fault_t fault;
                memset(&fault, 0, sizeof(fault));
                fault.phase = phases[p];
                fault.command = command;
                fault.occurrence = 0U;
                fault.byte_offset = offsets[o];
                fault.kind = faults[f].kind;
                fault.param = faults[f].param;
                T_CHECK(sd_card_add_fault(&fault));

                sd_guarded_buffer_t buffer;
                sd_fx_guard_init(&buffer, blocks);
                const uint64_t start_us = pico_mock_now_us();
                const block_device_result_t result = block_device_read_blocks(
                    fx.device, 42U, sd_fx_guard_data(&buffer), blocks);
                const uint64_t elapsed_us = pico_mock_now_us() - start_us;

                /* 1. The call returns, and inside the budgets the driver
                 *    declares: one pre-command wait, at most two generic
                 *    waits (after CMD12, and the R1-error cleanup), one data
                 *    wait per block plus one stop, with slack for the release
                 *    clocks. */
                const uint64_t budget = (uint64_t)DRIVER_COMMAND_READY_WAIT_US
                    + (uint64_t)DRIVER_READY_WAIT_US * 2U
                    + (uint64_t)DRIVER_DATA_WAIT_US * (blocks + 1U)
                    + UINT64_C(10000);
                T_CHECK(elapsed_us <= budget);

                /* 2. Chip select is released and the card is not mid-answer. */
                const char *problem = sd_fx_check_bus_quiescent(&fx);
                if (problem != NULL) {
                    t_context("blocks=%zu phase=%s fault=%s offset=%u: %s",
                        blocks, phase_label(phases[p]),
                        fault_label(faults[f].kind), (unsigned)offsets[o],
                        problem);
                }
                T_CHECK(problem == NULL);

                /* 3. Nothing was written outside the destination. */
                T_CHECK(sd_fx_guard_intact(&buffer));

                /* 4. On success the data must be right. The driver now
                 *    validates the data CRC16 the card sends after each
                 *    block (SD-003 closed), so a fault that rewrites payload
                 *    or CRC bytes must surface as an error rather than as a
                 *    successful read of the wrong bytes. Every fault kind in
                 *    the table substitutes bytes when injected at
                 *    DATA_PAYLOAD or DATA_CRC - STALL answers 0xFF onward,
                 *    BUSY_FOREVER 0x00 onward, GARBAGE substitutes, FLIP_BITS
                 *    alters and TRUNCATE ends the payload and idles at 0xFF -
                 *    so none of those rows may report OK, with one exception
                 *    the specification's CRC cannot close: CRC-16/XMODEM
                 *    starts from a zero register with no final XOR, so a
                 *    512-byte block of zeros has CRC 0x0000. A bus stuck low
                 *    from the first payload byte therefore delivers a
                 *    self-consistent frame that is indistinguishable from a
                 *    legitimately erased block, and BUSY_FOREVER at payload
                 *    offset 0 is expected to read as OK (RESIDUAL_RISK.md).
                 *    From any later offset the real prefix makes the running
                 *    CRC nonzero and the zero CRC bytes are rejected. The
                 *    1-in-65536 chance of some other corruption matching its
                 *    CRC does not apply: the model's data is deterministic,
                 *    so a row either always collides or never does. */
                const bool stuck_low_from_payload_start =
                    phases[p] == SD_PHASE_DATA_PAYLOAD
                    && faults[f].kind == SD_FAULT_BUSY_FOREVER
                    && offsets[o] == 0U;
                if (phases[p] == SD_PHASE_DATA_PAYLOAD
                        || phases[p] == SD_PHASE_DATA_CRC) {
                    if (stuck_low_from_payload_start && blocks == 1U) {
                        /* Pinned in the direction it actually goes so a
                         * change here has to be deliberate. A stream fails
                         * later instead: the next token never arrives. */
                        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, result);
                    } else {
                        T_CHECK(result != BLOCK_DEVICE_RESULT_OK);
                    }
                }
                if (result == BLOCK_DEVICE_RESULT_OK
                        && !stuck_low_from_payload_start) {
                    T_CHECK(sd_fx_guard_matches_card(&buffer, 42U));
                }

                /* 5. The driver must not be wedged: the SPI peripheral is
                 *    still owned and the next read succeeds. */
                T_CHECK(pico_mock_spi_is_initialized());
                T_CHECK(fx.sd.initialized);
                const char *recovery = sd_fx_check_recovers(&fx, 9U);
                if (recovery != NULL) {
                    t_context("blocks=%zu phase=%s fault=%s offset=%u: %s",
                        blocks, phase_label(phases[p]),
                        fault_label(faults[f].kind), (unsigned)offsets[o],
                        recovery);
                }
                T_CHECK(recovery == NULL);

                /* 6. And teardown still releases the hardware exactly once. */
                const size_t deinits = pico_mock_spi_deinit_count();
                T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
                    block_device_deinit(fx.device));
                T_EQ_U(deinits + 1U, pico_mock_spi_deinit_count());
            }
        }
    }
    t_clear_context();
}

static void test_single_block_fault_sweep(void) { sweep_read_faults(1U); }
static void test_multi_block_fault_sweep(void) { sweep_read_faults(3U); }

/* ------------------------------------------- partial-operation semantics */

static void test_failure_after_partial_success(void)
{
    /* A multiple-block read that fails on a later block is the case where a
     * driver is most likely to leak a half-populated destination to the
     * caller. The block-device contract says the whole destination is
     * unspecified after a failure, so the test does not demand particular
     * contents - but it does record exactly how much was written, so a change
     * in that behaviour has to be deliberate, and it proves the failure is
     * reported rather than swallowed. */
    for (uint32_t failing_block = 0U; failing_block < 4U; ++failing_block) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("error token on block %u of 4", (unsigned)failing_block);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        sd_fault_t fault;
        memset(&fault, 0, sizeof(fault));
        fault.phase = SD_PHASE_DATA_TOKEN;
        fault.command = 18U;
        fault.occurrence = failing_block;
        fault.kind = SD_FAULT_DATA_ERROR_TOKEN;
        fault.param = 0x08U; /* OUT_OF_RANGE */
        T_CHECK(sd_card_add_fault(&fault));

        sd_guarded_buffer_t buffer;
        sd_fx_guard_init(&buffer, 4U);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, block_device_read_blocks(
            fx.device, 100U, sd_fx_guard_data(&buffer), 4U));
        T_EQ_U(1U, sd_card_fault_activations(0U));

        /* Exactly the blocks before the failure reached the destination, and
         * not one byte more: the failing block contributed nothing. */
        T_EQ_U((unsigned long long)failing_block * SD_FX_BLOCK,
            (unsigned long long)sd_fx_guard_prefix_from_card(&buffer, 100U));
        T_CHECK(sd_fx_guard_suffix_is_fill(&buffer,
            (size_t)failing_block * SD_FX_BLOCK));
        T_CHECK(sd_fx_guard_intact(&buffer));
        /* The transfer was terminated properly and the card released. */
        T_EQ_U(1U, (unsigned)sd_card_command_count(12U));
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
        T_CHECK(sd_fx_check_recovers(&fx, 7U) == NULL);
    }
    t_clear_context();
}

static void test_stop_transmission_failure_after_good_data(void)
{
    /* Every block arrived correctly and only the CMD12 that ends the transfer
     * failed. The driver must not report success: the card has not left the
     * data-transfer state, so the next command would be issued into an
     * undefined state. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    sd_fault_t fault;
    memset(&fault, 0, sizeof(fault));
    fault.phase = SD_PHASE_R1;
    fault.command = 12U;
    fault.kind = SD_FAULT_SUBSTITUTE_R1;
    fault.param = 0x40U; /* parameter error */
    T_CHECK(sd_card_add_fault(&fault));

    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, 3U);
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, block_device_read_blocks(
        fx.device, 200U, sd_fx_guard_data(&buffer), 3U));
    T_EQ_U(1U, sd_card_fault_activations(0U));
    /* All three blocks were transferred before the stop failed, so the
     * destination is fully populated even though the call reports failure.
     * Callers must discard it: the block-device contract makes the whole
     * destination unspecified after any non-OK read. */
    T_EQ_U(3U * SD_FX_BLOCK,
        (unsigned long long)sd_fx_guard_prefix_from_card(&buffer, 200U));
    T_CHECK(sd_fx_guard_intact(&buffer));
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    T_CHECK(sd_fx_check_recovers(&fx, 1U) == NULL);
}

static void test_busy_at_each_phase_is_bounded(void)
{
    /* A card that goes busy and never releases must cost the driver exactly
     * one declared wait, and must produce BUSY_TIMEOUT rather than a generic
     * I/O error where the driver has said it distinguishes the two. */
    const struct {
        const char *name;
        sd_phase_t phase;
        uint8_t command;
        block_device_result_t expected;
        uint64_t min_us;
    } rows[] = {
        { "before the read command", SD_PHASE_NONE, SD_ANY_COMMAND,
          BLOCK_DEVICE_RESULT_BUSY_TIMEOUT, DRIVER_COMMAND_READY_WAIT_US },
        { "during the stop response", SD_PHASE_R1, 12U,
          BLOCK_DEVICE_RESULT_BUSY_TIMEOUT, DRIVER_READY_WAIT_US },
    };

    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("busy %s", rows[i].name);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        if (rows[i].phase == SD_PHASE_NONE) {
            pico_mock_sd_set_busy_forever();
        } else {
            sd_fault_t fault;
            memset(&fault, 0, sizeof(fault));
            fault.phase = rows[i].phase;
            fault.command = rows[i].command;
            fault.kind = SD_FAULT_BUSY_FOREVER;
            T_CHECK(sd_card_add_fault(&fault));
        }

        sd_guarded_buffer_t buffer;
        sd_fx_guard_init(&buffer, 2U);
        const uint64_t start_us = pico_mock_now_us();
        T_EQ_RESULT(rows[i].expected, block_device_read_blocks(
            fx.device, 3U, sd_fx_guard_data(&buffer), 2U));
        const uint64_t elapsed_us = pico_mock_now_us() - start_us;
        T_CHECK(elapsed_us >= rows[i].min_us);
        /* One wait, not several: a driver that retried would take multiples. */
        T_CHECK(elapsed_us < rows[i].min_us * 2U);
        T_CHECK(sd_fx_guard_intact(&buffer));
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    }
    t_clear_context();
}

/* ------------------------------------------------------ removal faults */

static void test_deinit_busy_timeout_can_be_retried(void)
{
    /* The architecture states that a failed clean shutdown deliberately leaves
     * the device initialised and its hardware configured, so shutdown can be
     * retried rather than being half torn down. Assert the retry actually
     * works: a BUSY_TIMEOUT must leave a device that still reads, and that
     * tears down successfully once the card is ready. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    pico_mock_sd_set_busy_forever();
    const uint64_t start_us = pico_mock_now_us();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_BUSY_TIMEOUT,
        block_device_deinit(fx.device));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    T_CHECK(elapsed_us >= (uint64_t)DRIVER_READY_WAIT_US);
    T_CHECK(elapsed_us < (uint64_t)DRIVER_READY_WAIT_US * 2U);

    /* Nothing was released. */
    T_CHECK(fx.sd.initialized);
    T_CHECK(pico_mock_spi_is_initialized());
    T_EQ_U(0U, pico_mock_spi_deinit_count());
    T_CHECK(pico_mock_gpio_irq_is_registered(SD_FX_PIN_CARD_DETECT));
    T_CHECK(!pico_mock_gpio_was_deinitialized(SD_FX_PIN_CS));
    T_CHECK(pico_mock_gpio_level(SD_FX_PIN_CS));

    /* The device is still usable, and the retry succeeds once the card is. */
    sd_card_set_busy_bytes(0U);
    T_CHECK(sd_fx_check_recovers(&fx, 4U) == NULL);
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_deinit(fx.device));
    T_EQ_U(1U, pico_mock_spi_deinit_count());
    T_CHECK(!fx.sd.initialized);
    T_CHECK(pico_mock_gpio_was_deinitialized(SD_FX_PIN_CS));
    T_CHECK(!pico_mock_gpio_irq_is_registered(SD_FX_PIN_CARD_DETECT));

    /* And a third call changes nothing. */
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_deinit(fx.device));
    T_EQ_U(1U, pico_mock_spi_deinit_count());
}

static void test_removal_at_each_read_phase(void)
{
    /* The card-detect edge fires from inside a phase rather than at a byte
     * index, so the case stays meaningful if the driver's byte layout changes.
     * Every one must fail with INVALID_DEVICE, must not populate the
     * destination, must not send CMD12 to an absent card, and must leave a
     * device that can be torn down exactly once. */
    static const sd_phase_t phases[] = {
        SD_PHASE_RESPONSE_WAIT,
        SD_PHASE_R1,
        SD_PHASE_DATA_TOKEN,
        SD_PHASE_DATA_PAYLOAD,
        SD_PHASE_DATA_CRC,
        SD_PHASE_BETWEEN_BLOCKS,
    };

    for (size_t blocks = 1U; blocks <= 2U; ++blocks) {
        for (size_t p = 0U; p < sizeof(phases) / sizeof(phases[0]); ++p) {
            if (blocks == 1U && phases[p] == SD_PHASE_BETWEEN_BLOCKS) {
                continue;
            }
            sd_fixture_t fx;
            sd_card_desc_t desc = sd_fx_card_sdhc();
            t_context("removal during %s of a %zu-block read",
                phase_label(phases[p]), blocks);
            T_CHECK(sd_fx_require_init(&fx, &desc));

            sd_fault_t fault;
            memset(&fault, 0, sizeof(fault));
            fault.phase = phases[p];
            fault.command = blocks == 1U ? 17U : 18U;
            fault.byte_offset = phases[p] == SD_PHASE_DATA_PAYLOAD ? 100U : 0U;
            fault.kind = SD_FAULT_EJECT;
            T_CHECK(sd_card_add_fault(&fault));

            sd_guarded_buffer_t buffer;
            sd_fx_guard_init(&buffer, blocks);
            const size_t cmd12_before = sd_card_command_count(12U);
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
                block_device_read_blocks(
                    fx.device, 55U, sd_fx_guard_data(&buffer), blocks));

            T_EQ_U(1U, sd_card_fault_activations(0U));
            T_CHECK(atomic_load(&fx.sd.removal_latched));
            T_CHECK(sd_fx_guard_intact(&buffer));
            /* No stop command may be sent to a card that is no longer there. */
            T_EQ_U(cmd12_before, sd_card_command_count(12U));
            /* The interrupt handler must not tear anything down itself. */
            T_CHECK(pico_mock_spi_is_initialized());
            T_EQ_U(0U, pico_mock_spi_deinit_count());
            T_CHECK(!pico_mock_gpio_irq_is_enabled(SD_FX_PIN_CARD_DETECT));
            T_CHECK(pico_mock_gpio_level(SD_FX_PIN_CS));

            const char *problem = sd_fx_check_removed_and_teardown_once(&fx);
            if (problem != NULL) {
                t_context("removal during %s of a %zu-block read: %s",
                    phase_label(phases[p]), blocks, problem);
            }
            T_CHECK(problem == NULL);
        }
    }
    t_clear_context();
}

static void test_removal_during_the_release_clock(void)
{
    /* The last thing a read does is raise chip select and clock one more byte.
     * A removal edge landing on that byte arrives while the operation is still
     * executing, so the architecture requires the transfer to fail: a late
     * completion must never turn a cancelled request into a success. This was
     * a reproduced defect (KNOWN_GAPS.md SD-001, now fixed); the case is
     * expressed against the release phase rather than a byte index so it stays
     * valid if the driver's byte layout changes. */
    for (size_t blocks = 1U; blocks <= 3U; blocks += 2U) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("%zu-block read, removal during the release clock", blocks);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        sd_fault_t fault;
        memset(&fault, 0, sizeof(fault));
        fault.phase = SD_PHASE_RELEASE;
        fault.command = SD_ANY_COMMAND;
        /* Bring-up also clocks bytes with chip select released, so target the
         * next entry into the phase rather than the first. */
        fault.occurrence = sd_card_phase_entries(SD_PHASE_RELEASE);
        fault.kind = SD_FAULT_EJECT;
        T_CHECK(sd_card_add_fault(&fault));

        sd_guarded_buffer_t buffer;
        sd_fx_guard_init(&buffer, blocks);
        const block_device_result_t result = block_device_read_blocks(
            fx.device, 21U, sd_fx_guard_data(&buffer), blocks);
        if (sd_card_fault_activations(0U) == 0U) {
            /* If the fault never fired the case proves nothing; say so rather
             * than passing silently. */
            t_context("%zu-block read: the release-phase fault never fired",
                blocks);
            T_CHECK(false);
        }
        T_CHECK(atomic_load(&fx.sd.removal_latched));
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_DEVICE, result);
        T_CHECK(sd_fx_guard_intact(&buffer));
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
        T_CHECK(sd_fx_check_removed_and_teardown_once(&fx) == NULL);
    }
    t_clear_context();
}

static void test_ocr_reports_card_still_powering_up(void)
{
    /* CMD58 returns the OCR. Bit 31 is the power-up status: a card that is
     * still initialising leaves it clear. Reaching CMD58 at all means ACMD41
     * already reported ready, so the two disagree - the card is in a state the
     * driver cannot rely on and bring-up must fail rather than proceed to read
     * the CSD of a card that is not up. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    sd_fx_begin(&fx, &desc);

    sd_fault_t fault;
    memset(&fault, 0, sizeof(fault));
    fault.phase = SD_PHASE_TRAILER;
    fault.command = 58U;
    fault.byte_offset = 0U; /* the OCR's most significant byte */
    fault.kind = SD_FAULT_GARBAGE;
    fault.param = 0x40U;    /* CCS set, power-up status clear */
    T_CHECK(sd_card_add_fault(&fault));

    T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, sd_fx_init(&fx));
    T_EQ_U(1U, sd_card_fault_activations(0U));
    T_CHECK(!fx.sd.initialized);
    T_EQ_U(0U, fx.sd.block_count);
    /* The CSD must not have been requested for a card that is not up. */
    T_EQ_U(0U, (unsigned)sd_card_command_count(9U));
    T_CHECK(!pico_mock_spi_is_initialized());
    T_CHECK(pico_mock_gpio_level(SD_FX_PIN_CS));
}

static void test_reinsertion_requires_fresh_initialization(void)
{
    /* Bounce and reinsertion: after removal the latch must survive until a
     * fresh initialisation clears it, and that initialisation must re-arm the
     * card-detect interrupt. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    sd_fault_t fault;
    memset(&fault, 0, sizeof(fault));
    fault.phase = SD_PHASE_DATA_PAYLOAD;
    fault.command = 17U;
    fault.byte_offset = 10U;
    fault.kind = SD_FAULT_EJECT;
    T_CHECK(sd_card_add_fault(&fault));

    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, 1U);
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_read_blocks(fx.device, 0U, sd_fx_guard_data(&buffer), 1U));
    T_CHECK(sd_fx_check_removed_and_teardown_once(&fx) == NULL);

    /* Media comes back: the availability line goes low again. */
    sd_card_clear_faults();
    sd_card_reset(&desc);
    sd_card_set_response_policy(SD_RESPONSE_MODELLED);
    pico_mock_gpio_set_input(SD_FX_PIN_CARD_DETECT, false);

    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_init(fx.device));
    T_CHECK(!atomic_load(&fx.sd.removal_latched));
    T_CHECK(pico_mock_gpio_irq_is_registered(SD_FX_PIN_CARD_DETECT));
    T_CHECK(pico_mock_gpio_irq_is_enabled(SD_FX_PIN_CARD_DETECT));
    T_CHECK(sd_fx_check_recovers(&fx, 3U) == NULL);
}

/* ------------------------------------------------------- data integrity */

/*
 * Read-path CRC validation (formerly gap SD-003). The card model computes a
 * real CRC16-CCITT over the block it sends and appends it MSB first; the
 * driver folds every payload byte through crc_helper_rolling_16() and
 * compares against the two bytes that follow. Each row corrupts one thing -
 * a payload byte at the start, middle or end of a block, or the CRC bytes
 * themselves - on CMD17 and on a chosen block of a CMD18 stream, and demands
 * the same outcome: a detected error, a quiescent bus, nothing written past
 * the destination, no protocol error recorded by the card (the CMD12 the
 * driver sends after a mismatch must be well formed) and a device that still
 * works afterwards.
 */
typedef struct {
    const char *name;
    size_t blocks;
    uint32_t occurrence;   /* which block of the stream is corrupted */
    sd_phase_t phase;
    uint32_t byte_offset;
    sd_fault_kind_t kind;
    uint32_t param;
} crc_row_t;

static void check_corruption_is_detected(
    const crc_row_t *row, sd_card_desc_t (*make)(void), const char *card)
{
    sd_fixture_t fx;
    sd_card_desc_t desc = make();
    t_context("%s: %s (blocks=%zu block=%u phase=%s offset=%u)",
        card, row->name, row->blocks, (unsigned)row->occurrence,
        phase_label(row->phase), (unsigned)row->byte_offset);
    T_CHECK(sd_fx_require_init(&fx, &desc));

    sd_fault_t fault;
    memset(&fault, 0, sizeof(fault));
    fault.phase = row->phase;
    fault.command = row->blocks == 1U ? 17U : 18U;
    fault.occurrence = row->occurrence;
    fault.byte_offset = row->byte_offset;
    fault.kind = row->kind;
    fault.param = row->param;
    T_CHECK(sd_card_add_fault(&fault));

    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, row->blocks);
    const block_device_result_t result = block_device_read_blocks(
        fx.device, 12U, sd_fx_guard_data(&buffer), row->blocks);

    /* The fault fired exactly once, so the read really saw corruption. */
    T_EQ_U(1U, sd_card_fault_activations(0U));
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, result);
    T_CHECK(sd_fx_guard_intact(&buffer));
    T_EQ_U(0U, sd_card_protocol_errors());
    const char *problem = sd_fx_check_bus_quiescent(&fx);
    if (problem != NULL) {
        t_context("%s: %s: %s", card, row->name, problem);
    }
    T_CHECK(problem == NULL);
    const char *recovery = sd_fx_check_recovers(&fx, 12U);
    if (recovery != NULL) {
        t_context("%s: %s: %s", card, row->name, recovery);
    }
    T_CHECK(recovery == NULL);
}

static void test_read_data_crc_is_validated(void)
{
    static const crc_row_t rows[] = {
        /* CMD17: a single bit in the first, a middle and the last payload
         * byte; the whole of a byte; and each CRC byte on its own. */
        { "first payload byte, one bit", 1U, 0U,
            SD_PHASE_DATA_PAYLOAD, 0U, SD_FAULT_FLIP_BITS, 0x01U },
        { "middle payload byte, all bits", 1U, 0U,
            SD_PHASE_DATA_PAYLOAD, 3U, SD_FAULT_FLIP_BITS, 0xFFU },
        { "last payload byte, top bit", 1U, 0U,
            SD_PHASE_DATA_PAYLOAD, 511U, SD_FAULT_FLIP_BITS, 0x80U },
        { "CRC high byte, one bit", 1U, 0U,
            SD_PHASE_DATA_CRC, 0U, SD_FAULT_FLIP_BITS, 0x01U },
        { "CRC low byte, one bit", 1U, 0U,
            SD_PHASE_DATA_CRC, 1U, SD_FAULT_FLIP_BITS, 0x01U },
        /* The payload arrives intact but the card's CRC is wrong: the
         * driver cannot tell which side is at fault and must still reject. */
        { "intact payload, inverted CRC", 1U, 0U,
            SD_PHASE_DATA_PAYLOAD, 0U, SD_FAULT_BAD_DATA_CRC, 0U },
        /* CMD18, four blocks: the first, a middle and the last block of the
         * stream, plus a bad CRC on a middle block. */
        { "stream block 0, one payload bit", 4U, 0U,
            SD_PHASE_DATA_PAYLOAD, 17U, SD_FAULT_FLIP_BITS, 0x01U },
        { "stream block 1, last payload byte", 4U, 1U,
            SD_PHASE_DATA_PAYLOAD, 511U, SD_FAULT_FLIP_BITS, 0x01U },
        { "stream block 3, first payload byte", 4U, 3U,
            SD_PHASE_DATA_PAYLOAD, 0U, SD_FAULT_FLIP_BITS, 0x40U },
        { "stream block 2, CRC low byte", 4U, 2U,
            SD_PHASE_DATA_CRC, 1U, SD_FAULT_FLIP_BITS, 0x10U },
        { "stream block 2, inverted CRC", 4U, 2U,
            SD_PHASE_DATA_PAYLOAD, 100U, SD_FAULT_BAD_DATA_CRC, 0U },
    };
    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        check_corruption_is_detected(&rows[i], sd_fx_card_sdhc, "SDHC");
    }
    /* The check does not depend on the addressing mode; one byte-addressed
     * card, single and multiple block, is enough to show that. */
    check_corruption_is_detected(&rows[1], sd_fx_card_v1_sdsc, "v1 SDSC");
    check_corruption_is_detected(&rows[8], sd_fx_card_v1_sdsc, "v1 SDSC");
    t_clear_context();
}

static void test_read_data_crc_mismatch_stops_the_stream_only_once(void)
{
    /* On a mismatch mid-stream the driver must abort with exactly one CMD12
     * and release the bus; the blocks before the bad one had already been
     * delivered, which the contract allows to remain but does not require.
     * Pin the exact frame sequence so an accidental second stop, or a
     * missing one that leaves the card streaming, cannot pass. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    sd_fault_t fault;
    memset(&fault, 0, sizeof(fault));
    fault.phase = SD_PHASE_DATA_PAYLOAD;
    fault.command = 18U;
    fault.occurrence = 1U;
    fault.byte_offset = 5U;
    fault.kind = SD_FAULT_FLIP_BITS;
    fault.param = 0x01U;
    T_CHECK(sd_card_add_fault(&fault));

    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, 4U);
    const size_t before = sd_card_trace_length();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, block_device_read_blocks(
        fx.device, 20U, sd_fx_guard_data(&buffer), 4U));

    /* Command frames after the read began: CMD18 then exactly one CMD12. */
    unsigned int cmd18 = 0U, cmd12 = 0U, other = 0U, blocks_sent = 0U;
    for (size_t i = before; i < sd_card_trace_length(); ++i) {
        const sd_event_t *ev = sd_card_trace_at(i);
        if (ev->kind == SD_EV_COMMAND) {
            if (ev->command == 18U) {
                cmd18++;
            } else if (ev->command == 12U) {
                cmd12++;
            } else {
                other++;
            }
        } else if (ev->kind == SD_EV_BLOCK_SENT) {
            blocks_sent++;
        }
    }
    T_EQ_U(1U, cmd18);
    T_EQ_U(1U, cmd12);
    T_EQ_U(0U, other);
    /* The card had sent two blocks (good, then corrupt) when it was told to
     * stop; no third block was consumed. */
    T_EQ_U(2U, blocks_sent);
    T_EQ_U(1U, sd_fx_guard_prefix_from_card(&buffer, 20U) / SD_FX_BLOCK);
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    T_CHECK(sd_fx_check_recovers(&fx, 20U) == NULL);
}

/* --------------------------------------------------------------- main */

int main(void)
{
    t_run(test_single_block_fault_sweep, "fault sweep across single-block read phases");
    t_run(test_multi_block_fault_sweep, "fault sweep across multi-block read phases");
    t_run(test_failure_after_partial_success, "error after N good blocks of a stream");
    t_run(test_stop_transmission_failure_after_good_data, "CMD12 failure after all data arrived");
    t_run(test_busy_at_each_phase_is_bounded, "busy periods cost one declared wait");
    t_run(test_deinit_busy_timeout_can_be_retried, "a busy-timeout teardown is retryable");
    t_run(test_removal_at_each_read_phase, "removal injected at each read phase");
    t_run(test_removal_during_the_release_clock, "removal during the release clock cancels success");
    t_run(test_ocr_reports_card_still_powering_up, "OCR power-up status is checked");
    t_run(test_reinsertion_requires_fresh_initialization, "reinsertion needs a fresh init");
    t_run(test_read_data_crc_is_validated, "read data CRC is validated on CMD17 and CMD18");
    t_run(test_read_data_crc_mismatch_stops_the_stream_only_once,
        "a mid-stream CRC mismatch is aborted with exactly one CMD12");
    return t_summary("sd_faults");
}
