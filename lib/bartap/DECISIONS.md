<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Decisions

Decisions that shape Bartap. Open questions are in [DESIGN-QUESTIONS.md](DESIGN-QUESTIONS.md); a
settled one moves here, naming its number.

## D1 — A dependency-free C library for EPUB on small devices

- Context: A microcontroller reader that opens EPUB files directly needs zip, DEFLATE, XML and EPUB
  package handling in tight memory, with no operating system, no heap and no large dependencies.
- Decision: Bartap is a standalone, freestanding C library for reading EPUB files on microcontrollers.
- Rationale: Most small readers are written in C or C++, so a C library can be used by nearly all of
  them. Building Tavernkeep's EPUB handling as a library also forces cleaner boundaries than building it
  into the firmware.
- Alternatives considered: Using or adapting an existing EPUB implementation; building EPUB handling
  into Tavernkeep.
- Consequences and trade-offs: More design and test effort up front than firmware-only code.

## D2 — Scope: container, package and navigation, not layout

- Context: From the bottom up, a reader needs the zip container, DEFLATE, the OCF container
  (`META-INF/container.xml`), the OPF package (metadata, manifest, spine), navigation (the EPUB 3
  navigation document or the EPUB 2 NCX), the XHTML content documents, and layout and rendering.
- Decision: Bartap covers zip, inflate, OCF, OPF and spine, navigation, and EPUB CFI parsing and
  resolution. Layout, pagination, fonts and rendering are out of scope. Whether it also turns XHTML into
  a styled text stream is open (Q17).
- Rationale: Everything up to navigation and CFI is defined by specifications and the same for every
  reader. Layout depends on the display, the fonts and the reader's policy, so it belongs to the caller.
- Alternatives considered: Zip and inflate only (too thin to be worth using); a full reading engine with
  layout (too device-specific to reuse).
- Consequences and trade-offs: Callers write their own content and layout handling. Bartap must work by
  streaming on devices with no PSRAM to hold a whole chapter, so CFI resolution has to work over content
  read in pieces; this needs testing once CFI resolution exists.

## D3 — No dynamic allocation; the caller owns all memory

- Context: Target devices have a few hundred KiB of RAM, often in regions of different speed (on-chip
  SRAM, external PSRAM), and many have no heap.
- Decision: Bartap never allocates and keeps no global or static mutable state. All state is in structs
  the caller owns, and the caller provides all working memory and output buffers.
- Rationale: Memory use is predictable, and the caller decides where each buffer lives: the inflate
  window in fast SRAM and bulk output in PSRAM, for example. Independent instances can coexist.
- Alternatives considered: Caller-supplied allocator callbacks; an internal fixed pool.
- Consequences and trade-offs: The API must state how much memory each part needs (Q2), and its calls
  take more parameters than an allocating library's would.

## D4 — All I/O through caller-supplied callbacks

- Context: Books may come from a FAT filesystem, raw flash, memory, or anything else a device has.
- Decision: Bartap makes no file or OS calls. It reads through a byte source the caller implements
  (read at an offset, total size). A memory-backed byte source is included.
- Rationale: Platform independence, and one test path: host tests and devices run the same code over
  different byte sources.
- Alternatives considered: Built-in POSIX or FatFs backends.
- Consequences and trade-offs: Each platform writes a small adapter, and the callback contract,
  including short reads and errors, must be precise (Q3).

## D5 — Streaming calls do bounded work

- Context: Small devices often run one cooperative loop with no RTOS, servicing audio, input and
  displays between other work, so a call that decompresses a whole chapter can starve the loop. In
  Tavernkeep, reading while audio plays must stay possible.
- Decision: Every streaming call does a bounded amount of work, keeps its progress in caller-owned
  state, and returns. Each header states which calls are bounded. Run-to-completion convenience calls
  built on the streaming calls may be offered as documented exceptions (Q5).
- Rationale: Callers can interleave Bartap with real-time work without threads.
- Alternatives considered: Blocking calls only; callback-driven streaming, where the library decides
  when to stop.
- Consequences and trade-offs: Every loop in the library needs a resume point, which is hardest in the
  inflater, and the tests must cover resuming at every point (one-byte input and output buffers, for
  example). This rules out a `bartap_zip_entry_read` that runs until its buffer is full (Q6), and a
  `bartap_zip_open` that scans to completion other than as a convenience call built on bounded steps
  (Q7). `bartap_zip_find`, as drafted, is such a convenience call (Q8).

## D6 — Every input file is untrusted

- Context: EPUB files come from the internet and may be corrupt or crafted.
- Decision: An attacker controls every byte of the file. Bartap guarantees:
  - no out-of-bounds reads or writes;
  - no unbounded loops or work;
  - output capped by limits the caller sets, so decompression bombs and endless streams fail cleanly;
  - errors as result codes, never undefined behaviour or aborts.

  Sizes and offsets in the file are checked, never trusted.
- Rationale: A reader parses files its user did not create, on devices with no memory protection.
- Alternatives considered: Trusting well-formed files and checking only for obvious errors.
- Consequences and trade-offs: Every offset and length is checked against the data present, which costs
  code and some speed. A few valid but extreme files may be refused.

## D7 — Portability targets and C conventions

- Context: Bartap is developed on the RP2350, which has Arm Cortex-M33 and RISC-V (Hazard3) cores, and
  is meant for other small devices too.
- Decision:
  - Targets: Arm Cortex-M is the planned target, and RISC-V a possible addition. 32- and 64-bit x86
    builds under gcc and clang are the host test rig, not targets.
  - Big-endian support should need only an extra test configuration (under QEMU, for example), not code
    changes.
  - Language: ISO C11 without extensions (this settles Q16). Only `<stdint.h>`, `<stddef.h>`,
    `<stdbool.h>` and `<string.h>` (for memcpy, memset and memcmp).
  - Byte data is `uint8_t`, never plain `char`, whose signedness C leaves to the implementation (it is
    signed on x86 and unsigned on Arm and RISC-V).
  - Values in files are decoded byte by byte, with no assumptions about byte order or alignment.
- Rationale: Platform independence becomes a property of the code rather than of a list of tested
  platforms.
- Alternatives considered: C99, which reaches some older vendor toolchains but loses `static_assert` and
  `_Alignas` (Q16); allowing selected libc functions.
- Consequences and trade-offs: The include check enforces the header rule on the sources. The symbol
  check enforces the no-state and allowed-calls rules on the host build and on the firmware's Arm build.
  x86 tolerates misaligned access, so the host tests use UBSan's alignment check.

## D8 — Robustness backed by evidence

- Context: A device that parses files from the internet needs robustness that can be checked, not just
  claimed.
- Decision: Bartap's robustness is to be backed by:
  - coverage-guided fuzzing (libFuzzer or AFL++) under AddressSanitizer and UBSan, one harness per entry
    point, with the corpus committed and every crash kept as a regression test, once there is code to
    fuzz;
  - differential tests: the inflater against zlib, and zip handling against archives written by
    Python's zipfile, both of which the fixture generator already prepares;
  - the compiler matrix (gcc and clang, 64- and 32-bit) and the Arm symbol check, which already run.
- Rationale: These find the bugs that matter for untrusted input, including wrong results that do not
  crash.
- Alternatives considered: Unit tests alone.
- Consequences and trade-offs: More test infrastructure to maintain.
