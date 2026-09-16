# Validation

This document records evidence about Tavernkeep behavior. Its purpose is to
keep the existence of code distinct from demonstrated correctness.

Use the following status terms where appropriate:

- **PLANNED** — the behavior or validation method has been identified.
- **IMPLEMENTED** — supporting implementation exists, without implying that it
  has been tested successfully.
- **HOST TESTED** — the recorded behavior has been exercised successfully in a
  host-side test environment.
- **HARDWARE VALIDATED** — the recorded behavior has been demonstrated on the
  identified target hardware and under the stated conditions.

These statuses are descriptive, not a mandatory progression. A feature need
not pass through every status.

## Validation Philosophy / Method

Evidence is chosen to match the risk. Protocol logic, arithmetic, state
machines and error recovery are validated on the host, where a card can be made
to behave in ways a real one rarely does on demand. Anything physical —
electrical timing, pad state, real card behaviour, DMA and PIO cancellation,
power — is not claimed until it has been demonstrated on target hardware.

Host evidence is kept honest three ways. Simulated time advances with modelled
bus work rather than with poll counts, so a timeout assertion is about elapsed
time and not about how often a function was called. Mutation testing measures
whether the suite would notice the code being wrong, rather than whether it
merely executed. And behaviour the driver does not yet have is recorded as a
failing, deliberately disabled regression rather than removed, so a green run
is never mistaken for completeness.

Coverage is used to find unexercised paths, not as an acceptance criterion.

Repeatability: every randomised test is seeded and prints its seed, every
result below names the toolchain that produced it, and the diagnostics are
scripted in `tests/tools/run_diagnostics.sh`.

### Validation record format

Use a concise record for each meaningful behavior or claim:

<!--
### VAL-NNN — Feature or behavior

- Test method:
- Environment / hardware:
- Expected result:
- Observed result:
- Status: PLANNED | IMPLEMENTED | HOST TESTED | HARDWARE VALIDATED
- Relevant commit / test / reference:
-->

## Validation Environment

Records under "Host-Side Tests" are host-side. The one record under "Hardware
Validation" was taken on target hardware; its scope is stated there.

- Host suite: `tests/`, built with CMake 3.16 or newer against small Pico SDK
  fakes; no SDK, card or firmware build required. The firmware entry point
  itself is one of the host executables: `main_host_tests` runs `src/main.c`
  with the real driver and dispatcher against the card model, so the demo
  that gets flashed is the demo that ran on the host.
- Toolchains exercised: GCC 13.3.0 and Clang 18.1.3 on Linux x86-64;
  GCC 15.2.0 (MSYS2 MinGW) on Windows.
- Simulated time is derived from SPI byte duration at the programmed baud rate
  rather than from poll counts. It bounds how long a loop takes; it is not a
  throughput or latency measurement.
- Randomised tests are seeded and print their seed; `--seed N` replays a run.
- Diagnostics are scripted in `tests/tools/run_diagnostics.sh`.

## Host-Side Tests

### VAL-001 — SD SPI bring-up across supported card generations

- Test method: `sd_protocol_host_tests`, `sd_spi_host_tests`
- Environment: host suite, GCC 13.3.0 / Linux and GCC 15.2 / MinGW
- Expected result: v1 SDSC, v2 SDSC, SDHC and SDXC initialize with the correct
  addressing mode and capacity; SDUC (CSD v3) and an MMC-like card are refused
  without leaving hardware configured; the command sequence and both SPI clock
  rates are as designed
- Observed result: as expected
- Status: HOST TESTED
- Reference: `tests/test_sd_protocol.c`, `tests/PROTOCOL.md` P-01 to P-11

### VAL-002 — CSD capacity parsing against registers from real cards

- Test method: `sd_protocol_host_tests`, `sd_property_host_tests`
- Environment: host suite
- Expected result: five CSD registers captured from real cards decode to their
  marketed capacities; every CSD v1 field encoding and the CSD v2 C_SIZE range
  produce the capacity the specification formula gives; invalid structures and
  READ_BL_LEN values are refused
