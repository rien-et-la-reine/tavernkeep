<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Design questions

These are open questions about Bartap's draft interfaces. Most were forced by making the drafts
portable; a few were found along the way. None is decided here. The drafts in `include/bartap/` are
kept as close as possible to their earlier form, and they refer to these questions by number (Q1,
Q2, ...).

Each question gives the draft as it stands, then the options with their trade-offs.

---

## Q1. Who owns the inflate window and decode tables, and how they stay in fast memory

**Draft:** the caller supplies the 32 KiB window as a pointer to `bartap_inflate_init`. The decode
tables are to live inside `bartap_inflate_t`, which the caller allocates, so the caller already
chooses their memory, but only as part of the whole state. Output goes to whatever buffer each
`bartap_inflate_run` call is given.

**Why it matters:** the window is read at random on every match copy, and the tables on every symbol.
Both want the fastest memory there is (on-chip SRAM, a tightly coupled bank). Bulk output is written
once and read later, so it can go to slower memory (PSRAM over QSPI). A device may have only a few tens
of KiB of fast memory to spare.

**Options:**
- **A. As drafted:** window by pointer, tables embedded in the state. Simple, and the window and the
  rest of the state can be placed separately. But the state struct becomes large (from about 1 KiB with
  compact canonical-Huffman tables to several KiB with fast lookup tables), and all of it has to sit
  wherever the caller put the struct.
- **B. Tables in a separate caller-supplied workspace** (pointer and size), keeping the state struct
  small. The window, the tables and the small state can each go in a different memory. Costs one more
  pointer, a size to validate, and a size to communicate (Q2).
- **C. One combined workspace** (window and tables) in a single caller buffer. The simplest signature,
  but the window and tables can't be split between memories.
- **D. Output as the window:** when the caller decodes a whole entry into one contiguous buffer (Q5),
  the output already holds the history, so no separate window is needed, saving 32 KiB. This only works
  for whole-entry output, and that buffer must then be fast enough to be read at random.
- **Orthogonal:** how big the tables are (compact and slower versus lookup tables and faster) sets how
  much fast memory is needed. It could be fixed, or chosen at build time (Q11).

Also to state, whichever option is chosen: whether one window may be reused by successive streams
(yes, if they never overlap in time), and that two live streams must never share one.

---

## Q2. How working-memory requirements are sized and communicated

**Draft:** every state struct (`bartap_inflate_t`, `bartap_zip_stream_t`, ...) is fully declared in the
public headers, marked private, so `sizeof` gives its size and the caller can allocate it statically.
The window's size is a macro.

**Options:**
- **A. Visible structs (as drafted).** Static allocation is trivial and checked by the compiler. But
  every internal field is part of the API: any change to the state changes the ABI, and a caller can
  touch fields it shouldn't.
- **B. Opaque storage of a published size:** for example
  `typedef struct { uint32_t opaque[BARTAP_INFLATE_STATE_WORDS]; } bartap_inflate_t;`, with a
  `static_assert` in the library that the real state fits. Layout is hidden, allocation is still
  static, and the element type gives the alignment. But the library needs a cast or union internally,
  done with care for strict aliasing, and the size macro must be updated by hand.
- **C. Run-time query:** `size_t bartap_inflate_state_size(void)`, with the caller passing in raw
  memory. This suits a precompiled binary library, but static sizing becomes impossible, which is the
  usual way on small devices.
- **D. Macros computed from build configuration** (window bits, table kind; Q11), used with A or B.

**Also part of "working memory": stack.** Callers on small devices need each call's worst-case stack
depth (tavernkeep's main stack is 4 KiB). The options are to publish a measured figure per call (from
`-fstack-usage` in CI), to promise a ceiling and test against it, or to say nothing. Whatever Q7 and Q8
decide about scratch buffers feeds into this directly.

---

## Q3. Callback signatures for I/O, and how read errors propagate

