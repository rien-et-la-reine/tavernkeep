/*
 * Unit tests for the production CRC7 helper in src/storage/sd_crc.c.
 *
 * Two independent sources of truth, deliberately kept separate from the
 * implementation:
 *
 *   - literal frames and CRC7 values quoted from the SD Physical Layer
 *     Simplified Specification, section 4.5. These are asserted as exact
 *     constants and must never be adjusted to whatever the helper returns;
 *     if they disagree, the helper is wrong.
 *   - a differential cross-check against sd_crc7() in the card model, a
 *     separate byte-at-a-time implementation of the same polynomial. Two
 *     unrelated derivations agreeing on random input is much stronger
 *     evidence than either agreeing with a handful of vectors.
 *
 * The randomised case is seeded and prints its seed; pass --seed N to replay
 * a failure exactly.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "sd_card_model.h"
#include "test_harness.h"
#include "test_rand.h"

/* The unit under test, through its own public header so this suite and the
 * firmware build agree on one declaration. */
#include "storage/sd_crc.h"

static uint64_t suite_seed = 20260908U;

/* Longest buffer the differential case generates. 32 covers both frame sizes
 * that matter on the wire - a 5-byte command frame and the 15-byte CID/CSD
 * prefix - with room either side, and keeps the hex dump inside the harness's
 * 256-byte case context. */
#define CRC_MAX_LENGTH 32U

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
        used--; /* drop the trailing separator */
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

/*
 * Section 4.5 gives the CRC7 of a command frame as the 7-bit remainder over
 * the first five bytes, transmitted left-aligned with the stop bit: the byte
 * on the wire is (crc << 1) | 1. These assert the raw 7-bit remainder, which
 * is what crc_helper_7 documents itself as returning.
 *
 *   CMD0,  argument 0: 40 00 00 00 00 -> CRC7 0x4A (wire byte 0x95)
 *   CMD17, argument 0: 51 00 00 00 00 -> CRC7 0x2A (wire byte 0x55)
 */
static void test_specification_vectors(void)
{
    uint8_t cmd0_frame[5] = { 0x40U, 0x00U, 0x00U, 0x00U, 0x00U };
    uint8_t cmd17_frame[5] = { 0x51U, 0x00U, 0x00U, 0x00U, 0x00U };

    t_context("CMD0 frame 40 00 00 00 00 (spec 4.5)");
    T_EQ_U(0x4AU, crc_helper_7(cmd0_frame, 5U));

    t_context("CMD17 frame 51 00 00 00 00 (spec 4.5)");
    T_EQ_U(0x2AU, crc_helper_7(cmd17_frame, 5U));

    t_clear_context();
}

/* ------------------------------------------------------ differential case */

/*
 * Fixed lengths first, chosen for the sizes the protocol actually uses and
 * the boundaries around them, then random lengths so nothing about the
 * length itself is special-cased by accident.
 */
static void test_matches_independent_implementation(void)
{
    static const size_t lengths[] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
        14U, 15U, 16U, 17U, 31U, 32U,
    };
    uint8_t buffer[CRC_MAX_LENGTH];
    char hex[(3U * CRC_MAX_LENGTH) + 1U];
    t_rand_t rng;
    t_rand_seed(&rng, suite_seed);

    for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        const size_t length = lengths[i];
        for (unsigned int iteration = 0U; iteration < 64U; ++iteration) {
            fill_random(&rng, buffer, length);
            format_hex(hex, sizeof(hex), buffer, length);
            t_context("length %zu iteration %u bytes [%s] (replay: --seed %"
                PRIu64 ")", length, iteration, hex, suite_seed);
            T_EQ_U(sd_crc7(buffer, length), crc_helper_7(buffer, length));
        }
    }

    for (unsigned int iteration = 0U; iteration < 512U; ++iteration) {
        const size_t length = (size_t)t_rand_below(&rng, CRC_MAX_LENGTH + 1U);
        fill_random(&rng, buffer, length);
        format_hex(hex, sizeof(hex), buffer, length);
        t_context("random length %zu iteration %u bytes [%s] (replay: --seed %"
            PRIu64 ")", length, iteration, hex, suite_seed);
        T_EQ_U(sd_crc7(buffer, length), crc_helper_7(buffer, length));
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
    (void)printf("sd_crc: seed %" PRIu64 " (replay with --seed %" PRIu64 ")\n",
        suite_seed, suite_seed);

    /* No SD bus is driven here, so there is no protocol trace to dump. */
    t_dump_trace_on_failure = false;

    t_run(test_specification_vectors, "CRC7 specification vectors (CMD0, CMD17)");
    t_run(test_matches_independent_implementation,
        "CRC7 matches the independent model implementation");
    return t_summary("sd_crc");
}
