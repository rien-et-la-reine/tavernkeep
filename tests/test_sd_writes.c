/*
 * The write path: CMD24 single-block and CMD25 multiple-block writes.
 *
 * Every case asks three things of sd_spi_device_write_blocks(): did the card
 * end up holding exactly the bytes the caller passed (read out of the model,
 * never back through the driver alone, so a driver that writes and reads the
 * same wrong thing cannot agree with itself); did the bus carry what the
 * specification requires (the right start-block token per command, the N_WR
 * idle byte, a real CRC16, stop-tran for CMD25, CMD12 only where 7.3.3.1 asks
 * for it); and, when the card refuses, stalls, stays busy or disappears, is
 * the driver's answer honest and is the device still usable afterwards.
 *
 * The card model rejects a block whose CRC16 does not match, insists on 0xFE
 * for CMD24 and 0xFC for CMD25, records a start token sent with no idle byte
 * after R1, and keeps a CMD25 transfer open across per-block programming busy.
 * Each of those is a way a plausible driver could be wrong while a permissive
 * fake said it was right.
 */
#include <inttypes.h>
#include <string.h>

#include "hardware/gpio.h"
#include "pico_mock.h"
#include "sd_card_model.h"
#include "sd_fixture.h"
#include "test_harness.h"

/* Budgets the driver declares for itself, in microseconds. */
enum {
    WRITE_BUSY_WAIT_US = 1000000,   /* every programming wait in the write path */
    COMMAND_READY_WAIT_US = 500000, /* sd_spi_command's pre-frame ready wait */
    MAX_BLOCKS = 16,
};

/* Data-response tokens, specification 7.3.3.1: xxx0sss1. */
enum {
    TOKEN_ACCEPTED = 0x05,
    TOKEN_CRC_ERROR = 0x0B,
    TOKEN_WRITE_ERROR = 0x0D,
};

/* ------------------------------------------------------------- helpers */

/*
 * Distinct content per block so a driver that sends the wrong slice of the
 * source (a stride, an off-by-one block, the first block repeated) cannot
 * match. Block 0 is all zeros: its CRC16 is 0x0000, so a CRC with a stray
 * end bit or wrong initial value fails on it. Block 1 is all 0xFF, whose
 * CRC 0x7FA1 is the published check value. Block 2 contains every token
 * byte and a command-frame lookalike, so a card that scans payload for
 * tokens or frames would trip over it if the driver framed it wrongly.
 */
static void fill_payload(uint8_t *out, size_t blocks, uint8_t salt)
{
    for (size_t block = 0U; block < blocks; ++block) {
        uint8_t *const p = out + (block * SD_FX_BLOCK);
        if (block == 0U) {
            memset(p, 0x00, SD_FX_BLOCK);
        } else if (block == 1U) {
            memset(p, 0xFF, SD_FX_BLOCK);
        } else {
            uint32_t x = (uint32_t)(0x9E3779B9U * (block + 1U)) ^ (uint32_t)salt;
            for (size_t i = 0U; i < SD_FX_BLOCK; ++i) {
                x ^= x << 13U;
                x ^= x >> 17U;
                x ^= x << 5U;
                p[i] = (uint8_t)(x >> 5U);
            }
            if (block == 2U) {
                static const uint8_t lookalikes[] = {
                    0xFE, 0xFC, 0xFD, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x61,
                    0x40, 0x00, 0x00, 0x00, 0x00, 0x95,
                };
                memcpy(p + 100U, lookalikes, sizeof(lookalikes));
            }
        }
    }
}

/* Phases a command visits once, regardless of how many blocks it carries. */
static bool phase_is_per_command(sd_phase_t phase)
{
    return phase == SD_PHASE_RESPONSE_WAIT || phase == SD_PHASE_R1;
}

static bool card_holds(uint64_t first_lba, const uint8_t *payload, size_t blocks)
{
    uint8_t stored[SD_FX_BLOCK];
    for (size_t block = 0U; block < blocks; ++block) {
        if (!sd_card_get_block(first_lba + block, stored)) {
            return false;
        }
        if (memcmp(stored, payload + (block * SD_FX_BLOCK), SD_FX_BLOCK) != 0) {
            return false;
        }
    }
    return true;
}

/* Trace queries scoped to events recorded after `from`, so bring-up traffic
 * never counts. */
static size_t events_from(size_t from, sd_event_kind_t kind)
{
    size_t count = 0U;
    for (size_t i = from; i < sd_card_trace_length(); ++i) {
        if (sd_card_trace_at(i)->kind == kind) {
            count++;
        }
    }
    return count;
}

static size_t write_tokens_from(size_t from, uint8_t value)
{
    size_t count = 0U;
    for (size_t i = from; i < sd_card_trace_length(); ++i) {
        const sd_event_t *const e = sd_card_trace_at(i);
        if (e->kind == SD_EV_WRITE_TOKEN && e->value == value) {
            count++;
        }
    }
    return count;
}

static size_t commands_from(size_t from, uint8_t command)
{
    size_t count = 0U;
    for (size_t i = from; i < sd_card_trace_length(); ++i) {
        const sd_event_t *const e = sd_card_trace_at(i);
        if (e->kind == SD_EV_COMMAND && e->command == command) {
            count++;
        }
    }
    return count;
}

/* Index of the last chip-select release after `from`, or SIZE_MAX. */
static size_t last_release_from(size_t from)
{
    size_t found = SIZE_MAX;
    for (size_t i = from; i < sd_card_trace_length(); ++i) {
        const sd_event_t *const e = sd_card_trace_at(i);
        if (e->kind == SD_EV_CHIP_SELECT && e->value == 1U) {
            found = i;
        }
    }
    return found;
}

static size_t last_event_from(size_t from, sd_event_kind_t kind)
{
    size_t found = SIZE_MAX;
    for (size_t i = from; i < sd_card_trace_length(); ++i) {
        if (sd_card_trace_at(i)->kind == kind) {
            found = i;
        }
    }
    return found;
}

/* The shape of a correct transfer, checked from the trace rather than from
 * counters: one command with the right argument, the right token before each
 * block, each block stored at the next address, stop-tran exactly once for
 * CMD25 and never for CMD24, and CMD12 never on a success path. */