**Draft:** `bartap_byte_source_read_fn(void *context, uint32_t offset, void *buffer, size_t size)`
returns `OK`, `INVALID_ARGUMENT` or `IO_ERROR`. Bartap clips each request to the source's size, which
is fixed in the `bartap_byte_source_t`. The callback must deliver every byte asked for or fail. An
`IO_ERROR` becomes `BARTAP_ZIP_RESULT_IO_ERROR` at the zip level and the platform's own error is lost.

**Open points and options:**
- **Short reads:** all-or-nothing (as drafted; simplest for Bartap) versus a `size_t *bytes_read` out
  parameter, which is easier for platforms whose reads naturally come back short, at the cost of a
  loop in every Bartap read site.
- **Platform error detail:** drop it (as drafted), or let the callback hand back a platform code (an
  `int32_t`) that Bartap carries out unchanged (ties to Q4).
- **Non-blocking I/O:** a "not ready, call again" result would let a DMA- or interrupt-driven storage
  driver be used without blocking the cooperative loop, which is the bounded-work rule taken all the
  way down to I/O. The cost: every Bartap call that reads through a source must then be resumable at
  any read. That is a large change to `bartap_zip_open`, `bartap_zip_find` and `bartap_zip_entry_read`.
- **Offset width:** `uint32_t` (as drafted) versus `uint64_t` (see Q13).
- **Reentrancy:** what a callback may do. At least: it must not call back into the Bartap object that
  called it. And when two streams on one `bartap_zip_t` run on different cores or threads, the same
  callback is called concurrently, so either the callback must be safe for that or the caller must
  never do it. This needs stating.
- **Operations table versus a single function pointer:** a single pointer, as drafted, until a second
  operation appears (a size query, an async completion).

---

## Q4. How errors carry detail beyond a result code

**Draft:** one result enum per module. Nothing records *which* check failed (bad CRC, local header
mismatch, end record not found, invalid distance...), *where* (offset, entry index), or the platform's
error (Q3).

**Options:**
- **A. Result codes only** (as drafted). The smallest and simplest, and enough to decide what to do, but
  poor for diagnosing a book that won't open.
- **B. Finer-grained result codes:** for example `CORRUPT_CRC` or `CORRUPT_LOCAL_HEADER`. Still no
  offsets. Callers that only care about the category need a helper or ranges.
- **C. An optional caller-supplied detail struct**, for example
  `bartap_error_t { code, reason, offset, entry_index, platform_code }`, passed per call or registered
  on the object. It costs nothing when `NULL`, and a few stores when present.
- **D. "Last error" fields inside each state struct.** No signature changes, but every state grows.
- **Not an option under the rules:** strings in the library (they cost read-only data, and a log
  belongs to the platform). A separate optional `bartap_result_name()` could be offered if the space is
  acceptable.

Also to decide: whether numeric values are stable (append-only, as tavernkeep's filesystem result
enum is), since logs and bug reports quote them.

---

## Q5. A run-to-completion convenience call that inflates a whole entry

**Proposed shape:** for example
`bartap_zip_entry_extract(zip, entry, window, out, out_capacity, max_size, &out_length)`, built only on
the streaming API, enforcing in one combined result:
1. the declared uncompressed size, capped at `max_size` (the file is untrusted), fits `out_capacity`,
   checked before any decoding;
2. output never exceeds the buffer, whatever the stream does;
3. the final length equals the declared size;
4. the CRC-32 matches.

**Options:**
- **A. No convenience call.** The smallest API, but every caller re-implements four checks that are easy
  to get subtly wrong.
- **B. Provide it, as a call that runs to completion.** It is honest about not meeting the bounded-work
  rule, and documented as such: for small entries (`container.xml`, the OPF) when the caller can afford
  the time.
- **C. Provide it as begin/step calls with a per-step budget**, returning "in progress" until done.
  This keeps the bounded-work rule, at the cost of a little more API.
- **D. B or C with output as the window** (Q1 D), saving 32 KiB. But then it is no longer purely built
  on the streaming API.