- Observed result: as expected. Each vector's own CRC7 is verified before use;
  one candidate vector was discarded because its CRC7 did not check out
- Status: HOST TESTED
- Reference: `tests/PROTOCOL.md` P-06

### VAL-003 — block and byte addressing, and the address conversion boundaries

- Test method: `sd_protocol_host_tests`, `sd_property_host_tests`
- Environment: host suite
- Expected result: high-capacity cards receive block addresses and
  standard-capacity cards receive byte addresses; the largest CSD v1 card's last
  block address (0xFFFFFE00) and the largest high-capacity card's last LBA
  (0xFFFFFFFF) reach the card untruncated; out-of-range and straddling requests
  are refused before any bus activity
- Observed result: as expected
- Status: HOST TESTED
- Reference: `tests/PROTOCOL.md` P-10

### VAL-004 — error recognition is immediate, not a timeout

- Test method: `sd_protocol_host_tests`, `sd_faults_host_tests`
- Environment: host suite, simulated time driven by bus work
- Expected result: all fifteen data error tokens are recognised on the byte they
  arrive on both read paths, in under a microsecond and under forty bus bytes;
  all 128 R1 values are rejected except 0x00; a card that answers R1 and then
  never produces a data token costs exactly one 100 ms wait and no retry
- Observed result: as expected
- Status: HOST TESTED
- Reference: `tests/PROTOCOL.md` P-05

### VAL-005 — failure, cancellation and recovery

- Test method: `sd_faults_host_tests`, `sd_property_host_tests`
- Environment: host suite
- Expected result: for every fault kind at every read phase, the operation
  terminates inside the driver's declared budgets, releases chip select, writes
  nothing outside the destination, leaves the device usable, and releases
  hardware exactly once on teardown. Removal at any phase fails with
  INVALID_DEVICE, sends no CMD12 to an absent card, performs no teardown in the
  interrupt handler, suppresses repeat edges, and requires a fresh
  initialization before the card can be used again
- Observed result: as expected, after the two production fixes recorded under
  Validation Issues below
- Status: HOST TESTED
- Reference: `tests/KNOWN_GAPS.md`

### VAL-006 — the suite detects deliberate defects

- Test method: `python3 tests/tools/mutate.py`
- Environment: host suite, GCC 13.3.0 Release; one mutation additionally under
  ASan/UBSan
- Expected result: representative incorrect implementations are caught
- Observed result: last full run (2026-09-11, GCC 15.2 / MinGW) 73 of 78
  detected, 3 documented equivalent mutants, 1 out of the harness's reach, 1
  survivor of the Release run caught under the sanitizer configuration.
  Records added since (read/write CRC, card-detect polarity, R1 window) were
  confirmed by hand against the suites named in `tests/MUTATION.md`, not by a
  full run; the catalogue stands at 95 entries
- Status: HOST TESTED
- Reference: `tests/MUTATION.md`

### VAL-007 — memory safety and defined behaviour under the host suite

- Test method: AddressSanitizer and UndefinedBehaviorSanitizer, plus a strict
  warning set treated as errors, plus `gcc -fanalyzer`
- Environment: GCC 13.3.0, `-fno-sanitize-recover=all`, `detect_leaks=1`
- Expected result: no reports
- Observed result: no reports; 22/22 cases pass under sanitizers (2026-09-05).
  The Windows/MinGW toolchain used since has no sanitizer runtimes; the
  rolling CRC helper and the faults suite were additionally run under UBSan's
  trap mode there (no runtime needed) with no traps
- Status: HOST TESTED
- Reference: `tests/VALIDATION_RESULTS.md`

### VAL-008 — data CRC16 validated on reads and on the CSD register

- Test method: `sd_crc16_host_tests`, `sd_faults_host_tests`,
  `sd_protocol_host_tests`
