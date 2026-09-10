# Working with the host test harness

This is the guide for whoever changes Tavernkeep's storage code next. It
explains what the harness can represent, how to express a new adversarial case
cheaply, and which assumptions must not be quietly broken.

Read [AUDIT.md](AUDIT.md) for what the harness was like before and what changed,
[PROTOCOL.md](PROTOCOL.md) for the specification rules the tests encode,
[MUTATION.md](MUTATION.md) for evidence that the suite actually detects bugs,
and [RESIDUAL_RISK.md](RESIDUAL_RISK.md) for what host tests cannot prove.

## Layout

```
tests/
  support/sim_clock.[ch]      simulated time, driven by bus work
  support/sd_card_model.[ch]  a stateful SD-over-SPI card
  support/pico_mock.[ch]      Pico SDK peripheral fakes (GPIO, SPI, time)
  support/sd_fixture.[ch]     driver fixture, guarded buffers, state checks
  support/test_harness.[ch]   assertions with context and trace dumps
  support/test_rand.h         seeded PRNG for property and fuzz tests
  support/gpio_irq_*          dispatcher fakes for the platform layer
  fakes/                      minimal SDK headers, so no SDK is needed
  test_sd_spi.c               the original suite, on the new engine
  test_sd_protocol.c          protocol conformance and boundary matrices
  test_sd_faults.c            fault injection, partial operations, recovery
  test_sd_property.c          property and fuzz tests (seeded)
  test_sd_gaps.c              regressions for gaps that are still open
  tools/mutate.py             mutation harness
  tools/mutations.txt         the mutation catalogue
  tools/run_diagnostics.sh    sanitizers, strict warnings, coverage, mutation
```

## Running things

