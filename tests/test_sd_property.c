/*
 * Property and fuzz tests for the SD driver.
 *
 * These cover the parts where a hand-written table is likely to miss the one
 * combination that matters: CSD field arithmetic, address conversion, and the
 * driver's tolerance of arbitrary bytes on the bus. Every randomised case is
 * seeded and prints its seed; pass --seed N to replay a failure exactly.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/gpio.h"
#include "sd_card_model.h"
#include "sd_fixture.h"
#include "test_harness.h"
#include "test_rand.h"

static uint64_t suite_seed = 20260904U;

/* ------------------------------------------------------ CSD arithmetic */

/*
 * Build a CSD v1 register directly from the fields, independently of the
 * model's own encoder, and hand it to the driver through a real CMD9. The
 * expected capacity is computed here from the specification formula, so the
 * driver's decoder is checked against arithmetic rather than against another
 * copy of itself.
 */
static void encode_csd_v1(
    uint8_t csd[16],
    uint32_t c_size,
    uint8_t c_size_mult,
    uint8_t read_bl_len)
{
    memset(csd, 0, 16U);
    csd[0] = 0x00U;                                   /* CSD_STRUCTURE = 0 */
    csd[5] = (uint8_t)(0x50U | (read_bl_len & 0x0FU));/* READ_BL_LEN */
    csd[6] = (uint8_t)((c_size >> 10U) & 0x03U);      /* C_SIZE 73..72 */
    csd[7] = (uint8_t)(c_size >> 2U);                 /* C_SIZE 71..64 */
    csd[8] = (uint8_t)((c_size & 0x03U) << 6U);       /* C_SIZE 63..62 */
    csd[9] = (uint8_t)((c_size_mult >> 1U) & 0x03U);  /* C_SIZE_MULT 49..48 */
    csd[10] = (uint8_t)((c_size_mult & 0x01U) << 7U); /* C_SIZE_MULT 47 */
    csd[15] = (uint8_t)(((unsigned)sd_crc7(csd, 15U) << 1U) | 1U);
}

static void encode_csd_v2(uint8_t csd[16], uint32_t c_size)
{
    memset(csd, 0, 16U);
    csd[0] = 0x40U;                                   /* CSD_STRUCTURE = 1 */
    csd[5] = 0x59U;                                   /* READ_BL_LEN = 9 */
    csd[7] = (uint8_t)((c_size >> 16U) & 0x3FU);      /* C_SIZE 69..64 */
    csd[8] = (uint8_t)(c_size >> 8U);                 /* C_SIZE 63..56 */
    csd[9] = (uint8_t)c_size;                         /* C_SIZE 55..48 */
    csd[15] = (uint8_t)(((unsigned)sd_crc7(csd, 15U) << 1U) | 1U);
}

/*
 * Returns NULL when the case holds, or a description of what went wrong.
 * A bare bool would report only "the case failed", leaving the reader to work
 * out whether it was the result, the capacity or the address bound.
 */
static const char *check_csd_v1_case(
    uint32_t c_size,
    uint8_t c_size_mult,
    uint8_t read_bl_len)
{
    const uint64_t capacity = ((uint64_t)c_size + 1U)
        * (UINT64_C(1) << (c_size_mult + 2U))
        * (UINT64_C(1) << read_bl_len);
    const uint64_t expected_blocks = capacity / 512U;

    /* Callers only pass READ_BL_LEN 9..11, the range CSD v1 permits, so the
     * capacity is always a whole number of 512-byte blocks and always
     * non-zero. Encodings outside that range are a rejection case rather than
     * a capacity case, and belong to test_csd_structure_and_field_rejection
     * in the protocol suite. Asserted rather than branched on, because a
     * branch here would be unreachable and would read as though the
     * unencodable case were covered. */
    if (read_bl_len < 9U || read_bl_len > 11U) {
        return "test bug: READ_BL_LEN outside the CSD v1 range";
    }

    sd_fixture_t fx;
    sd_card_desc_t desc = sd_fx_card_v2_sdsc();
    sd_fx_begin(&fx, &desc);
    uint8_t csd[16];
    encode_csd_v1(csd, c_size, c_size_mult, read_bl_len);
    sd_card_set_raw_csd(csd);
    const block_device_result_t result = sd_fx_init(&fx);

    t_context("CSD v1 C_SIZE=%u MULT=%u RBL=%u -> capacity %" PRIu64,
        (unsigned)c_size, (unsigned)c_size_mult, (unsigned)read_bl_len,
        capacity);

    if (result != BLOCK_DEVICE_RESULT_OK) {
        return "a legal CSD v1 encoding was refused";
    }
    if (fx.sd.block_count != expected_blocks) {
        return "block count does not match the capacity formula";
    }

    /* The property that keeps the driver's uint32_t command argument honest:
     * for any CSD v1 encoding, the byte address of the last block must fit in
     * 32 bits. The largest legal encoding lands 512 bytes below the limit, so
     * this holds by exactly one block. */
    const uint64_t last_byte_address = (expected_blocks - 1U) * 512U;
    if (last_byte_address > UINT32_MAX) {
        return "the last block's byte address overflows 32 bits";
    }
    return NULL;
}