- Environment: host suite, GCC 15.2 / MinGW
- Expected result: the rolling CRC16 helper matches the CRC-16/XMODEM
  catalogue value, the one-shot helper and the card model's independent
  implementation, at every (register, byte) pair exhaustively; a corrupted
  payload byte at any position, a corrupted CRC byte, or a bad CRC on an
  intact payload is reported as IO_ERROR with the bus released and the device
  still usable, on CMD17 and on any block of a CMD18 stream; a bad CSD CRC
  fails initialization
- Observed result: as expected. Documented limit: an all-zero payload with an
  all-zero CRC is a valid frame under this parameterisation, so a bus stuck
  low from the first payload byte of a single-block read is not detectable by
  the CRC alone (`tests/RESIDUAL_RISK.md`)
- Status: HOST TESTED; HARDWARE VALIDATED for intact frames only, see VAL-H01
- Reference: `tests/PROTOCOL.md` P-12, `tests/KNOWN_GAPS.md` SD-003 (closed)

### VAL-009 — single- and multiple-block writes

- Test method: `sd_writes_host_tests`
- Environment: host suite
- Expected result: CMD24 and CMD25 on every card kind store data the model
  reads back; byte addressing on SDSC; the start-block token each command
  requires, the N_WR idle byte, CRC16 on every block, stop-tran and the N_BR
  window; data-response tokens decoded under the specification mask; CRC and
  write errors reported as IO_ERROR; programming busy waited out with a
  bounded budget and overrun reported as BUSY_TIMEOUT; removal at every write
  phase; no false success under the fault sweep
- Observed result: as expected
- Status: HOST TESTED; HARDWARE VALIDATED at 1 MHz on SDHC, see VAL-H01
- Reference: `tests/PROTOCOL.md` P-13 onwards, `tests/KNOWN_GAPS.md` SD-007

### VAL-010 — card-detect switch sense

- Test method: `sd_faults_host_tests` (both `--card-detect` senses),
  `sd_writes_host_tests` (both senses)
- Environment: host suite
- Expected result: with `card_detect_active_high` false or true, the absent
  level is refused before any bus traffic with the pull-up selected; the
  present level brings the card up with the interrupt armed on exactly the
  removal edge for that sense; the insertion edge does not latch; the removal
  edge latches with exactly one teardown; a card that vanishes between the
  debounce and the arming is refused; every removal case in both suites holds
  under both senses
- Observed result: as expected; five mutants restoring the old hard-coded
  sense at each touch point are killed under both senses
- Status: HOST TESTED; HARDWARE VALIDATED for the active-high sense (presence
  only, not removal), see VAL-H01
- Reference: `tests/test_sd_faults.c` `test_card_detect_polarity`

### VAL-011 — R1 response window

- Test method: `sd_protocol_host_tests`, `sd_spi_host_tests`
- Environment: host suite
- Expected result: a response on any of sixteen reads after the command frame
  is accepted (fifteen filler bytes), for bring-up, CMD17 and CMD12 at the end
  of a CMD18 stream; sixteen filler bytes fail immediately with IO_ERROR
  rather than a timeout
- Observed result: as expected; three mutants (off-by-one, the old 8-byte
  window on either poll) are killed
- Status: HOST TESTED
- Reference: `tests/PROTOCOL.md` P-04, `tests/KNOWN_GAPS.md` SD-005 (closed)

## Hardware Validation

### VAL-H01 — SD driver demo on a real card, 2026-09-15

- Test method: the demo in `src/main.c` (the same code `main_host_tests`
  runs against the card model), reporting each step over RTT
- Environment / hardware: Raspberry Pi Pico 2 (RP2350, Arm), Pico SDK 2.3.0,
  flashed and observed through the Debug Probe with OpenOCD and Cortex-Debug;
  Adafruit MicroSD card breakout board+ on a breadboard, SPI1 on GPIO 11-14,
  card detect on GPIO 10 (active-high switch); RP2350 internal pull-ups on DO
  and CS as stand-ins for the final board's resistors; 400 kHz bring-up then
  1 MHz; an 8 GB microSDHC card
