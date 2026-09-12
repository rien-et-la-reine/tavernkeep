/*
 * Unit tests for the production CRC16 helper in src/storage/sd_crc.c.
 *
 * The SD data CRC16 is the CRC-16/XMODEM parameterisation: generator
 * x^16 + x^12 + x^5 + 1 (0x1021), register initialised to zero, most
 * significant bit first, no input or output reflection and no final XOR
 * (SD Physical Layer Simplified Specification, section 4.5). Every literal
 * below is either the published catalogue check value for that
 * parameterisation or derivable from it by hand - none is copied from an
 * implementation, so a helper that disagrees is wrong rather than different.
 *
 * The differential case then cross-checks against sd_crc16_ccitt() in the
 * card model, a separate implementation of the same polynomial, which is
 * already trusted as the oracle for the read-path data CRC.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "sd_card_model.h"
#include "test_harness.h"
#include "test_rand.h"

#include "storage/sd_crc.h"

static uint64_t suite_seed = 20260908U;

/* A full data block is the length that matters on the wire; 528 gives the
 * differential case somewhere past it to run. */
#define CRC16_MAX_LENGTH 528U

/* ------------------------------------------------------------- helpers */

static void format_hex(char *out, size_t out_size, const uint8_t *data,
    size_t length)
{
    static const char digits[] = "0123456789abcdef";
    size_t used = 0U;
    for (size_t i = 0U; i < length && used + 3U < out_size; ++i) {
        out[used++] = digits[(data[i] >> 4U) & 0x0FU];
        out[used++] = digits[data[i] & 0x0FU];
        out[used++] = ' ';
    }
    if (used > 0U) {
        used--;
    }
    out[used] = '\0';
}

static void fill_random(t_rand_t *rng, uint8_t *buffer, size_t length)
{
    for (size_t i = 0U; i < length; ++i) {
        buffer[i] = (uint8_t)(t_rand_next(rng) & 0xFFU);
    }
}

/* -------------------------------------------------- specification vectors */

static void test_specification_vectors(void)
{
    /* The published CRC-16/XMODEM check value: the CRC of the nine ASCII
     * bytes "123456789". This is the standard way a CRC catalogue pins a
     * parameterisation, and it is what distinguishes this variant from
     * CCITT-FALSE (init 0xFFFF, check 0x29B1) and from KERMIT (reflected,
     * check 0x2189). If the helper matches this, its polynomial, its initial
     * value, its bit order and its lack of a final XOR are all confirmed at
     * once. */
    uint8_t check[9] = { 0x31U, 0x32U, 0x33U, 0x34U, 0x35U,
                         0x36U, 0x37U, 0x38U, 0x39U };
    t_context("CRC-16/XMODEM catalogue check value for ASCII 123456789");
    T_EQ_U(0x31C3U, crc_helper_16(check, 9U));

    /* An empty message leaves the register at its initial value. */
    uint8_t empty[1] = { 0x00U };
    t_context("empty message returns the initial register value");
    T_EQ_U(0x0000U, crc_helper_16(empty, 0U));

    /* Feeding zero bits into a zero register can never set one: every
     * feedback decision is 0 XOR 0, so the polynomial is never applied. This
     * holds for any length, including a full 512-byte data block - which is
     * exactly what a freshly erased card returns, so a helper that gets this
     * wrong corrupts the most common block on the card. */
    uint8_t zeros[512];
    memset(zeros, 0, sizeof(zeros));
    t_context("one zero byte");
    T_EQ_U(0x0000U, crc_helper_16(zeros, 1U));
    t_context("a full 512-byte block of zeros");
    T_EQ_U(0x0000U, crc_helper_16(zeros, 512U));

    /* A single 0x01 byte sets exactly one message bit, at bit position 7.
     * Shifted through the remaining 8 bit times it reaches the top of the
     * register and the polynomial is applied exactly once, with no further
     * feedback, leaving the register holding the generator's low 16 bits. */
    uint8_t one[1] = { 0x01U };
    t_context("single 0x01 byte leaves the generator in the register");
    T_EQ_U(0x1021U, crc_helper_16(one, 1U));

    t_clear_context();
}