**Smaller decisions within it:** whether the cap is a parameter or a build-time constant; which result
says "declared size too large" (a new `TOO_LARGE`, or `UNSUPPORTED`); what the buffer holds after a
failure (unspecified, or zeroed, which costs a pass over it); and whether stored entries go through the
same call (presumably yes).

---

## Q6. Bounded work in `bartap_zip_entry_read`

**Draft:** "fewer than size bytes only at the end of the entry". Each call must keep reading until it
has produced `size` bytes. A deflated entry can consume input without producing output (runs of empty
stored blocks, long dynamic headers) for as long as its compressed size allows, which a hostile file
sets.

**Options:**
- **A. Keep the contract** and document the call as bounded by the entry's compressed size, not by
  anything the caller chooses. Simplest for callers; breaks the rule on hostile input.
- **B. A per-call budget** (a number of source reads or input bytes) after which the call returns OK
  with fewer bytes, possibly none. End of entry is then reported separately (a `bool *end`, as
  `bartap_zip_next` does) rather than by a short read.
- **C. As B, with the budget a parameter.** The caller tunes it to its deadline, at the cost of one more
  argument.
- **D. As B, with a fixed internal budget** (a build-time constant; Q11).

---

## Q7. `bartap_zip_open`'s end-record scan: run to completion, and where its buffer lives

**Draft:** scans backwards over up to 22 + 65535 bytes, a sector at a time, inside one call. No memory
for the sector buffer is named.

**Options for the buffer:** on the stack (512 B, an eighth of a 4 KiB stack); inside `bartap_zip_t`
(512 B kept for the life of the archive for a one-off use); a caller-supplied scratch argument (the
most explicit); or borrowing a stream's input buffer (couples opening to having a stream).

**Options for bounded work:** keep it running to completion (as drafted); split it into begin/step
calls; or a fast path that checks the last 22 bytes first, which is bounded for the overwhelmingly
common case of an archive with no comment, with the full scan as a fallback (still unbounded in the
worst case).

---

## Q8. `bartap_zip_find`: run to completion, and the memory for comparing names

**Draft:** walks the directory until it finds the name, in one call, so its work grows with the number
of entries. It compares the whole name, even past `BARTAP_ZIP_NAME_MAX`, so it needs to read names in
pieces into some buffer.

**Options:** keep it, documented as running to completion (as drafted); a cursor-based step version
returning "in progress"; or drop it in favour of `bartap_zip_next` plus a caller-side lookup or cache,
which the package layer will want anyway for the spine. A caller-built index (hashes of names, in
caller memory) is a possible later addition. For the comparison buffer, the same choices as Q7.

---

## Q9. The entry stream's input buffer

**Draft:** `bartap_zip_stream_t` embeds a fixed 512-byte input buffer (`BARTAP_ZIP_STREAM_INPUT_SIZE`),
sized as one SD sector for the device this was first written for.

**Options:**
- keep it fixed;
- make the size a build-time setting (Q11);
- have the caller supply buffer and size at `bartap_zip_entry_open`. The caller can then put it in
  DMA-capable memory (useful if the byte source fills it by DMA), size it to its storage, or share it
  between streams that never run at once.

Larger buffers mean fewer callback calls but more memory per open stream.

---

## Q10. `BARTAP_ZIP_NAME_MAX` and names stored in entries

**Draft:** 255, the longest name FAT and most filesystems allow. Each `bartap_zip_entry_t` embeds a
256-byte name, so an entry is about 290 bytes. A package layer caching the spine's entries would
spend about 29 KB on 100 of them.

**Options:**
- keep it fixed;
- make it a build-time setting (Q11);
- have the caller supply a name buffer (pointer and size) per call;
- store only the name's offset and length in the entry, with a call to read the name when wanted,
  which makes entries small enough to cache many.