- Expected result: configure, init, get_info, three argument rejections with
  no bus traffic, block 0 read with `55 aa` at bytes 510-511, a single-block
  write and readback 64 blocks from the end, a 50-block write in five CMD25
  transfers with ACMD23, readback of all 51 blocks against a per-block
  LBA-tagged pattern, an untouched neighbour block on each side, deinit, then
  the heartbeat
- Observed result: 15 of 15 steps PASS; `15523840 blocks of 512 bytes`
  (7.95 GB); `55 aa` present. Two findings on the way, both resolved before
  the passing run: the breakout's detect switch is active-high (VAL-010), and
  a floating DO before CMD0 fails init with BUSY_TIMEOUT rather than anything
  pointing at the line (`tests/KNOWN_GAPS.md`, decision open)
- Status: HARDWARE VALIDATED under the stated conditions: bring-up with CRC
  checking on, CSD CRC against a real register, CRC-checked reads, CRC-checked
  single and multiple-block writes, capacity, argument rejection, card
  presence in the active-high sense, deinit. Not covered: any rate above
  1 MHz, hot removal, standard-capacity cards, write protect
- Reference: `tests/VALIDATION_RESULTS.md`, run of 2026-09-15

## Measurements / Performance Characterization

<!--
Record measured timing, throughput, memory, power, signal, or other quantitative
results. Include method, equipment, configuration, uncertainty or limitations,
and expected limits where applicable.
-->

## Known Untested or Partially Tested Behavior

Enumerated with consequences and proposed closure in
`tests/RESIDUAL_RISK.md`. In summary:

- One real-card run exists (VAL-H01), at 1 MHz on one SDHC card. No bus
  capture, no power measurement, no rate above 1 MHz, no hot removal on a real
  socket, no standard-capacity card.
- Interrupts can only be injected between SPI bytes or at a named transaction
  phase, never between two machine instructions.
- The interrupt fake models one executing core at a time, so the multicore
  memory ordering the architecture anticipates for the removal latch is
  untested.
- The card model is a reading of the specification. A card that is out of
  specification is not represented; one such deviation known from field
  reports remains open as SD-004.
- The data CRC cannot detect an all-zero packet with an all-zero CRC, which
  is what a bus stuck low across a whole single-block read produces
  (`tests/RESIDUAL_RISK.md`). An idle-byte check after the CRC would close
  it and is not implemented.
- FatFs, DMA, PIO, USB mass storage and power management are not
  implemented, so nothing is claimed about them.

## Validation Issues / Failures

One contract gap is open. Its regression asserts the intended behavior and
therefore fails against the current source; it is registered in CTest and
disabled by default so a passing default run is not read as evidence that it
is resolved. Enable with `-DTAVERNKEEP_TEST_KNOWN_GAPS=ON`.

- **SD-004** — CMD12 can mistake read data still in flight for its own
  response, turning a read whose data all arrived correctly into an I/O error.
  Impact: false negatives whose outcome depends on the contents of the next
  block. Not observed on hardware at 10-block streams.

Closed since this document's first version, each with its regression promoted
into the enabled suite: **SD-003** (read and CSD data CRC now validated,
2026-09-12), **SD-005** (R1 window raised to 16 reads, 2026-09-15), **SD-006**
(real CRC7 on every command frame and CMD59 enabling the card's checking,
2026-09-09) and **SD-007** (writes implemented, 2026-09-11). Details:
`tests/KNOWN_GAPS.md`.

Two defects found during the original review were fixed, each with a
regression written first and now part of the enabled suite:

- **SD-001** — a removal edge landing on a read's final release clock left the
  read returning success while cancellation was already latched, against this
  document's own requirement that a late completion never override a
  cancellation.
- **SD-002** — teardown of a device that was never initialized, or already torn
  down, released the SPI peripheral again, against the exactly-once teardown
  the architecture requires.

