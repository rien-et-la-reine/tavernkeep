/*
 * SD SPI protocol conformance, driven by the stateful card model.
 *
 * Every case here answers a question the original harness could not ask,
 * because its fake could not produce the behaviour involved. Where a rule
 * comes from the specification, PROTOCOL.md records the source and the
 * finding number quoted in the comment.
 */
#include <inttypes.h>
#include <string.h>

#include "hardware/gpio.h"
#include "sd_card_model.h"
#include "sd_fixture.h"
#include "test_harness.h"

/* ------------------------------------------------------------- helpers */

static bool command_sequence_is(const uint8_t *expected, size_t length)
{
    uint8_t actual[32];
    const size_t count = sd_card_trace_command_sequence(actual, 32U);
    if (count != length) {
        return false;
    }
    return memcmp(actual, expected, length) == 0;
}

/* BLOCK_SENT also fires for the 16-byte CSD register read during bring-up, so
 * a raw count would silently include it. Filter by the command that produced
 * the block. */
static size_t blocks_streamed_for(uint8_t command)
{
    size_t count = 0U;
    for (size_t i = 0U; i < sd_card_trace_length(); ++i) {
        const sd_event_t *const event = sd_card_trace_at(i);
        if (event->kind == SD_EV_BLOCK_SENT && event->command == command) {
            count++;
        }
    }
    return count;
}

static const sd_event_t *nth_block_sent_for(uint8_t command, size_t n)
{
    size_t seen = 0U;
    for (size_t i = 0U; i < sd_card_trace_length(); ++i) {
        const sd_event_t *const event = sd_card_trace_at(i);
        if (event->kind != SD_EV_BLOCK_SENT || event->command != command) {
            continue;
        }
        if (seen == n) {
            return event;
        }
        seen++;
    }
    return NULL;
}

static void print_command_sequence(void)
{
    uint8_t actual[32];
    const size_t count = sd_card_trace_command_sequence(actual, 32U);
    (void)fprintf(stderr, "  observed command sequence:");
    for (size_t i = 0U; i < count && i < 32U; ++i) {
        (void)fprintf(stderr, " %u", (unsigned)actual[i]);
    }
    (void)fprintf(stderr, "%s\n", count > 32U ? " ..." : "");
}

/* ------------------------------------------------- card variant matrix */

static void test_card_variant_initialization(void)
{
    /* The driver must handle every card generation it claims to support, and
     * must refuse the two it does not. Addressing mode and the CMD16 decision
     * are properties of the card, so each row pins both. */
    const struct {
        const char *name;
        sd_card_desc_t desc;
        block_device_result_t expected;
        bool legacy;
        bool hcxc;
        bool expect_cmd16;
    } rows[] = {
        { "v1 SDSC 1GB", sd_fx_card_v1_sdsc(),
          BLOCK_DEVICE_RESULT_OK, true, false, true },
        { "v2 SDSC 2GB", sd_fx_card_v2_sdsc(),
          BLOCK_DEVICE_RESULT_OK, false, false, true },
        { "SDHC 8GB", sd_fx_card_sdhc(),
          BLOCK_DEVICE_RESULT_OK, false, true, false },
        { "SDXC 64GB", sd_fx_card_sdxc(),
          BLOCK_DEVICE_RESULT_OK, false, true, false },
        { "SDUC 4TB (CSD v3)",
          sd_card_desc(SD_CARD_SDUC, UINT64_C(4) * 1024U * 1024U * 1024U * 1024U),
          BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED, false, true, false },
        { "MMC-like", sd_card_desc(SD_CARD_MMC_LIKE, UINT64_C(1) << 30U),
          BLOCK_DEVICE_RESULT_IO_ERROR, true, false, false },
    };

    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        sd_fixture_t fx;
        t_context("card=%s", rows[i].name);
        sd_fx_begin(&fx, &rows[i].desc);
        T_EQ_RESULT(rows[i].expected, sd_fx_init(&fx));

        if (rows[i].expected != BLOCK_DEVICE_RESULT_OK) {
            /* A refused card must leave nothing configured behind. */
            T_CHECK(!fx.sd.initialized);
            T_EQ_U(0U, fx.sd.block_count);
            T_CHECK(!pico_mock_spi_is_initialized());
            T_CHECK(!pico_mock_gpio_irq_is_registered(SD_FX_PIN_CARD_DETECT));
            T_CHECK(pico_mock_gpio_level(SD_FX_PIN_CS));
            continue;
        }

        T_CHECK(fx.sd.initialized);
        T_EQ_U(rows[i].legacy ? 1U : 0U, fx.sd.card_type_legacy ? 1U : 0U);
        T_EQ_U(rows[i].hcxc ? 1U : 0U, fx.sd.card_type_hcxc ? 1U : 0U);
        /* Capacity must come from the card, not from a driver default. */
        T_EQ_U(sd_card_block_count(), fx.sd.block_count);
        T_EQ_U(rows[i].expect_cmd16 ? 1U : 0U,
            (unsigned)sd_card_command_count(16U));
        if (rows[i].expect_cmd16) {
            T_EQ_U(512U, sd_card_last_argument(16U));
        }
        /* Speed must only rise after the card is up. */
        T_EQ_U(400000U, pico_mock_spi_initial_baudrate());
        T_EQ_U((unsigned)SD_FX_BAUD_HZ, pico_mock_spi_baudrate());
        T_EQ_U(0U, sd_card_protocol_errors());
    }
    t_clear_context();
}

static void test_initialization_command_order(void)
{
    /* The exact sequence matters: CMD8 must precede ACMD41 so the HCS bit can
     * be chosen, CMD58 must follow ACMD41 so CCS is meaningful, and CMD16
     * must only appear for byte-addressed cards. A command-count assertion
     * cannot see an order mistake; this can. CMD59 must also precede ACMD41 so
     * the rest of bring-up is CRC-protected. */
    static const uint8_t sdhc[] = { 0U, 8U, 59U, 55U, 41U, 58U, 9U };
    static const uint8_t sdsc[] = { 0U, 8U, 59U, 55U, 41U, 58U, 9U, 16U };

    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    t_context("SDHC");
    T_CHECK(sd_fx_require_init(&fx, &desc));
    if (!command_sequence_is(sdhc, sizeof(sdhc))) {
        print_command_sequence();
    }
    T_CHECK(command_sequence_is(sdhc, sizeof(sdhc)));

    desc = sd_fx_card_v2_sdsc();
    t_context("v2 SDSC");
    T_CHECK(sd_fx_require_init(&fx, &desc));
    if (!command_sequence_is(sdsc, sizeof(sdsc))) {
        print_command_sequence();
    }
    T_CHECK(command_sequence_is(sdsc, sizeof(sdsc)));
    t_clear_context();
}