static const char *check_success_trace(
    size_t from,
    uint64_t first_lba,
    size_t blocks,
    bool byte_addressed)
{
    const uint8_t command = blocks > 1U ? 25U : 24U;
    const uint8_t token = blocks > 1U ? 0xFCU : 0xFEU;
    if (commands_from(from, command) != 1U) {
        return "expected exactly one write command";
    }
    if (commands_from(from, blocks > 1U ? 24U : 25U) != 0U) {
        return "the other write command was also sent";
    }
    if (commands_from(from, 12U) != 0U) {
        return "CMD12 sent on a successful write";
    }
    const uint64_t expected_argument =
        byte_addressed ? first_lba * SD_FX_BLOCK : first_lba;
    if (sd_card_last_argument(command) != (uint32_t)expected_argument) {
        return "write command carried the wrong address";
    }
    if (write_tokens_from(from, token) != blocks) {
        return "wrong number of start-block tokens";
    }
    if (write_tokens_from(from, token == 0xFCU ? 0xFEU : 0xFCU) != 0U) {
        return "the other command's start-block token was sent";
    }
    if (events_from(from, SD_EV_STOP_TRAN) != (blocks > 1U ? 1U : 0U)) {
        return "stop-tran count is wrong for this command";
    }
    if (events_from(from, SD_EV_BLOCK_WRITTEN) != blocks) {
        return "the card stored a different number of blocks";
    }
    size_t seen = 0U;
    for (size_t i = from; i < sd_card_trace_length(); ++i) {
        const sd_event_t *const e = sd_card_trace_at(i);
        if (e->kind != SD_EV_BLOCK_WRITTEN) {
            continue;
        }
        if (e->argument != (uint32_t)(first_lba + seen)) {
            return "blocks were stored out of order or at the wrong address";
        }
        seen++;
    }
    if (sd_card_protocol_errors() != 0U) {
        return sd_card_last_protocol_error();
    }
    return NULL;
}

/* ------------------------------------------------- data reaches the card */

static void test_single_block_write_stores_the_data_on_every_card_kind(void)
{
    static const struct {
        const char *name;
        sd_card_desc_t (*make)(void);
        bool byte_addressed;
        bool strict_crc;
    } rows[] = {
        { "SDHC", sd_fx_card_sdhc, false, false },
        { "SDXC", sd_fx_card_sdxc, false, false },
        { "v1 SDSC (byte addressing)", sd_fx_card_v1_sdsc, true, false },
        { "v2 SDSC (byte addressing, CMD16)", sd_fx_card_v2_sdsc, true, false },
        { "SDHC, strict command CRC", sd_fx_card_sdhc, false, true },
    };
    /* Zeros, 0xFF, and the token/frame lookalike block, each on its own. */
    static const size_t contents[] = { 0U, 1U, 2U };

    for (size_t r = 0U; r < sizeof(rows) / sizeof(rows[0]); ++r) {
        for (size_t c = 0U; c < sizeof(contents) / sizeof(contents[0]); ++c) {
            sd_fixture_t fx;
            sd_card_desc_t desc = rows[r].make();
            desc.crc_check_enabled = rows[r].strict_crc;
            t_context("%s, content pattern %zu", rows[r].name, contents[c]);
            T_CHECK(sd_fx_require_init(&fx, &desc));

            uint8_t source[3U * SD_FX_BLOCK];
            fill_payload(source, 3U, 0x11U);
            const uint8_t *const payload = source + (contents[c] * SD_FX_BLOCK);

            /* The last block is the address most likely to expose a
             * byte-address overflow on a standard-capacity card. */
            const uint64_t lba = c == 2U ? fx.sd.block_count - 1U : 37U;
            const size_t from = sd_card_trace_length();
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
                block_device_write_blocks(fx.device, lba, payload, 1U));

            T_CHECK(card_holds(lba, payload, 1U));
            const char *shape =
                check_success_trace(from, lba, 1U, rows[r].byte_addressed);
            if (shape != NULL) {
                t_context("%s, content pattern %zu: %s",
                    rows[r].name, contents[c], shape);
            }
            T_CHECK(shape == NULL);
            T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);

            /* And the round trip through the driver agrees. */
            sd_guarded_buffer_t buffer;
            sd_fx_guard_init(&buffer, 1U);
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
                fx.device, lba, sd_fx_guard_data(&buffer), 1U));
            T_CHECK(sd_fx_guard_intact(&buffer));
            T_CHECK(memcmp(sd_fx_guard_data(&buffer), payload, SD_FX_BLOCK) == 0);
        }
    }
    t_clear_context();
}

static void test_multi_block_write_stores_every_block_in_order(void)
{
    static const struct {
        const char *name;
        sd_card_desc_t (*make)(void);
        bool byte_addressed;
    } rows[] = {
        { "SDHC", sd_fx_card_sdhc, false },
        { "v2 SDSC (byte addressing)", sd_fx_card_v2_sdsc, true },
    };
    static const size_t counts[] = { 2U, 3U, 5U, MAX_BLOCKS };

    for (size_t r = 0U; r < sizeof(rows) / sizeof(rows[0]); ++r) {
        for (size_t c = 0U; c < sizeof(counts) / sizeof(counts[0]); ++c) {
            const size_t blocks = counts[c];
            sd_fixture_t fx;
            sd_card_desc_t desc = rows[r].make();
            t_context("%s, %zu blocks", rows[r].name, blocks);
            T_CHECK(sd_fx_require_init(&fx, &desc));

            uint8_t payload[MAX_BLOCKS * SD_FX_BLOCK];
            fill_payload(payload, blocks, (uint8_t)blocks);

            /* Ending on the last block exercises the range boundary and, on
             * a standard-capacity card, the largest byte address. */
            const uint64_t lba = fx.sd.block_count - blocks;
            const size_t from = sd_card_trace_length();
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
                block_device_write_blocks(fx.device, lba, payload, blocks));

            T_CHECK(card_holds(lba, payload, blocks));
            const char *shape =
                check_success_trace(from, lba, blocks, rows[r].byte_addressed);
            if (shape != NULL) {
                t_context("%s, %zu blocks: %s", rows[r].name, blocks, shape);
            }
            T_CHECK(shape == NULL);
            T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);

            /* The block before the range was not touched: the card still
             * answers its generated content, and no write event named it. */
            uint8_t expected[SD_FX_BLOCK];
            sd_card_fill_expected_block(lba - 1U, expected);
            for (size_t i = from; i < sd_card_trace_length(); ++i) {
                const sd_event_t *const e = sd_card_trace_at(i);
                T_CHECK(e->kind != SD_EV_BLOCK_WRITTEN
                    || e->argument != (uint32_t)(lba - 1U));
            }
            sd_guarded_buffer_t before;
            sd_fx_guard_init(&before, 1U);
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
                fx.device, lba - 1U, sd_fx_guard_data(&before), 1U));
            T_CHECK(memcmp(sd_fx_guard_data(&before), expected, SD_FX_BLOCK) == 0);

            /* The round trip through CMD18 returns every block. */
            sd_guarded_buffer_t buffer;
            sd_fx_guard_init(&buffer, blocks);
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
                fx.device, lba, sd_fx_guard_data(&buffer), blocks));
            T_CHECK(sd_fx_guard_intact(&buffer));
            T_CHECK(memcmp(sd_fx_guard_data(&buffer), payload,
                blocks * SD_FX_BLOCK) == 0);
        }
    }
    t_clear_context();
}

