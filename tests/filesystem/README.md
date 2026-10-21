# Storage-layer host tests

Tests for everything in `src/storage/` above the SD driver: the storage coordinator (`storage.c`),
the filesystem layer (`filesystem.c`) and its FatFs adapter (`diskio.c`), running the real FatFs over a
RAM-backed fake block device. Standalone until they are folded into the main host suite. (The folder
keeps its first name; it grew the coordinator's tests later.)

```sh
cmake -S tests/filesystem -B build-fs-tests -G Ninja
cmake --build build-fs-tests
./build-fs-tests/fs_tests                 # all cases, each in its own process
(cd build-fs-tests && ctest)              # the above, a shuffled order, and the harness selftest
python3 tests/filesystem/mutate.py        # mutation testing; see below (about 5 minutes)
```

`fs_tests --list`, `fs_tests NAME_SUBSTRING...`, `fs_tests --shuffle SEED`, and `fs_tests --no-fork`
(in one process, for a debugger) are also available. With `--no-fork`, select one `storage_` case at a
time: the coordinator can be initialized only once per process, as it can only once per boot.

## Layout

| File | What it is |
|---|---|
| `test_diskio.c` | The adapter, driven directly through FatFs's `disk_*()` entry points |
| `test_filesystem.c` | The layer, through `filesystem.h`, with the adapter and FatFs underneath |
| `test_storage.c` | The coordinator, through `storage.h`, with everything above underneath and explicit time |
| `fake_block_device.c/.h` | RAM card that mirrors the SPI SD driver's contract (including `is_present`), counts calls, injects faults (once or several calls in a row) into reads, writes, `get_info`, `init` and `deinit`, can mark one sector unreadable until it is rewritten, and can pull the card during a chosen `get_info` |
| `fake_platform.c/.h` | Settable clock (`platform/clock.h`) and a log that counts warnings (`platform/debug.h`) |
| `harness.h`, `test_main.c` | `EXPECT`/`REQUIRE` macros, fixture, runner |
| `fixture.h` | Shared fixture: the disk, the content pattern, the case tables |
| `mutate.py` | Mutation testing |

## Design decisions that keep the tests honest

- **Every case starts from a cold boot.** By default each case runs in a forked child, so the
  layer's static state (slot generations, FatFs's registered volume) cannot carry over from an earlier
  case. A single-process run once hid a real class of bug: anything that only shows on the first open
  after boot was masked because earlier cases had already advanced every generation. A crash in a case
  is reported as that case failing.
- **A case that executes no checks fails**, so an early return or an empty loop cannot make a test
  pass vacuously. `--selftest` proves the harness reports a failing check, a vacuous case and a crash.
- **After every case** the runner checks the layer is unmounted and the adapter unbound, then forces
  both, so one failure cannot cascade.
- **The fake mirrors `sd_spi.c`, not an idealised disk:** the removal latch is checked before
  initialization, a second `init` is refused, a removed card stays latched after reinsertion until the
  owner re-initializes it, a `deinit` after a removal cannot fail (the driver skips the bus), presence
  reports the socket rather than the latch, and a failed read scribbles over its buffer (the
  block-device contract says its contents are unspecified).
- **The coordinator's clock is the test's.** Every `storage_step` gets an explicit time, so retry
  intervals are checked to the millisecond (one short of due, then due), including across the wrap of
  the 32-bit millisecond clock.
- **Expected values come from documents, not from the code under test:** the contract in
  `filesystem.h`, the tables in `diskio.c`'s header, FatFs's own documentation (each FatFs behaviour
  asserted is stated there: dot entries filtered, seek clipping and expansion, short writes on a full
  volume, `FR_DENIED` for a non-empty directory), hand-packed FAT timestamps with their derivation
  written beside them.
- **Nothing unpromised is asserted:** which slot a handle gets, generation values (only "never 0" and
  "different on reuse"), directory order (listings compare as sets), a directory's size, the bytes of a
  seek-extended gap, the contents of a file whose writer was unmounted, and which error wins when a call
  has several things wrong.
- **White-box calls into FatFs are few and marked:** creating directories and a volume label (the
  interface has no call for either), reading a file's timestamp, and checking that the volume is no
  longer registered after unmount or a failed mount. The test volumes themselves are made with `f_mkfs`
  (`fixture_format`), as a computer would make them: the firmware has no format, so `FF_USE_MKFS` is 1
  in this build only.

## Mutation testing