/* ---------------------------------------------------------- CRC and ACMD */

static void test_command_framing_and_crc(void)
{
    /* P-01: in SPI mode CRC checking is off by default, but CMD0 and CMD8 are
     * still validated by the card. A driver with the wrong constant for either
     * cannot initialise a real card, and the old fake could not see it because
     * it never looked at the CRC byte. The model checks those two, so the
     * constants are now load-bearing. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    const sd_event_t *const cmd0 = sd_card_trace_nth(SD_EV_COMMAND, 0U);
    const sd_event_t *const cmd8 = sd_card_trace_nth(SD_EV_COMMAND, 1U);
    T_CHECK(cmd0 != NULL && cmd8 != NULL);
    T_EQ_U(0U, cmd0->command);
    T_CHECK(cmd0->crc_ok);
    T_EQ_U(8U, cmd8->command);
    T_CHECK(cmd8->crc_ok);
    T_EQ_U(0x1AAU, cmd8->argument);

    /* Independent oracle: recompute the frames from the specification's CRC7
     * polynomial rather than trusting either the driver or the model. */
    const uint8_t cmd0_frame[5] = { 0x40U, 0U, 0U, 0U, 0U };
    const uint8_t cmd8_frame[5] = { 0x48U, 0U, 0U, 0x01U, 0xAAU };
    T_EQ_U(0x95U, (unsigned)(((unsigned)sd_crc7(cmd0_frame, 5U) << 1U) | 1U));
    T_EQ_U(0x87U, (unsigned)(((unsigned)sd_crc7(cmd8_frame, 5U) << 1U) | 1U));

    /* And the bytes the driver actually put on the wire match them. */
    const uint8_t *const tx = pico_mock_spi_tx_log();
    const size_t logged = pico_mock_spi_tx_log_length();
    T_CHECK(logged > 32U);
    bool found_cmd0 = false;
    bool found_cmd8 = false;
    for (size_t i = 0U; i + 6U <= logged; ++i) {
        if (tx[i] == 0x40U && tx[i + 5U] == 0x95U) {
            found_cmd0 = true;
        }
        if (tx[i] == 0x48U && tx[i + 5U] == 0x87U) {
            found_cmd8 = true;
        }
    }
    T_CHECK(found_cmd0);
    T_CHECK(found_cmd8);
}

static void test_command_crc_checking_is_actually_enabled(void)
{
    /* Follow-on to SD-006. A correct crc_helper_7() is necessary but not
     * sufficient: the driver must also switch the card's checking ON, or every
     * CRC it computes is ignored and a corrupted command frame is accepted
     * silently - the failure mode the CRC exists to prevent.
     *
     * CMD59 (CRC_ON_OFF) takes [31:1] stuff bits and [0] as the CRC option,
     * 1 = on and 0 = off (Physical Layer Simplified Specification). A zero
     * argument therefore disables the checking the command exists to enable,
     * and it does so without any error the driver could notice.
     *
     * This asserts the card's own state, not the driver's, because the card is
     * what decides whether a bad frame is refused. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    /* Exactly one CMD59, and it must carry the CRC-on bit. */
    T_EQ_U(1U, (unsigned)sd_card_command_count(59U));
    t_context("CMD59 argument 0x%08lx",
        (unsigned long)sd_card_last_argument(59U));
    T_EQ_U(1U, sd_card_last_argument(59U) & 1U);
    t_clear_context();

    /* The card must be in CRC-checking mode once bring-up completes. */
    T_CHECK(sd_card_description()->crc_check_enabled);

    /* CMD59 must land before ACMD41, so the rest of bring-up is protected. */
    T_CHECK(sd_card_command_count(41U) > 0U);

    /* Nothing in the sequence may have tripped a protocol error. */
    T_EQ_U(0U, sd_card_protocol_errors());
}

static void test_acmd41_is_always_prefixed_by_cmd55(void)
{
    /* P-02: ACMD41 is only ACMD41 if CMD55 immediately precedes it. The old
     * fake keyed responses on the command index alone, so a driver that
     * dropped CMD55 still saw the ACMD41 answer. The model tracks the
     * application-command state, so a dropped CMD55 now fails the handshake. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    desc.acmd41_ready_after = 3U; /* force several retries */
    T_CHECK(sd_fx_require_init(&fx, &desc));

    size_t cmd41_seen = 0U;
    for (size_t i = 0U; i < sd_card_trace_length(); ++i) {
        const sd_event_t *const event = sd_card_trace_at(i);
        if (event->kind != SD_EV_COMMAND || event->command != 41U) {
            continue;
        }
        cmd41_seen++;
        t_context("CMD41 occurrence %zu", cmd41_seen);
        T_CHECK(event->app_command);
        /* P-03: HCS must be set for a high-capacity card and clear for a
         * legacy card, otherwise the card never leaves the idle state. */
        T_EQ_U(UINT32_C(0x40000000), event->argument);
    }
    t_clear_context();
    T_EQ_U(4U, (unsigned)cmd41_seen); /* three idle answers, then ready */
    T_EQ_U(cmd41_seen, sd_card_command_count(55U));
}

