/*
 * The firmware entry point, run on the host against the card model.
 *
 * src/main.c is the demo of the highest layer that currently works: today the
 * status-LED heartbeat, next the SD driver's hardware checks, then the block
 * device layer's, then FatFs, then the application. Each replacement is
 * flashed to hardware, and this test is what makes each one runnable on the
 * host first with the same expected output.
 *
 * Everything below main() is real: the driver, the CRC helpers and the GPIO
 * dispatcher are production code, and the SD card is the stateful model the
 * other suites use. Only the SDK (pico_mock), the board and the debug sink are
 * faked, so that the LED and logging failure modes stay reproducible.
 *
 * The contract a demo in main.c has to meet, and all this file asserts:
 *
 *   1. It reports through DEBUG_INFO / debug_log, never bare printf. A line
 *      beginning "PASS " or "FAIL " is a result; anything else is commentary.
 *      No "FAIL " line may be emitted against the model (the irq-failure mode
 *      is the exception, see below). The result lines are printed as they
 *      arrive so a hardware run over RTT and a host run are directly diffable.
 *   2. Its SD configuration uses DEMO_PIN_CHIP_SELECT and DEMO_PIN_CARD_DETECT
 *      below with the card-detect sense DEMO_CARD_DETECT_ACTIVE_HIGH, because
 *      that is where and how the model listens. When the board changes, set
 *      these to match; a chip-select mismatch is reported with a hint.
 *   3. It falls into the cooperative foreground loop afterwards rather than
 *      returning. The loop is left from the fake sleep_ms() once the simulated
 *      clock has advanced DEMO_RUN_BUDGET_MS past entry, which is far longer
 *      than any bring-up or transfer against the model takes.
 *   4. It leaves the bus quiescent: chip select released, and the card has
 *      recorded no protocol error at any point.
 *
 * Nothing here depends on how many log lines main.c prints, in what order it
 * initialises subsystems, or what the demo does - so replacing the demo does
 * not require touching this file, only the pin constants if they change.
 *
 * Modes (one optional argument):
 *   no-led        the board reports no status LED; the loop must still run
 *   debug-failure debug_init() fails; logging attempts are still captured,
 *                 because that is what a hardware run would show on RTT
 *   irq-failure   the GPIO dispatcher already lives on the other core, so
 *                 platform_gpio_irq_init() fails in main. Startup must still
 *                 reach the loop. The SD driver cannot register its removal
 *                 interrupt from this core, so a demo is allowed to report
 *                 FAIL lines here; they are printed, not required.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gpio_irq_hardware_mock.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico_mock.h"
#include "platform/board.h"
#include "platform/debug.h"
#include "platform/gpio_irq.h"
#include "sd_card_model.h"
#include "test_check.h"

int firmware_main(void);

/* ------------------------------------------------------- the contract */

enum {
    /* Must match the sd_spi_config_t the demo in main.c builds. */
    DEMO_PIN_CHIP_SELECT = 13,
    DEMO_PIN_CARD_DETECT = 10,
    /* Sense of the demo's card-detect switch: 1 for a socket that closes to
     * ground when empty (the Adafruit microSD breakout), 0 for one that closes
     * when a card is present. Must match card_detect_active_high in main.c. */
    DEMO_CARD_DETECT_ACTIVE_HIGH = 1,
    /* Simulated milliseconds after entering main() at which the foreground
     * loop is abandoned. Bring-up against the model is a few milliseconds of
     * bus time; the longest wait the driver can perform is 1.2 s (ACMD41). */
    DEMO_RUN_BUDGET_MS = 5000,
    /* The modelled card: an 8 GB SDHC, the same card the SD suites use by
     * default. */
    DEMO_CARD_BYTES_SHIFT = 33,
    MAX_LOG_LINE = 512,
};

/* ------------------------------------------------------------ modes */

static bool led_available = true;
static bool debug_ready = true;
static bool irq_failure = false;

/* ---------------------------------------------------------- captures */

static jmp_buf end_loop;
static uint64_t entry_ms;
static unsigned int led_toggles;
static unsigned int log_lines, log_attempts_while_debug_down;
static unsigned int pass_lines, fail_lines;
static char first_fail[MAX_LOG_LINE];

/* ------------------------------------------------------ board (fake) */

void board_init(void) {}
bool board_status_led_available(void) { return led_available; }
bool board_status_led_set(bool on)
{
    (void)on;
    REQUIRE(led_available);
    led_toggles++;
    return true;
}

/* ------------------------------------------------------ debug (fake) */

bool debug_init(void) { return debug_ready; }

