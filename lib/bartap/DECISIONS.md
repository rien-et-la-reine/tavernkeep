<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Decisions

Decisions that shape Bartap. They were renumbered for the rewrite, which dropped the stepped calls, EPUB
CFI and snapshots; the decisions behind the previous implementation (D1-D22) and its open questions are in
this branch's history.

## D1 — A dependency-free C library for EPUB on small devices

- Context: A microcontroller reader that opens EPUB files directly needs zip, DEFLATE, XML and EPUB
  package handling in tight memory, with no operating system, no heap and no large dependencies.
- Decision: Bartap is a standalone, freestanding C library for reading EPUB files on microcontrollers,
  of about 2,500-3,000 lines.
- Rationale: Most small readers are written in C or C++, so a C library can be used by nearly all of
  them. Building Tavernkeep's EPUB handling as a library also forces cleaner boundaries than building it
  into the firmware. A size target keeps the library small enough to read in one sitting.
- Alternatives considered: Using or adapting an existing EPUB implementation; building EPUB handling
  into Tavernkeep.
- Consequences and trade-offs: More design and test effort up front than firmware-only code.

## D2 — Scope: container to text tokens, not layout

- Context: From the bottom up, a reader needs the zip container, DEFLATE, the OCF container
  (`META-INF/container.xml`), the OPF package (metadata, manifest, spine), navigation (the EPUB 3
  navigation document or the EPUB 2 NCX), the XHTML content documents, and layout and rendering.
- Decision: Bartap covers zip, inflate, OCF, OPF and spine, navigation, and XHTML read as a stream of
  words, spaces, line breaks, blocks and images with their inline styles. Layout, pagination, fonts,
  CSS and rendering are out of scope.
- Rationale: Everything up to the token stream is defined by specifications and the same for every
  reader. Layout depends on the display, the fonts and the reader's policy, so it belongs to the caller.
- Alternatives considered: Container and package only, leaving XHTML to the caller (the previous
  scope); a full reading engine with layout (too device-specific to reuse).
- Consequences and trade-offs: Callers write their own layout. Whitespace handling, including `<pre>`,
  is Bartap's, so every caller gets the same words.

## D3 — No dynamic allocation; the caller owns all memory

- Context: Target devices have a few hundred KiB of RAM, often in regions of different speed (on-chip
  SRAM, external PSRAM), and many have no heap.
- Decision: Bartap never allocates and keeps no global or static mutable state. All state is in structs
  the caller owns, and the caller provides all working memory and output buffers.
- Rationale: Memory use is predictable, and the caller decides where each buffer lives: the inflate
  window in fast SRAM and an inflated chapter in PSRAM, for example. Independent instances can coexist.
- Alternatives considered: Caller-supplied allocator callbacks; an internal fixed pool.
- Consequences and trade-offs: Calls take more parameters than an allocating library's would.

## D4 — All I/O through one read callback

- Context: Books may come from a FAT filesystem, raw flash, memory, or anything else a device has.
- Decision: Bartap makes no file or OS calls. It reads through a `bartap_source_t`: a read callback
  (offset, buffer, length; 0 on success), a user pointer and the archive's size. A read blocks and
  delivers everything asked for or fails; Bartap never asks for bytes past the size. A memory source is
  included.
- Rationale: Platform independence, and one test path: host tests and devices run the same code over
  different sources. All-or-nothing reads keep a loop out of every read site, and the platform's error
  detail stays in its own context.
- Alternatives considered: Built-in POSIX or FatFs backends; short reads; non-blocking reads.
- Consequences and trade-offs: Each platform writes a small adapter. Every callback failure is
  `BARTAP_ERR_IO`; the cause is the adapter's to log.

## D5 — Synchronous calls; real-time work is the caller's

- Context: The previous design made every call a bounded step with at most one read, so a cooperative
  loop could keep audio fed while a book opened. That inverted control flow through every module and
  cost hundreds of lines of resumable state machines.
- Decision: Opening a book and finding things in it run to completion. Only resource bytes
  (`bartap_stream_read`) and text tokens (`bartap_text_next`) are streamed, one buffer or one token per
  call. A caller with real-time work on the same core does it inside its read callback, which Bartap
  calls often and from nowhere else.
- Rationale: On the dual-core parts Bartap is written for (RP2350, ESP32-S3), real-time audio belongs on
  the second core or behind DMA, not in cooperative yields on the core that parses books. The read
  callback is a hook for the single-core case with no extra API.
- Alternatives considered: Bounded steps everywhere (the previous design); a yield callback separate
  from reads.