static void test_cmd8_response_is_matched_exactly(void)
{
    /* A v1 card answers CMD8 with illegal-command plus idle, which is 0x05,
     * and the driver tests for exactly that value. Any other R1 - including
     * one that still carries the illegal-command bit alongside another, such
     * as an erase-reset flag - is refused rather than read as "this is a v1
     * card". That strictness is deliberate as a contract, but it is a contract
     * worth pinning: a driver that widened the test to (r1 & 0x04) would
     * behave differently on a card that sets an extra bit, and nothing else in
     * the suite would notice the change. */
    for (unsigned int r1 = 0U; r1 < 0x80U; ++r1) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("CMD8 answers R1=0x%02x", r1);
        sd_fx_begin(&fx, &desc);

        sd_fault_t fault;
        memset(&fault, 0, sizeof(fault));
        fault.phase = SD_PHASE_R1;
        fault.command = 8U;
        fault.kind = SD_FAULT_SUBSTITUTE_R1;
        fault.param = r1;
        T_CHECK(sd_card_add_fault(&fault));

        const block_device_result_t result = sd_fx_init(&fx);
        if (r1 == 0x01U) {
            /* The normal v2 path: R7 follows and the card is high capacity. */
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, result);
            T_CHECK(!fx.sd.card_type_legacy);
        } else if (r1 == 0x05U) {
            /* The v1 path. The model is still a high-capacity card, so its
             * CSD disagrees with the legacy conclusion and bring-up fails -
             * which is itself the CSD cross-check doing its job. */
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, result);
        } else {
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, result);
        }
        T_CHECK(!fx.sd.initialized || result == BLOCK_DEVICE_RESULT_OK);
        T_CHECK(pico_mock_gpio_level(SD_FX_PIN_CS));
    }
    t_clear_context();
}

static void test_legacy_card_omits_the_hcs_bit(void)
{
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_v1_sdsc();
    T_CHECK(sd_fx_require_init(&fx, &desc));
    T_EQ_U(0U, sd_card_last_argument(41U));
    T_CHECK(fx.sd.card_type_legacy);
}

static void test_high_capacity_card_needs_hcs_to_leave_idle(void)
{
    /* A high-capacity card that is polled with HCS clear stays idle forever.
     * The driver must therefore give up on its own 1200 ms budget rather than
     * hang, and must report a failure rather than a half-initialised device. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    sd_fx_begin(&fx, &desc);
    /* Make the card behave as if the driver had cleared HCS by refusing to
     * leave idle for the whole run. */
    desc.acmd41_ready_after = UINT32_MAX;
    sd_card_reset(&desc);
    sd_card_set_response_policy(SD_RESPONSE_MODELLED);
    pico_mock_sd_use_chip_select(SD_FX_PIN_CS);
    sd_fx_set_card_present(true);

    const uint64_t start_us = pico_mock_now_us();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, sd_fx_init(&fx));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    T_CHECK(elapsed_us >= UINT64_C(1200000));
    T_CHECK(elapsed_us < UINT64_C(1200000) + UINT64_C(40000));
    T_CHECK(!fx.sd.initialized);
    T_CHECK(pico_mock_gpio_level(SD_FX_PIN_CS));
}

/* -------------------------------------------------------- N_CR boundary */

enum { SD_DRIVER_R1_POLL_LIMIT = 8 };

static void test_response_latency_boundary(void)
{
    /* P-04: the driver polls at most eight bytes for R1, so it tolerates at
     * most seven filler bytes before the response. This pins that boundary
     * exactly from both sides, and pins that overrunning it fails fast rather
     * than by burning a timeout - the failure mode this suite exists to
     * distinguish. Whether eight reads is enough tolerance against the
     * specification's N_CR window and against real cards is a separate
     * question; see the gap case registered in KNOWN_GAPS.md. */
    for (uint32_t filler = 0U; filler <= SD_DRIVER_R1_POLL_LIMIT; ++filler) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        desc.ncr_bytes = filler;
        t_context("%u filler byte(s): R1 on read %u",
            (unsigned)filler, (unsigned)filler + 1U);
        sd_fx_begin(&fx, &desc);
        const uint64_t start_us = pico_mock_now_us();
        const block_device_result_t result = sd_fx_init(&fx);
        const uint64_t elapsed_us = pico_mock_now_us() - start_us;
        if (filler < SD_DRIVER_R1_POLL_LIMIT) {
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, result);
            T_EQ_U(sd_card_block_count(), fx.sd.block_count);
        } else {
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, result);
            /* Recognised as "no response", not waited out: the pre-command
             * ready wait alone would have cost 500 ms. */
            T_CHECK(elapsed_us < UINT64_C(100000));
            T_CHECK(!fx.sd.initialized);
            T_CHECK(pico_mock_gpio_level(SD_FX_PIN_CS));
        }
    }

    /* The same boundary must hold for a data command, not just bring-up. */
    for (uint32_t filler = 0U; filler <= SD_DRIVER_R1_POLL_LIMIT; ++filler) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        desc.ncr_bytes = 0U;
        t_context("CMD17 with %u filler byte(s)", (unsigned)filler);
        T_CHECK(sd_fx_require_init(&fx, &desc));
        T_CHECK(pico_mock_sd_set_response_delay(17U, filler));
        T_CHECK(pico_mock_sd_set_command(17U, 0x00U, NULL, 0U));
        /* Script CMD17 with the real block so a success is verifiable. */
        uint8_t payload[SD_FX_BLOCK + 3];
        payload[0] = 0xFEU;
        sd_card_fill_expected_block(0U, &payload[1]);
        const uint16_t crc = sd_crc16_ccitt(&payload[1], SD_FX_BLOCK);
        payload[SD_FX_BLOCK + 1] = (uint8_t)(crc >> 8U);
        payload[SD_FX_BLOCK + 2] = (uint8_t)crc;
        T_CHECK(pico_mock_sd_set_command(17U, 0x00U, payload, sizeof(payload)));

        sd_guarded_buffer_t buffer;
        sd_fx_guard_init(&buffer, 1U);
        const block_device_result_t result = block_device_read_blocks(
            fx.device, 0U, sd_fx_guard_data(&buffer), 1U);
        if (filler < SD_DRIVER_R1_POLL_LIMIT) {
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, result);
            T_CHECK(sd_fx_guard_matches_card(&buffer, 0U));
        } else {
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, result);
            T_CHECK(sd_fx_guard_untouched(&buffer));
        }
        T_CHECK(sd_fx_guard_intact(&buffer));
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    }
    t_clear_context();
}

/* ------------------------------------------------------- R1 error sweep */

