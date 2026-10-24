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
  including short reads and errors, must be precise (D14).

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
  example). This rules out a `bartap_ocf_entry_read` that runs until its buffer is full (D12), and a
  `bartap_ocf_open` that scans to completion other than as a convenience call built on bounded steps
  (D15). `bartap_ocf_find`, as drafted, is such a convenience call (Q8).

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
  - Bytes read from files are `uint8_t`, never plain `char`, whose signedness C leaves to the
    implementation (it is signed on x86 and unsigned on Arm and RISC-V): Bartap's own code inspects them
    (UTF-8 validation, path parsing), where that signedness matters. Strings a caller passes in only to
    be matched are `const char *`, so callers can pass string literals without casts; Bartap compares
    them with memcmp, which compares as unsigned char, so their signedness never matters.
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

## D9 — A book-shaped public API over internal container and DEFLATE layers

- Context: Bartap has three layers: the zip container, DEFLATE, and the book (package, spine,
  navigation, CFI). The first drafts made the zip reader and the inflater public headers. Bartap is
  meant to move to its own repository, where whatever is public becomes a contract.
- Decision: The public API is book-shaped. `include/bartap/` holds only public headers: `epub.h`, the
  book-level API (its shape is Q18), and `byte_source.h`. The OCF container layer (`src/ocf.h`) and the
  DEFLATE decoder (`src/inflate.h`) are internal: private to the library and its host tests. Internal
  symbols keep the `bartap_` prefix, because a static library's internal symbols are still global.
- Rationale: A small public surface is easier to keep stable when Bartap is split out, and internal
  layers can change freely.
- Alternatives considered: Publishing every layer, as the first drafts did.
- Consequences and trade-offs: Callers cannot use the zip or DEFLATE layers directly; `inflate.h` has
  nothing EPUB-specific in it and could be made public later if other users want raw DEFLATE. A public
  struct that embeds internal state still has to make its size known (Q2). The include check enforces
  that no public header includes an internal one.

## D10 — The OCF layer is EPUB-specific and enforces the OCF rules

- Context: An EPUB's container is a zip archive with rules of its own, the OCF's. Q14 asked where those
  rules are checked: in a layer over a general zip reader, inside the zip reader, or by the caller.
- Decision: This settles Q14. The internal container layer (`src/ocf.h`) is not a general zip reader:
  it is EPUB-specific and enforces the OCF rules itself:
  - only the stored and deflated methods;
  - no zip encryption;
  - entry names compared as raw bytes, case-sensitively, with UTF-8 checked only where the archive
    flags it (D20);
  - mimetype at most once and `META-INF/container.xml` exactly once (D21).

  The packaging rules for mimetype (present, the first entry, stored, no extra field, the content
  `application/epub+zip`) are checked in this layer too, and applied leniently (D19).
- Rationale: Bartap reads EPUBs and nothing else, so a general zip reader would be generality no
  caller uses, and an OCF layer over it would be a second set of headers, states and results to keep
  in step for the same archive. Checking the rules where the archive is parsed checks each one once,
  with the entry's method, flags and position already in hand, and enforces the safety rules among
  them (methods, encryption) before any caller sees an entry, as D6 requires.
- Alternatives considered: A general zip reader with the OCF rules checked in a layer over it (Q14's
  first option); leaving the rules to the caller.
- Consequences and trade-offs: The container layer cannot be reused as-is for archives that are not
  EPUBs. Where `META-INF/container.xml` and the rootfile are read, in this layer or the book layer, is
  open in Q18.

## D11 — Inflate memory: a caller-supplied window, decode tables inside the state

- Context: Q1 asked who owns the inflate window and decode tables. The 32 KiB window is read at random
  on every match copy and the tables on every symbol, so both want fast memory, of which a device may
  have only a few tens of KiB to spare.
- Decision: This settles Q1. The window is caller-supplied: `bartap_inflate_init` takes the window and
  its size, and refuses a window smaller than `BARTAP_INFLATE_WINDOW_SIZE` with
  `BARTAP_INFLATE_RESULT_INVALID_ARGUMENT`. The decode tables live inside the state struct, and their
  size is not part of the contract.
- Rationale: A state struct containing the 32 KiB window is easy to put on a small stack by mistake. A
  caller-supplied window can live in a memory region the caller chooses: fast SRAM, or shared across
  modes that never run together. Keeping the tables internal leaves the decoder's table design free to
  change.
- Alternatives considered: The window inside the state struct; the tables in a separate caller-supplied
  workspace (Q1 B); the window and tables in one combined caller buffer (Q1 C); the caller's output
  buffer as the window when a whole entry is decoded at once (Q1 D, which Q5 keeps open for a
  whole-entry call).
