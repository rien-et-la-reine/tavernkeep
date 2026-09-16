# Mutation results

Coverage says which lines ran. It says nothing about whether the suite would
notice those lines being wrong. This is the evidence that it would.

`tools/mutations.txt` holds 78 deliberate mistakes in `src/storage/sd_spi.c` and
`src/platform/gpio_irq.c`, one per record, chosen to span the failure classes
that matter for this driver: wrong constants, reversed conditions, removed
validation, incorrect bit masks, truncated integer widths, off-by-one range
checks, removed cleanup, skipped state transitions, ignored error codes,
incorrect device-type handling, wrong addressing conversion, premature success,
delayed error recognition, eliminated timeout behaviour, and output mutated on
failure.

```sh
python3 tests/tools/mutate.py                 # everything
python3 tests/tools/mutate.py --list
python3 tests/tools/mutate.py --only command-crc-frame-length
```

The script applies one mutation, rebuilds, runs the enabled host suite, records
which executables failed, and restores the tree — including after Ctrl-C or a
build failure. It normalises line endings before matching, because the
repository mixes LF and CRLF sources and a pattern that fails to apply would
otherwise be reported as a surviving mutation, which is a false clean bill of
health.

## Pending re-run

Two records were replaced after the recorded run below. `cmd0-crc-constant` and
`cmd8-crc-constant` patched the two hardcoded CRC bytes in `sd_spi_command()`,
which no longer exist now that the frame builder calls `crc_helper_7()`. A
record whose pattern cannot apply is reported as a surviving mutation, so they
were replaced rather than left to rot:

| Mutation | Class | Confirmed against |
| --- | --- | --- |
| `command-crc-frame-length` | wrong length | `sd_protocol` (24 of 24 cases), `sd_spi` (168 checks) |
| `cmd59-crc-disabled` | wrong constant | `sd_protocol`, via `test_command_crc_checking_is_actually_enabled` |

Both were confirmed by compiling the mutated source against those two suites,
not by a full run across every executable, so their "detected by" counts are a
floor rather than a measurement.

`cmd59-crc-disabled` is worth reading closely. It restores the exact defect that
existed briefly during development — CMD59 sent with argument 0 — and under it
`test_long_multiple_block_read` *passes*, because a card with checking off never
examines CMD12's placeholder CRC. Only the CMD59 enablement assertion catches
it. A mutation that makes one test pass while breaking another is the case
mutation testing exists to find.

**Mutation runs need a green baseline.** The script records which executables
failed, so any case already failing counts as a false kill for every mutation.
Between `4629e1d` (which cut `sd_spi_wait_ready()` from 1 s to 250 ms) and
2026-09-11, seven cases asserting the old 1 s wait were failing and the script
correctly refused to run. The budgets were settled on 2026-09-11 (500 ms before
a command, 250 ms elsewhere; PROTOCOL.md P-16), the tests updated, and the
catalogue re-run in full - see "Result, 2026-09-11" below.

## R1 poll window, 2026-09-15

The window went from 8 to 16 reads (`SD_SPI_R1_POLL_LIMIT`), closing SD-005.
Confirmed by hand against `sd_protocol` and `sd_spi`, not by a full run.

| Mutation | Class | Confirmed against |
| --- | --- | --- |
| `r1-poll-limit-off-by-one` (re-anchored) | off-by-one | `sd_protocol` (boundary sweep), `sd_spi` |
| `r1-poll-limit-old-eight` | regression to the old window | `sd_protocol`, `sd_spi` |
| `cmd12-poll-limit-old-eight` | stop command left at the old window | `sd_protocol` only, via the CMD18-then-CMD12 sweep added for it |

The third row is the reason the sweep exists: before it, that mutant survived
every enabled suite and was noticed only by the disabled SD-004 case.

## Card-detect polarity, 2026-09-15

`card_detect_active_high` touches four reads of the detect line in
`sd_spi.c` plus the pull selection. One record per touch point restores the
old hard-coded form; each was confirmed by hand against `sd_faults_host_tests`
under both `--card-detect` senses (not a full catalogue run). Every one is
killed in *both* runs, because `test_card_detect_polarity` sets each sense
itself; the active-high run additionally kills the first two through every
removal case in the suite.