static void test_every_nonzero_r1_fails_a_read(void)
{
    /* Only R1 == 0x00 means the command was accepted. Sweeping all 128 legal
     * R1 values kills a mask mistake such as testing (r1 & 0x7E) or (r1 !=
     * 0xFF), which a handful of hand-picked values would miss. */
    for (unsigned int command = 17U; command <= 18U; command += 1U) {
        for (unsigned int r1 = 0U; r1 < 0x80U; ++r1) {
            sd_fixture_t fx;
            sd_card_desc_t desc = sd_fx_card_sdhc();
            t_context("CMD%u R1=0x%02x", command, r1);
            T_CHECK(sd_fx_require_init(&fx, &desc));

            sd_fault_t fault;
            memset(&fault, 0, sizeof(fault));
            fault.phase = SD_PHASE_R1;
            fault.command = (uint8_t)command;
            fault.kind = SD_FAULT_SUBSTITUTE_R1;
            fault.param = r1;
            T_CHECK(sd_card_add_fault(&fault));

            sd_guarded_buffer_t buffer;
            const size_t blocks = command == 17U ? 1U : 2U;
            sd_fx_guard_init(&buffer, blocks);
            const block_device_result_t result = block_device_read_blocks(
                fx.device, 0U, sd_fx_guard_data(&buffer), blocks);

            if (r1 == 0U) {
                T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, result);
                T_CHECK(sd_fx_guard_matches_card(&buffer, 0U));
            } else {
                T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, result);
                /* The destination is unspecified after a failed read, but the
                 * driver must not have written into it at all when the card
                 * never entered the data phase. */
                T_CHECK(sd_fx_guard_untouched(&buffer));
            }
            T_CHECK(sd_fx_guard_intact(&buffer));
            T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
            T_EQ_U(1U, sd_card_fault_activations(0U));
        }
    }
    t_clear_context();
}

/* -------------------------------------------------- data error tokens */

static void test_data_error_tokens_are_recognised_immediately(void)
{
    /* P-05: a read data error token has bits 7..4 zero and at least one of
     * bits 3..0 set (out of range, card ECC failed, CC error, error). The
     * driver must recognise every combination on the byte it arrives, not
     * reach the same generic result by timing out - that is the exact failure
     * mode this suite exists to prevent. */
    for (unsigned int token = 1U; token <= 15U; ++token) {
        for (unsigned int blocks = 1U; blocks <= 2U; ++blocks) {
            sd_fixture_t fx;
            sd_card_desc_t desc = sd_fx_card_sdhc();
            t_context("token=0x%02x blocks=%u", token, blocks);
            T_CHECK(sd_fx_require_init(&fx, &desc));

            sd_fault_t fault;
            memset(&fault, 0, sizeof(fault));
            fault.phase = SD_PHASE_DATA_TOKEN;
            fault.command = blocks == 1U ? 17U : 18U;
            fault.kind = SD_FAULT_DATA_ERROR_TOKEN;
            fault.param = token;
            T_CHECK(sd_card_add_fault(&fault));

            sd_guarded_buffer_t buffer;
            sd_fx_guard_init(&buffer, blocks);
            const size_t bytes_before = pico_mock_spi_transfer_count();
            const uint64_t time_before = pico_mock_now_us();
            const block_device_result_t result = block_device_read_blocks(
                fx.device, 0U, sd_fx_guard_data(&buffer), blocks);
            const size_t bytes_used =
                pico_mock_spi_transfer_count() - bytes_before;
            const uint64_t elapsed_us = pico_mock_now_us() - time_before;

            T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, result);
            T_EQ_U(1U, sd_card_fault_activations(0U));
            /* Immediate recognition: the command frame, the response, the
             * token, an optional CMD12 and the release clock. Nothing that
             * looks like a 100 ms data-token wait. */
            T_CHECK(elapsed_us < UINT64_C(1000));
            T_CHECK(bytes_used < 40U);
            /* CMD12 terminates a multiple-block transfer and only that. */
            T_EQ_U(blocks == 1U ? 0U : 1U,
                (unsigned)sd_card_command_count(12U));
            T_CHECK(sd_fx_guard_untouched(&buffer));
            T_CHECK(sd_fx_guard_intact(&buffer));
            T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
            /* And the device is still usable afterwards. */
            const char *problem = sd_fx_check_recovers(&fx, 5U);
            if (problem != NULL) {
                t_context("token=0x%02x blocks=%u: %s", token, blocks, problem);
            }
            T_CHECK(problem == NULL);
        }
    }
    t_clear_context();
}

static void test_zero_byte_is_not_a_data_error_token(void)
{
    /* 0x00 has no error bits set, so it is not an error token. The driver must
     * keep waiting rather than treat it as a failure - and must still give up
     * on its own 100 ms budget. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    sd_fault_t fault;
    memset(&fault, 0, sizeof(fault));
    fault.phase = SD_PHASE_DATA_TOKEN;
    fault.command = 17U;
    fault.kind = SD_FAULT_BUSY_FOREVER; /* answer 0x00 from here on */
    T_CHECK(sd_card_add_fault(&fault));

    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, 1U);
    const uint64_t start_us = pico_mock_now_us();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_read_blocks(fx.device, 0U, sd_fx_guard_data(&buffer), 1U));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;
    T_CHECK(elapsed_us >= UINT64_C(100000));
    T_CHECK(elapsed_us < UINT64_C(140000));
    T_CHECK(sd_fx_guard_untouched(&buffer));
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
}

/* --------------------------------------------------- long transactions */

static void test_long_multiple_block_read(void)
{
    /* The original fake could script at most about seven blocks, because a
     * multiple-block response had to fit in one 4096-byte payload buffer. The
     * model streams from a generated block store, so length is no longer a
     * harness limit. Sixteen blocks is 8 KB of payload and 8240 bus bytes. */
    static const size_t block_counts[] = { 1U, 2U, 3U, 8U, 16U };

    for (size_t i = 0U; i < sizeof(block_counts) / sizeof(block_counts[0]); ++i) {
        const size_t blocks = block_counts[i];
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("%zu block(s)", blocks);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        sd_guarded_buffer_t buffer;
        sd_fx_guard_init(&buffer, blocks);
        const uint64_t first_lba = 1000U;
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
            fx.device, first_lba, sd_fx_guard_data(&buffer), blocks));

        T_CHECK(sd_fx_guard_intact(&buffer));
        T_CHECK(sd_fx_guard_matches_card(&buffer, first_lba));
        /* Exactly one command of the right kind, and CMD12 only for the
         * multiple-block form. */
        T_EQ_U(blocks == 1U ? 1U : 0U, (unsigned)sd_card_command_count(17U));
        T_EQ_U(blocks == 1U ? 0U : 1U, (unsigned)sd_card_command_count(18U));
        T_EQ_U(blocks == 1U ? 0U : 1U, (unsigned)sd_card_command_count(12U));
        /* The driver must consume exactly the blocks it asked for. Taking
         * fewer would leave the destination short; taking more would mean it
         * kept reading a stream it had already satisfied. */
        const uint8_t read_command = blocks == 1U ? 17U : 18U;
        T_EQ_U((unsigned long long)blocks,
            (unsigned long long)blocks_streamed_for(read_command));
        T_EQ_U((unsigned long long)first_lba,
            (unsigned long long)nth_block_sent_for(read_command, 0U)->argument);
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
        /* A truncated trace would quietly weaken the assertions above. */
        T_CHECK(!sd_card_trace_overflowed());
        /* Every frame in this sequence - CMD18 and CMD12 included - must
         * satisfy the card CRC check that CMD59 turned on during bring-up.
         * The model records a rejected frame but still answers the stop
         * sequence normally, so without this assertion nothing here would
         * notice a CMD12 carrying a placeholder CRC. */
        T_EQ_U(0U, sd_card_protocol_errors());
    }
    t_clear_context();
}