static void test_writes_are_independent_transactions(void)
{
    /* Back-to-back writes of different shapes, interleaved with reads, must
     * not leak state: a second CMD25 after a CMD24, a CMD24 after a CMD25,
     * and overwriting a block already written. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    uint8_t a[SD_FX_BLOCK];
    uint8_t b[3U * SD_FX_BLOCK];
    uint8_t c[SD_FX_BLOCK];
    fill_payload(a, 1U, 0xA1U);
    fill_payload(b, 3U, 0xB2U);
    fill_payload(c, 1U, 0xC3U);
    memset(c, 0x3CU, sizeof(c));

    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(fx.device, 100U, a, 1U));
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(fx.device, 101U, b, 3U));
    T_CHECK(sd_fx_check_recovers(&fx, 50U) == NULL);
    /* Overwrite the middle of the CMD25 range with a CMD24. */
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(fx.device, 102U, c, 1U));
    T_EQ_U(0U, sd_card_protocol_errors());

    T_CHECK(card_holds(100U, a, 1U));
    T_CHECK(card_holds(101U, b, 1U));
    T_CHECK(card_holds(102U, c, 1U));
    T_CHECK(card_holds(103U, b + (2U * SD_FX_BLOCK), 1U));
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
}

/* ------------------------------------------------ data-response tokens */

static void test_data_response_upper_bits_are_dont_care(void)
{
    /* 7.3.3.1 defines the token as xxx0sss1. Real cards drive the x bits
     * high (0xE5 is what a bus capture shows), the model's default drives
     * them low; a driver must accept both and everything in between. */
    static const uint8_t tokens[] = { 0x05U, 0xE5U, 0x25U, 0xC5U };

    for (size_t blocks = 1U; blocks <= 2U; ++blocks) {
        for (size_t i = 0U; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
            sd_fixture_t fx;
            sd_card_desc_t desc = sd_fx_card_sdhc();
            t_context("%zu block(s), accepted token 0x%02X",
                blocks, (unsigned)tokens[i]);
            T_CHECK(sd_fx_require_init(&fx, &desc));
            sd_card_set_write_response_token(tokens[i]);

            uint8_t payload[2U * SD_FX_BLOCK];
            fill_payload(payload, 2U, 0x55U);
            const size_t from = sd_card_trace_length();
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
                block_device_write_blocks(fx.device, 8U, payload, blocks));
            T_CHECK(card_holds(8U, payload, blocks));
            T_CHECK(check_success_trace(from, 8U, blocks, false) == NULL);
            T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
        }
    }
    t_clear_context();
}

static void test_rejected_data_response_is_an_error(void)
{
    /* CRC error and write error, with the x bits both low and high. For
     * CMD25 the specification's recovery is CMD12, and the stop-tran token
     * must not be sent instead. For CMD24 there is no transfer to stop, so
     * CMD12 has no place; the driver simply reports the failure. */
    static const uint8_t tokens[] = { 0x0BU, 0xEBU, 0x0DU, 0xEDU };

    for (size_t blocks = 1U; blocks <= 3U; blocks += 2U) {
        for (size_t i = 0U; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
            sd_fixture_t fx;
            sd_card_desc_t desc = sd_fx_card_sdhc();
            t_context("%zu block(s), rejection token 0x%02X",
                blocks, (unsigned)tokens[i]);
            T_CHECK(sd_fx_require_init(&fx, &desc));
            sd_card_set_write_response_token(tokens[i]);

            uint8_t payload[3U * SD_FX_BLOCK];
            fill_payload(payload, 3U, 0x66U);
            const size_t from = sd_card_trace_length();
            const uint64_t start_us = pico_mock_now_us();
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR,
                block_device_write_blocks(fx.device, 20U, payload, blocks));
            const uint64_t elapsed_us = pico_mock_now_us() - start_us;

            /* The first rejection ends the transfer: one block was sent,
             * no more start tokens followed. */
            T_EQ_U(1U, write_tokens_from(from, blocks > 1U ? 0xFCU : 0xFEU));
            T_EQ_U(0U, events_from(from, SD_EV_STOP_TRAN));
            T_EQ_U(blocks > 1U ? 1U : 0U, commands_from(from, 12U));
            T_EQ_U(0U, sd_card_protocol_errors());
            /* A rejection is not a timeout; the driver must not have burnt
             * its busy budget waiting for something that already arrived. */
            T_CHECK(elapsed_us < (uint64_t)WRITE_BUSY_WAIT_US);
            T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);

            /* The driver is still usable, and a later write succeeds. */
            sd_card_set_write_response_token(TOKEN_ACCEPTED);
            T_CHECK(sd_fx_check_recovers(&fx, 3U) == NULL);
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
                block_device_write_blocks(fx.device, 40U, payload, blocks));
            T_CHECK(card_holds(40U, payload, blocks));
        }
    }
    t_clear_context();
}

static void test_rejection_followed_by_busy_is_waited_out(void)
{
    /* A write error (0x0D) is reported after the card tried to program, and
     * the card can be busy for a while after saying so. The driver must wait
     * for that busy to end before it releases the card - for CMD25 before it
     * sends CMD12 as well - so that the failure leaves an idle card behind
     * rather than one the next command has to wait for. */
    enum { PROGRAM_US = 200000 };
    for (size_t blocks = 1U; blocks <= 2U; ++blocks) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        desc.program_us = PROGRAM_US;
        t_context("%zu block(s), write error then %d us busy", blocks, PROGRAM_US);
        T_CHECK(sd_fx_require_init(&fx, &desc));
        sd_card_set_write_response_token(TOKEN_WRITE_ERROR);

        uint8_t payload[2U * SD_FX_BLOCK];
        fill_payload(payload, 2U, 0x6AU);
        const size_t from = sd_card_trace_length();
        const uint64_t start_us = pico_mock_now_us();
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR,
            block_device_write_blocks(fx.device, 25U, payload, blocks));
        const uint64_t elapsed_us = pico_mock_now_us() - start_us;

        T_CHECK(elapsed_us >= (uint64_t)PROGRAM_US);
        /* CMD25: the busy after the rejected block, then the busy the card
         * signals after CMD12 while it finishes up. CMD24: just the first. */
        T_EQ_U(blocks > 1U ? 2U : 1U, events_from(from, SD_EV_BUSY_END));
        const size_t release = last_release_from(from);
        T_CHECK(release != SIZE_MAX);
        T_CHECK(last_event_from(from, SD_EV_BUSY_END) < release);
        if (blocks > 1U) {
            /* CMD12 is sent to a card that has finished being busy. */
            size_t first_busy_end = SIZE_MAX;
            size_t cmd12 = SIZE_MAX;
            for (size_t i = from; i < sd_card_trace_length(); ++i) {
                const sd_event_t *const e = sd_card_trace_at(i);
                if (e->kind == SD_EV_BUSY_END && first_busy_end == SIZE_MAX) {
                    first_busy_end = i;
                }
                if (e->kind == SD_EV_COMMAND && e->command == 12U) {
                    cmd12 = i;
                    break;
                }
            }
            T_CHECK(cmd12 != SIZE_MAX);
            T_CHECK(first_busy_end < cmd12);
        }
        T_EQ_U(0U, sd_card_protocol_errors());
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
        sd_card_set_write_response_token(TOKEN_ACCEPTED);
        T_CHECK(sd_fx_check_recovers(&fx, 3U) == NULL);
    }
    t_clear_context();
}