| Mutation | Class | Confirmed against |
| --- | --- | --- |
| `card-detect-debounce-ignores-sense` | hard-coded polarity | active-low 1 check, active-high 25 checks |
| `card-detect-recheck-ignores-sense` | hard-coded polarity | active-low 1 check, active-high 25 checks |
| `card-detect-armed-edge-ignores-sense` | hard-coded polarity | active-low 1 check, active-high 4 checks |
| `card-detect-callback-edge-ignores-sense` | hard-coded polarity | active-low 1 check, active-high 4 checks |
| `card-detect-pull-removed` | missing pull | 2 checks under each sense |

## Read data CRC, 2026-09-12

Read CRC validation changed the text of `sd_spi_device_read_blocks()`, which
broke the patterns of six existing records (`data-token-constant`,
`block-length-511`, `crc-bytes-not-consumed`, `sdsc-byte-address-dropped`,
`sdsc-byte-address-inverted`, `removal-check-in-payload-removed`). They were
re-anchored to the current source rather than left to rot, and eight records
were added for the new code. Every one was confirmed by compiling the mutated
source by hand against four suites (`sd_crc16`, `sd_faults`, `sd_protocol`,
`sd_spi`), not by a full catalogue run: the enabled suite was not green at the
time (two `sd_spi` cases fail on a separate, uncommitted change to
`sd_spi_configure()`), so `mutate.py` correctly refused to run.

| Mutation | Class | Confirmed against |
| --- | --- | --- |
| `data-token-constant` (re-anchored) | wrong constant | `sd_faults`, `sd_protocol`, `sd_spi` |
| `block-length-511` (re-anchored) | off-by-one | `sd_faults`, `sd_protocol`, `sd_spi` |
| `crc-bytes-not-consumed` (re-anchored) | skipped state transition | `sd_faults`, `sd_protocol`, `sd_spi` |
| `sdsc-byte-address-dropped` (re-anchored) | wrong addressing | `sd_faults`, `sd_protocol`, `sd_spi` |
| `sdsc-byte-address-inverted` (re-anchored) | wrong addressing | `sd_faults`, `sd_protocol`, `sd_spi` |
| `removal-check-in-payload-removed` (re-anchored) | removed cancellation | `sd_spi` only |
| `read-crc-bytes-swapped` | wrong byte order | `sd_faults`, `sd_protocol`, `sd_spi` (every good multi-block read fails) |
| `read-crc-bytes-swapped-single` | wrong byte order | `sd_faults`, `sd_protocol`, `sd_spi` (every good single-block read fails) |
| `read-crc-check-removed-multi` | removed validation | `sd_faults` only, via the sweep and `test_read_data_crc_is_validated` |
| `read-crc-check-removed-single` | removed validation | `sd_faults` only, via the sweep and `test_read_data_crc_is_validated` |
| `read-crc-register-not-reset` | stale state | `sd_faults`, `sd_protocol`, `sd_spi` (second block of every stream) |
| `rolling-crc-wrong-generator` | wrong constant | `sd_crc16` (5 checks), and every read in the other three |
| `rolling-crc-ignores-register` | dropped feedback | `sd_crc16` (5 checks), and every read in the other three |
| `rolling-crc-seven-bits` | off-by-one | `sd_crc16` (5 checks), and every read in the other three |

The two `read-crc-check-removed-*` rows are the ones that matter: they are
killed **only** by `sd_faults_host_tests`, because it is the only suite that
sends a corrupt block. Every other suite sends correct CRCs and cannot tell a
validating driver from a discarding one. `removal-check-in-payload-removed`
is likewise killed only by `sd_spi`, as before.

## Write path, 2026-09-11

Twenty-two records were added with the write implementation
(`sd_spi_device_write_blocks()`, CMD24/CMD25). Each stands for a mistake the
implementation actually made, or nearly made, during review. They were first
confirmed by applying each mutation, rebuilding `sd_writes_host_tests` alone
and running it, then by the full catalogue run recorded in "Result,
2026-09-11" below. "Detected by" names cases in `sd_writes`; the full run adds
`sd_spi` and `sd_protocol` for `write-crc16-short` and
`single-write-multi-token` (their "write is implemented" cases store a block
and read it out of the model) and `sd_protocol` for
`single-write-nwr-gap-dropped` (its zero-protocol-error assertion).