static void test_smallest_card_and_whole_device_read(void)
{
    /* The smallest capacity a CSD v1 register can express is C_SIZE 0,
     * C_SIZE_MULT 0, READ_BL_LEN 9: four blocks. Reading the entire device in
     * one call is the case where the range check, the multiple-block loop and
     * the address arithmetic all sit on their limits at once. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_card_desc(SD_CARD_V2_SDSC, 4U * SD_FX_BLOCK);
    desc.read_bl_len = 9U;
    T_CHECK(sd_fx_require_init(&fx, &desc));
    T_EQ_U(4U, fx.sd.block_count);

    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, 4U);
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
        fx.device, 0U, sd_fx_guard_data(&buffer), 4U));
    T_CHECK(sd_fx_guard_matches_card(&buffer, 0U));
    T_CHECK(sd_fx_guard_intact(&buffer));
    T_EQ_U(0U, sd_card_last_argument(18U));
    T_EQ_U(1U, (unsigned)sd_card_command_count(12U));

    /* One block beyond, from every start position, must be refused. */
    for (uint64_t lba = 0U; lba <= 4U; ++lba) {
        t_context("lba=%" PRIu64 " count=%" PRIu64, lba, 5U - lba);
        sd_fx_guard_init(&buffer, 4U);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OUT_OF_RANGE, block_device_read_blocks(
            fx.device, lba, sd_fx_guard_data(&buffer), (size_t)(5U - lba)));
        T_CHECK(sd_fx_guard_untouched(&buffer));
    }
    t_clear_context();

    /* And the last single block is still readable. */
    sd_fx_guard_init(&buffer, 1U);
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
        fx.device, 3U, sd_fx_guard_data(&buffer), 1U));
    T_CHECK(sd_fx_guard_matches_card(&buffer, 3U));
    T_EQ_U(3U * 512U, sd_card_last_argument(17U));
}

static void test_multiple_block_addresses_advance_by_one(void)
{
    /* A stream must be contiguous from the requested LBA. If the driver ever
     * re-issued a command per block, or the card were asked for the wrong
     * start, the streamed addresses would show it. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, 6U);
    const uint64_t first = 4096U;
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
        fx.device, first, sd_fx_guard_data(&buffer), 6U));
    T_EQ_U(6U, (unsigned)blocks_streamed_for(18U));
    for (size_t i = 0U; i < 6U; ++i) {
        t_context("streamed block %zu", i);
        T_EQ_U((unsigned long long)(first + i),
            (unsigned long long)nth_block_sent_for(18U, i)->argument);
    }
    t_clear_context();
    T_EQ_U((unsigned long)first, (unsigned long)sd_card_last_argument(18U));
}

/* ------------------------------------------------------ addressing */

static void test_addressing_mode_per_card_type(void)
{
    /* Block addressing for high-capacity cards, byte addressing for standard
     * capacity. Getting this backwards is a classic SD bug that still returns
     * data - just the wrong data - so the assertion is on the argument the
     * card received, not on the return code. */
    const struct {
        const char *name;
        sd_card_desc_t desc;
        bool byte_addressed;
    } rows[] = {
        { "SDHC", sd_fx_card_sdhc(), false },
        { "SDXC", sd_fx_card_sdxc(), false },
        { "v1 SDSC", sd_fx_card_v1_sdsc(), true },
        { "v2 SDSC", sd_fx_card_v2_sdsc(), true },
    };
    static const uint64_t lbas[] = { 0U, 1U, 2U, 255U, 256U, 65535U, 65536U };

    for (size_t r = 0U; r < sizeof(rows) / sizeof(rows[0]); ++r) {
        for (size_t i = 0U; i < sizeof(lbas) / sizeof(lbas[0]); ++i) {
            sd_fixture_t fx;
            sd_card_desc_t desc = rows[r].desc;
            const uint64_t lba = lbas[i];
            t_context("%s lba=%" PRIu64, rows[r].name, lba);
            T_CHECK(sd_fx_require_init(&fx, &desc));

            sd_guarded_buffer_t buffer;
            sd_fx_guard_init(&buffer, 1U);
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
                fx.device, lba, sd_fx_guard_data(&buffer), 1U));
            const uint64_t expected =
                rows[r].byte_addressed ? lba * 512U : lba;
            T_EQ_U(expected, sd_card_last_argument(17U));
            T_CHECK(sd_fx_guard_matches_card(&buffer, lba));
            /* The card would have rejected a misaligned byte address. */
            T_EQ_U(0U, sd_card_protocol_errors());
        }
    }
    t_clear_context();
}

/* ---------------------------------------------------- capacity limits */