- Consequences and trade-offs: `bartap_open` on a large book takes as long as it takes; a caller that
  must stay responsive runs it where a long call is acceptable. A whole-chapter walk is still the
  caller's loop of `bartap_text_next` calls.

## D6 — Every input file is untrusted

- Context: EPUB files come from the internet and may be corrupt or crafted.
- Decision: An attacker controls every byte of the file. Bartap guarantees:
  - no out-of-bounds reads or writes;
  - no unbounded loops: every loop is bounded by the input's size or a fixed limit;
  - fixed limits (`BARTAP_*_MAX`, the spine array's capacity) on everything kept, with longer values
    truncated and warned about rather than overflowed;
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
  - Targets: Arm Cortex-M is the planned target, Xtensa (ESP32-S3) and RISC-V possible additions. 32-
    and 64-bit x86 builds under gcc and clang are the host test rig, not targets.
  - Big-endian support should need only an extra test configuration, not code changes.
  - Language: ISO C11 without extensions. Only `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` and
    `<string.h>`.
  - Bytes read from files are `uint8_t`, never plain `char`. Strings a caller passes in only to be
    matched are `const char *`, compared with `memcmp`.
  - Values in files are decoded byte by byte, with no assumptions about byte order or alignment.
  - No object is accessed through an lvalue of another type (C11 6.5p7): state is declared by the
    caller as its real struct type (D9), never as a byte array cast to one.
- Rationale: Platform independence becomes a property of the code rather than of a list of tested
  platforms.
- Alternatives considered: C99; allowing selected libc functions.
- Consequences and trade-offs: The include check enforces the header rule on the sources. The symbol
  check enforces the no-state and allowed-calls rules on the host build and on the firmware's Arm build.

## D8 — Robustness backed by evidence

- Context: A device that parses files from the internet needs robustness that can be checked, not just
  claimed.
- Decision: Bartap's robustness is backed by coverage-guided fuzzing (libFuzzer) under AddressSanitizer
  and UBSan, one harness per layer, with the corpus committed and every crash kept as a regression
  test; differential tests of the inflater against zlib and of zip handling against Python's zipfile;
  and the compiler matrix and the Arm symbol check.
- Rationale: These find the bugs that matter for untrusted input, including wrong results that do not
  crash.
- Alternatives considered: Unit tests alone.
- Consequences and trade-offs: More test infrastructure to maintain.

## D9 — One public header, with every state struct's layout

- Context: The previous design published opaque byte arrays of a fixed size and cast them to internal
  structs. Accessing a declared `unsigned char` array through a struct lvalue is undefined behaviour
  (C11 6.5p7), whatever its alignment, and an optimiser that assumes the rule can miscompile it.
- Decision: `include/bartap.h` is the whole public API and defines every state struct
  (`bartap_book_t`, `bartap_stream_t`, `bartap_toc_t`, `bartap_text_t`, `bartap_work_t` and the inflate,
  zip and XML state they embed). Their fields are Bartap's: callers declare them and read nothing from
  them. `src/bartap_internal.h` holds only the interfaces between the modules.
- Rationale: The caller declares an object of the type Bartap uses, so there is nothing to alias and
  no size to keep in step by hand: `sizeof` is the size.
- Alternatives considered: Opaque byte arrays with build-time size checks (the previous design);
  incomplete types with a size query, which needs allocation or `alloca`.
- Consequences and trade-offs: Internal layouts are visible, and a change to one changes the ABI; a
  caller that touches a field is on its own. `BARTAP_INFLATE_FAST_LUT` changes layouts, so it is a CMake
  option that defines it for the library and its users alike.

## D10 — The OCF layer is EPUB-specific

- Context: An EPUB's container is a zip archive with rules of its own, the OCF's.
- Decision: The container layer (`src/ocf.c`) is not a general zip reader. It reads only the stored and
  deflated methods, refuses zip encryption, compares entry names as raw bytes, and finds entries by name
  or by the FNV-1a hash and length of their name. The end record is found by scanning back over the last
  min(size, 65535 + 22) bytes, so bytes after it are tolerated (`BARTAP_WARN_TRAILING_BYTES`).
- Rationale: Bartap reads EPUBs and nothing else. Hash matching lets the OPF pass find every spine
  item's entry without keeping path strings. Archives with transfer padding after the end record are
  common and otherwise valid.
- Alternatives considered: A general zip reader with the OCF rules in a layer over it.
- Consequences and trade-offs: A hash collision between two of one book's paths would misplace an item;
  the length check makes that rarer still.

## D11 — Inflate memory: one caller window, decode tables inside the state

- Context: The 32 KiB window is read at random on every match copy and the tables on every symbol, so
  both want fast memory.
- Decision: Every inflating call takes a caller-supplied `BARTAP_WINDOW_SIZE` window. The decode tables
  live inside `bartap_inflate_t`, with an optional 256-entry literal/length lookup table
  (`BARTAP_INFLATE_FAST_LUT`, 512 bytes).
- Rationale: A struct containing the 32 KiB window is easy to put on a small stack by mistake; a
  separate window can live where the caller chooses and be shared by objects that never run at once.
  The lookup table resolves most codes in one load, which matters on a Cortex-M33 and little on x86.
- Alternatives considered: The window inside the state; tables in a separate workspace.
- Consequences and trade-offs: A window serves one live stream at a time.

## D12 — One work buffer for opening

- Context: Opening reads container.xml and the OPF, which may be deflated, so it needs an inflate
  window, a stream, a tokenizer and path buffers, about 35 KiB. A 4 KiB stack cannot hold the
  non-window part safely.
- Decision: `bartap_open` takes one `bartap_work_t`, holding its parsing state and a window, used only
  during the call. Its `window` member can then serve any stream, TOC or text iterator.
- Rationale: One buffer to place, no stack use worth mentioning, and the window is not allocated twice.
- Alternatives considered: A window and a separate scratch buffer; a byte buffer of a published size,
  which would bring back D9's aliasing problem.
- Consequences and trade-offs: The work buffer cannot be reused while a book is opening.

## D13 — 32-bit offsets; ZIP64 read where it fits

- Context: Some archivers write ZIP64 records even for small files, but no EPUB needs a value over
  4 GiB, and 64-bit arithmetic is costly on 32-bit cores.
- Decision: Positions and sizes in an archive are `uint32_t`. ZIP64 records are read where their values
  fit in 32 bits; a larger value is `BARTAP_ERR_UNSUPPORTED`.
- Rationale: ZIP64 is supported where EPUBs use it, at no cost in arithmetic or struct size.
- Alternatives considered: Refusing ZIP64; 64-bit offsets.
- Consequences and trade-offs: An archive with a real value over 4 GiB is refused.

## D14 — The spine is a caller-allocated array of entry locations

- Context: A book's spine may have a handful of items or several hundred, and each is read many times.
- Decision: `bartap_open` fills a `bartap_spine_item_t` array the caller supplies, with its capacity,
  in reading order. Each item holds its entry's local header offset, sizes, method and flags (`linear`),
  so opening a chapter needs no central directory walk. More items than the capacity are dropped with
  `BARTAP_WARN_SPINE_TRUNCATED`.
- Rationale: The book struct stays a fixed size whatever the spine's, and the caller sizes the array
  to its memory and the books it expects.
- Alternatives considered: A fixed spine inside the book; looking items up again each time they open.
- Consequences and trade-offs: The array must outlive the book.

## D15 — Metadata is copied into the book at open

- Context: Title, author, language and the unique identifier are wanted for menus long after opening.
- Decision: `bartap_open` copies them into `bartap_book_t`, each up to `BARTAP_META_MAX` bytes, cut at a
  UTF-8 character boundary. The identifier is the one `<package unique-identifier>` names.
- Rationale: Reading them again would mean re-inflating the OPF; 4 x 128 bytes is cheap.
- Alternatives considered: Re-parsing the OPF on demand.
- Consequences and trade-offs: Longer values are truncated with `BARTAP_WARN_META_TRUNCATED`.

## D16 — Positions are a spine index and a byte offset

- Context: The previous design located positions as EPUB CFIs, which meant a DOM-indexed path counted
  in UTF-16 code units and a resolver to walk it.
- Decision: A position is a `bartap_loc_t`: the spine index and the byte offset of a token in that
  item's uncompressed XHTML. Seeking forward continues; seeking back re-reads the item from its start.
  A chapter already inflated in memory (`bartap_text_open_memory`) seeks without inflating.
- Rationale: Re-inflating a 100 KiB chapter takes tens of milliseconds, well under an e-ink refresh.
  A byte offset needs no parser, no resolver and no tree.
- Alternatives considered: EPUB CFI (the previous design); a token count.
- Consequences and trade-offs: A position is tied to the exact bytes of the book: if the file is
  replaced by another edition, saved positions may land in a different place. Positions cannot be
  exchanged with other reading systems. To guard against a replaced file, a position carries an anchor:
  the raw XHTML bytes at its offset, `BARTAP_ANCHOR_SIZE` (32) of them or fewer at an item's end.
  `bartap_text_loc` takes them from the tokenizer's lookahead, which it keeps at least that far ahead, so
  taking a position at the top of a page costs a copy and no extra read. Restoring a position is Bartap's: it compares the bytes at the
  offset with the anchor; on a mismatch it searches up to two anchor lengths either side and takes the
  match nearest the offset; with no match it restores the bare offset. The search window only recovers
  small shifts, such as an errata fix earlier in the chapter; anything larger falls back to the offset,
  which is no worse than having no anchor. `bartap_text_seek` reports which of the three happened.
  32 bytes is five or six words of prose: unique within the 160 bytes searched, short enough that an
  edit rarely lands inside it, and it makes a position 40 bytes. A longer anchor would widen the search
  but not reach the shifts a different edition brings, which are far larger.

## D17 — The text iterator pulls from its own source

- Context: Text can come from a deflated entry or from a chapter already inflated in memory.
- Decision: `bartap_text_t` reads its bytes itself, from an embedded stream or from memory, and returns
  one token per call. Whitespace is collapsed outside `<pre>` and kept inside it.
- Rationale: The caller's loop is one call per token, with no input buffers to feed, and seeking is the
  iterator's to do.
- Alternatives considered: A push-fed iterator, given bytes by the caller (the previous design).
- Consequences and trade-offs: `bartap_text_t` embeds a stream, about 1.3 KiB, even when reading from
  memory.

## D18 — Results: one readable enum

- Decision: Every call returns a `bartap_err_t`; `BARTAP_END` marks the end of a stream or iterator and
  is not an error. `bartap_strerror` names each code. Values are not promised stable before a release.
- Rationale: One list of codes is easier to handle and log than one per module.
- Alternatives considered: Per-module result lists with append-only values (the previous design).

## D19 — Lenient only, reported as warnings

- Context: Real-world EPUBs often break the OCF packaging rules and XML well-formedness, and most
  reading systems open them anyway.
- Decision: Bartap has one mode. Packaging faults (mimetype missing, compressed or not first), malformed
  markup and invalid UTF-8 are recovered from and reported as `BARTAP_WARN_*` bits. Invalid UTF-8 is
  read as Windows-1252, whose 0x80-0x9F range holds the curly quotes and dashes legacy books use. The
  safety rules (offsets, sizes, methods, encryption) are always enforced (D6).
- Rationale: Books other readers open should open in Bartap; the warnings let a caller log or refuse.
- Alternatives considered: A strict mode beside the lenient one (the previous design).

## D20 — Names compared as raw bytes

- Decision: Entry names are compared as raw bytes, case-sensitively, whatever their UTF-8 flag.
- Consequences and trade-offs: An OPF href and a zip name that are the same text in different Unicode
  normalization forms do not match. Percent-decoding hrefs before lookup is the book layer's.

## D21 — Duplicate names

- Decision: Lookups return the first match in the central directory.
- Rationale: A fixed, documented rule keeps readers from disagreeing about which entry is meant.

## D22 — Entries are checked against their CRC-32

- Context: DEFLATE detects some damage but not all, and a stored entry has no check of its own; the
  ZIP central directory gives every entry's CRC-32.
- Decision: Every entry read through a stream (chapters, container.xml, the OPF, the TOC, images) is
  checked against the CRC-32 in its central directory record, folded in as bytes are delivered. At the
  entry's end, a stream or text iterator returns `BARTAP_END` only if the CRC-32 and the size match, and
  `BARTAP_ERR_CORRUPT` instead if not. Rewinding or seeking backwards restarts the check; seeking
  forwards still passes every byte through it. Text opened from memory is not checked again.
- Rationale: A damaged card or a bad copy should not show as silently wrong text. Folding the CRC in as
  bytes pass costs no extra reads and no buffer. A mismatch is `BARTAP_ERR_CORRUPT` rather than a code of
  its own: once the check fails, nothing says the data decoded correctly either. The CRC uses the usual
  256-entry table (1 KiB of read-only data), one lookup per byte.
- Alternatives considered: No check (DEFLATE's own consistency only); checking an entry in full before
  delivering any of it, which needs a second pass or the whole entry in memory.
- Consequences and trade-offs: Bytes and tokens delivered before an entry's end are unverified until
  then, and a caller that stops reading early never learns the result. Tavernkeep accepts this: it
  inflates whole chapters into PSRAM, so each is checked before any of it is shown. A streaming-only
  caller sees damage only at a chapter's end.