static void test_rejection_mid_stream_stops_the_transfer(void)
{
    /* A four-block CMD25 whose second block is refused with a write error.
     * The driver must stop after that block - no third token - send CMD12
     * rather than stop-tran, report IO_ERROR, and leave the first block
     * intact on the card. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));

    sd_fault_t fault;
    memset(&fault, 0, sizeof(fault));
    fault.phase = SD_PHASE_WRITE_RESPONSE;
    fault.command = 25U;
    fault.occurrence = 1U; /* the second block's response */
    fault.kind = SD_FAULT_GARBAGE;
    fault.param = TOKEN_WRITE_ERROR;
    T_CHECK(sd_card_add_fault(&fault));

    uint8_t payload[4U * SD_FX_BLOCK];
    fill_payload(payload, 4U, 0x77U);
    const size_t from = sd_card_trace_length();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR,
        block_device_write_blocks(fx.device, 60U, payload, 4U));

    T_EQ_U(1U, sd_card_fault_activations(0U));
    T_EQ_U(2U, write_tokens_from(from, 0xFCU));
    T_EQ_U(0U, events_from(from, SD_EV_STOP_TRAN));
    T_EQ_U(1U, commands_from(from, 12U));
    T_CHECK(card_holds(60U, payload, 1U));
    T_EQ_U(0U, sd_card_protocol_errors());
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    T_CHECK(sd_fx_check_recovers(&fx, 60U) == NULL);
}

static void test_unknown_data_response_is_an_error(void)
{
    /* Bytes that are not a legal data-response token at the position where
     * one is due: 0xFF (the card never saw the block), busy, and status
     * patterns the specification does not define. None may be treated as
     * acceptance, and none may be followed by another block. */
    static const struct { sd_fault_kind_t kind; uint32_t param; } faults[] = {
        { SD_FAULT_STALL, 0U },        /* 0xFF: no response at all */
        { SD_FAULT_GARBAGE, 0x00U },   /* looks like busy */
        { SD_FAULT_GARBAGE, 0x07U },   /* sss = 011, undefined */
        { SD_FAULT_GARBAGE, 0x15U },   /* bit 4 set */
        { SD_FAULT_GARBAGE, 0x1FU },
        { SD_FAULT_GARBAGE, 0x04U },   /* end bit clear */
    };

    for (size_t blocks = 1U; blocks <= 2U; ++blocks) {
        for (size_t i = 0U; i < sizeof(faults) / sizeof(faults[0]); ++i) {
            sd_fixture_t fx;
            sd_card_desc_t desc = sd_fx_card_sdhc();
            t_context("%zu block(s), response byte fault %u param 0x%02X",
                blocks, (unsigned)faults[i].kind, (unsigned)faults[i].param);
            T_CHECK(sd_fx_require_init(&fx, &desc));

            sd_fault_t fault;
            memset(&fault, 0, sizeof(fault));
            fault.phase = SD_PHASE_WRITE_RESPONSE;
            fault.command = blocks > 1U ? 25U : 24U;
            fault.kind = faults[i].kind;
            fault.param = faults[i].param;
            T_CHECK(sd_card_add_fault(&fault));

            uint8_t payload[2U * SD_FX_BLOCK];
            fill_payload(payload, 2U, 0x88U);
            const size_t from = sd_card_trace_length();
            const block_device_result_t result =
                block_device_write_blocks(fx.device, 70U, payload, blocks);

            T_EQ_U(1U, sd_card_fault_activations(0U));
            T_CHECK(result != BLOCK_DEVICE_RESULT_OK);
            T_EQ_U(1U, write_tokens_from(from, blocks > 1U ? 0xFCU : 0xFEU));
            T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
            T_CHECK(sd_fx_check_recovers(&fx, 5U) == NULL);
        }
    }
    t_clear_context();
}

/* ------------------------------------------------------- R1 failures */

static void test_r1_errors_abort_before_any_data(void)
{
    /* A non-zero R1 to CMD24/CMD25 means the card did not enter the data
     * state; sending a data block anyway would be interpreted as noise at
     * best. Every error bit, and the idle bit, must abort with no token. */
    static const uint8_t r1s[] = { 0x01U, 0x04U, 0x08U, 0x20U, 0x40U };

    for (size_t blocks = 1U; blocks <= 2U; ++blocks) {
        for (size_t i = 0U; i < sizeof(r1s) / sizeof(r1s[0]); ++i) {
            sd_fixture_t fx;
            sd_card_desc_t desc = sd_fx_card_sdhc();
            t_context("%zu block(s), R1 0x%02X", blocks, (unsigned)r1s[i]);
            T_CHECK(sd_fx_require_init(&fx, &desc));

            sd_fault_t fault;
            memset(&fault, 0, sizeof(fault));
            fault.phase = SD_PHASE_R1;
            fault.command = blocks > 1U ? 25U : 24U;
            fault.kind = SD_FAULT_SUBSTITUTE_R1;
            fault.param = r1s[i];
            T_CHECK(sd_card_add_fault(&fault));

            uint8_t payload[2U * SD_FX_BLOCK];
            fill_payload(payload, 2U, 0x99U);
            const size_t from = sd_card_trace_length();
            const uint64_t start_us = pico_mock_now_us();
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR,
                block_device_write_blocks(fx.device, 80U, payload, blocks));
            const uint64_t elapsed_us = pico_mock_now_us() - start_us;

            T_EQ_U(1U, sd_card_fault_activations(0U));
            T_EQ_U(0U, events_from(from, SD_EV_WRITE_TOKEN));
            T_EQ_U(0U, events_from(from, SD_EV_BLOCK_WRITTEN));
            T_EQ_U(0U, commands_from(from, 12U));
            T_EQ_U(0U, sd_card_protocol_errors());
            /* An R1 error is immediate; it must not cost a busy timeout. */
            T_CHECK(elapsed_us < (uint64_t)WRITE_BUSY_WAIT_US);
            T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
            T_CHECK(sd_fx_check_recovers(&fx, 5U) == NULL);
        }
    }
    t_clear_context();
}