- Consequences and trade-offs: The tables sit wherever the caller puts the state, so a caller wanting
  them in fast memory places the whole state there, and how large they are, compact or fast lookup
  tables, stays an implementation matter (Q11 if it ever becomes a setting). A window serves one live
  stream at a time, and may be reused once that stream has finished or been abandoned. Init always
  writes the state's status, so a failed init leaves a state whose every later run returns
  `BARTAP_INFLATE_RESULT_INVALID_ARGUMENT`, as sticky as the other final results.

## D12 — Bounded work in `bartap_ocf_entry_read`: one read per call

- Context: D5 requires `bartap_ocf_entry_read` to be bounded. Without a bound, a deflated entry can
  consume input without producing output (runs of empty stored blocks, long dynamic headers) for as
  long as its compressed size allows, which a hostile file sets. Q6 asked what sets the bound.
- Decision: This settles Q6. Each call makes at most one byte-source read (D14) - for a deflated
  entry a refill of the stream's input buffer, up to its size; for a stored entry a read straight into
  the caller's buffer - then decodes from what it has into the caller's buffer. Work per call is
  bounded by the input buffer's size plus the caller's output size, the same model as
  `bartap_inflate_run`, with no budget parameter. A call may return OK with
  zero bytes, after a run of empty stored blocks for instance; `*end` reports the end of the entry.
- Rationale: Consistent with inflate; no extra parameter; the caller controls the bound through its
  buffer sizes.
- Alternatives considered: A budget passed per call (Q6 A); a fixed internal budget set at build time
  (Q6 B).
- Consequences and trade-offs: The caller supplies the input buffer (D16), so both sides of the bound
  are the caller's to choose. A caller that stops on a zero-byte read stops too early; it must loop
  until `*end`.

## D13 — ZIP64 read where it fits in 32 bits; `bartap_offset_t`

- Context: Q13 asked whether offsets stay 32-bit and ZIP64 stays refused. Some archivers write ZIP64
  records even for small files, so refusing ZIP64 outright refuses valid EPUBs; but no EPUB needs a
  value over 4 GiB, and 64-bit arithmetic is costly on 32-bit cores.
- Decision: This settles Q13. The OCF layer reads ZIP64 records - the ZIP64 end of central directory
  record and locator, and the ZIP64 extra field of a directory entry - whose values fit in 32 bits.
  Each 8-byte field is decoded as two 32-bit halves, and a value whose upper half is not zero is
  `UNSUPPORTED`. No 64-bit arithmetic is used. A `bartap_offset_t` typedef (`uint32_t`, in
  `byte_source.h`) is used for every position and size in a source or archive; lengths of memory
  buffers stay `size_t`, and counts and indexes are `uint32_t`.
- Rationale: ZIP64 is supported where EPUBs actually use it, at no cost in arithmetic or struct size.
  One type for file positions makes the 32-bit limit visible in every signature, and a later change of
  width a change in one place.
- Alternatives considered: Refusing ZIP64 (the earlier draft); `uint64_t` offsets throughout; ZIP64
  behind a build-time setting (Q11).
- Consequences and trade-offs: An archive with a real value over 4 GiB is refused, which no EPUB
  needs. Wherever a `size_t` memory length meets a file position, the value is checked to fit before
  it is converted, so a wide `size_t` never truncates. Entry counts and indexes become 32-bit, since
  ZIP64 allows more than 65535 entries.

## D14 — Byte-source reads block, all or nothing; one read per call, made first

- Context: Q3 asked how the read callback behaves - short reads, errors, non-blocking I/O,
  reentrancy.
- Decision: This settles Q3. A read blocks, and delivers everything asked for or fails. Every Bartap
  call - every step, for the calls built on steps - makes at most one read, and makes it before
  changing any state. Bartap keeps no shared state, so using one source from several cores at once is
  the caller's responsibility. Platform error detail stays in the callback's context; Bartap reports
  only `IO_ERROR`.
- Rationale: All-or-nothing reads keep a loop out of every read site. One read per call, made before
  any state changes, means a call that stopped at its read can be repeated unchanged, so a "not ready,
  call again" result can be added later, for DMA- or interrupt-driven storage, without redesigning the
  callers. The platform's error belongs to the platform, which already holds the context.
- Alternatives considered: Short reads with a `bytes_read` out parameter; a non-blocking result now; a
  platform error code carried through Bartap's results (Q3's options).
- Consequences and trade-offs: Calls that need several reads become steppers: opening (D15), and
  `bartap_ocf_next` for an entry with a ZIP64 extra field, which returns OK with its `ready` flag unset
  until the entry is complete. Convenience calls that loop steps (`bartap_ocf_open`, `bartap_ocf_find`)
  make several reads, as D5 allows.