static void test_capacity_and_address_boundaries(void)
{
    /* The last addressable block of each card type, and the request one past
     * it. For a maximum-size SDSC card the byte address of the last block is
     * 0xFFFFFE00 - 512 bytes below a 32-bit overflow. For a maximum-size
     * high-capacity card the last LBA is exactly UINT32_MAX. Both are one
     * step away from silent truncation in the driver's uint32_t argument. */
    const struct {
        const char *name;
        sd_card_kind_t kind;
        uint64_t capacity;
        uint8_t read_bl_len;
        uint64_t expected_blocks;
        uint64_t expected_last_argument;
    } rows[] = {
        { "SDSC max (4 GiB, RBL 11)", SD_CARD_V1_SDSC,
          UINT64_C(4) * 1024U * 1024U * 1024U, 11U,
          UINT64_C(8388608), UINT64_C(0xFFFFFE00) },
        { "SDSC 1 GiB RBL 9", SD_CARD_V1_SDSC,
          UINT64_C(1) * 1024U * 1024U * 1024U, 9U,
          UINT64_C(2097152), UINT64_C(0x3FFFFE00) },
        { "high capacity max (2 TiB)", SD_CARD_SDXC,
          UINT64_C(0x400000) * 512U * 1024U, 9U,
          UINT64_C(0x100000000), UINT64_C(0xFFFFFFFF) },
        { "SDHC 32 GiB", SD_CARD_SDHC,
          UINT64_C(32) * 1024U * 1024U * 1024U, 9U,
          UINT64_C(67108864), UINT64_C(67108863) },
    };

    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_card_desc(rows[i].kind, rows[i].capacity);
        desc.read_bl_len = rows[i].read_bl_len;
        t_context("%s", rows[i].name);
        T_CHECK(sd_fx_require_init(&fx, &desc));
        T_EQ_U(rows[i].expected_blocks, fx.sd.block_count);

        const uint64_t last = rows[i].expected_blocks - 1U;
        sd_guarded_buffer_t buffer;
        sd_fx_guard_init(&buffer, 1U);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
            fx.device, last, sd_fx_guard_data(&buffer), 1U));
        /* The whole point: the address the card received is not truncated. */
        T_EQ_U(rows[i].expected_last_argument, sd_card_last_argument(17U));
        T_CHECK(sd_fx_guard_matches_card(&buffer, last));

        /* One past the end, and a two-block request straddling the end, must
         * both be refused before any bus activity. */
        const size_t bytes_before = pico_mock_spi_transfer_count();
        sd_fx_guard_init(&buffer, 2U);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OUT_OF_RANGE, block_device_read_blocks(
            fx.device, rows[i].expected_blocks, sd_fx_guard_data(&buffer), 1U));
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OUT_OF_RANGE, block_device_read_blocks(
            fx.device, last, sd_fx_guard_data(&buffer), 2U));
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OUT_OF_RANGE, block_device_read_blocks(
            fx.device, UINT64_MAX, sd_fx_guard_data(&buffer), 1U));
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OUT_OF_RANGE, block_device_read_blocks(
            fx.device, 0U, sd_fx_guard_data(&buffer), SIZE_MAX));
        T_EQ_U(bytes_before, pico_mock_spi_transfer_count());
        T_CHECK(sd_fx_guard_untouched(&buffer));

        /* A whole-device request at the exact capacity is in range. */
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
            fx.device, last, sd_fx_guard_data(&buffer), 1U));
    }
    t_clear_context();
}

/* ------------------------------------------------- golden CSD vectors */

static void test_real_card_csd_registers(void)
{
    /* The model's CSD encoder and the driver's CSD decoder were written from
     * the same specification, so agreement between them proves little. These
     * are register dumps captured from real cards, published with the card's
     * marketed capacity; each one's CRC7 is verified here first so a
     * transcription error cannot masquerade as a decoder bug. Sources are
     * listed in PROTOCOL.md finding P-06. */
    const struct {
        const char *name;
        uint8_t csd[16];
        bool high_capacity;
        uint64_t expected_blocks;
    } rows[] = {
        { "Kingston 2GB SDSC",
          { 0x00, 0x2d, 0x00, 0x32, 0x5b, 0x5a, 0x83, 0xd5,
            0xfe, 0xfb, 0xff, 0x80, 0x16, 0x80, 0x00, 0xcf },
          false, UINT64_C(4022272) },
        { "SanDisk 2GB Blue SDSC",
          { 0x00, 0x26, 0x00, 0x32, 0x5f, 0x5a, 0x83, 0xc9,
            0x3e, 0xfb, 0xcf, 0xff, 0x92, 0x80, 0x40, 0xcb },
          false, UINT64_C(3970048) },
        { "Samsung 32GB Class 10 SDHC",
          { 0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, 0x00,
            0xee, 0x9d, 0x7f, 0x80, 0x0a, 0x40, 0x00, 0x13 },
          true, UINT64_C(62552064) },
        { "Toshiba 64GB SDXC",
          { 0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, 0x01,
            0xdb, 0xff, 0x7f, 0x80, 0x0a, 0x40, 0x00, 0x3f },
          true, UINT64_C(124780544) },
        { "Kingston 128GB SDXC",
          { 0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, 0x03,
            0xa5, 0xdf, 0x7f, 0x80, 0x0a, 0x40, 0x00, 0x07 },
          true, UINT64_C(244809728) },
    };

    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        t_context("%s", rows[i].name);
        /* Guard the vector itself: a mistyped dump must fail here, loudly,
         * rather than turn into a false accusation against the parser. */
        const uint8_t expected_crc =
            (uint8_t)(((unsigned)sd_crc7(rows[i].csd, 15U) << 1U) | 1U);
        T_EQ_U(expected_crc, rows[i].csd[15]);

        sd_fixture_t fx;
        sd_card_desc_t desc = rows[i].high_capacity
            ? sd_card_desc(SD_CARD_SDHC, rows[i].expected_blocks * 512U)
            : sd_card_desc(SD_CARD_V2_SDSC, rows[i].expected_blocks * 512U);
        sd_fx_begin(&fx, &desc);
        sd_card_set_raw_csd(rows[i].csd);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, sd_fx_init(&fx));
        T_EQ_U(rows[i].expected_blocks, fx.sd.block_count);
        T_EQ_U(rows[i].high_capacity ? 1U : 0U,
            fx.sd.card_type_hcxc ? 1U : 0U);
    }
    t_clear_context();
}