static void test_missing_r1_is_an_error(void)
{
    for (size_t blocks = 1U; blocks <= 2U; ++blocks) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("%zu block(s), card never answers the command", blocks);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        sd_fault_t fault;
        memset(&fault, 0, sizeof(fault));
        fault.phase = SD_PHASE_RESPONSE_WAIT;
        fault.command = blocks > 1U ? 25U : 24U;
        fault.kind = SD_FAULT_STALL;
        T_CHECK(sd_card_add_fault(&fault));

        uint8_t payload[2U * SD_FX_BLOCK];
        fill_payload(payload, 2U, 0xAAU);
        const size_t from = sd_card_trace_length();
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_IO_ERROR,
            block_device_write_blocks(fx.device, 80U, payload, blocks));

        T_EQ_U(1U, sd_card_fault_activations(0U));
        T_EQ_U(0U, events_from(from, SD_EV_WRITE_TOKEN));
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
        T_CHECK(sd_fx_check_recovers(&fx, 5U) == NULL);
    }
    t_clear_context();
}

/* ----------------------------------------------------- programming busy */

static void test_programming_busy_is_awaited_between_blocks_and_before_release(void)
{
    /* Three blocks, each followed by 300 ms of programming, plus the final
     * busy after stop-tran. The driver may not send the next token while the
     * card is busy, and may not release chip select before the last busy
     * ends: a caller who gets OK is entitled to a programmed card. */
    enum { PROGRAM_US = 300000, BLOCKS = 3 };
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    desc.program_us = PROGRAM_US;
    T_CHECK(sd_fx_require_init(&fx, &desc));

    uint8_t payload[BLOCKS * SD_FX_BLOCK];
    fill_payload(payload, BLOCKS, 0xBBU);
    const size_t from = sd_card_trace_length();
    const uint64_t start_us = pico_mock_now_us();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(fx.device, 90U, payload, BLOCKS));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;

    T_CHECK(card_holds(90U, payload, BLOCKS));
    T_CHECK(check_success_trace(from, 90U, BLOCKS, false) == NULL);
    T_CHECK(elapsed_us >= (uint64_t)PROGRAM_US * (BLOCKS + 1U));
    T_EQ_U(BLOCKS + 1U, events_from(from, SD_EV_BUSY_BEGIN));
    T_EQ_U(BLOCKS + 1U, events_from(from, SD_EV_BUSY_END));

    /* Ordering: every start token after the first, and the stop-tran token,
     * comes after the busy period of the previous block ended. */
    size_t busy_ends_seen = 0U;
    size_t tokens_seen = 0U;
    for (size_t i = from; i < sd_card_trace_length(); ++i) {
        const sd_event_t *const e = sd_card_trace_at(i);
        if (e->kind == SD_EV_BUSY_END) {
            busy_ends_seen++;
        } else if (e->kind == SD_EV_WRITE_TOKEN || e->kind == SD_EV_STOP_TRAN) {
            /* token n (0-based) needs n busy periods to have ended */
            T_EQ_U(tokens_seen, busy_ends_seen);
            tokens_seen++;
        }
    }
    T_CHECK(last_event_from(from, SD_EV_BUSY_END) < last_release_from(from));
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
}

static void test_single_block_programming_busy_is_awaited(void)
{
    enum { PROGRAM_US = 400000 };
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    desc.program_us = PROGRAM_US;
    T_CHECK(sd_fx_require_init(&fx, &desc));

    uint8_t payload[SD_FX_BLOCK];
    fill_payload(payload, 1U, 0xCCU);
    const size_t from = sd_card_trace_length();
    const uint64_t start_us = pico_mock_now_us();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(fx.device, 91U, payload, 1U));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;

    T_CHECK(card_holds(91U, payload, 1U));
    T_CHECK(check_success_trace(from, 91U, 1U, false) == NULL);
    T_CHECK(elapsed_us >= (uint64_t)PROGRAM_US);
    T_EQ_U(1U, events_from(from, SD_EV_BUSY_END));
    T_CHECK(last_event_from(from, SD_EV_BUSY_END) < last_release_from(from));
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
}

static void test_programming_busy_beyond_the_budget_is_a_busy_timeout(void)
{
    /* A card that stays busy longer than the driver's declared wait. The
     * answer must be BUSY_TIMEOUT rather than OK, must arrive after the full
     * budget and not much later, and for CMD25 no further block may have
     * been pushed into the busy card. */
    enum { PROGRAM_US = 1500000 };
    for (size_t blocks = 1U; blocks <= 3U; blocks += 2U) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        desc.program_us = PROGRAM_US;
        t_context("%zu block(s), programming takes 1.5 s", blocks);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        uint8_t payload[3U * SD_FX_BLOCK];
        fill_payload(payload, 3U, 0xDDU);
        const size_t from = sd_card_trace_length();
        const uint64_t start_us = pico_mock_now_us();
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_BUSY_TIMEOUT,
            block_device_write_blocks(fx.device, 92U, payload, blocks));
        const uint64_t elapsed_us = pico_mock_now_us() - start_us;

        T_CHECK(elapsed_us >= (uint64_t)WRITE_BUSY_WAIT_US);
        T_CHECK(elapsed_us <= (uint64_t)WRITE_BUSY_WAIT_US
            + (uint64_t)COMMAND_READY_WAIT_US + UINT64_C(20000));
        T_EQ_U(1U, write_tokens_from(from, blocks > 1U ? 0xFCU : 0xFEU));
        T_EQ_U(0U, events_from(from, SD_EV_STOP_TRAN));
        /* The card did accept the block it was given. */
        T_CHECK(card_holds(92U, payload, 1U));
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
        T_CHECK(fx.sd.initialized);
        T_CHECK(sd_fx_check_recovers(&fx, 5U) == NULL);
    }
    t_clear_context();
}

static void test_stop_tran_busy_beyond_the_budget_is_a_busy_timeout(void)
{
    /* Every block was accepted quickly; only the final programming after
     * stop-tran overruns. That is still not a successful write. */
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));
    sd_card_set_stop_tran_busy(0U, UINT64_C(1500000));

    uint8_t payload[3U * SD_FX_BLOCK];
    fill_payload(payload, 3U, 0xEEU);
    const size_t from = sd_card_trace_length();
    const uint64_t start_us = pico_mock_now_us();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_BUSY_TIMEOUT,
        block_device_write_blocks(fx.device, 93U, payload, 3U));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;

    T_CHECK(elapsed_us >= (uint64_t)WRITE_BUSY_WAIT_US);
    T_EQ_U(3U, write_tokens_from(from, 0xFCU));
    T_EQ_U(1U, events_from(from, SD_EV_STOP_TRAN));
    T_EQ_U(0U, sd_card_protocol_errors());
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    T_CHECK(sd_fx_check_recovers(&fx, 5U) == NULL);
}