/* ------------------------------------------------------ differential case */

static void test_matches_independent_implementation(void)
{
    /* 512 is a data block and 16 is a CID/CSD register; the neighbours catch
     * an off-by-one in a block-sized loop. */
    static const size_t lengths[] = {
        0U, 1U, 2U, 3U, 4U, 5U, 15U, 16U, 17U,
        511U, 512U, 513U, 527U, 528U,
    };
    uint8_t buffer[CRC16_MAX_LENGTH];
    char hex[(3U * 32U) + 8U];
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed);

    for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        const size_t length = lengths[i];
        for (unsigned int iteration = 0U; iteration < 32U; ++iteration) {
            fill_random(&rng, buffer, length);
            /* Only the head is quoted: a 512-byte dump would not fit the
             * harness's case context, and the seed replays the rest. */
            format_hex(hex, sizeof(hex), buffer, length < 32U ? length : 32U);
            t_context("length %zu iteration %u head [%s] (replay: --seed %"
                PRIu64 ")", length, iteration, hex, suite_seed);
            T_EQ_U(sd_crc16_ccitt(buffer, length),
                crc_helper_16(buffer, length));
        }
    }

    for (unsigned int iteration = 0U; iteration < 256U; ++iteration) {
        const size_t length =
            (size_t)t_rand_below(&rng, CRC16_MAX_LENGTH + 1U);
        fill_random(&rng, buffer, length);
        format_hex(hex, sizeof(hex), buffer, length < 32U ? length : 32U);
        t_context("random length %zu iteration %u head [%s] (replay: --seed %"
            PRIu64 ")", length, iteration, hex, suite_seed);
        T_EQ_U(sd_crc16_ccitt(buffer, length), crc_helper_16(buffer, length));
    }

    t_clear_context();
}

/* ------------------------------------------------------ single-bit errors */

static void test_detects_every_single_bit_error_in_a_block(void)
{
    /* The property the CRC exists for: a data block differing from the
     * original in exactly one bit must produce a different CRC, or the
     * driver's forthcoming read-path validation would pass corruption
     * through. This generator detects every single-bit error in a message
     * this short, so all 4096 flips must be caught. */
    uint8_t block[512];
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed + 1U);
    fill_random(&rng, block, sizeof(block));

    const uint16_t original = crc_helper_16(block, sizeof(block));
    for (size_t byte = 0U; byte < sizeof(block); ++byte) {
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            const uint8_t mask = (uint8_t)(1U << bit);
            block[byte] = (uint8_t)(block[byte] ^ mask);
            const uint16_t corrupted = crc_helper_16(block, sizeof(block));
            block[byte] = (uint8_t)(block[byte] ^ mask);
            if (corrupted == original) {
                t_context("flipping bit %u of byte %zu left the CRC unchanged "
                    "(0x%04X, replay: --seed %" PRIu64 ")",
                    bit, byte, (unsigned)original, suite_seed);
            }
            T_CHECK(corrupted != original);
        }
    }
    t_clear_context();
}

/* --------------------------------------------------- integration contract */

/*
 * How the helper's return value becomes bytes on the bus, and how a receiver
 * uses it. Both directions of the forthcoming integration depend on this and
 * neither is visible from the value alone.
 *
 * The specification transmits the 16-bit remainder most significant byte
 * first, immediately after the payload. Because this parameterisation starts
 * from a zero register and applies no final XOR, running the CRC over the
 * payload *with those two bytes appended* leaves the register at zero. That
 * residue is the cheap way to validate a received block: append what arrived
 * and check for zero, instead of recomputing and comparing.
 *
 * Asserting it here pins the byte order. Appending the two bytes the other way
 * round does not give zero, so this case fails if the convention is ever
 * reversed at a call site.
 */