static void test_csd_v1_capacity_property(void)
{
    /* Every multiplier and every legal block length, against a set of C_SIZE
     * values chosen for their edges, plus a seeded random sample. */
    static const uint32_t interesting[] = {
        0U, 1U, 2U, 3U, 1023U, 1024U, 2047U, 2048U, 4093U, 4094U, 4095U,
    };
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed);

    for (uint8_t read_bl_len = 9U; read_bl_len <= 11U; ++read_bl_len) {
        for (uint8_t mult = 0U; mult <= 7U; ++mult) {
            for (size_t i = 0U;
                    i < sizeof(interesting) / sizeof(interesting[0]); ++i) {
                const char *problem =
                    check_csd_v1_case(interesting[i], mult, read_bl_len);
                if (problem != NULL) {
                    t_context("C_SIZE=%u MULT=%u RBL=%u: %s",
                        (unsigned)interesting[i], (unsigned)mult,
                        (unsigned)read_bl_len, problem);
                }
                T_CHECK(problem == NULL);
            }
            for (unsigned int sample = 0U; sample < 4U; ++sample) {
                const uint32_t c_size = (uint32_t)t_rand_below(&rng, 4096U);
                const char *problem =
                    check_csd_v1_case(c_size, mult, read_bl_len);
                if (problem != NULL) {
                    t_context("C_SIZE=%u MULT=%u RBL=%u (seed %" PRIu64
                        "): %s", (unsigned)c_size, (unsigned)mult,
                        (unsigned)read_bl_len, suite_seed, problem);
                }
                T_CHECK(problem == NULL);
            }
        }
    }
    t_clear_context();
}

static void test_csd_v1_maximum_encoding_is_addressable(void)
{
    /* The single most dangerous encoding: the largest CSD v1 card. Its last
     * block sits at byte address 0xFFFFFE00, one block below a 32-bit
     * overflow. Read that block for real and confirm the address the card
     * received was not truncated. */
    sd_fixture_t fx;
    /* The model's own capacity must match the register the driver is given,
     * or the card would legitimately answer a read past its end with an
     * out-of-range data error token - a useful safety net, but not what this
     * case is asking about. */
    sd_card_desc_t desc = sd_card_desc(SD_CARD_V2_SDSC,
        UINT64_C(4) * 1024U * 1024U * 1024U);
    desc.read_bl_len = 11U;
    sd_fx_begin(&fx, &desc);
    uint8_t csd[16];
    encode_csd_v1(csd, 4095U, 7U, 11U);
    sd_card_set_raw_csd(csd);
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, sd_fx_init(&fx));
    T_EQ_U(UINT64_C(8388608), fx.sd.block_count);

    sd_guarded_buffer_t buffer;
    sd_fx_guard_init(&buffer, 1U);
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
        fx.device, UINT64_C(8388607), sd_fx_guard_data(&buffer), 1U));
    T_EQ_U(UINT64_C(0xFFFFFE00), sd_card_last_argument(17U));
    /* And 512 more bytes would have wrapped, which the range check forbids. */
    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OUT_OF_RANGE, block_device_read_blocks(
        fx.device, UINT64_C(8388608), sd_fx_guard_data(&buffer), 1U));
}