static void test_stop_tran_busy_that_starts_one_byte_late_is_still_awaited(void)
{
    /* N_BR: the specification lets the card answer one idle byte after the
     * stop-tran token before it asserts busy. A driver that polls for
     * "not busy" immediately reads that idle byte, decides programming is
     * finished, and releases a card that is about to start. */
    enum { PROGRAM_US = 300000 };
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    T_CHECK(sd_fx_require_init(&fx, &desc));
    sd_card_set_stop_tran_busy(1U, PROGRAM_US);

    uint8_t payload[2U * SD_FX_BLOCK];
    fill_payload(payload, 2U, 0xF1U);
    const size_t from = sd_card_trace_length();
    const uint64_t start_us = pico_mock_now_us();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(fx.device, 94U, payload, 2U));
    const uint64_t elapsed_us = pico_mock_now_us() - start_us;

    T_CHECK(card_holds(94U, payload, 2U));
    T_CHECK(check_success_trace(from, 94U, 2U, false) == NULL);
    T_CHECK(elapsed_us >= (uint64_t)PROGRAM_US);
    T_EQ_U(1U, events_from(from, SD_EV_BUSY_END));
    T_CHECK(last_event_from(from, SD_EV_BUSY_END) < last_release_from(from));
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
}

static void test_a_busy_card_is_awaited_before_the_write_command(void)
{
    /* The card is still programming an earlier operation when the write is
     * requested. The command frame must wait for the busy signal to clear;
     * a frame clocked into a busy card is simply lost. */
    enum { BUSY_BYTES = 40 };
    for (size_t blocks = 1U; blocks <= 2U; ++blocks) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("%zu block(s), card busy for %d bytes beforehand",
            blocks, BUSY_BYTES);
        T_CHECK(sd_fx_require_init(&fx, &desc));
        sd_card_set_busy_bytes(BUSY_BYTES);

        uint8_t payload[2U * SD_FX_BLOCK];
        fill_payload(payload, 2U, 0x12U);
        const size_t from = sd_card_trace_length();
        const uint64_t bytes_before = sd_card_bytes_clocked();
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
            block_device_write_blocks(fx.device, 95U, payload, blocks));

        T_CHECK(card_holds(95U, payload, blocks));
        T_CHECK(check_success_trace(from, 95U, blocks, false) == NULL);
        const sd_event_t *command = NULL;
        for (size_t i = from; i < sd_card_trace_length(); ++i) {
            const sd_event_t *const e = sd_card_trace_at(i);
            if (e->kind == SD_EV_COMMAND) {
                command = e;
                break;
            }
        }
        T_CHECK(command != NULL);
        T_CHECK(command->byte_index >= bytes_before + (uint64_t)BUSY_BYTES);
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    }
    t_clear_context();
}

/* --------------------------------------------------------------- removal */

static void test_removal_at_each_write_phase(void)
{
    /* Card-detect rises in the middle of a write. The driver must report
     * INVALID_DEVICE, must not send CMD12 or stop-tran to a card that is no
     * longer there, must leave chip select high, and must still tear down
     * exactly once. */
    static const struct { sd_phase_t phase; uint32_t offset; } points[] = {
        { SD_PHASE_RESPONSE_WAIT, 0U },
        { SD_PHASE_R1, 0U },
        { SD_PHASE_WRITE_TOKEN, 0U },
        { SD_PHASE_WRITE_PAYLOAD, 0U },
        { SD_PHASE_WRITE_PAYLOAD, 100U },
        { SD_PHASE_WRITE_PAYLOAD, 511U },
        { SD_PHASE_WRITE_PAYLOAD, 513U }, /* the second CRC byte */
        { SD_PHASE_WRITE_RESPONSE, 0U },
        { SD_PHASE_BUSY, 0U },
        { SD_PHASE_BUSY, 3U },
    };

    for (size_t blocks = 1U; blocks <= 2U; ++blocks) {
        for (size_t p = 0U; p < sizeof(points) / sizeof(points[0]); ++p) {
            const uint32_t occurrences =
                phase_is_per_command(points[p].phase) ? 1U : (uint32_t)blocks;
            for (uint32_t occurrence = 0U; occurrence < occurrences; ++occurrence) {
                sd_fixture_t fx;
                sd_card_desc_t desc = sd_fx_card_sdhc();
                desc.program_us = 2000U; /* so a busy phase exists */
                t_context("removal during %s[%u] occurrence %u of a %zu-block write",
                    sd_phase_name(points[p].phase), (unsigned)points[p].offset,
                    (unsigned)occurrence, blocks);
                T_CHECK(sd_fx_require_init(&fx, &desc));

                sd_fault_t fault;
                memset(&fault, 0, sizeof(fault));
                fault.phase = points[p].phase;
                fault.command = blocks > 1U ? 25U : 24U;
                fault.occurrence = occurrence;
                fault.byte_offset = points[p].offset;
                fault.kind = SD_FAULT_EJECT;
                T_CHECK(sd_card_add_fault(&fault));

                uint8_t payload[2U * SD_FX_BLOCK];
                fill_payload(payload, 2U, 0x34U);
                const size_t from = sd_card_trace_length();
                const size_t cmd12_before = sd_card_command_count(12U);
                T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
                    block_device_write_blocks(fx.device, 96U, payload, blocks));

                if (sd_card_fault_activations(0U) != 1U) {
                    t_context("removal during %s[%u] occurrence %u of a "
                        "%zu-block write: the eject fault never fired",
                        sd_phase_name(points[p].phase),
                        (unsigned)points[p].offset, (unsigned)occurrence,
                        blocks);
                    T_CHECK(false);
                }
                T_CHECK(atomic_load(&fx.sd.removal_latched));
                T_EQ_U(cmd12_before, sd_card_command_count(12U));
                T_EQ_U(0U, events_from(from, SD_EV_STOP_TRAN));
                /* The driver stops clocking promptly: the removal is checked
                 * after every byte of the payload, not only at the end of the
                 * block, so at most the token and one data byte (or the
                 * response byte) may follow the edge before chip select
                 * rises. A driver that finishes the block first would still
                 * return INVALID_DEVICE, which is why the return code alone
                 * is not enough here. */
                const size_t release = last_release_from(from);
                T_CHECK(release != SIZE_MAX);
                T_CHECK(sd_card_trace_at(release)->byte_index
                    <= sd_card_fault_activation_byte(0U) + 3U);
                /* The interrupt handler must not tear anything down itself. */
                T_CHECK(pico_mock_spi_is_initialized());
                T_EQ_U(0U, pico_mock_spi_deinit_count());
                T_CHECK(!pico_mock_gpio_irq_is_enabled(SD_FX_PIN_CARD_DETECT));
                T_CHECK(pico_mock_gpio_level(SD_FX_PIN_CS));

                const char *problem = sd_fx_check_removed_and_teardown_once(&fx);
                if (problem != NULL) {
                    t_context("removal during %s[%u] occurrence %u of a "
                        "%zu-block write: %s",
                        sd_phase_name(points[p].phase),
                        (unsigned)points[p].offset, (unsigned)occurrence,
                        blocks, problem);
                }
                T_CHECK(problem == NULL);
            }
        }
    }
    t_clear_context();
}