static void test_csd_structure_and_field_rejection(void)
{
    /* Encodings the driver must refuse, each exercised through a real CMD9
     * response rather than through a unit call on an internal helper. */
    const struct {
        const char *name;
        uint8_t csd_structure;
        uint8_t read_bl_len;
        bool high_capacity;
        block_device_result_t expected;
    } rows[] = {
        { "CSD v3 (SDUC) on a high-capacity card", 2U, 9U, true,
          BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED },
        { "CSD structure 3 (reserved)", 3U, 9U, true,
          BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED },
        { "CSD v2 on a byte-addressed card", 1U, 9U, false,
          BLOCK_DEVICE_RESULT_IO_ERROR },
        { "CSD v1 on a block-addressed card", 0U, 9U, true,
          BLOCK_DEVICE_RESULT_IO_ERROR },
    };

    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        sd_fixture_t fx;
        sd_card_desc_t desc = rows[i].high_capacity
            ? sd_fx_card_sdhc() : sd_fx_card_v2_sdsc();
        t_context("%s", rows[i].name);
        sd_fx_begin(&fx, &desc);

        uint8_t csd[16];
        sd_card_csd(csd);
        csd[0] = (uint8_t)((csd[0] & 0x3FU)
            | (uint8_t)(rows[i].csd_structure << 6U));
        csd[5] = (uint8_t)((csd[5] & 0xF0U) | (rows[i].read_bl_len & 0x0FU));
        csd[15] = (uint8_t)(((unsigned)sd_crc7(csd, 15U) << 1U) | 1U);
        sd_card_set_raw_csd(csd);

        T_EQ_RESULT(rows[i].expected, sd_fx_init(&fx));
        T_CHECK(!fx.sd.initialized);
        T_EQ_U(0U, fx.sd.block_count);
        T_CHECK(!pico_mock_spi_is_initialized());
        T_EQ_U(0U, (unsigned)sd_card_command_count(16U));
    }

    /* READ_BL_LEN outside 9..11 is invalid for a CSD v1 card. Sweep every
     * encoding rather than sampling, so a range check that is off by one at
     * either end is caught. */
    for (uint8_t length = 0U; length < 16U; ++length) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_v2_sdsc();
        t_context("CSD v1 READ_BL_LEN=%u", (unsigned)length);
        sd_fx_begin(&fx, &desc);

        uint8_t csd[16];
        sd_card_csd(csd);
        const uint8_t original = (uint8_t)(csd[5] & 0x0FU);
        csd[5] = (uint8_t)((csd[5] & 0xF0U) | length);
        csd[15] = (uint8_t)(((unsigned)sd_crc7(csd, 15U) << 1U) | 1U);
        sd_card_set_raw_csd(csd);

        const block_device_result_t result = sd_fx_init(&fx);
        if (length >= 9U && length <= 11U) {
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, result);
            /* Capacity scales with the block length, so only the encoding the
             * card actually reported may reproduce the real block count. */
            if (length == original) {
                T_EQ_U(sd_card_block_count(), fx.sd.block_count);
            }
        } else {
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR, result);
            T_EQ_U(0U, fx.sd.block_count);
            T_EQ_U(0U, (unsigned)sd_card_command_count(16U));
        }
    }
    t_clear_context();
}

/* ------------------------------------------------- bring-up sequencing */

static void test_idle_clocks_precede_the_first_command(void)
{
    /* The card needs at least 74 clocks with chip select released before it
     * will accept CMD0. The driver sends ten 0xFF bytes; assert both the count
     * and that chip select really was high for all of them, since sending them
     * while selected would not satisfy the requirement. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    const uint8_t *const tx = pico_mock_spi_tx_log();
    T_CHECK(pico_mock_spi_tx_log_length() >= 11U);
    for (size_t i = 0U; i < 10U; ++i) {
        t_context("idle clock byte %zu", i);
        T_EQ_U(0xFFU, tx[i]);
        T_CHECK(pico_mock_spi_tx_chip_select_high(i));
    }
    t_clear_context();
    /* Ten bytes is 80 clocks, which satisfies the 74-clock minimum. */
    T_CHECK(10U * 8U >= 74U);
    /* And the first command byte only appears after chip select drops. */
    T_CHECK(!pico_mock_spi_tx_chip_select_high(11U));
    T_EQ_U(0x40U, tx[11]);
}

static void test_bus_is_released_after_every_outcome(void)
{
    /* Once chip select has been asserted the driver owns the release. Check
     * it across a representative spread of outcomes rather than trusting each
     * individual test to remember. */
    const struct { const char *name; unsigned int mode; } rows[] = {
        { "successful single read", 0U },
        { "successful multi read", 1U },
        { "R1 error", 2U },
        { "data error token", 3U },
        { "out of range", 4U },
        { "deinit", 5U },
    };

    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("%s", rows[i].name);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        sd_guarded_buffer_t buffer;
        sd_fx_guard_init(&buffer, 2U);
        sd_fault_t fault;
        memset(&fault, 0, sizeof(fault));

        switch (rows[i].mode) {
        case 0U:
            (void)block_device_read_blocks(
                fx.device, 7U, sd_fx_guard_data(&buffer), 1U);
            break;
        case 1U:
            (void)block_device_read_blocks(
                fx.device, 7U, sd_fx_guard_data(&buffer), 2U);
            break;
        case 2U:
            fault.phase = SD_PHASE_R1;
            fault.command = 17U;
            fault.kind = SD_FAULT_SUBSTITUTE_R1;
            fault.param = 0x40U;
            T_CHECK(sd_card_add_fault(&fault));
            (void)block_device_read_blocks(
                fx.device, 7U, sd_fx_guard_data(&buffer), 1U);
            break;
        case 3U:
            fault.phase = SD_PHASE_DATA_TOKEN;
            fault.command = 17U;
            fault.kind = SD_FAULT_DATA_ERROR_TOKEN;
            fault.param = 0x04U;
            T_CHECK(sd_card_add_fault(&fault));
            (void)block_device_read_blocks(
                fx.device, 7U, sd_fx_guard_data(&buffer), 1U);
            break;
        case 4U:
            (void)block_device_read_blocks(
                fx.device, UINT64_MAX, sd_fx_guard_data(&buffer), 1U);
            break;
        default:
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
                block_device_deinit(fx.device));
            break;
        }

        const char *problem = sd_fx_check_bus_quiescent(&fx);
        if (problem != NULL) {
            t_context("%s: %s", rows[i].name, problem);
        }
        T_CHECK(problem == NULL);
        T_CHECK(sd_fx_guard_intact(&buffer));
    }
    t_clear_context();
}

/* ------------------------------------------------------ write contract */

