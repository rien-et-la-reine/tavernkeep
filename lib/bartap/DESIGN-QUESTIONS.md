<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Design questions

Open questions about Bartap's draft interfaces, numbered so that the headers in `include/bartap/` can
refer to them (Q1, Q2, ...). Each gives the draft as it stands, then the options and their trade-offs.
Settled questions move to [DECISIONS.md](DECISIONS.md), as a decision that names the question's number.

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
`-fstack-usage` in CI), to promise a ceiling and test against it, or to say nothing. Whatever Q7 decides
about its scratch buffer feeds into this directly.

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

**Smaller decisions within it:** which result says "declared size too large" (a new `TOO_LARGE`, or `UNSUPPORTED`); what the buffer holds after a
failure (unspecified, or zeroed, which costs a pass over it); and whether stored entries go through the
same call (presumably yes).

---

## Q6. Bounded work in `bartap_zip_entry_read`

**Draft:** D5 (DECISIONS.md) requires the call to be bounded. Without a budget, a deflated entry can
consume input without producing output (runs of empty stored blocks, long dynamic headers) for as long
as its compressed size allows, which a hostile file sets. So the call returns OK with fewer than `size`
bytes, possibly none, once its budget is spent, and reports the end of the entry separately (`*end`, as
`bartap_zip_next` does) rather than by a short read. What sets the budget is open.

**Options:**
- **A. A budget passed per call** (a number of source reads or input bytes). The caller tunes it to its
  deadline, at the cost of one more argument.
- **B. A fixed internal budget** (a build-time constant; Q11). No extra argument, but one setting for
  every caller.

---

## Q7. `bartap_zip_open`'s end-record scan: run to completion, and where its buffer lives

**Draft:** scans backwards over up to 22 + 65535 bytes, a sector at a time, inside one call. D5
(DECISIONS.md) allows that only for a convenience call built on bounded steps, and the steps are not
drafted yet. No memory for the sector buffer is named.

**Options for the buffer:** on the stack (512 B, an eighth of a 4 KiB stack); inside `bartap_zip_t`
(512 B kept for the life of the archive for a one-off use); a caller-supplied scratch argument (the
most explicit); or borrowing a stream's input buffer (couples opening to having a stream).

**Options for bounded work:** D5 rules out a call that only runs to completion, so the scan becomes
begin/step calls, with `bartap_zip_open` kept, if at all, as a convenience call built on them. A fast
path that checks the last 22 bytes first makes the overwhelmingly common case (an archive with no
comment) a single step, with the full scan still needed as the fallback.

---

## Q8. `bartap_zip_find`: run to completion, or in steps

**Draft:** a convenience call built on `bartap_zip_next`, which D5 (DECISIONS.md) allows: it walks the
directory until it finds the name, in one call, so its work grows with the number of entries. A name
longer than `BARTAP_ZIP_NAME_MAX` is never matched through a truncated copy; how such an entry is
handled is Q10.

**Options:** keep it as drafted; a cursor-based step version returning "in progress"; or drop it in
favour of `bartap_zip_next` plus a caller-side lookup or cache, which the package layer will want
anyway for the spine. A caller-built index (hashes of names, in caller memory) is a possible later
addition.

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

Settled: ISO C11 without extensions. See D7 in [DECISIONS.md](DECISIONS.md).

---

## Q17. Does Bartap turn XHTML content documents into a styled text stream?

**Draft:** no. Bartap's scope ends at delivering a content document's bytes (D2 in DECISIONS.md),
through `bartap_zip_entry_read`. Bartap will parse XML anyway, for `container.xml`, the OPF and the
navigation document or NCX.

**Options:**
- **A. Stop at document bytes.** The smallest scope, and it keeps Bartap to what specifications define
  (D2's rationale). But every caller writes its own XHTML parser, so the largest body of untrusted
  parsing in a reader sits outside Bartap's guarantees (D6). It also leaves CFI resolution (D2)
  stopping at the spine item: resolving the path inside a content document means walking its element
  structure.
- **B. A streaming XHTML tokenizer:** start tag, attribute, text and end tag events, with entities
  decoded, and no styling. Bartap needs a tokenizer for its own XML documents anyway, so exposing it
  costs little, keeps untrusted parsing inside Bartap's checks, and lets CFI resolution reach inside a
  document. Callers still resolve CSS and decide what each element means.
- **C. A styled text stream:** text runs carrying resolved style (emphasis, headings, paragraph and line
  breaks, alignment, a chosen subset of CSS), with images as references. The most useful output for a
  small reader. But it brings in the CSS cascade, which is policy: which properties, how much of the
  cascade, how user settings override the book. That is where D2 says readers differ. It also needs
  memory for stylesheets and the open element stack (D3), and bounded selector matching (D5).
- **D. C as a separate, optional layer over B** (a `bartap/text.h`, say). Callers that want bytes or
  tokens don't pay for it, and it can come later without changing the layers under it.

**Smaller decisions within it:** a limit on element nesting depth, since the file is untrusted;
encodings (EPUB allows UTF-8 and UTF-16 content documents); and how much of CSS, if any, counts as
spec-defined rather than policy.