static void test_removal_during_the_release_clock_cancels_success(void)
{
    /* The card stored every block, and then the removal edge landed on the
     * byte the driver clocks after raising chip select. The operation is
     * still executing, so it must fail: a late completion must never turn a
     * cancelled request into a success. Read-path equivalent: SD-001. */
    for (size_t blocks = 1U; blocks <= 3U; blocks += 2U) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("%zu-block write, removal during the release clock", blocks);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        sd_fault_t fault;
        memset(&fault, 0, sizeof(fault));
        fault.phase = SD_PHASE_RELEASE;
        fault.command = SD_ANY_COMMAND;
        fault.occurrence = sd_card_phase_entries(SD_PHASE_RELEASE);
        fault.kind = SD_FAULT_EJECT;
        T_CHECK(sd_card_add_fault(&fault));

        uint8_t payload[3U * SD_FX_BLOCK];
        fill_payload(payload, 3U, 0x56U);
        const block_device_result_t result =
            block_device_write_blocks(fx.device, 97U, payload, blocks);
        if (sd_card_fault_activations(0U) == 0U) {
            t_context("%zu-block write: the release-phase fault never fired",
                blocks);
            T_CHECK(false);
        }
        T_CHECK(atomic_load(&fx.sd.removal_latched));
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_DEVICE, result);
        /* The data did reach the card; the result is about the request. */
        T_CHECK(card_holds(97U, payload, blocks));
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
        T_CHECK(sd_fx_check_removed_and_teardown_once(&fx) == NULL);
    }
    t_clear_context();
}

/* ---------------------------------------------------- the invariant sweep */

static const char *fault_label(sd_fault_kind_t kind)
{
    switch (kind) {
    case SD_FAULT_STALL: return "stall(0xFF forever)";
    case SD_FAULT_BUSY_FOREVER: return "busy(0x00 forever)";
    case SD_FAULT_GARBAGE: return "garbage-byte";
    case SD_FAULT_FLIP_BITS: return "flip-bits";
    case SD_FAULT_RANDOM_STREAM: return "random-stream";
    default: return "?";
    }
}

/*
 * Every fault at every phase of a write, against the invariants that hold no
 * matter what the card did: the call returns inside the budgets the driver
 * declares, the bus is quiescent, OK is only reported when every block is
 * actually on the card with the right content, and the device is usable and
 * tearable afterwards. The false-success check is the one that matters most
 * for a write: a read that fails loudly loses nothing, a write that fails
 * quietly loses the caller's data.
 */
static void sweep_write_faults(size_t blocks)
{
    static const sd_phase_t phases[] = {
        SD_PHASE_RESPONSE_WAIT,
        SD_PHASE_R1,
        SD_PHASE_WRITE_TOKEN,
        SD_PHASE_WRITE_PAYLOAD,
        SD_PHASE_WRITE_RESPONSE,
        SD_PHASE_BUSY,
    };
    static const struct { sd_fault_kind_t kind; uint32_t param; } faults[] = {
        { SD_FAULT_STALL, 0U },
        { SD_FAULT_BUSY_FOREVER, 0U },
        { SD_FAULT_GARBAGE, 0x5AU },
        { SD_FAULT_FLIP_BITS, 0xFFU },
        { SD_FAULT_RANDOM_STREAM, 0x2B7E1516U },
    };
    static const uint32_t offsets[] = { 0U, 1U, 300U };

    const uint8_t command = blocks > 1U ? 25U : 24U;

    for (size_t p = 0U; p < sizeof(phases) / sizeof(phases[0]); ++p) {
        for (size_t f = 0U; f < sizeof(faults) / sizeof(faults[0]); ++f) {
            for (size_t o = 0U; o < sizeof(offsets) / sizeof(offsets[0]); ++o) {
                if (offsets[o] > 1U && phases[p] != SD_PHASE_WRITE_PAYLOAD) {
                    continue;
                }
                const uint32_t occurrences =
                    phase_is_per_command(phases[p]) ? 1U : (uint32_t)blocks;
                for (uint32_t occurrence = 0U; occurrence < occurrences; ++occurrence) {
                    sd_fixture_t fx;
                    sd_card_desc_t desc = sd_fx_card_sdhc();
                    desc.program_us = 1000U; /* so the busy phase exists */
                    t_context("blocks=%zu phase=%s occurrence=%u fault=%s offset=%u",
                        blocks, sd_phase_name(phases[p]), (unsigned)occurrence,
                        fault_label(faults[f].kind), (unsigned)offsets[o]);
                    T_CHECK(sd_fx_require_init(&fx, &desc));

                    sd_fault_t fault;
                    memset(&fault, 0, sizeof(fault));
                    fault.phase = phases[p];
                    fault.command = command;
                    fault.occurrence = occurrence;
                    fault.byte_offset = offsets[o];
                    fault.kind = faults[f].kind;
                    fault.param = faults[f].param;
                    T_CHECK(sd_card_add_fault(&fault));

                    uint8_t payload[3U * SD_FX_BLOCK];
                    fill_payload(payload, 3U, (uint8_t)(0x78U + occurrence));
                    const uint64_t start_us = pico_mock_now_us();
                    const block_device_result_t result =
                        block_device_write_blocks(fx.device, 98U, payload, blocks);
                    const uint64_t elapsed_us = pico_mock_now_us() - start_us;

                    /* 1. Bounded: one command ready wait, one programming
                     *    wait per block, one for stop-tran, and the error
                     *    path's wait plus CMD12's own. */
                    const uint64_t budget = (uint64_t)COMMAND_READY_WAIT_US * 2U
                        + (uint64_t)WRITE_BUSY_WAIT_US * (blocks + 2U)
                        + UINT64_C(20000);
                    T_CHECK(elapsed_us <= budget);

                    /* 2. Released and not mid-answer. */
                    const char *problem = sd_fx_check_bus_quiescent(&fx);
                    if (problem != NULL) {
                        t_context("blocks=%zu phase=%s occurrence=%u fault=%s "
                            "offset=%u: %s", blocks, sd_phase_name(phases[p]),
                            (unsigned)occurrence, fault_label(faults[f].kind),
                            (unsigned)offsets[o], problem);
                    }
                    T_CHECK(problem == NULL);

                    /* 3. No false success. */
                    if (result == BLOCK_DEVICE_RESULT_OK) {
                        T_CHECK(card_holds(98U, payload, blocks));
                    }
                    /* And a defined result in every case. */
                    T_CHECK(result == BLOCK_DEVICE_RESULT_OK
                        || result == BLOCK_DEVICE_RESULT_IO_ERROR
                        || result == BLOCK_DEVICE_RESULT_BUSY_TIMEOUT);

                    /* 4. Still usable, still tearable exactly once. */
                    T_CHECK(pico_mock_spi_is_initialized());
                    T_CHECK(fx.sd.initialized);
                    const char *recovery = sd_fx_check_recovers(&fx, 9U);
                    if (recovery != NULL) {
                        t_context("blocks=%zu phase=%s occurrence=%u fault=%s "
                            "offset=%u: %s", blocks, sd_phase_name(phases[p]),
                            (unsigned)occurrence, fault_label(faults[f].kind),
                            (unsigned)offsets[o], recovery);
                    }
                    T_CHECK(recovery == NULL);
                    const size_t deinits = pico_mock_spi_deinit_count();
                    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
                        block_device_deinit(fx.device));
                    T_EQ_U(deinits + 1U, pico_mock_spi_deinit_count());
                }
            }
        }
    }
    t_clear_context();
}