static void test_appending_the_crc_leaves_a_zero_residue(void)
{
    static const size_t lengths[] = { 1U, 5U, 16U, 511U, 512U };
    uint8_t framed[CRC16_MAX_LENGTH + 2U];
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed + 2U);

    for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        const size_t length = lengths[i];
        for (unsigned int iteration = 0U; iteration < 16U; ++iteration) {
            fill_random(&rng, framed, length);
            const uint16_t crc = crc_helper_16(framed, length);

            /* Most significant byte first, as the card sends it. */
            framed[length] = (uint8_t)(crc >> 8U);
            framed[length + 1U] = (uint8_t)(crc & 0xFFU);
            t_context("length %zu iteration %u crc 0x%04X (replay: --seed %"
                PRIu64 ")", length, iteration, (unsigned)crc, suite_seed);
            T_EQ_U(0U, crc_helper_16(framed, length + 2U));

            /* And the reversed order does not, so a receiver that validated
             * by residue would reject a byte-swapped transmitter. */
            framed[length] = (uint8_t)(crc & 0xFFU);
            framed[length + 1U] = (uint8_t)(crc >> 8U);
            if (framed[length] != framed[length + 1U]) {
                t_context("length %zu iteration %u byte-swapped crc 0x%04X "
                    "(replay: --seed %" PRIu64 ")",
                    length, iteration, (unsigned)crc, suite_seed);
                T_CHECK(crc_helper_16(framed, length + 2U) != 0U);
            }
        }
    }
    t_clear_context();
}

/*
 * A zero-length message returns the initial register value, which for this
 * parameterisation is 0x0000 - the same value a 512-byte block of zeros
 * produces. The driver must therefore never use "the CRC came out zero" as a
 * stand-in for "there was no data": both are legitimate results. Pinned so the
 * early return in crc_helper_16 stays deliberate.
 */
static void test_empty_message_is_not_a_sentinel(void)
{
    uint8_t any[4] = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
    T_EQ_U(0x0000U, crc_helper_16(any, 0U));
    T_EQ_U(sd_crc16_ccitt(any, 0U), crc_helper_16(any, 0U));

    /* A real block that also CRCs to zero, so the two are indistinguishable
     * by value and the caller must track length itself. */
    uint8_t zeros[512];
    memset(zeros, 0, sizeof(zeros));
    T_EQ_U(0x0000U, crc_helper_16(zeros, sizeof(zeros)));
}

/* --------------------------------------------------------- rolling form */

/*
 * crc_helper_rolling_16() is the same computation as crc_helper_16(), but
 * one byte at a time with the register carried by the caller, so the read
 * path can fold each payload byte in as it leaves the SPI peripheral instead
 * of buffering the block and running the CRC afterwards. That makes two
 * things load-bearing that the block form never had to prove: the register
 * argument must be honoured exactly (a helper that zeroed it, or returned
 * only its low bits, would still get the first byte right), and the step must
 * be correct from *every* register state, not only the ones a zero-initial
 * fold happens to visit for the vectors above.
 */

static uint16_t fold(uint16_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0U; i < length; ++i) {
        crc = crc_helper_rolling_16(crc, data[i]);
    }
    return crc;
}

static void test_rolling_specification_vectors(void)
{
    /* The same vectors as the block form, folded from a zero register. */
    uint8_t check[9] = { 0x31U, 0x32U, 0x33U, 0x34U, 0x35U,
                         0x36U, 0x37U, 0x38U, 0x39U };
    t_context("rolling fold of ASCII 123456789 gives the catalogue value");
    T_EQ_U(0x31C3U, fold(0U, check, 9U));

    t_context("rolling a single 0x00 byte through a zero register");
    T_EQ_U(0x0000U, crc_helper_rolling_16(0U, 0x00U));
    t_context("rolling a single 0x01 byte leaves the generator in the register");
    T_EQ_U(0x1021U, crc_helper_rolling_16(0U, 0x01U));

    /* Only the top register bit is set and the data byte is zero: the
     * generator is applied on the first bit time and the result shifts up
     * through the remaining seven, feeding back once more when it reaches
     * the top again at bit time five. Derived by hand from the generator. */
    t_context("rolling 0x00 through a register holding 0x8000");
    T_EQ_U(0x9188U, crc_helper_rolling_16(0x8000U, 0x00U));

    /* A full 512-byte block of zeros stays at zero, as for the block form. */
    uint8_t zeros[512];
    memset(zeros, 0, sizeof(zeros));
    t_context("rolling a full 512-byte block of zeros");
    T_EQ_U(0x0000U, fold(0U, zeros, sizeof(zeros)));

    /* The register must come out 16 bits wide: a helper that truncated its
     * result (the first version returned bool) would still pass every vector
     * whose answer is 0 or 1, so pin one that is neither and has both bytes
     * nonzero. */
    t_context("the returned register is 16 bits wide");
    T_CHECK(crc_helper_rolling_16(0x1234U, 0x56U) > 0xFFU);
    t_clear_context();
}