```sh
cmake -S tests -B tests/build -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

Useful configurations:

| Option | Effect |
| --- | --- |
| `-DTAVERNKEEP_TEST_SANITIZE=ON` | AddressSanitizer and UndefinedBehaviorSanitizer, no error recovery |
| `-DTAVERNKEEP_TEST_STRICT_WARNINGS=ON` | `-Werror` on top of an already strict warning set |
| `-DTAVERNKEEP_TEST_COVERAGE=ON` | GCC coverage instrumentation (GCC only) |
| `-DTAVERNKEEP_TEST_KNOWN_GAPS=ON` | enables the regressions for gaps that are still open |
| `-DTAVERNKEEP_TEST_SEED=N` | fixes the property suite's seed |

`tools/run_diagnostics.sh` runs the whole set in sequence.

## The card model

`sd_card_model.h` is a byte-level state machine for an SD card in SPI mode. A
test describes a card and the model synthesises its registers:

```c
sd_fixture_t fx;
sd_card_desc_t card = sd_fx_card_sdhc();   /* or sdxc / v1_sdsc / v2_sdsc */
card.ncr_bytes = 3;                        /* response latency */
card.read_access_us = 500;                 /* N_AC before a data token */
T_CHECK(sd_fx_require_init(&fx, &card));
```

The model enforces things a real card enforces and the old fake did not:

- command framing (start bit, transmission bit, end bit) is validated;
- CRC7 is checked on CMD0 and CMD8 always, and on everything else when the
  card's `crc_check_enabled` is set, which bring-up now turns on via CMD59 — so
  every CRC the driver computes is load-bearing, not decoration;
- CMD41 only behaves as ACMD41 when CMD55 immediately precedes it;
- a high-capacity card refuses to leave the idle state unless ACMD41 carries
  the HCS bit;
- chip select gates everything: a deselected card answers 0xFF and forgets any
  partial command or pending response;
- a CMD18 stream continues until CMD12, from a generated block store, so a
  multiple-block read has no length limit.

`sd_card_set_raw_csd()` overrides the synthesised CSD with a literal register.
Use it to drive the parser with dumps captured from real cards — the model's
encoder and the driver's decoder were written from the same specification, so
agreement between them proves less than agreement with a real card does.

### Adding a card variant

Add an `sd_card_kind_t`, teach `card_is_high_capacity()`, `card_supports_cmd8()`
and `build_csd()` about it, and add a `sd_fx_card_*()` helper. `build_csd()`
deliberately aborts with a message when a capacity is not encodable rather than
emitting a nonsense register: a harness that silently produces an impossible
card lets a test pass against something that cannot exist.

## Fault injection

A fault names *where* it happens, not which byte index it lands on, so it
survives changes to the driver's byte layout:

```c
sd_fault_t fault = {0};
fault.phase = SD_PHASE_DATA_PAYLOAD;  /* see sd_phase_t for the list */
fault.command = 18U;                  /* or SD_ANY_COMMAND */
fault.occurrence = 1U;                /* the second block of this read */
fault.byte_offset = 200U;             /* 200 bytes into that payload */
fault.kind = SD_FAULT_EJECT;          /* raise the card-detect line here */
T_CHECK(sd_card_add_fault(&fault));
```

`occurrence` counts entries into the phase *within the named command*, so
"the second block of this read" does not shift when bring-up changes.
`sd_card_phase_entries(phase)` gives the current count, which is how you say
"the next time this happens" for a phase that bring-up also visits.

**Always assert `sd_card_fault_activations(i)`.** A fault that never fires
turns an adversarial test into a happy-path test that quietly passes. Several
cases here would have been worthless without that check.

Fault kinds cover: stalling, permanent busy, substituted R1, data error tokens,
single-byte corruption, bit flips, truncation, corrupted data CRC, card
ejection, extra busy time, and a pseudo-random response stream for fuzzing.
Adding one means adding an enumerator and a case in `emit()`; set
`state_overridden` if the fault takes over the state machine, or the caller
will advance its own state on top of yours.

## Simulated time

`sim_clock` advances time from modelled work, not from poll counts:

- an SPI byte costs 8 bit-times at the currently programmed baud rate;
- `sleep_ms()` costs what it says;
- `time_reached()` is a pure query.

This is why a timeout assertion here reads
`T_CHECK(elapsed_us >= 100000)` rather than counting calls. Write new timeout
tests that way. `sd_fx_byte_budget()` converts a duration into the number of
bus bytes it can carry, for "the driver did no more work than the clock allows"
assertions.

`SIM_CLOCK_POLL_TICK` reproduces the old model, where each `time_reached()` cost
a millisecond. It exists so `sd_spi_host_tests` can be run under both models and
any case that depends on the artifact is exposed. **Do not write new tests
against it.** The clock model survives `pico_mock_reset()` on purpose, so a
suite launched in poll-tick mode stays there.

What this still is not: byte time ignores inter-byte gaps, FIFO depth, DMA,
interrupt latency and card timing variation. It gives a defensible lower bound
on how long a bounded loop takes. It is not a throughput measurement and proves
nothing about real-time behaviour.

## Tracing

Every protocol event is recorded: commands with arguments and CRC validity,
R1 bytes, trailers, data tokens, error tokens, blocks streamed, busy windows,
chip-select changes, write tokens and protocol errors. Assert on the sequence
rather than on counters where order carries meaning:

```c
static const uint8_t expected[] = { 0, 8, 55, 41, 58, 9 };
T_CHECK(command_sequence_is(expected, sizeof(expected)));
```

`t_report_failure()` dumps the trace automatically, so a failing test tells you
the byte-level story rather than just a return code. `sd_card_protocol_errors()`
counts things the model rejected; asserting it is zero on a success path is a
cheap way to catch a driver that got the right answer the wrong way.

## State and recovery assertions

Checking the return value is the least interesting half of a failure test.
These helpers cover the rest:

| Helper | Question it answers |
| --- | --- |
| `sd_fx_check_bus_quiescent` | is chip select released and the card not mid-answer? |
| `sd_fx_check_recovers` | does the next read work, and return the right data? |
| `sd_fx_check_removed_and_teardown_once` | are new operations refused without bus traffic, and is hardware released exactly once? |
| `sd_fx_guard_intact` | did anything write outside the destination? |
| `sd_fx_guard_untouched` | was the destination left completely alone? |
| `sd_fx_guard_prefix_from_card` | exactly how many bytes were populated before the failure? |
| `sd_fx_guard_matches_card` | is the data actually what the card holds? |

Use `sd_fx_guard_prefix_from_card` rather than counting bytes that differ from
the fill pattern: real block data contains the fill byte by coincidence, and a
"how much was written" assertion built that way under-counts and looks like an
off-by-one in the driver.

`sd_fx_check_recovers` returns the card to a responsive state first, so it asks
whether the *driver* survived rather than whether the fault is still active.

## Property and fuzz tests

`test_sd_property.c` seeds every generator explicitly, prints the seed it ran
with, and accepts `--seed N` to replay. Keep that property: a randomised test
whose failure cannot be reproduced is worse than no test.

The response fuzz points `SD_FAULT_RANDOM_STREAM` at a chosen phase, so the
driver's parser sees arbitrary bytes where it expects R1, tokens, payload and
CRC. It asserts only what must hold regardless: termination inside the declared
budgets, a defined result code, no writes outside the destination, a released
bus, and a driver that still works afterwards.

The operation fuzz keeps a hand-written model of the driver's documented state
machine — initialised, latched, media present, interrupt armed — and checks the
driver against it. Keep that model independent of the implementation; the moment
it is derived from the code it stops being a test.

## Mutation testing

```sh
python3 tests/tools/mutate.py                       # the whole catalogue
python3 tests/tools/mutate.py --only command-crc-frame-length
python3 tests/tools/mutate.py --list
python3 tests/tools/mutate.py --build-dir tests/build-mutation \
    --cmake-arg=-DTAVERNKEEP_TEST_SANITIZE=ON --cmake-arg=-DCMAKE_BUILD_TYPE=Debug