static void test_csd_v2_capacity_property(void)
{
    static const uint32_t interesting[] = {
        0U, 1U, 4111U, 4112U, 4113U,          /* around the SDHC minimum */
        65375U, 65376U,                        /* around the 32 GB boundary */
        0x3FFFFEU, 0x3FFFFFU,                  /* the largest encodings */
    };
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed + 1U);

    for (size_t i = 0U;
            i < (sizeof(interesting) / sizeof(interesting[0])) + 24U; ++i) {
        const uint32_t c_size = i < sizeof(interesting) / sizeof(interesting[0])
            ? interesting[i]
            : (uint32_t)t_rand_below(&rng, 0x400000U);
        const uint64_t expected_blocks = ((uint64_t)c_size + 1U) * 1024U;

        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("CSD v2 C_SIZE=%u -> %" PRIu64 " blocks",
            (unsigned)c_size, expected_blocks);
        sd_fx_begin(&fx, &desc);
        uint8_t csd[16];
        encode_csd_v2(csd, c_size);
        sd_card_set_raw_csd(csd);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, sd_fx_init(&fx));
        T_EQ_U(expected_blocks, fx.sd.block_count);
        /* Block addressing means the last LBA is sent verbatim, so it must
         * fit in the driver's 32-bit argument. The 22-bit C_SIZE field makes
         * the largest possible last LBA exactly UINT32_MAX. */
        T_CHECK(expected_blocks - 1U <= UINT32_MAX);
    }
    t_clear_context();
}

/* -------------------------------------------------- address conversion */

static void test_address_conversion_property(void)
{
    /* For random cards and random in-range addresses, the argument the card
     * receives must be the LBA for block-addressed cards and exactly 512
     * times the LBA for byte-addressed ones - with no truncation anywhere. */
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed + 2U);

    /* Every iteration whose card the CSD cannot express is skipped below. If
     * bring-up were to break entirely, every iteration would skip and this
     * case would pass having asserted nothing, so the number that actually
     * ran is itself asserted. */
    unsigned int exercised = 0U;

    for (unsigned int iteration = 0U; iteration < 200U; ++iteration) {
        const bool high_capacity = (t_rand_next(&rng) & 1U) != 0U;
        sd_fixture_t fx;
        sd_card_desc_t desc;
        if (high_capacity) {
            const uint32_t c_size = (uint32_t)t_rand_range(&rng, 4112U, 0x3FFFFFU);
            desc = sd_card_desc(SD_CARD_SDHC,
                ((uint64_t)c_size + 1U) * 512U * 1024U);
        } else {
            const uint8_t rbl = (uint8_t)t_rand_range(&rng, 9U, 11U);
            const uint32_t c_size = (uint32_t)t_rand_below(&rng, 4096U);
            const uint8_t mult = (uint8_t)t_rand_below(&rng, 8U);
            const uint64_t capacity = ((uint64_t)c_size + 1U)
                * (UINT64_C(1) << (mult + 2U)) * (UINT64_C(1) << rbl);
            desc = sd_card_desc(SD_CARD_V2_SDSC, capacity);
            desc.read_bl_len = rbl;
        }
        t_context("iteration %u (%s, seed %" PRIu64 ")", iteration,
            high_capacity ? "high capacity" : "byte addressed", suite_seed);
        sd_fx_begin(&fx, &desc);
        if (!high_capacity) {
            /* Drive the exact fields rather than the model's rounding. */
            uint8_t csd[16];
            const uint64_t units = desc.capacity_bytes >> desc.read_bl_len;
            uint32_t c_size = 0U;
            uint8_t mult = 0U;
            for (uint8_t m = 0U; m <= 7U; ++m) {
                const uint64_t divisor = UINT64_C(1) << (m + 2U);
                if (units != 0U && (units % divisor) == 0U
                        && (units / divisor) >= 1U
                        && (units / divisor) <= 4096U) {
                    c_size = (uint32_t)((units / divisor) - 1U);
                    mult = m;
                    break;
                }
            }
            encode_csd_v1(csd, c_size, mult, desc.read_bl_len);
            sd_card_set_raw_csd(csd);
        }
        if (sd_fx_init(&fx) != BLOCK_DEVICE_RESULT_OK) {
            continue; /* capacity the CSD cannot express; covered elsewhere */
        }
        exercised++;
        T_CHECK(fx.sd.block_count > 0U);

        for (unsigned int probe = 0U; probe < 4U; ++probe) {
            const uint64_t lba = probe == 0U ? 0U
                : (probe == 1U ? fx.sd.block_count - 1U
                    : t_rand_below(&rng, fx.sd.block_count));
            sd_guarded_buffer_t buffer;
            sd_fx_guard_init(&buffer, 1U);
            t_context("iteration %u lba=%" PRIu64 " (seed %" PRIu64 ")",
                iteration, lba, suite_seed);
            T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
                fx.device, lba, sd_fx_guard_data(&buffer), 1U));
            const uint64_t expected = high_capacity ? lba : lba * 512U;
            T_EQ_U(expected, sd_card_last_argument(17U));
            T_CHECK(sd_fx_guard_matches_card(&buffer, lba));
            /* A byte address the card considers misaligned or out of range
             * would have been recorded as a protocol error. */
            T_EQ_U(0U, sd_card_protocol_errors());
        }
    }
    t_context("%u of 200 iterations produced an encodable card", exercised);
    /* Comfortably below what a healthy run reaches, but far above zero: this
     * fails loudly if bring-up regresses into skipping every iteration. */
    T_CHECK(exercised >= 150U);
    t_clear_context();
}