## D15 — Opening in steps, with a caller-supplied scratch buffer

- Context: Q7: finding the end record may scan up to 22 + 65535 bytes, which D5 rules out in one call,
  and the scan's buffer had no named memory.
- Decision: This settles Q7. Opening is `bartap_ocf_open_begin` and `bartap_ocf_open_step`. The first
  step reads the last 22 bytes, for an end record with no comment. Later steps scan backwards one chunk
  per call, chunks overlapping by 3 bytes so that a 4-byte signature always lies in one chunk; a
  signature found is confirmed by reading its record in the next step. Then come the ZIP64 records, if
  any, and a walk over the directory (D19, D20, D21). The scratch buffer is caller-supplied (pointer and
  size, at least `BARTAP_OCF_OPEN_SCRATCH_MIN`) and needed only until open finishes.
  `bartap_ocf_open` remains as a convenience call that loops the steps.
- Rationale: The common case, an archive with no comment, opens in one read; a long comment or a
  hostile file costs steps, never one long call. A caller-supplied scratch buffer keeps a sector off a
  small stack and lets the caller size the chunks to its storage.
- Alternatives considered: The scratch buffer on the stack, inside `bartap_ocf_t`, or borrowed from a
  stream (Q7's options); open as a call that only runs to completion.
- Consequences and trade-offs: Opening's step count grows with the comment scanned and the entries
  walked. `bartap_ocf_t` holds the steps' progress, unused once open has finished. When open finishes,
  on success or failure, the scratch pointer and size in `bartap_ocf_t` are cleared to NULL and 0, so
  the caller can reuse that memory at once (as an inflate window, say) with no stale pointer left
  behind.

## D16 — The entry stream's input buffer is caller-supplied

- Context: Q9: the stream embedded a fixed 512-byte input buffer.
- Decision: This settles Q9. `bartap_ocf_entry_open` takes the input buffer (pointer and size). It may
  be NULL for a stored entry, whose data is read straight into the caller's buffer.
- Rationale: The caller can put it in DMA-capable memory, size it to its storage, or share it between
  streams that never run at once; and with D12 the caller then sets both sides of a read's bound.
- Alternatives considered: A fixed size; a build-time setting (Q9's options).
- Consequences and trade-offs: `BARTAP_OCF_STREAM_INPUT_SIZE` is gone, and the stream struct is
  smaller. One more pointer and size at `entry_open`.

## D17 — Entries hold their name's location, not the name

- Context: Q10: each entry embedded a 256-byte name, so an entry was about 290 bytes and a cached
  spine of 100 entries about 29 KB, and names over 255 bytes needed rejecting or skipping.
- Decision: This settles Q10. An entry stores its name's offset in the central directory and its
  length, not the name. `bartap_ocf_find` compares lengths first and reads only names whose length
  matches, a piece at a time, from the source. `bartap_ocf_entry_name` copies a name into a caller
  buffer on demand. There is no name-length limit; `BARTAP_OCF_NAME_MAX` and the name array are gone.
  A local header's name is checked against the directory's by length only, with its signature and
  method, not byte for byte.
- Rationale: Entries become small enough to cache many, and no name is ever truncated or refused for
  its length. The central directory is the authority (D10), so comparing the local header's name bytes
  too would add reads for no safety.
- Alternatives considered: A fixed name limit; a build-time limit; a caller-supplied name buffer per
  call (Q10's options).
- Consequences and trade-offs: Reading a name costs a read; `find` reads only the names it must. A
  local header whose name differs from the directory's in its bytes but not its length is not
  noticed, and the entry reads normally.

## D18 — Result codes: explicit values, append-only, codes only; LIMIT_EXCEEDED

- Context: Q4 asked how errors carry detail, and whether numeric values are stable. The size limit at
  `entry_open` needed a result of its own (a smaller decision within Q5).
- Decision: This settles Q4. Every result enum gives each code an explicit value, and from now on the
  lists are append-only: never renumbered, never reused. Results are codes only, with no error-detail
  records. Each module keeps its own result list, and a code that means the same thing in several has
  the same name in each: `LIMIT_EXCEEDED`, for any limit the caller sets - `entry_open`'s `max_size`
  now, a buffer too small for what it must hold, the XML layer's limits later. An entry that
  decompresses to more than its own declared size stays `CORRUPT`. Warning bits (D19) are append-only
  too.
- Rationale: Logs and bug reports quote numbers, which must keep their meaning. Codes are enough to
  decide what to do; a reason string or detail struct costs memory and API on every call for a
  diagnosis the platform can make by other means.
- Alternatives considered: Finer-grained codes; an optional detail struct; "last error" fields in each
  state (Q4's options).
- Consequences and trade-offs: A book that will not open is reported only as a category. A limit the
  caller set is told apart from damage in the file.

## D19 — Lenient packaging, reported as warnings

- Context: Q19: real-world EPUBs often break the OCF's packaging rules for mimetype, and most reading
  systems open them anyway.
- Decision: This settles Q19. A mimetype that is missing, empty, compressed, not the first entry, or
  has an extra field in its local header opens anyway, and sets a bit in a warnings bitmask on
  `bartap_ocf_t`. A stored mimetype's content is accepted if it is `application/epub+zip`, optionally
  after a UTF-8 byte order mark and before ASCII spaces, tabs, CRs and LFs, none of which sets a
  warning. A mimetype that declares another format returns `NOT_AN_EPUB`. A compressed mimetype's
  content is not checked. The safety rules (offsets, sizes, methods, zip encryption) are always
  enforced (D6).
- Rationale: Books other readers open should open in Bartap; the warnings let a caller that cares
  warn, log or refuse, as its own policy. Refusing another declared format keeps Bartap from reading an
  ODF document or other zip-based format as if it were a book. Checking a compressed mimetype would
  need inflate, and a window, at open, for a rare case.
- Alternatives considered: Strict rejection; silent acceptance (Q19 A and B).
- Consequences and trade-offs: The fixtures' packaging cases open, with the warnings each should set.
  A compressed mimetype declaring another format is not noticed by this layer.

## D20 — Names compared as raw bytes; UTF-8 checked only where flagged

- Context: Q15: zip names with general-purpose flag bit 11 clear are CP437 by the format, but EPUBs
  often leave the bit clear while using UTF-8.
- Decision: This settles Q15. Names are compared as raw bytes. If an entry's flag bit 11 is set, its
  name is validated as UTF-8 at open, and an invalid one sets a warning bit rather than being refused.
  If it is clear, nothing is assumed or checked.
- Rationale: Byte comparison is what OCF's case-sensitive names need, and costs nothing; refusing a
  book for a badly flagged name helps no reader.
- Alternatives considered: Treating every name as UTF-8 and validating it; refusing invalid names
  (Q15's options).
- Consequences and trade-offs: Known limitation: an OPF href and a zip name that are the same text in
  different Unicode normalization forms (composed and decomposed) do not match, and the entry is not
  found. Percent-decoding hrefs before lookup is still the book layer's (Q18).

## D21 — Duplicate names

- Context: A hostile archive can hold two entries of one name, and zip readers that disagree on which
  one wins is a known way to show different content to different readers.
- Decision: Lookups return the first match in the directory, as documented in `ocf.h`. At open,
  mimetype must appear at most once (missing: a warning, D19; a duplicate: `CORRUPT`), and
  `META-INF/container.xml` exactly once (a duplicate: `CORRUPT`; missing: `NOT_AN_EPUB`). Whichever
  layer reads `container.xml` (Q18) must likewise require the OPF it names to appear exactly once.
- Rationale: The entries an EPUB is identified and read by cannot be ambiguous; for the rest, a fixed
  and documented rule is enough.
- Alternatives considered: Refusing any duplicate name, which costs a comparison of every pair of
  names; returning the last match.
- Consequences and trade-offs: The walk at open compares the names of the entries whose length
  matches those two. Duplicates of other entries are allowed, and only the first is reachable by name.

## D22 — Streams: sticky errors, reads after the end, sharing, stored sizes

- Context: What a stream does after an error or its end, whether streams may share an open archive,
  and how a stored entry's two sizes relate, were unstated.
- Decision: Every stream error is final for that stream, `IO_ERROR` included: recover by opening the
  entry again. A read after `*end` returns OK, zero bytes, with `*end` set. Any number of streams and
  cursors may share one `bartap_ocf_t`, which is read-only once open has finished. A stored entry whose
  compressed and uncompressed sizes differ is `CORRUPT` at `entry_open`; for a deflated entry, a
  compressed size larger than the uncompressed one is legal and not refused.
- Rationale: Sticky errors mean a caller cannot carry on past a failure by mistake, and reopening is
  cheap. A read-only archive needs no locking to share. Stored data is its own content, so its sizes
  must agree; DEFLATE can expand incompressible data slightly, so a deflated entry's must not be held
  to that.
- Alternatives considered: Retrying a read after `IO_ERROR` in place; an error for reads after the end.
- Consequences and trade-offs: A transient I/O error costs reopening the entry and reading it again
  from the start.

