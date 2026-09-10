# Host tests

These compile production C against small Pico SDK fakes. No SDK, card, download
or firmware build is needed. Requirements: a C11 host compiler with atomics,
CMake 3.16+ and a build tool. Verified with GCC 13.3 on Linux and GCC 15.2 /
MinGW on Windows; MSVC has not been validated.

```sh
cmake -S tests -B tests/build -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

On Windows with MSYS2/MinGW installed but not on `PATH`:

```powershell
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
cmake -S tests -B tests/build -G "Unix Makefiles" `
    -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
    -DCMAKE_MAKE_PROGRAM=C:/msys64/usr/bin/make.exe `
    -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build -j 4
ctest --test-dir tests/build --output-on-failure
```

Generated files stay inside `tests`. There are **24 enabled CTest cases** and
five disabled ones for gaps that are still open. Checks remain active under
`NDEBUG`; every case has a real-time timeout.

## Where to look

| Document | For |
| --- | --- |
| [HARNESS.md](HARNESS.md) | **start here to add or change a test** — what the harness can represent and how to use it |
| [AUDIT.md](AUDIT.md) | what the previous harness could not detect and what replaced it |
| [PROTOCOL.md](PROTOCOL.md) | the specification rules the tests encode, with sources |
| [MUTATION.md](MUTATION.md) | evidence that the suite detects deliberate bugs |
| [KNOWN_GAPS.md](KNOWN_GAPS.md) | production gaps that are still open |
| [RESIDUAL_RISK.md](RESIDUAL_RISK.md) | what host testing cannot prove |
| [VALIDATION_PLAN.md](VALIDATION_PLAN.md) | acceptance criteria for functionality not yet written |
| [VALIDATION_RESULTS.md](VALIDATION_RESULTS.md) | recorded build, test, sanitizer and coverage results |

## Suites

| Suite | Behaviour checked |
| --- | --- |
| `sd_spi_host_tests` | 63 groups: SDHC/legacy/v2 SDSC, ACMD41 retries, CSD capacities and rejection, SPI framing, R1 byte limits, all error tokens, reads, canaries, address limits, cleanup, IRQ races, removal sweeps, a refused CMD59, explicit write/info stubs. Run twice, once under each clock model |
| `sd_protocol_host_tests` | card variant matrix, command ordering, per-frame CRC7, CMD59 actually enabling the card's command CRC checking, application commands, HCS, the R1 poll boundary, all 128 R1 values, all error tokens, long multiple-block reads, addressing per card type, capacity boundaries, CSD registers from real cards, 74-clock bring-up, bus release after every outcome |
| `sd_faults_host_tests` | fault injection across every read phase, error after partial success, CMD12 failure after good data, bounded busy periods, removal at each phase and during the release clock, OCR power-up status, reinsertion, the data-CRC gap |
| `sd_crc_host_tests` | `crc_helper_7()` against the specification's CMD0/CMD17 vectors and, differentially, against the card model's independent CRC7. No bus |
| `sd_crc16_host_tests` | `crc_helper_16()` against the CRC-16/XMODEM catalogue check value and hand-derivable vectors, differentially against the card model, every single-bit error in a 512-byte block, and the zero-residue property a receiver validates with. No bus |
| `sd_property_host_tests` | CSD arithmetic across all field encodings, the largest addressable card, address conversion on random cards, random card responses, random operation and removal sequences. Seeded; prints its seed and takes `--seed N` |
| `sd_removal_*_host_tests` | regressions for the two production defects fixed during the test overhaul |
| `gpio_irq_host_tests` | ownership and core rules, validation, routing and masking, multiple GPIO users, callback retention, nested interrupt state, self-unregistration, out-of-range dispatch indexes |
| `block_device_host_tests` | every wrapper and missing callback, exact context/buffer/64-bit LBA/count forwarding, all result categories |
| `filesystem_host_tests` | invalid inputs preserve state, preparation and rebinding, mount/unmount explicitly unimplemented and never touching storage |
| `sd_irq_integration_host_tests` | the real SD driver and the real dispatcher together; an unrelated input IRQ survives removal, cleanup, reinsertion and interrupted reads |
| `board_*`, `debug_*`, `main*` | LED variants, logging enabled and disabled, the real startup and foreground loop including initialisation failures |

## Diagnostics

```sh
sh tests/tools/run_diagnostics.sh                 # all stages
sh tests/tools/run_diagnostics.sh strict sanitize # a subset
```

| Option | Effect |
| --- | --- |
| `-DTAVERNKEEP_TEST_SANITIZE=ON` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `-DTAVERNKEEP_TEST_STRICT_WARNINGS=ON` | `-Werror` on an already strict warning set |
| `-DTAVERNKEEP_TEST_COVERAGE=ON` | GCC coverage instrumentation |
| `-DTAVERNKEEP_TEST_KNOWN_GAPS=ON` | enables the regressions for open gaps |
| `-DTAVERNKEEP_TEST_SEED=N` | fixes the property suite's seed |

Coverage identifies unexercised paths. It does not prove protocol compliance,
physical timing or concurrency correctness, and `gcov` reports that share a
basename overwrite each other, so per-target numbers must not be summed.
Mutation testing is the stronger signal: see [MUTATION.md](MUTATION.md).

## Fake boundaries

- The SD fake is a stateful card model, not a card. It validates command
  framing and CRC7, tracks application-command state, streams multiple-block
  reads of any length, and injects faults at named transaction phases. It has
  no electrical behaviour and no timing variation.
- Simulated time advances with modelled bus work — an SPI byte costs 8 bit
  times at the programmed baud rate — not with poll count. Assertions on
  elapsed time are therefore about the driver's declared budgets. They are not
  throughput measurements and prove nothing about real-time behaviour.
- The legacy poll-tick clock is retained and the SD suite runs under both, so a
  case that depends on the old artifact is exposed rather than hidden.
- The interrupt fake models one executing core at a time, not simultaneous
  multicore execution or electrical bounce. Faults land between SPI bytes or at
  a phase boundary, never between two machine instructions.
- The integration executable initialises the real dispatcher's static state
  once per process; do not reset the hardware fake beneath it.
- Logging is intercepted in a test-only compilation wrapper, and the main
  wrapper renames the entry point and leaves the real infinite loop with
  `longjmp` from the fake sleep. Neither replaces production behaviour.
- The card model validates the CRC16 the host appends to a written block and
  rejects a mismatch with data-response token `0x0B`. It does **not** validate
  the CRC7 on command frames unless the card description asks for it
  (`crc_check_enabled`), which mirrors the SPI-mode default.
- Production discards the read and CSD data CRC, and sends a real CRC7 only on
  CMD0 and CMD8. Passing tests do not demonstrate CRC validation, per-frame
  command CRC, working writes, FatFs, PIO/DMA or USB storage. The acceptance
  tests for the first three are written and failing: see
  [KNOWN_GAPS.md](KNOWN_GAPS.md) SD-003 and SD-007.
