/*
 * Regressions for production gaps that are still open.
 *
 * Each case asserts the behaviour the driver *should* have, so it fails
 * against the current source. They are registered in CTest but disabled by
 * default, exactly like the earlier gap cases: a green default suite must not
 * be read as evidence that these are resolved. KNOWN_GAPS.md explains each
 * one, what it would cost to fix, and why the fix was not made here.
 *
 * Run them with:
 *   cmake -S tests -B tests/build -DTAVERNKEEP_TEST_KNOWN_GAPS=ON
 *   ctest --test-dir tests/build -L known-gap --output-on-failure
 *
 * The SD-007 write acceptance cases that used to live here were promoted to
 * test_sd_writes.c (sd_writes_host_tests) when the write path was implemented,
 * and the SD-003 read-CRC case to test_sd_faults.c (sd_faults_host_tests)
 * when read data CRC validation was implemented.
 */
#include <inttypes.h>
#include <string.h>

#include "pico_mock.h"
#include "sd_card_model.h"
#include "sd_fixture.h"
#include "test_harness.h"

/* ------------------------------------------------------------- SD-004 */

static void gap_stop_transmission_tolerates_in_flight_data(void)
{
    /* During a CMD18 stream the card keeps sending until it decodes CMD12, so
     * residual read data can still be on the bus while the host is already
     * looking for CMD12's R1. sd_spi_stop_transmission() discards exactly one
     * stuff byte and then treats the first byte with bit 7 clear as R1, so a
     * residual data byte with bit 7 clear is mistaken for the response.
     *
     * Every block's data arrived intact in each case below, so the read should
     * succeed regardless of how much residual the card had in flight. Today it
     * starts failing once eight or more residual bytes overlap the response
     * window, and the specific byte values decide the outcome - the same read
     * can pass or fail depending on the contents of the next block. */
    for (size_t residual = 0U; residual <= 16U; ++residual) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        t_context("%zu residual byte(s) in flight when CMD12 is decoded",
            residual);
        T_CHECK(sd_fx_require_init(&fx, &desc));
        sd_card_set_stop_residual_bytes(residual);

        sd_guarded_buffer_t buffer;
        sd_fx_guard_init(&buffer, 3U);
        const block_device_result_t result = block_device_read_blocks(
            fx.device, 77U, sd_fx_guard_data(&buffer), 3U);
        /* The data is verifiably correct either way, which is what makes the
         * failure a false negative rather than a detected error. */
        T_CHECK(sd_fx_guard_matches_card(&buffer, 77U));
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, result);
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    }
    t_clear_context();
}

/* ------------------------------------------------------------- SD-005 */

static void gap_r1_poll_tolerance(void)
{
    /* sd_spi_command() reads at most eight bytes while waiting for R1, so it
     * tolerates at most seven filler bytes. The specification's N_CR window
     * for an SD card in SPI mode is quoted as 0 to 8 bytes, and Linux's
     * mmc_spi driver raised its own limit to sixteen after observing real
     * cards that needed twelve. A card at the specification's worst case, or
     * a slow real card, fails initialisation here with a generic I/O error
     * that gives no hint of the cause. */
    for (uint32_t filler = 8U; filler <= 12U; ++filler) {
        sd_fixture_t fx;
        sd_card_desc_t desc = sd_fx_card_sdhc();
        desc.ncr_bytes = filler;
        t_context("%u filler byte(s) before R1", (unsigned)filler);
        sd_fx_begin(&fx, &desc);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, sd_fx_init(&fx));
        T_EQ_U(sd_card_block_count(), fx.sd.block_count);
    }
    t_clear_context();
}

/* ------------------------------------------------------------- SD-006 */

static void gap_every_command_frame_carries_a_valid_crc7(void)
{
    /* A strict card must be indistinguishable from a permissive one: full
     * bring-up, a single-block read and a multiple-block read all succeed, and
     * the card records no protocol error at any point.
     *
     * sd_spi_command() computes a real CRC7 for every frame it builds, bring-up
     * enables the card's checking with CMD59, and sd_spi_stop_transmission()
     * computes CMD12's CRC the same way. This case passes as of 2026-09-09;
     * SD-006 is closed and it can be promoted out of the known-gap label.
     *
     * The zero-protocol-error assertion below is the load-bearing one. A
     * return-code check alone would pass, because the model still answers the
     * stop sequence normally after recording a rejected frame - which is how
     * the CMD12 placeholder hid for as long as it did. */
    static const struct {
        const char *name;
        sd_card_desc_t (*make)(void);
    } rows[] = {
        { "SDHC, CSD v2", sd_fx_card_sdhc },
        { "v1 SDSC, CMD8 illegal", sd_fx_card_v1_sdsc },
        { "v2 SDSC, CMD16 required", sd_fx_card_v2_sdsc },
    };

    for (size_t i = 0U; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        sd_fixture_t fx;
        sd_card_desc_t desc = rows[i].make();
        desc.crc_check_enabled = true; /* every frame is checked */
        t_context("%s, strict CRC checking", rows[i].name);
        sd_fx_begin(&fx, &desc);

        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, sd_fx_init(&fx));
        T_EQ_U(sd_card_block_count(), fx.sd.block_count);
        T_EQ_U(0U, sd_card_protocol_errors());

        /* CMD17 framing. */
        sd_guarded_buffer_t single;
        sd_fx_guard_init(&single, 1U);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
            fx.device, 5U, sd_fx_guard_data(&single), 1U));
        T_CHECK(sd_fx_guard_matches_card(&single, 5U));
        T_EQ_U(0U, sd_card_protocol_errors());

        /* CMD18 and CMD12, the two frames a single-block read never sends. */
        sd_guarded_buffer_t multi;
        sd_fx_guard_init(&multi, 3U);
        T_EQ_RESULT(BLOCK_DEVICE_RESULT_OK, block_device_read_blocks(
            fx.device, 9U, sd_fx_guard_data(&multi), 3U));
        T_CHECK(sd_fx_guard_matches_card(&multi, 9U));
        T_EQ_U(0U, sd_card_protocol_errors());
        T_CHECK(sd_fx_check_bus_quiescent(&fx) == NULL);
    }
    t_clear_context();
}

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr,
            "usage: %s --gap-{stop-residual|r1-tolerance|command-crc}\n",
            argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--gap-stop-residual") == 0) {
        t_run(gap_stop_transmission_tolerates_in_flight_data,
            "SD-004 CMD12 must tolerate in-flight read data");
    } else if (strcmp(argv[1], "--gap-r1-tolerance") == 0) {
        t_run(gap_r1_poll_tolerance,
            "SD-005 R1 wait must cover the specified N_CR window");
    } else if (strcmp(argv[1], "--gap-command-crc") == 0) {
        t_run(gap_every_command_frame_carries_a_valid_crc7,
            "SD-006 every command frame must carry a valid CRC7");
    } else {
        (void)fprintf(stderr, "unknown selector: %s\n", argv[1]);
        return 2;
    }
    return t_summary("sd_gaps");
}