/* ------------------------------------------------------- response fuzz */

/*
 * Two predicates rather than one, because the two fuzzes call reads under
 * different preconditions and a single permissive set would be unfailable.
 *
 * Three results are a driver logic error in *both* fuzzes, because the
 * corresponding situation never holds: the arguments are always valid
 * (INVALID_ARGUMENT), the LBA is always inside the card's capacity
 * (OUT_OF_RANGE), and reads are implemented (NOT_IMPLEMENTED). Excluding them
 * is what gives these checks teeth.
 */
static bool read_result_is_legal_on_ready_device(block_device_result_t result)
{
    /* The device is initialised and the media is present; only the card's
     * byte stream is random. NOT_INITIALIZED would therefore be wrong too. */
    switch (result) {
    case BLOCK_DEVICE_RESULT_OK:
    case BLOCK_DEVICE_RESULT_IO_ERROR:
    case BLOCK_DEVICE_RESULT_BUSY_TIMEOUT:
    case BLOCK_DEVICE_RESULT_INVALID_DEVICE:
        return true;
    case BLOCK_DEVICE_RESULT_INVALID_ARGUMENT:
    case BLOCK_DEVICE_RESULT_NOT_INITIALIZED:
    case BLOCK_DEVICE_RESULT_OUT_OF_RANGE:
    case BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED:
    default:
        return false;
    }
}

static bool read_result_is_legal_in_any_state(block_device_result_t result)
{
    /* The lifecycle fuzz reads at arbitrary points, so an uninitialised or
     * removed device is a legitimate state to be in. The exact result is
     * still asserted per state by the branches at the call site; this is the
     * coarse net that catches a result no state could justify. */
    switch (result) {
    case BLOCK_DEVICE_RESULT_OK:
    case BLOCK_DEVICE_RESULT_IO_ERROR:
    case BLOCK_DEVICE_RESULT_BUSY_TIMEOUT:
    case BLOCK_DEVICE_RESULT_INVALID_DEVICE:
    case BLOCK_DEVICE_RESULT_NOT_INITIALIZED:
        return true;
    case BLOCK_DEVICE_RESULT_INVALID_ARGUMENT:
    case BLOCK_DEVICE_RESULT_OUT_OF_RANGE:
    case BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED:
    default:
        return false;
    }
}