static void test_rolling_fold_matches_block_form(void)
{
    /* A fold from zero over any message must equal the block helper and
     * the card model's oracle, at the lengths the driver uses and around
     * them. */
    static const size_t lengths[] = {
        0U, 1U, 2U, 3U, 15U, 16U, 17U, 511U, 512U, 513U, 528U,
    };
    uint8_t buffer[CRC16_MAX_LENGTH];
    char hex[(3U * 32U) + 8U];
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed + 3U);

    for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        const size_t length = lengths[i];
        for (unsigned int iteration = 0U; iteration < 16U; ++iteration) {
            fill_random(&rng, buffer, length);
            format_hex(hex, sizeof(hex), buffer, length < 32U ? length : 32U);
            t_context("length %zu iteration %u head [%s] (replay: --seed %"
                PRIu64 ")", length, iteration, hex, suite_seed);
            const uint16_t rolled = fold(0U, buffer, length);
            T_EQ_U(crc_helper_16(buffer, length), rolled);
            T_EQ_U(sd_crc16_ccitt(buffer, length), rolled);
        }
    }
    t_clear_context();
}

static void test_rolling_honours_the_carried_register(void)
{
    /* The register argument is the state after some prefix, so folding a
     * suffix from that state must give the CRC of prefix and suffix
     * together. A helper that ignored or partially used its first argument
     * still passes the from-zero cases; this one it cannot. */
    static const size_t splits[][2] = {
        { 1U, 1U }, { 1U, 511U }, { 511U, 1U }, { 256U, 256U },
        { 16U, 496U }, { 512U, 16U }, { 3U, 525U },
    };
    uint8_t buffer[CRC16_MAX_LENGTH];
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed + 4U);

    for (size_t i = 0U; i < sizeof(splits) / sizeof(splits[0]); ++i) {
        const size_t prefix = splits[i][0];
        const size_t suffix = splits[i][1];
        for (unsigned int iteration = 0U; iteration < 16U; ++iteration) {
            fill_random(&rng, buffer, prefix + suffix);
            const uint16_t after_prefix = crc_helper_16(buffer, prefix);
            t_context("prefix %zu suffix %zu iteration %u register 0x%04X "
                "(replay: --seed %" PRIu64 ")", prefix, suffix, iteration,
                (unsigned)after_prefix, suite_seed);
            T_EQ_U(sd_crc16_ccitt(buffer, prefix + suffix),
                fold(after_prefix, buffer + prefix, suffix));
        }
    }
    t_clear_context();
}

