# Host validation record

Runs are recorded oldest first. The 2026-09-05 record below is the original;
later runs are appended at the end. The 2026-09-15 record is the first with
hardware evidence.

## Run of 2026-09-05

Environment: Linux x86-64, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Unix
Makefiles. The suite is also maintained for GCC 15.2 / MSYS2 MinGW on Windows,
which is the toolchain the previous record used; the Windows numbers below are
from that earlier run and are marked as such.

### Baseline, before this work

Recorded on the tree as it stood, with the previous harness:

| Run | Result |
| --- | --- |
| Enabled suite | 15/15 CTest cases passed; SD executable reported 60 groups passed |
| Registered gap cases (disabled by default) | 3/3 failed as documented |

### Final

| Run | Result |
| --- | --- |
| Enabled suite, Release | **22/22 CTest cases passed** |
| `sd_spi_host_tests`, bus-time clock | 60/60 groups passed |
| `sd_spi_host_tests`, poll-tick clock | 60/60 groups passed |
| `sd_protocol_host_tests` | 22/22 cases, 3231 checks |
| `sd_faults_host_tests` | 11/11 cases, 2052 checks |
| `sd_property_host_tests` | 6/6 cases, 12217 checks (default seed) |
| `sd_property_host_tests`, seeds 1, 7, 42, 999, 20260905 | passed at every seed |
| Strict warnings (`-Werror`, `-Wconversion`, `-Wsign-conversion`, `-Wshadow`, `-Wcast-qual`, `-Wmissing-prototypes`, `-Wformat=2` and more) | clean, production and test sources |
| ASan + UBSan, Debug, `-fno-sanitize-recover=all`, `detect_leaks=1` | 22/22 passed, no reports |
| Clang 18 Release build | 22/22 passed; only `-Wformat-nonliteral` from the harness's own `vsnprintf` and a pre-existing `REQUIRE(!"...")` idiom |
| `gcc -fanalyzer` on `sd_spi.c` and `gpio_irq.c` | no diagnostics |
| Registered gap cases (disabled by default) | 3/3 failed as documented |

The three failing gap cases are the point, not a defect in the run: they assert
the behaviour the driver should have and are enabled explicitly. See
[KNOWN_GAPS.md](KNOWN_GAPS.md).

Clang's sanitizers were **not** exercised: this environment lacks the
compiler-rt runtime (`libclang-rt-18-dev`), so the clang sanitizer link fails.
GCC's ASan and UBSan were used instead, and clang was used as a second static
analysis pass.

### Mutation testing

The primary evidence that the suite detects bugs rather than merely executing
code. 53 deliberate incorrect implementations across `sd_spi.c` and
`gpio_irq.c`, spanning wrong constants, reversed conditions, removed
validation, incorrect bit masks, truncated integer widths, off-by-one range
checks, removed cleanup, skipped state transitions, ignored error codes,
incorrect device-type handling, wrong addressing conversion, premature success,
delayed error recognition and eliminated timeout behaviour.

| Outcome | Count |
| --- | --- |
| Detected by the enabled suite | 49 |
| Documented equivalent mutants | 2 |
| Out of this harness' reach, with the reason recorded | 1 |
| Survived the Release suite | 1 (caught under the sanitizer configuration) |

Full table, the arguments for the equivalent mutants, and the reason the
unreachable one cannot be closed on the host: [MUTATION.md](MUTATION.md).

Running the mutation suite against the *finished* work, not only against the
starting point, is what surfaced the last of these: a removal check in
`sd_spi_stop_transmission()` that guards a window this harness cannot reach.
It also caught a bug in the harness itself — the card model's capacity guard
rejected the smallest legal CSD v1 card, because it treated a `C_SIZE` of zero
as a failed search.

### Coverage

GCC coverage, per target. Reports that share a basename overwrite each other,
so these must not be summed; each row is one executable's view of one source.

| Source / measuring target | Lines executed | Branch outcomes taken |
| --- | --- | --- |
| `sd_spi.c` / `sd_spi_host_tests` | 96.81% of 439 | 92.05% of 302 |
| `sd_spi.c` / `sd_protocol_host_tests` | 76.77% of 439 | 61.26% of 302 |
| `sd_spi.c` / `sd_faults_host_tests` | 75.63% of 439 | 59.93% of 302 |
| `sd_spi.c` / `sd_property_host_tests` | 71.53% of 439 | 57.95% of 302 |
| `gpio_irq.c` / `gpio_irq_host_tests` | 100% of 64 | 100% of 40 |
| `block_device.h` / `block_device_host_tests` | 100% of 30 | 100% of 36 |
| `filesystem.c` / `filesystem_host_tests` | 100% of 17 | 100% of 14 |
| `main.c` / all main variants | 100% of 15 | 100% of 8 |

The four SD rows overlap heavily; the new suites are not attempts to raise the
number but to test different questions about the same code, which is why each
alone covers less than the original suite does. Percentages refer to
compiler-instrumented lines and branch outcomes, not to requirements. High line
coverage does not establish real-time bounds or validate a card protocol
physically, which is why the mutation results above carry more weight here.