static void test_random_response_stream(void)
{
    /* From a chosen point in the transaction the card answers arbitrary bytes.
     * The driver's response parser then sees random R1 values, random tokens,
     * random payload and random CRC. Whatever it decides, it must terminate
     * inside its declared budgets, release the bus, stay inside the caller's
     * buffer and return a defined result. This is the case a hand-written
     * table cannot enumerate: there are 2^8 possibilities at every byte. */
    static const sd_phase_t entry_points[] = {
        SD_PHASE_RESPONSE_WAIT,
        SD_PHASE_R1,
        SD_PHASE_DATA_TOKEN,
        SD_PHASE_DATA_PAYLOAD,
    };

    for (unsigned int iteration = 0U; iteration < 500U; ++iteration) {
        t_rand_t rng;
        t_rand_seed(&rng, suite_seed + 1000U + iteration);
        const size_t blocks = (size_t)t_rand_range(&rng, 1U, 3U);
        const uint8_t command = blocks == 1U ? 17U : 18U;
        const sd_phase_t phase = entry_points[
            t_rand_below(&rng, sizeof(entry_points) / sizeof(entry_points[0]))];
        const uint32_t offset = (uint32_t)t_rand_below(&rng, 8U);
        const uint32_t stream_seed = (uint32_t)t_rand_next(&rng);

        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("iteration %u phase=%s offset=%u stream_seed=0x%08x "
            "(replay: --seed %" PRIu64 ")",
            iteration, sd_phase_name(phase), (unsigned)offset,
            (unsigned)stream_seed, suite_seed);
        T_CHECK(sd_fx_require_init(&fx, &desc));

        sd_fault_t fault;
        memset(&fault, 0, sizeof(fault));
        fault.phase = phase;
        fault.command = command;
        fault.byte_offset = offset;
        fault.kind = SD_FAULT_RANDOM_STREAM;
        fault.param = stream_seed;
        T_CHECK(sd_card_add_fault(&fault));

        sd_guarded_buffer_t buffer;
        sd_fx_guard_init(&buffer, blocks);
        const uint64_t start_us = pico_mock_now_us();
        const block_device_result_t result = block_device_read_blocks(
            fx.device, 64U, sd_fx_guard_data(&buffer), blocks);
        const uint64_t elapsed_us = pico_mock_now_us() - start_us;

        T_CHECK(read_result_is_legal_on_ready_device(result));
        /* Termination inside the budgets the driver declares for itself. */
        T_CHECK(elapsed_us <= UINT64_C(3000000)
            + (UINT64_C(100000) * (blocks + 1U)));
        T_CHECK(sd_fx_guard_intact(&buffer));
        const char *problem = sd_fx_check_bus_quiescent(&fx);
        if (problem != NULL) {
            t_context("iteration %u: %s (replay: --seed %" PRIu64 ")",
                iteration, problem, suite_seed);
        }
        T_CHECK(problem == NULL);
        /* And the driver survives: once the card behaves again, so does it. */
        const char *recovery = sd_fx_check_recovers(&fx, 5U);
        if (recovery != NULL) {
            t_context("iteration %u: %s (replay: --seed %" PRIu64 ")",
                iteration, recovery, suite_seed);
        }
        T_CHECK(recovery == NULL);
    }
    t_clear_context();
}

/* ---------------------------------------------------- operation fuzz */