| Mutation | Class | Detected by (cases in `sd_writes`) |
| --- | --- | --- |
| `write-crc16-end-bit` | wrong framing | 13 cases, first `CMD25 stores every block in order` (the all-zero block) |
| `write-crc16-short` | wrong length | `CMD24 stores the data`, `writes and reads interleave` |
| `write-source-stride` | wrong indexing | 13 cases |
| `single-write-multi-token` | wrong constant | 11 cases |
| `multi-write-single-token` | wrong constant | 14 cases |
| `multi-write-only-first-block` | loop exit | 14 cases |
| `multi-write-nwr-gap-dropped` | missing timing gap | 10 cases, via `sd_card_protocol_errors() == 0` |
| `single-write-nwr-gap-dropped` | missing timing gap | 7 cases, via `sd_card_protocol_errors() == 0` |
| `stop-tran-nbr-gap-dropped` | missing timing gap | `N_BR: busy that starts one byte after stop-tran is awaited` only |
| `stop-tran-dropped` | missing token | 6 cases |
| `final-busy-wait-ignored` | ignored result | the two `BUSY_TIMEOUT` cases |
| `inter-block-busy-wait-ignored` | ignored result | `programming busy beyond the budget is BUSY_TIMEOUT` only |
| `multi-write-cmd12-dropped` | skipped recovery step | the two rejection cases |
| `multi-write-error-mapping-precedence` | operator precedence | the two rejection cases |
| `multi-write-unknown-token-accepted` | missing validation | `unknown data-response byte`, multi-block fault sweep |
| `single-write-unknown-token-accepted` | missing validation | `unknown data-response byte`, single-block fault sweep |
| `multi-write-response-mask` | incorrect bit mask | `upper bits are don't-care`, `rejected data-response token` |
| `multi-write-rejection-ignored` | delayed error recognition | the two rejection cases |
| `single-write-rejection-ignored` | delayed error recognition | `a rejection followed by busy is waited out` only |
| `write-payload-removal-check-dropped` | delayed cancellation | `removal injected at each write phase` only |
| `write-sdsc-byte-address-dropped` | missing conversion | `CMD24 stores the data`, `CMD25 stores every block` |
| `write-range-check-off-by-one` | off-by-one | *equivalent, argument in the record* |

Four of these are killed by exactly one case. That is deliberate rather than
thin: each of those cases exists because an earlier draft of the suite let the
mutation survive, and the case was written to pin the one observable
difference (a one-byte busy delay, a busy overrun between blocks, a busy after
a write error, how soon after an eject the driver stops clocking). Deleting
any of them reopens a hole the catalogue now documents.

Three older records were disambiguated at the same time because the write
path duplicated their target lines: `command-crc-frame-length` now carries the
`sd_spi_command()` frame-builder context (the same line in
`sd_spi_stop_transmission()` is left unmutated), and `sdsc-byte-address-dropped`
/ `-inverted` now name the read path; the write path's copy has its own record.

## Result, 2026-09-11, GCC 15.2.0 (MSYS2), Release

**73 of 78 detected. 3 documented equivalent mutants. 1 documented as out of
this harness' reach. 1 survivor of the Release run**, the same
`irq-bounds-check-removed` as before, still caught only under the sanitizer
configuration (see "The one survivor"). Every record in the catalogue applied
and compiled.

```sh
python3 tests/tools/mutate.py --build-dir tests/build-mutation \
    "--cmake-arg=-GUnix Makefiles" \
    "--cmake-arg=-DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe"
```

Changes against the 2026-09-05 table: the three `get-info-*` records and the
twenty-two write-path records are now in the measured set; `command-crc-frame-length`
is caught by ten executables (it was two), because every suite now brings a
card up through the CRC-checked frame builder; `wait-ready-ignores-removal`
gained `sd_writes` as a killer; and the write-path column above is a floor
that the full run only widened.

One thing learned the hard way about the catalogue format: a comment block
placed *between* records is not skipped - the parser splits records only at a
blank line followed by `id:`, so comment lines are appended to the previous
record's replacement text and that mutation stops compiling. The first full
run reported `get-info-skips-usability-check` as "did not compile" for exactly
that reason; the comment was removed and the record re-run (killed by three
executables, as recorded). Put commentary in a record's `why:` continuation
lines or at the top of the file, never between records.

## Earlier additions

Three records were added after the recorded run below, covering
`sd_spi_device_get_info()`, which became live code in `7676adb` and had no
mutation coverage until then:

| Mutation | Class | Detected by |
| --- | --- | --- |
| `get-info-block-count-off-by-one` | off-by-one range check | `sd_spi`, `sd_protocol` |
| `get-info-block-size-wrong` | wrong constant | `sd_spi`, `sd_protocol` |
| `get-info-skips-usability-check` | removed validation | `sd_spi` (3 cases), `sd_protocol` |

Each was confirmed detected by compiling the mutated source against the two
suites directly; the table below has not been regenerated through
`tools/mutate.py`, so the totals in it exclude these three.

## Result, 2026-09-05, GCC 13.3.0, Release

**49 of 53 detected. 2 documented equivalent mutants. 1 documented as out of
this harness' reach. 1 survivor of the Release run, caught under the sanitizer
configuration.**

Three classifications are used, and they are not interchangeable:

- **equivalent** — no program can distinguish the mutation from the original,
  so no test could ever kill it. The argument is written out in
  `tools/mutations.txt` and must actually hold.
- **unkillable here** — the behaviour is real and worth guarding, but this
  harness cannot reach it. Closing it needs a different kind of test, named in
  the note.
- **survived** — a hole. Either write the test or move the behaviour into
  [KNOWN_GAPS.md](KNOWN_GAPS.md) with a reason.

Every mutation and its killer:

| Mutation | Class | Detected by |
| --- | --- | --- |
| `command-crc-frame-length` | wrong length | `sd_spi`, `sd_protocol` (see note) |
| `cmd59-crc-disabled` | wrong constant | `sd_protocol` (see note) |
| `acmd41-hcs-bit` | wrong constant | 5 executables |
| `drop-cmd55` | skipped state transition | 5 executables |
| `data-error-token-mask` | incorrect bit mask | 4 executables |
| `data-error-token-ignored` | delayed error recognition | 4 executables |
| `ocr-ccs-bit` | incorrect device-type handling | 9 executables |
| `ocr-powerup-check-removed` | removed validation | `sd_faults` |
| `sdsc-byte-address-dropped` | wrong addressing conversion | 4 executables |
| `sdsc-byte-address-inverted` | wrong addressing conversion | 5 executables |
| `address-truncated-to-16-bits` | truncated integer width | 4 executables |
| `range-check-off-by-one` | off-by-one range check | *equivalent, see below* |
| `range-check-count-removed` | removed validation | 3 executables |
| `r1-poll-limit-off-by-one` | off-by-one | 3 executables (re-anchored 2026-09-15 to `SD_SPI_R1_POLL_LIMIT`; see the R1 window record) |
| `r1-accept-nonzero` | reversed condition | `sd_protocol` |
| `data-token-constant` | wrong constant | 6 executables |
| `block-length-511` | off-by-one payload length | 5 executables |
| `crc-bytes-not-consumed` | skipped state transition | 4 executables |
| `cmd12-not-sent` | removed cleanup | 4 executables |
| `cmd12-result-ignored` | ignored error code | 3 executables |
| `cmd12-stuff-byte-missing` | protocol misunderstanding | 2 executables |
| `premature-success` | premature success | 3 executables |
| `release-bus-removed-single` | removed cleanup | 5 executables |
| `removal-check-in-payload-removed` | removed cancellation | 2 executables |
| `removal-latch-never-set` | skipped state transition | 7 executables |
| `removal-irq-not-suppressed` | removed bounce suppression | 7 executables |
| `require-usable-skips-latch` | reversed precedence | 3 executables |
| `csd-structure-check-removed` | removed validation | 3 executables |
| `csd-v2-capacity-multiplier` | wrong constant | 5 executables |
| `csd-v2-c-size-mask` | incorrect bit mask | 4 executables |
| `csd-v1-mult-off-by-one` | wrong constant | 4 executables |
| `csd-v1-read-bl-len-range` | off-by-one range check | 3 executables |
| `cmd16-skipped-for-sdsc` | skipped state transition | 3 executables |
| `cmd8-echo-check-removed` | removed validation | 2 executables |
| `cmd8-voltage-check-removed` | removed validation | 9 executables |
| `baud-raised-before-init` | skipped state transition | 3 executables |
| `baud-limit-removed` | removed validation | 2 executables |
| `init-marked-before-success` | premature state transition | *equivalent, see below* |
| `deinit-skips-wait-ready` | eliminated timeout behaviour | 2 executables |
| `wait-ready-accepts-busy` | reversed condition | 9 executables |
| `debounce-single-sample` | eliminated debounce | 2 executables |
| `pull-up-after-gpio-init` | incorrect hardware assumption | 2 executables |
| `rollback-leaves-spi-configured` | removed cleanup | 4 executables |
| `deinit-releases-twice` | reintroduces SD-002 | 5 executables |
| `release-race-unchecked-single` | reintroduces SD-001 | 2 executables |
| `irq-dispatch-ignores-event-mask` | incorrect bit mask | `gpio_irq` |
| `irq-register-overwrites-owner` | removed validation | `gpio_irq` |
| `irq-register-ignores-core` | removed validation | `gpio_irq` |
| `irq-events-validation-removed` | removed validation | `gpio_irq` |
| `irq-bounds-check-removed` | removed validation | *sanitizer only, see below* |
| `release-bus-clocks-absent-card` | removed cancellation | 2 executables |
| `wait-ready-ignores-removal` | removed cancellation | 2 executables |
| `stop-transmission-ignores-removal` | removed cancellation | *out of reach, see below* |