void debug_log(debug_level_t level, const char *format, ...)
{
    REQUIRE(format != NULL);
    static const char *const level_names[] = { "info", "warn", "error" };
    const char *const level_name =
        (unsigned)level < 3U ? level_names[level] : "?";

    char line[MAX_LOG_LINE];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    log_lines++;
    if (!debug_ready) {
        log_attempts_while_debug_down++;
    }
    if (strncmp(line, "PASS ", 5) == 0) {
        pass_lines++;
    } else if (strncmp(line, "FAIL ", 5) == 0) {
        if (fail_lines++ == 0U) {
            (void)snprintf(first_fail, sizeof(first_fail), "%s", line);
        }
    }
    (void)printf("  [%s] %s\n", level_name, line);
}

/* --------------------------------------------------- leaving the loop */

static void leave_loop_after_budget(uint32_t milliseconds, void *context)
{
    (void)milliseconds;
    (void)context;
    if (pico_mock_now_ms() - entry_ms >= (uint64_t)DEMO_RUN_BUDGET_MS) {
        longjmp(end_loop, 1);
    }
}

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "no-led") == 0) { led_available = false; }
        else if (strcmp(argv[1], "debug-failure") == 0) { debug_ready = false; }
        else if (strcmp(argv[1], "irq-failure") == 0) { irq_failure = true; }
        else { return 2; }
    } else if (argc != 1) { return 2; }

    /* pico_mock_reset() also brings up the real dispatcher on core 0, as the
     * firmware would on the core that runs main(). */
    pico_mock_reset();
    sd_card_desc_t card = sd_card_desc(SD_CARD_SDHC,
        UINT64_C(1) << DEMO_CARD_BYTES_SHIFT);
    sd_card_reset(&card);
    sd_card_set_response_policy(SD_RESPONSE_MODELLED);
    pico_mock_sd_use_chip_select(DEMO_PIN_CHIP_SELECT);
    pico_mock_sd_use_card_detect(DEMO_PIN_CARD_DETECT);
    pico_mock_sd_set_card_detect_active_high(DEMO_CARD_DETECT_ACTIVE_HIGH != 0);
    /* A card is present: the line sits at the level the sense calls present. */
    pico_mock_gpio_set_input(DEMO_PIN_CARD_DETECT, DEMO_CARD_DETECT_ACTIVE_HIGH != 0);
    if (irq_failure) {
        /* The dispatcher was initialised on core 0; main() now runs on the
         * other core, where platform_gpio_irq_init() must refuse. */
        gpio_irq_hardware_mock_set_core(1U);
    }
    pico_mock_set_sleep_hook(leave_loop_after_budget, NULL);
    entry_ms = pico_mock_now_ms();

    if (setjmp(end_loop) == 0) {
        (void)firmware_main();
        REQUIRE(!"firmware main unexpectedly returned");
    }

    /* Startup reached the foreground loop and it kept running. */
    REQUIRE(pico_mock_now_ms() - entry_ms >= (uint64_t)DEMO_RUN_BUDGET_MS);
    if (led_available) {
        REQUIRE(led_toggles > 0U);
    } else {
        REQUIRE(led_toggles == 0U);
    }

    /* The demo's verdict. */
    const size_t transfers = pico_mock_spi_transfer_count();
    const size_t commands = sd_card_trace_count(SD_EV_COMMAND);
    if (transfers > 0U
            && !pico_mock_gpio_direction_is_output(DEMO_PIN_CHIP_SELECT)) {
        /* The SPI fake reads an undriven chip-select pin as low, i.e.
         * selected, so a demo on the wrong pin would otherwise sail through
         * every step and only fail the release check below with a message
         * that points the wrong way. */
        (void)fprintf(stderr,
            "hint: %zu SPI byte(s) were clocked but GPIO %d was never driven "
            "as an output - the demo's chip-select pin is not "
            "DEMO_PIN_CHIP_SELECT\n", transfers, DEMO_PIN_CHIP_SELECT);
        REQUIRE(pico_mock_gpio_direction_is_output(DEMO_PIN_CHIP_SELECT));
    }
    if (fail_lines > 0U) {
        (void)fprintf(stderr, "%u FAIL line(s), first: %s\n",
            fail_lines, first_fail);
    }
    if (!irq_failure) {
        REQUIRE(fail_lines == 0U);
    }

    /* The bus is quiescent and the driver never violated the protocol. */
    if (transfers > 0U) {
        REQUIRE(pico_mock_gpio_level(DEMO_PIN_CHIP_SELECT));
    }
    if (sd_card_protocol_errors() != 0U) {
        (void)fprintf(stderr, "card model recorded %u protocol error(s)\n",
            (unsigned)sd_card_protocol_errors());
    }
    REQUIRE(sd_card_protocol_errors() == 0U);

    (void)printf("PASS firmware startup, demo and cooperative heartbeat "
        "(%u log line(s), %u PASS, %u FAIL, %u logged while debug down, "
        "%zu SD command(s), %u LED toggle(s))\n",
        log_lines, pass_lines, fail_lines, log_attempts_while_debug_down,
        commands, led_toggles);
    return 0;
}