static void test_random_operation_sequences(void)
{
    /* Random lifecycles: configure, initialise, read, tear down, remove and
     * reinitialise in arbitrary order. The driver must never report success
     * for an operation its state cannot support, and the observable state must
     * always agree with what it returned. */
    for (unsigned int run = 0U; run < 100U; ++run) {
        t_rand_t rng;
        t_rand_seed(&rng, suite_seed + 5000U + run);
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        sd_fx_begin(&fx, &desc);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
            sd_spi_configure(&fx.sd, &fx.config));
        fx.device = sd_spi_as_block_device(&fx.sd);
        T_CHECK(fx.device != NULL);

        /* The expectations below are a hand-written model of the driver's
         * documented state machine, kept deliberately independent of the
         * implementation: the latch is only armed by an interrupt, which can
         * only fire while the card-detect handler is registered, which only
         * happens while the device is initialised. */
        bool initialized = false;
        bool latched = false;
        bool media_present = true;
        /* The handler disables itself on the first edge so contact bounce
         * cannot produce an interrupt storm; only a fresh bring-up re-arms
         * it. Modelling that here means the sequence test asserts the
         * suppression rather than tripping over it. */
        bool irq_armed = false;

        for (unsigned int step = 0U; step < 12U; ++step) {
            const unsigned int action = (unsigned int)t_rand_below(&rng, 5U);
            t_context("run %u step %u action %u (init=%d latch=%d media=%d, "
                "replay: --seed %" PRIu64 ")",
                run, step, action, (int)initialized, (int)latched,
                (int)media_present, suite_seed);

            if (action == 0U) { /* initialise */
                const block_device_result_t result =
                    block_device_init(fx.device);
                if (initialized) {
                    /* Re-initialising an already initialised device is a
                     * caller error and must not disturb it. */
                    T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_ARGUMENT, result);
                    T_CHECK(fx.sd.initialized);
                } else if (!media_present) {
                    T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_DEVICE, result);
                    T_CHECK(!fx.sd.initialized);
                } else {
                    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, result);
                    initialized = true;
                    /* A successful bring-up clears the removal latch and
                     * re-arms the card-detect interrupt. */
                    latched = false;
                    irq_armed = true;
                }
            } else if (action == 1U) { /* read */
                sd_guarded_buffer_t buffer;
                const size_t blocks = (size_t)t_rand_range(&rng, 1U, 3U);
                sd_fx_guard_init(&buffer, blocks);
                const uint64_t lba = fx.sd.block_count > blocks
                    ? t_rand_below(&rng, fx.sd.block_count - blocks) : 0U;
                const block_device_result_t result = block_device_read_blocks(
                    fx.device, lba, sd_fx_guard_data(&buffer), blocks);
                T_CHECK(read_result_is_legal_in_any_state(result));
                T_CHECK(sd_fx_guard_intact(&buffer));
                if (latched) {
                    /* The latch outranks everything: it is checked before the
                     * initialisation state, and it survives teardown. */
                    T_EQ_RESULT(BLOCK_DEVICE_RESULT_INVALID_DEVICE, result);
                    T_CHECK(sd_fx_guard_untouched(&buffer));
                } else if (!initialized) {
                    T_EQ_RESULT(BLOCK_DEVICE_RESULT_NOT_INITIALIZED, result);
                    T_CHECK(sd_fx_guard_untouched(&buffer));
                } else {
                    T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, result);
                    T_CHECK(sd_fx_guard_matches_card(&buffer, lba));
                }
            } else if (action == 2U) { /* tear down */
                const size_t before = pico_mock_spi_deinit_count();
                T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK,
                    block_device_deinit(fx.device));
                /* Exactly one hardware release per initialisation, ever. */
                T_EQ_U(before + (initialized ? 1U : 0U),
                    pico_mock_spi_deinit_count());
                T_CHECK(!fx.sd.initialized);
                initialized = false;
                irq_armed = false; /* teardown unregisters the handler */
            } else if (action == 3U) { /* removal edge */
                media_present = false;
                const bool fired = sd_fx_remove_card();
                T_EQ_U(irq_armed ? 1U : 0U, fired ? 1U : 0U);
                if (fired) {
                    latched = true;
                    irq_armed = false; /* suppressed until a fresh bring-up */
                }
                T_EQ_U(latched ? 1U : 0U,
                    atomic_load(&fx.sd.removal_latched) ? 1U : 0U);
                T_EQ_U(irq_armed ? 1U : 0U,
                    pico_mock_gpio_irq_is_enabled(SD_FX_PIN_CARD_DETECT)
                        ? 1U : 0U);
            } else { /* reinsertion */
                sd_fx_set_card_present(true);
                sd_card_reset(&desc);
                sd_card_set_response_policy(SD_RESPONSE_MODELLED);
                media_present = true;
            }

            /* Whatever happened, the SPI peripheral is owned exactly while
             * the driver says it is initialised, and once the driver has ever
             * driven chip select it is never left asserted between
             * operations. */
            T_EQ_U(fx.sd.initialized ? 1U : 0U,
                pico_mock_spi_is_initialized() ? 1U : 0U);
            if (pico_mock_gpio_was_initialized(SD_FX_PIN_CS)
                    || pico_mock_gpio_was_deinitialized(SD_FX_PIN_CS)) {
                T_CHECK(pico_mock_gpio_level(SD_FX_PIN_CS));
            }
        }
    }
    t_clear_context();
}

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--seed=", 7) == 0) {
            suite_seed = strtoull(&argv[i][7], NULL, 0);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            suite_seed = strtoull(argv[++i], NULL, 0);
        } else {
            (void)fprintf(stderr, "usage: %s [--seed N]\n", argv[0]);
            return 2;
        }
    }
    (void)printf("sd_property: seed %" PRIu64 " (replay with --seed %" PRIu64 ")\n",
        suite_seed, suite_seed);

    t_run(test_csd_v1_capacity_property, "CSD v1 capacity across all field encodings");
    t_run(test_csd_v1_maximum_encoding_is_addressable, "largest CSD v1 card is addressable");
    t_run(test_csd_v2_capacity_property, "CSD v2 capacity across C_SIZE range");
    t_run(test_address_conversion_property, "address conversion on random cards");
    t_run(test_random_response_stream, "random card responses never break the driver");
    t_run(test_random_operation_sequences, "random operation and removal sequences");
    return t_summary("sd_property");
}