## The two equivalent mutants

Both were checked by argument, not assumed. Neither is a hole in the suite: no
test can distinguish them because no input can.

**`range-check-off-by-one`** changes `first_lba >= block_count` to
`first_lba > block_count`. The two forms differ only when
`first_lba == block_count`, and there the second clause of the same condition
already fires: `block_count - first_lba` is zero and the requested count is at
least one, because both the block-device wrapper and the backend reject a zero
count. The redundancy is worth keeping as defence in depth — if the count check
were ever relaxed, this clause would become load-bearing.

**`init-marked-before-success`** moves `sd->initialized = true` earlier, to just
before the final release clock. Nothing in that window reads the flag, and the
only exit from it is `sd_spi_init_rollback()`, which clears it. Moving the
assignment any earlier — before the final removal check, for instance — is a
different mutation and is detected.

## The one out of reach

**`stop-transmission-ignores-removal`** deletes the removal check at the top of
`sd_spi_stop_transmission()`. Every caller already re-checks the latch after its
last SPI transfer, and this harness can only fire a removal edge during a
transfer, so the window the check guards — a removal landing between the
caller's check and the CMD12 frame, which on real hardware is between two
instructions — is unreachable from the host.

The check is defence in depth and must stay. This is recorded as
`unkillable:` rather than `equivalent:` because it is not equivalent: on real
hardware the two versions differ, and closing it needs a target test that fires
the edge from a hardware timer at randomised offsets. See
[RESIDUAL_RISK.md](RESIDUAL_RISK.md) section 1.1.

Finding this was itself the point of running the mutation suite against the
finished work rather than only against the starting point.

## The one survivor

**`irq-bounds-check-removed`** changes `gpio >= NUM_BANK0_GPIOS` to
`gpio > NUM_BANK0_GPIOS` in the interrupt dispatcher, so the boundary index
subscripts one element past the slot table. The Release suite does not catch it:
the byte read past the array happens to hold a null handler, so nothing visible
changes. The sanitizer configuration does catch it, as a global-buffer-overflow:

```sh
python3 tests/tools/mutate.py --only irq-bounds-check-removed \
    --build-dir tests/build-mutation-san \
    --cmake-arg=-DTAVERNKEEP_TEST_SANITIZE=ON --cmake-arg=-DCMAKE_BUILD_TYPE=Debug
```
```
[ 1/ 1] irq-bounds-check-removed           KILLED by gpio_irq_host_tests
```

It only catches it because a test now *delivers* the boundary index —
`test_gpio_irq.c` calls `gpio_irq_hardware_mock_deliver()` with
`NUM_BANK0_GPIOS`, `+1` and `+2`. Without that input the sanitizer has nothing
to trip on. This is the general shape: memory-safety bugs need a test that
reaches them and a tool that notices, and neither alone is enough. It is
recorded as a survivor rather than reclassified, because the default
configuration genuinely does not catch it.

## When to run this

After any non-trivial change to `src/storage/sd_spi.c` or
`src/platform/gpio_irq.c`, and whenever you add a test you want to trust. Add a
mutation for the mistake you were most worried about making; if it survives, the
test does not check what you think it checks.

Do not retire a surviving mutation by writing an `equivalent:` note unless the
argument actually holds. If a mutation cannot be caught and is not equivalent,
that is a gap — write the test, or move the behaviour into
[KNOWN_GAPS.md](KNOWN_GAPS.md) with a reason.