static void test_rolling_step_is_correct_from_every_register_state(void)
{
    /* The one-byte step of a non-reflected CRC is linear over GF(2), which
     * gives the classic table identity:
     *
     *     step(crc, d) = (crc << 8) ^ T[(crc >> 8) ^ d]
     *
     * where T[x] is the CRC of the single byte x from a zero register. Every
     * term on the right comes from the card model's independent
     * implementation, so this checks the helper against the oracle at every
     * one of the 65536 x 256 (register, byte) pairs rather than only along the
     * paths random messages happen to take. It takes well under a second. */
    uint16_t table[256];
    for (unsigned int x = 0U; x < 256U; ++x) {
        const uint8_t byte = (uint8_t)x;
        table[x] = sd_crc16_ccitt(&byte, 1U);
    }

    unsigned long mismatches = 0UL;
    for (unsigned int crc = 0U; crc < 0x10000U; ++crc) {
        for (unsigned int d = 0U; d < 256U; ++d) {
            const uint16_t expected = (uint16_t)(
                ((crc << 8U) & 0xFFFFU) ^ table[((crc >> 8U) ^ d) & 0xFFU]);
            const uint16_t actual =
                crc_helper_rolling_16((uint16_t)crc, (uint8_t)d);
            if (actual != expected) {
                if (mismatches == 0UL) {
                    t_context("first mismatch: register 0x%04X byte 0x%02X "
                        "expected 0x%04X got 0x%04X",
                        crc, d, (unsigned)expected, (unsigned)actual);
                    T_CHECK(actual == expected);
                }
                mismatches++;
            }
        }
    }
    t_context("%lu mismatching (register, byte) pairs", mismatches);
    T_EQ_U(0U, mismatches);
    t_clear_context();
}

static void test_rolling_receiver_conventions(void)
{
    /* What the read path does with the helper: fold the 512 payload bytes,
     * then assemble the two received CRC bytes most significant first and
     * compare. Pin both that the comparison is met for an intact block and
     * that the residue form (folding the CRC bytes in as well) reaches zero,
     * so either receiver convention is valid against this helper. */
    uint8_t block[514];
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed + 5U);

    for (unsigned int iteration = 0U; iteration < 16U; ++iteration) {
        fill_random(&rng, block, 512U);
        const uint16_t sent = sd_crc16_ccitt(block, 512U);
        block[512] = (uint8_t)(sent >> 8U);
        block[513] = (uint8_t)(sent & 0xFFU);

        const uint16_t received = (uint16_t)(
            ((uint16_t)block[512] << 8U) | (uint16_t)block[513]);
        t_context("iteration %u crc 0x%04X (replay: --seed %" PRIu64 ")",
            iteration, (unsigned)sent, suite_seed);
        T_EQ_U(received, fold(0U, block, 512U));
        T_EQ_U(0U, fold(0U, block, 514U));

        /* And a byte-swapped receiver would be rejected, as for the block
         * form, unless the two bytes happen to be equal. */
        if (block[512] != block[513]) {
            const uint16_t swapped = (uint16_t)(
                ((uint16_t)block[513] << 8U) | (uint16_t)block[512]);
            T_CHECK(swapped != fold(0U, block, 512U));
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
    (void)printf("sd_crc16: seed %" PRIu64 " (replay with --seed %" PRIu64 ")\n",
        suite_seed, suite_seed);

    /* No SD bus is driven here, so there is no protocol trace to dump. */
    t_dump_trace_on_failure = false;

    t_run(test_specification_vectors,
        "CRC16 specification and catalogue vectors");
    t_run(test_matches_independent_implementation,
        "CRC16 matches the independent model implementation");
    t_run(test_detects_every_single_bit_error_in_a_block,
        "CRC16 detects every single-bit error in a 512-byte block");
    t_run(test_appending_the_crc_leaves_a_zero_residue,
        "appending the CRC MSB-first leaves a zero residue");
    t_run(test_empty_message_is_not_a_sentinel,
        "an empty message and an all-zero block share a CRC of 0x0000");
    t_run(test_rolling_specification_vectors,
        "rolling CRC16 specification vectors and register width");
    t_run(test_rolling_fold_matches_block_form,
        "rolling CRC16 fold matches the block form and the model");
    t_run(test_rolling_honours_the_carried_register,
        "rolling CRC16 continues correctly from a carried register");
    t_run(test_rolling_step_is_correct_from_every_register_state,
        "rolling CRC16 step is correct from every register state");
    t_run(test_rolling_receiver_conventions,
        "rolling CRC16 supports compare and residue receivers MSB-first");
    return t_summary("sd_crc16");
}