`mutate.py` plants 150 single bugs, one at a time, in copies of the three sources (the repository is
never touched), rebuilds and runs the suite. Every one must be caught. Each mutation names the behaviour
it breaks. In the filesystem layer and adapter: every row of both result-mapping tables, every status
bit, every geometry check, binding, gating, slot bounds and generations, mount/unmount cleanup, the retry
of a failed close,
unmount doing no I/O, access modes, byte counts, seek/tell/size, pattern copying and limits,
end-of-directory, entry fields and the timestamp packing. In the coordinator: noticing removal and
insertion, everything the loss path must and must not do, the attempt count and when it resets, retry
timing and its wraparound, unseated versus failed cards, removal versus an unusable card, every request
rule, the order of a step's phases, and what is lent to USB and when. If you change either source, re-run it: a mutation whose target text has
gone is reported as STALE rather than silently skipped.

Validation history, kept because it shaped the suite:

- The first run caught 96 of 102. Two of the misses were mutations that didn't compile and needed
  rewriting. One was equivalent and removed (see the end of `mutate.py`). One revealed the
  first-open-after-boot blind spot, which led to running each case in its own process. One was a test
  coincidence: 2108-01-01 00:00 wraps to exactly the 1980 sentinel, so the test now uses a different date.
- Moving to one process per case made two mutations that had been caught slip through. Both had been
  caught only by state leaking between cases. The fixes:
  - **Generation wrap:** an open/close cycle advances the generation by two, so one pass only ever
    issues one parity. The test now runs two passes, and asserts that they started on different parities.
  - **Unmount leaking the directory slot:** the test freed the leaked slot itself, by closing the stale
    handle before checking the pool. It now checks the pool first.
- Adding the coordinator: its first run caught 139 of 144. One mutation didn't compile. One was an
  equivalent pair: a loss clears a pending format, and `format_now` re-checks anyway, so each check
  covers the other (kept as a pair, documented). Three were real gaps, now each caught by its own case:
  - **A quick reseat:** a card pulled and pushed back between two steps must not inherit the pulled
    card's failure count. Normally an empty-socket check resets it in between.
  - **A failed format:** it must count its attempts from zero, not on top of failures from before the
    card's last success.
  - **A USB handover requested as a card arrives:** the owner change must take its own step, so the
    card is never mounted only to be unmounted again.
- Writing the coordinator's cases also turned up two mistakes in the tests themselves, not the code.
  An expectation of an in-between state the coordinator correctly passes through within one step. And
  a fake that let a `deinit` fail after a removal, which the real driver cannot do.
- The firmware's format path was later removed (formatting is left to a computer), and its cases and
  mutations with it, the failed-format case above included. That made one mutation equivalent: a
  successful mount not resetting the attempt count, which only the format path could observe. It moved
  to the equivalents at the end of `mutate.py`.
- One mutation is not reliably detectable and is documented in `mutate.py`. The directory-slot bound
  check off by one reads one element past a one-element array, and AddressSanitizer only reports that
  when the read lands in its guard zone. Here it lands beyond it.

## Not covered

- **FAT32 volumes.** The 2 and 32 MiB test cards always get FAT12/16, so FAT32 runs only on hardware. A
  FAT32-sized card needs a sparse fake, since the current fake allocates its whole capacity.
- **Short writes at the 4 GiB − 1 file limit**, and `clip_to_uint` (it only matters for a single
  request over 4 GiB on a 64-bit host).
- **Breaking the open-entry caller rule** (removing or renaming an open file). The layer cannot detect
  it, so the behaviour is undefined and there is nothing to assert.
- **Uninitialized-memory reads.** No MemorySanitizer (clang) or valgrind was available when this
  was written; worth one run with either.
- **The real `clock.c` and `debug.c`**, which the fakes replace, **and the real SD driver**, including
  its new `is_present` and the card-detect pin it now keeps. These tests prove the layers against the
  driver's *contract*. The card model and hardware prove the rest.
- **`main.c`**, the demo that drives the coordinator; your card-model host test covers that.

## Folding into the main suite

- `fake_platform.c` defines `clock_is_valid`, `clock_get_datetime` and `debug_log`. Don't link it
  alongside the real `clock.c` or `debug.c`.
- The per-case `fork()` is POSIX. Keep it, or replace it with whatever per-test process isolation the
  main suite uses. Don't drop it: the layer's static state makes in-process ordering matter.
- `FS_TESTS_FILESYSTEM_SOURCE`/`FS_TESTS_DISKIO_SOURCE` exist for `mutate.py`. If the main suite
  builds these tests, give it the same two overrides, or point `mutate.py` at this standalone build.