Whatever limit is chosen, an entry whose name is longer than it must be rejected or skipped, never
truncated: a truncated name can match a different entry's name, and the caller would then read the
wrong entry.

---

## Q11. Build-time configuration

**Draft:** there is none. Every constant is fixed in the headers.

**Options:**
- `-D` macros with `#ifndef` defaults in the headers;
- a user-supplied configuration header (`BARTAP_CONFIG_FILE`, included if defined);
- CMake options that become compile definitions.

**The trade-off is flexibility against mismatch.** Any setting that changes a struct's layout (Q1, Q9,
Q10) must be identical in the library and every file that includes its headers, or memory is silently
corrupted. Settings would then have to be `PUBLIC` compile definitions. A link-time guard (a symbol
whose name encodes the configuration) could catch a mismatch.

---

## Q12. CRC-32

**Draft:** computed privately in `zip.c` with a 1 KiB read-only table.

**Options:**
- 1 KiB table (fastest in software);
- 16-entry nibble table (64 B, about 4 times slower);
- bitwise, no table (slowest);
- a caller-supplied CRC function, which lets a device use hardware: the RP2350's DMA sniffer computes
  CRC-32, for one.

Where a table lives also matters. In execute-in-place flash it may miss the cache; a caller-supplied
table pointer would let the caller place it, but that is memory placement again (Q1). Separately:
whether `bartap_crc32_update` is public, which would be useful to callers and to tests.

---

## Q13. 32-bit offsets and no ZIP64

**Draft:** offsets and sizes are `uint32_t`; ZIP64 is refused as unsupported. This fit the first
platform (FAT, files under 4 GiB) and keeps structs and arithmetic small on 32-bit cores.

**Options:** keep it (EPUBs are far below 4 GiB, and EPUB does not need ZIP64); move to `uint64_t`
offsets in the byte source and entries (costs 64-bit arithmetic and larger structs on 32-bit cores);
or add ZIP64 later behind a build-time setting (Q11). Note that some archivers write ZIP64 records even
for small files when streaming; how common that is in real EPUBs is worth checking before deciding.

---

## Q14. Where the OCF rules are checked

**Draft:** `zip.h` knows nothing of EPUB. The OCF rules (mimetype first, stored, no extra field,
`META-INF/container.xml`) are "checked a layer above", which, now that Bartap's scope includes the
container and package, means somewhere in Bartap.

**Options:** a new `bartap/ocf.h` (container: mimetype, `container.xml`, rootfile) over `zip.h`, with
`bartap/package.h` (OPF) over that; or OCF checks folded into the zip reader (simpler, and less
general); or left entirely to the caller. **Strictness is a separate decision:** real-world EPUBs break
these rules (a deflated mimetype, an extra field, mimetype not first). Reject, accept and report a
warning, or make it a caller policy? The fixtures already include `mimetype_not_first`.

---

## Q15. Entry-name encoding

**Draft:** names are compared byte for byte and treated as UTF-8, as EPUB requires.

**Open:** in zip, a name with general-purpose flag bit 11 clear is CP437, not UTF-8. EPUBs often leave
the bit clear while using UTF-8 anyway. The options are to treat every name as UTF-8 (common practice),
to validate UTF-8 and reject or flag invalid names, or to report the flag and let the caller decide.
Related, for the package layer: OPF `href`s are URLs, so they must be percent-decoded (and possibly
Unicode-normalised) before they can match a byte-exact zip name.

---

## Q16. Minimum C standard

**Draft:** C11 without extensions (`C_STANDARD 11`, `C_EXTENSIONS OFF`), as the rest of tavernkeep.
Nothing C11-specific is used yet: `//` comments, `stdbool.h` and `stdint.h` are all C99.

**Options:** stay at C11, which allows `static_assert` (useful for Q2 B) and `_Alignas`; or require
only C99, which reaches some older vendor toolchains for small devices, at the cost of those two
features (workarounds exist).

Tavernkeep builds as C11 (gnu11); the library's standard is a deliberate choice for portability.