static void test_write_reaches_the_bus_and_lands_on_the_card(void)
{
    /* Until 2026-09 this case pinned write_blocks as a NOT_IMPLEMENTED stub
     * that never touched the bus. The write path exists now; the protocol
     * detail lives in test_sd_writes.c, and this case keeps the one promise
     * a caller of this layer needs: OK means the card holds the data. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    uint8_t payload[SD_FX_BLOCK];
    memset(payload, 0x5AU, sizeof(payload));

    const size_t bytes_before = pico_mock_spi_transfer_count();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(fx.device, 0U, payload, 1U));
    T_CHECK(pico_mock_spi_transfer_count() > bytes_before);
    T_EQ_U(0U, sd_card_protocol_errors());

    uint8_t stored[SD_FX_BLOCK];
    T_CHECK(sd_card_get_block(0U, stored));
    T_CHECK(memcmp(stored, payload, sizeof(payload)) == 0);
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    T_CHECK(sd_fx_check_recovers(&fx, 11U) == NULL);
}

/* ---------------------------------------------------- reported geometry */

/*
 * get_info must report what the card actually is. The oracle here is the card
 * model's own block count rather than fx.sd.block_count, so this cannot pass
 * by comparing the driver against another copy of its own decode. Reporting
 * geometry is answered from state cached at initialization, so it must also
 * cost no bus traffic, and the capacity it advertises has to agree with the
 * range read_blocks enforces.
 */
static void test_get_info_matches_the_card_model(void)
{
    static const struct {
        const char *name;
        sd_card_desc_t (*make)(void);
    } rows[] = {
        { "SDHC 8 GB, CSD v2", sd_fx_card_sdhc },
        { "SDXC 64 GB, CSD v2", sd_fx_card_sdxc },
        { "v1 SDSC 1 GB, CSD v1", sd_fx_card_v1_sdsc },
        { "v2 SDSC 2 GB, CSD v1 with READ_BL_LEN 10", sd_fx_card_v2_sdsc },
    };

    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        sd_fixture_t fx;
        sd_card_desc_t desc = rows[i].make();
        t_context("%s", rows[i].name);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        block_device_info_t info;
        memset(&info, 0xEEU, sizeof(info));

        size_t bytes_before = pico_mock_spi_transfer_count();
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
            block_device_get_info(fx.device, &info));
        T_EQ_U(bytes_before, pico_mock_spi_transfer_count());

        /* The driver normalises every card to 512-byte logical blocks, CMD16
         * forcing it where READ_BL_LEN says otherwise. */
        T_EQ_U(SD_FX_BLOCK, info.block_size_bytes);
        T_EQ_U(sd_card_block_count(), info.block_count);
        /* Pins today's behaviour: writable is hardcoded true; no
         * write-protect state is ever read. */
        T_CHECK(info.writable);

        /* One past the advertised end is out of range, and rejected without
         * bus traffic. */
        uint8_t block[SD_FX_BLOCK];
        bytes_before = pico_mock_spi_transfer_count();
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OUT_OF_RANGE,
            block_device_read_blocks(fx.device, info.block_count, block, 1U));
        T_EQ_U(bytes_before, pico_mock_spi_transfer_count());
    }
    t_clear_context();
}

/*
 * A card that was removed after a successful bring-up must not keep answering
 * geometry questions from stale cached state.
 */
static void test_get_info_after_removal_is_rejected(void)
{
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    block_device_info_t info;
    block_device_info_t untouched;
    memset(&info, 0xEEU, sizeof(info));
    memset(&untouched, 0xEEU, sizeof(untouched));

    /* The line goes to its absent level and the removal edge fires. */
    T_CHECK(sd_fx_remove_card());

    const size_t bytes_before = pico_mock_spi_transfer_count();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_get_info(fx.device, &info));
    T_EQ_U(bytes_before, pico_mock_spi_transfer_count());
    /* A rejected getter must not half-fill its output. */
    T_CHECK(memcmp(&info, &untouched, sizeof(info)) == 0);
}

/* --------------------------------------------------------------- main */

int main(void)
{
    t_run(test_card_variant_initialization, "card variant initialization matrix");
    t_run(test_initialization_command_order, "initialization command order");
    t_run(test_command_framing_and_crc, "command framing and CMD0/CMD8 CRC7");
    t_run(test_command_crc_checking_is_actually_enabled,
        "CMD59 actually enables command CRC checking");
    t_run(test_acmd41_is_always_prefixed_by_cmd55, "ACMD41 requires CMD55 and carries HCS");
    t_run(test_cmd8_response_is_matched_exactly, "CMD8 R1 sweep: only 0x01 and 0x05 are meaningful");
    t_run(test_legacy_card_omits_the_hcs_bit, "legacy card ACMD41 omits HCS");
    t_run(test_high_capacity_card_needs_hcs_to_leave_idle, "ACMD41 budget is bounded");
    t_run(test_response_latency_boundary, "R1 poll boundary: 7 filler bytes accepted, 8 rejected fast");
    t_run(test_every_nonzero_r1_fails_a_read, "all 128 R1 values on CMD17 and CMD18");
    t_run(test_data_error_tokens_are_recognised_immediately, "all data error tokens, both read paths");
    t_run(test_zero_byte_is_not_a_data_error_token, "0x00 is not a data error token");
    t_run(test_long_multiple_block_read, "multiple-block reads up to 16 blocks");
    t_run(test_smallest_card_and_whole_device_read, "smallest card, whole-device read");
    t_run(test_multiple_block_addresses_advance_by_one, "streamed block addresses are contiguous");
    t_run(test_addressing_mode_per_card_type, "block versus byte addressing per card type");
    t_run(test_capacity_and_address_boundaries, "capacity and address conversion boundaries");
    t_run(test_real_card_csd_registers, "CSD registers captured from real cards");
    t_run(test_csd_structure_and_field_rejection, "CSD structure and READ_BL_LEN rejection");
    t_run(test_idle_clocks_precede_the_first_command, "74-clock bring-up requirement");
    t_run(test_bus_is_released_after_every_outcome, "bus released after every outcome");
    t_run(test_write_reaches_the_bus_and_lands_on_the_card, "a write reaches the bus and lands on the card");
    t_run(test_get_info_matches_the_card_model, "get_info matches the card model across variants");
    t_run(test_get_info_after_removal_is_rejected, "get_info after removal reports INVALID_DEVICE");
    return t_summary("sd_protocol");
}