### Production changes made

Two defects, each with a regression written first. Both were previously
reproduced and documented but left unfixed.

| Defect | Change | Regression |
| --- | --- | --- |
| SD-001: removal during the release clock published a successful read | recheck the removal latch after `sd_spi_release_bus()` on both read success paths | `sd_removal_single-release_host_tests`, `sd_removal_multi-release_host_tests`, `test_removal_during_the_release_clock` |
| SD-002: teardown of an uninitialised device released hardware again | `deinit` returns `OK` without touching hardware when nothing was acquired | `sd_removal_repeated-teardown_host_tests`, `sd_fx_check_removed_and_teardown_once`, the operation-sequence fuzz |

Both are guarded by mutations (`release-race-unchecked-single`,
`deinit-releases-twice`) that the enabled suite catches, so a reintroduction
fails the default run.

Documentation corrections: the root `README.md` no longer claims the storage
layer is unimplemented, and `docs/validation.md` carries a validation record
instead of empty templates.

### Not performed in the 2026-09-05 run

No Pico SDK cross-build, no target flashing, no physical card test, no bus
capture, no power measurement, no hardware validation of any kind. Everything
above is host-side evidence about host-side behaviour of the production
sources. What that cannot establish is enumerated in
[RESIDUAL_RISK.md](RESIDUAL_RISK.md); acceptance criteria for functionality not
yet written are in [VALIDATION_PLAN.md](VALIDATION_PLAN.md).

---

## Run of 2026-09-09 — command CRC7 integration

Environment: Windows 11 x86-64, GCC 15.2.0 (MSYS2 MinGW), CMake 4.2.3, Ninja,
Release. Covers the change that replaced the two hardcoded command CRC bytes
with `crc_helper_7()` over the whole frame and added CMD59 to bring-up.

| Run | Result |
| --- | --- |
| Enabled suite, Release | **24/24 CTest cases passed** |
| `sd_protocol_host_tests` | 24/24 cases |
| `sd_spi_host_tests`, bus-time clock | 63/63 groups passed |
| `sd_spi_host_tests`, poll-tick clock | 63/63 groups passed |
| `sd_faults_host_tests` | 11/11 cases, 2052 checks |
| `sd_crc_host_tests` | 2/2 cases, 1474 checks |
| `sd_crc16_host_tests` | 5/5 cases, 4968 checks |
| `sd_property_host_tests` | 6/6 cases, 12218 checks (default seed) |
| Strict warnings (`-Werror` on the full warning set), Release | **fails** - `-Wshadow` in `sd_spi_stop_transmission()`, see below |
| Registered gap cases (disabled by default) | 4/5 failed as documented; `sd_gap_command-crc` passes (SD-006 closed) |

SD-006 was closed during this run. `test_long_multiple_block_read` asserting
`sd_card_protocol_errors() == 0` failed until `sd_spi_stop_transmission()` was
changed to compute CMD12's CRC7; it passes now, as does `sd_gap_command-crc`.
The CMD12 frame byte pinned in `test_sd_spi.c` was updated from the placeholder
0x01 to 0x61, recomputed from the polynomial rather than copied from the driver.

CRC7 was cross-checked three independent ways: the specification's CMD0 (0x95)
and CMD8 (0x87) frame bytes, a differential sweep against the card model's
separate implementation (1474 checks), and a reference implementation written
from the polynomial for this run, which reproduced the driver's bytes for CMD0,
CMD8, CMD59, CMD55, ACMD41, CMD58 and CMD9.

**The strict-warnings build is broken.** `sd_spi_stop_transmission()` declares
`uint8_t transmit_buffer[5], i;` and then a nested `for (uint8_t i = 0U; ...)`
shadows it, which `-Wshadow` reports and `-Werror` turns fatal:

```
sd_spi.c:601:18: error: declaration of 'i' shadows a previous local [-Werror=shadow]
sd_spi.c:575:33: note: shadowed declaration is here
```

This blocks `tools/run_diagnostics.sh strict`. The numbers in the table above
were obtained from a build with `TAVERNKEEP_TEST_STRICT_WARNINGS=OFF`; the
behaviour under test is identical, but the strict stage cannot currently run.

### Not performed in this run

ASan and UBSan were **not** exercised: this MinGW toolchain has no `libasan` or
`libubsan`, so the sanitizer link fails with `cannot find -lasan`. The
sanitizer evidence in the 2026-09-05 record stands and was not re-obtained.

One thing that record's environment would surface and this one cannot: under
UBSan instrumentation GCC loses the range analysis that keeps `-Wconversion`
quiet, and warns at `sd_crc.c:25`, `sd_crc.c:60` and `sd_spi.c:850`. Because
`tools/run_diagnostics.sh` sets `TAVERNKEEP_TEST_SANITIZE=ON` together with
`TAVERNKEEP_TEST_STRICT_WARNINGS=ON`, that stage will fail on `-Werror` on a
toolchain that has the runtimes. Both files are clean at `-O0` and `-O3`
without sanitizers.