static void test_single_block_write_fault_sweep(void) { sweep_write_faults(1U); }
static void test_multi_block_write_fault_sweep(void) { sweep_write_faults(3U); }

/* ---------------------------------------------------- argument contract */

static void test_argument_validation_costs_no_bus_traffic(void)
{
    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_sdhc();
    sd_fx_begin(&fx, &desc);

    uint8_t payload[2U * SD_FX_BLOCK];
    fill_payload(payload, 2U, 0x9AU);

    /* Before initialisation the backend has no geometry to check against. */
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, sd_spi_configure(&fx.sd, &fx.config));
    fx.device = sd_spi_as_block_device(&fx.sd);
    T_CHECK(fx.device != NULL);
    size_t before = pico_mock_spi_transfer_count();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_NOT_INITIALIZED,
        block_device_write_blocks(fx.device, 0U, payload, 1U));
    T_EQ_U(before, pico_mock_spi_transfer_count());

    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_init(fx.device));
    const uint64_t capacity = fx.sd.block_count;
    T_EQ_U(sd_card_block_count(), capacity);

    static const struct {
        const char *name;
        uint64_t lba_from_end; /* subtracted from capacity; UINT64_MAX = raw */
        uint64_t raw_lba;
        size_t blocks;
        block_device_result_t expected;
    } rows[] = {
        { "first block past the end", 0U, 0U, 1U, BLOCK_DEVICE_RESULT_OUT_OF_RANGE },
        { "last block, two requested", 1U, 0U, 2U, BLOCK_DEVICE_RESULT_OUT_OF_RANGE },
        { "far past the end", UINT64_MAX, UINT64_MAX, 1U, BLOCK_DEVICE_RESULT_OUT_OF_RANGE },
        { "count wraps the address space", UINT64_MAX, 1U, SIZE_MAX, BLOCK_DEVICE_RESULT_OUT_OF_RANGE },
        { "zero blocks", UINT64_MAX, 5U, 0U, BLOCK_DEVICE_RESULT_INVALID_ARGUMENT },
    };
    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        t_context("%s", rows[i].name);
        const uint64_t lba = rows[i].lba_from_end == UINT64_MAX
            ? rows[i].raw_lba
            : capacity - rows[i].lba_from_end;
        before = pico_mock_spi_transfer_count();
        T_EQ_RESULT(rows[i].expected,
            block_device_write_blocks(fx.device, lba, payload, rows[i].blocks));
        T_EQ_U(before, pico_mock_spi_transfer_count());
    }
    t_context("null source");
    before = pico_mock_spi_transfer_count();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        block_device_write_blocks(fx.device, 5U, NULL, 1U));
    /* The backend must hold the same line without the wrapper's guard. */
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        fx.device->operations->write_blocks(fx.device->context, 5U, NULL, 1U));
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT,
        fx.device->operations->write_blocks(fx.device->context, 5U, payload, 0U));
    T_EQ_U(before, pico_mock_spi_transfer_count());

    /* The boundary itself is writable: the last block alone, and the last
     * two together. */
    t_context("last block");
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(fx.device, capacity - 1U, payload, 1U));
    T_CHECK(card_holds(capacity - 1U, payload, 1U));
    t_context("last two blocks");
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
        block_device_write_blocks(fx.device, capacity - 2U, payload, 2U));
    T_CHECK(card_holds(capacity - 2U, payload, 2U));
    T_EQ_U(0U, sd_card_protocol_errors());
    T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);

    /* After removal every write is refused before the bus, even in range. */
    t_context("after removal");
    T_CHECK(pico_mock_gpio_irq_fire(SD_FX_PIN_CARD_DETECT, GPIO_IRQ_EDGE_RISE));
    before = pico_mock_spi_transfer_count();
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_DEVICE,
        block_device_write_blocks(fx.device, 5U, payload, 1U));
    T_EQ_U(before, pico_mock_spi_transfer_count());
    T_CHECK(sd_fx_check_removed_and_teardown_once(&fx) == NULL);
    t_clear_context();
}

/* --------------------------------------------------------------- main */

int main(void)
{
    t_run(test_single_block_write_stores_the_data_on_every_card_kind,
        "CMD24 stores the data on every card kind");
    t_run(test_multi_block_write_stores_every_block_in_order,
        "CMD25 stores every block in order");
    t_run(test_writes_are_independent_transactions,
        "writes and reads interleave without leaking state");
    t_run(test_data_response_upper_bits_are_dont_care,
        "data-response token upper bits are don't-care");
    t_run(test_rejected_data_response_is_an_error,
        "a rejected data-response token is an error, CMD12 only for CMD25");
    t_run(test_rejection_followed_by_busy_is_waited_out,
        "a rejection followed by busy is waited out before release");
    t_run(test_rejection_mid_stream_stops_the_transfer,
        "a rejection mid-stream stops the transfer with CMD12");
    t_run(test_unknown_data_response_is_an_error,
        "an unknown data-response byte is never acceptance");
    t_run(test_r1_errors_abort_before_any_data,
        "an R1 error aborts before any data is sent");
    t_run(test_missing_r1_is_an_error,
        "a missing R1 is an error");
    t_run(test_programming_busy_is_awaited_between_blocks_and_before_release,
        "programming busy is awaited between blocks and before release");
    t_run(test_single_block_programming_busy_is_awaited,
        "single-block programming busy is awaited before release");
    t_run(test_programming_busy_beyond_the_budget_is_a_busy_timeout,
        "programming busy beyond the budget is BUSY_TIMEOUT");
    t_run(test_stop_tran_busy_beyond_the_budget_is_a_busy_timeout,
        "stop-tran busy beyond the budget is BUSY_TIMEOUT");
    t_run(test_stop_tran_busy_that_starts_one_byte_late_is_still_awaited,
        "N_BR: busy that starts one byte after stop-tran is awaited");
    t_run(test_a_busy_card_is_awaited_before_the_write_command,
        "a busy card is awaited before the write command");
    t_run(test_removal_at_each_write_phase,
        "removal injected at each write phase");
    t_run(test_removal_during_the_release_clock_cancels_success,
        "removal during the release clock cancels success");
    t_run(test_single_block_write_fault_sweep,
        "fault sweep across single-block write phases");
    t_run(test_multi_block_write_fault_sweep,
        "fault sweep across multi-block write phases");
    t_run(test_argument_validation_costs_no_bus_traffic,
        "argument validation costs no bus traffic");
    return t_summary("sd_writes");
}