```

**Run it after any non-trivial change to `src/storage/sd_spi.c` or
`src/platform/gpio_irq.c`**, and add a mutation for the mistake you were most
worried about making. If it survives, the test you just wrote does not check
what you think it checks.

A mutation that cannot be killed by any test because no reachable input
distinguishes it gets an `equivalent:` field carrying the argument for why.
Do not use that field to retire a mutation you simply could not catch — write
the test instead, or move the behaviour into KNOWN_GAPS.md.

## Assumptions that must not be forgotten

1. **The legacy scripted path still exists.** `pico_mock_sd_set_command()`
   answers a command from an opaque byte string, and `pico_mock_reset()` selects
   `SD_RESPONSE_SCRIPTED_ONLY` so unscripted commands answer with a single 0xFF,
   exactly as the original fake did. `sd_fx_begin()` selects
   `SD_RESPONSE_MODELLED` instead. New tests should use the fixture; if you use
   the raw mock, know which policy you are in.
2. **Chip select is opt-in in the legacy path.** With no chip-select pin
   registered the model treats the card as permanently selected, because the
   original fixtures were written that way. `sd_fx_begin()` always registers it.
3. **The interrupt fake models one core at a time.** It cannot represent two
   cores running simultaneously, and it cannot represent an interrupt landing
   between two machine instructions — only between two SPI bytes or at a named
   phase. Removal coverage is therefore dense but not exhaustive.
4. **The integration executable initialises the real dispatcher's static state
   once per process.** Do not reset the hardware fake underneath an already
   initialised dispatcher.
5. **`build_csd()` aborts on an unencodable capacity.** That is deliberate. If
   you hit it, pick a capacity the register can express rather than working
   around it.
6. **Coverage numbers are per target and must not be summed.** `gcov`
   overwrites reports that share a basename.
7. **A green default run does not mean there are no known gaps.** Three
   regressions are registered and disabled; see [KNOWN_GAPS.md](KNOWN_GAPS.md).

## Adding a test: the short version

1. Decide what incorrect implementation you are trying to catch.
2. Write the case with `sd_fx_begin`/`sd_fx_require_init` and a card
   description; inject a fault by phase if you need a failure.
3. Assert the return value **and** the state: bus quiescent, buffer guards,
   how much of the destination was populated, and that the device still works.
4. Assert `sd_card_fault_activations()` if you injected anything.
5. Add the mutation you were guarding against to `tools/mutations.txt` and run
   `mutate.py --only <id>`. If it survives, the test is not doing its job.