No mutation run. Two catalogue records were repaired and separately confirmed —
see [MUTATION.md](MUTATION.md). The green baseline a full run needs now exists.

No Pico SDK cross-build, no target flashing, no physical card test, no bus
capture, no hardware validation of any kind.

## Run of 2026-09-15 — first hardware run: SD driver demo on a real card

The first time any of this firmware ran against a physical card. The demo in
`src/main.c` (also run on the host by `main_host_tests`, see below) reported
15 of 15 steps `PASS` over RTT.

### Setup

- Raspberry Pi Pico 2 (RP2350, Arm core), Pico SDK 2.3.0, toolchain
  15.2.Rel1, RTT stdio; flashed and observed through the Debug Probe with
  OpenOCD 0.12.0+dev and Cortex-Debug.
- Adafruit MicroSD card breakout board+ (5 V-ready, 74AHC125 level shifter),
  on a breadboard. SPI1: GPIO 11 MOSI, 12 MISO, 13 CS, 14 SCK; card detect on
  GPIO 10. Data rate 1 MHz after the 400 kHz bring-up.
- An 8 GB microSDHC card, formatted (block 0 carries `55 aa`), disposable.
- The breakout carries no pull-ups. The RP2350 internal pull-ups were enabled
  on MISO and CS from `main.c` as breadboard stand-ins for the discrete
  resistors the final board will carry; card detect gets its pull-up from the
  driver. See "Findings" for why MISO's is not optional.
- The breakout's detect switch closes to ground when the socket is *empty*,
  so the driver's new `card_detect_active_high` was set.

### Result

```
[INFO] PASS configure
[INFO] PASS init
[INFO] PASS get_info
[INFO] card reports 15523840 blocks of 512 bytes, writable=1
[INFO] PASS reject null buffer
[INFO] PASS reject zero block count
[INFO] PASS reject write past end
[INFO] PASS read block 0
[INFO] block 0 bytes 510..511: 55 aa (55 aa on a formatted card)
[INFO] PASS write single block
[INFO] PASS read back single block
[INFO] PASS read neighbour block
[INFO] PASS write multiple blocks
[INFO] PASS single block untouched by multi-block write
[INFO] PASS read back multiple blocks
[INFO] PASS neighbour block untouched by multi-block write
[INFO] PASS deinit
```

15,523,840 × 512 = 7.95 GB, consistent with an 8 GB card. The demo writes
`base = block_count - 64` (one block, CMD24) and `base+1..base+50` (five
CMD25 transfers of ten blocks, each preceded by ACMD23), then verifies
`base..base+50` against a per-block pattern carrying the LBA and reads
`base+51` before and after to prove nothing spilled.

### What this establishes

- Bring-up with command CRC checking enabled (CMD59) on a real card: CMD0,
  CMD8, CMD59, ACMD41 (inside the 1.2 s budget), CMD58, CMD9, and every R1
  arriving inside the driver's 8-byte N_CR window with this card.
- The CSD register's CRC16 validated against a real card, and the capacity
  decoded from it matches the card.
- Read-path data CRC16 validation against real data (block 0 signature
  `55 aa`) and against the driver's own writes.
- The write path's rolling CRC16 is what the card computes: with checking on,
  the card accepted every block of both CMD24 and CMD25, and the data read
  back byte-for-byte.
- Byte addressing is not exercised (SDHC card); the SDSC path remains
  host-only.
- Card detect in the active-high sense, argument rejection with no bus
  traffic, and a clean `deinit` into the foreground loop.

### Findings on the way to this result

1. **Detect switch sense.** The breakout's socket reads high with a card in.
   The driver assumed active-low only; it now takes
   `card_detect_active_high` and the host suites run under both senses.
2. **A floating DO reports `BUSY_TIMEOUT` before CMD0.** With no pull-up on
   DO, the first attempt failed `init` with result 5. The driver waits for a
   ready (`0xFF`) byte before every command frame, including CMD0, while the
   card is still in SD native mode and leaves DAT0 undriven. The wait
   exhausted 500 ms on a floating line and reported "busy" for a card that
   had never been addressed. Enabling the MISO pull-up fixed it. Recorded in
   KNOWN_GAPS.md as a decision to make: skip the ready wait for CMD0, have
   the driver own the DAT0 pull-up, or leave it to the board.
3. **The card model's write overlay was 32 blocks**, found when the demo's
   51-block sequence read back wrong *on the host* before the hardware run;
   raised to 64 and the silent-drop behaviour documented in RESIDUAL_RISK.md.

### Not performed in this run

Nothing above 1 MHz; no hot removal (the demo completes in milliseconds and
the removal interrupt has not fired on hardware); no logic-analyser capture,
so N_CR, N_AC (the byte after each CRC), CMD12 residual data (SD-004) and
ACMD23's R1 were not observed; no SDSC card; write-protect is not read. No
sanitizer or mutation run on the host for this change set - see the host
record for the polarity change in MUTATION.md.
