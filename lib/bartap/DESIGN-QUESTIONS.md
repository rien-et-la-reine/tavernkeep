<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Design questions

Open questions about Bartap's draft interfaces, numbered so that the headers in `include/bartap/` and `src/` can
refer to them (Q1, Q2, ...). Each gives the draft as it stands, then the options and their trade-offs.
Settled questions move to [DECISIONS.md](DECISIONS.md), as a decision that names the question's number.

---

## Q1. Who owns the inflate window and decode tables, and how they stay in fast memory

Settled: a caller-supplied window whose size `bartap_inflate_init` checks, and decode tables inside the
state struct. See D11 in [DECISIONS.md](DECISIONS.md).

---

## Q2. How working-memory requirements are sized and communicated

**Draft:** every state struct (`bartap_inflate_t`, `bartap_ocf_stream_t`, ...) is fully declared in its
header, marked private, so `sizeof` gives its size and the caller can allocate it statically. The
window's size is a macro. Those two headers are now internal (D9 in DECISIONS.md), but the public
book-level structs that will embed them (Q18) face the same question.

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
`-fstack-usage` in CI), to promise a ceiling and test against it, or to say nothing. Opening's scratch
buffer is the caller's (D15), and `bartap_ocf_find` compares names through a small fixed piece on its
own stack (D17).

---

## Q3. Callback signatures for I/O, and how read errors propagate

Settled: reads block and deliver everything or fail; every call makes at most one read, before
changing any state; concurrent use of one source is the caller's responsibility; platform error
detail stays in the callback's context. See D14 in [DECISIONS.md](DECISIONS.md).

---

## Q4. How errors carry detail beyond a result code

Settled: codes only, with explicit values, append-only. See D18 in [DECISIONS.md](DECISIONS.md).

---

## Q5. A run-to-completion convenience call that inflates a whole entry

**Proposed shape:** for example
`bartap_ocf_entry_extract(ocf, entry, window, window_size, input, input_size, out, out_capacity, max_size, &out_length)`
(internal, D9 in DECISIONS.md, or a book-level counterpart in `epub.h`, Q18), built only on the
streaming API, enforcing in one combined result:
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
- **D. B or C with output as the window** (an alternative D11 records), saving 32 KiB. But then it is
  no longer purely built on the streaming API, and it needs an init path other than
  `bartap_inflate_init`, which refuses a window smaller than `BARTAP_INFLATE_WINDOW_SIZE` (D11).

**Smaller decisions within it:** what the buffer holds after a failure (unspecified, or zeroed, which
costs a pass over it); and whether stored entries go through the same call (presumably yes). A
declared size over `max_size` or `out_capacity` is `LIMIT_EXCEEDED` (D18).

---

## Q6. Bounded work in `bartap_ocf_entry_read`

Settled: each call makes at most one read and decodes from what it has, with no budget parameter. See
D12 in [DECISIONS.md](DECISIONS.md).

---

## Q7. `bartap_ocf_open`'s end-record scan: run to completion, and where its buffer lives

Settled: begin/step calls with a caller-supplied scratch buffer, and `bartap_ocf_open` as a
convenience call that loops them. See D15 in [DECISIONS.md](DECISIONS.md).

---

## Q8. `bartap_ocf_find`: run to completion, or in steps

**Draft:** a convenience call built on `bartap_ocf_next`, which D5 (DECISIONS.md) allows: it walks the
directory until it finds the name, in one call, so its work grows with the number of entries. It
compares name lengths first and reads only the names whose length matches (D17), and returns the first
match (D21).

**Options:** keep it as drafted; a cursor-based step version returning "in progress"; or drop it in
favour of `bartap_ocf_next` plus a caller-side lookup or cache, which the package layer will want
anyway for the spine. A caller-built index (hashes of names, in caller memory) is a possible later
addition.

---

## Q9. The entry stream's input buffer

Settled: caller-supplied at `bartap_ocf_entry_open`, NULL allowed for stored entries. See D16 in
[DECISIONS.md](DECISIONS.md).

---

## Q10. `BARTAP_OCF_NAME_MAX` and names stored in entries

Settled: entries hold their name's location and length, with no length limit, and
`bartap_ocf_entry_name` copies a name on demand. See D17 in [DECISIONS.md](DECISIONS.md).

---

## Q11. Build-time configuration

**Draft:** there is none. Every constant is fixed in the headers.

**Options:**
- `-D` macros with `#ifndef` defaults in the headers;
- a user-supplied configuration header (`BARTAP_CONFIG_FILE`, included if defined);
- CMake options that become compile definitions.

**The trade-off is flexibility against mismatch.** Any setting that changes a struct's layout (the decode
tables' size, D11) must be identical in the library and every file that includes its headers, or memory is silently
corrupted. Settings would then have to be `PUBLIC` compile definitions. A link-time guard (a symbol
whose name encodes the configuration) could catch a mismatch.

---

## Q12. CRC-32

**Draft:** computed privately in `ocf.c` with a 1 KiB read-only table.

**Options:**
- 1 KiB table (fastest in software);
- 16-entry nibble table (64 B, about 4 times slower);
- bitwise, no table (slowest);
- a caller-supplied CRC function, which lets a device use hardware: the RP2350's DMA sniffer computes
  CRC-32, for one.

Where a table lives also matters. In execute-in-place flash it may miss the cache; a caller-supplied
table pointer would let the caller place it, but that is memory placement again (D11). Separately:
whether `bartap_crc32_update` is public, which would be useful to callers and to tests.

---

## Q13. 32-bit offsets and no ZIP64

Settled: ZIP64 is read where its values fit in 32 bits, and `bartap_offset_t` (32 bits) is used for
every position and size. See D13 in [DECISIONS.md](DECISIONS.md).

---

## Q14. Where the OCF rules are checked

Settled: in the internal OCF layer, which is EPUB-specific rather than a general zip reader. See D10 in
[DECISIONS.md](DECISIONS.md). Packaging violations are accepted with warnings (D19); where
`container.xml` is read is an open point in Q18.

---

## Q15. Entry-name encoding

Settled: names are compared as raw bytes, and UTF-8 is checked, as a warning, only where flag bit
11 is set. See D20 in [DECISIONS.md](DECISIONS.md).

---

## Q16. Minimum C standard

Settled: ISO C11 without extensions. See D7 in [DECISIONS.md](DECISIONS.md).

---

## Q17. Does Bartap turn XHTML content documents into a styled text stream?

**Draft:** no. Bartap's scope ends at delivering a content document's bytes (D2 in DECISIONS.md),
through the book-level API (`epub.h`, Q18) over the internal `bartap_ocf_entry_read`. Bartap will
parse XML anyway, for `container.xml`, the OPF and the navigation document or NCX.

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

---

## Q18. The shape of the book-level API

**Draft:** none. `bartap/epub.h` is a placeholder (D9 in DECISIONS.md). It is meant to open a book from
a byte source and give its metadata, its spine, its navigation (the EPUB 3 navigation document or the
EPUB 2 NCX), a content document's bytes, and EPUB CFI parsing and resolution.

**Open points:**
- **Opening:** one call or begin/step calls. Opening reads the end record (D15), `container.xml` and
  the OPF, so D5 asks for bounded steps, with a run-to-completion call at most as a convenience (Q5).
- **`container.xml` and the rootfile:** whether the OCF layer or the book layer reads them, and which
  rendition is used when `container.xml` lists more than one rootfile. Whichever layer reads it must
  require the OPF it names to appear exactly once in the archive (D21).
- **Metadata:** copied into caller buffers (sizes fixed or caller-chosen; ties to Q11), or
  kept as offsets and lengths and read on demand.
- **Spine:** access by index, by cursor, or both; whether the book object caches the spine's entries,
  and in whose memory (D3).
- **Manifest lookup by href:** percent-decoding, relative paths against the OPF's directory, and
  matching raw-byte entry names (D20). Known limitation: an href and a name in different Unicode
  normalization forms do not match.
- **Navigation:** one model over both the EPUB 3 navigation document and the NCX, or one per format;
  a tree or an iterator; a limit on nesting depth, since the file is untrusted (D6).
- **Reading a content document:** a stream handle over the OCF layer's bounded reads (D12), how many
  can be open at once, and whether they share one inflate window (D11); what Q17 decides sits on top.
- **CFI:** parsing (a string to a structure) separate from resolution (a structure to a position in
  the book), and how resolution works over content read in pieces (D2).
- **Errors:** one result enum for the public API, or the internal layers' results passed through;
  keeping "the storage failed" apart from "the book is damaged" (byte_source.h); passing on the OCF
  layer's warnings (D19); codes only (D18).
- **Memory:** how the sizes of public structs that embed internal state are communicated (Q2), and
  each call's worst-case stack.

---

## Q19. Strictness: safety rules versus packaging conformance

Settled: lenient packaging, with violations reported as warning bits, and `NOT_AN_EPUB` for a
mimetype declaring another format. See D19 in [DECISIONS.md](DECISIONS.md).

---

## Q20. The shape of the internal XML tokenizer

**Draft:** none. `src/xml.h` is a placeholder: a streaming, non-validating tokenizer for
`container.xml`, the OPF and navigation, and content documents if Q17 decides so, with bounded work per
call (D5), limits on nesting depth, name length and token length (D6), and UTF-8 input.

**Open points:**
- **Event model:** pull (the caller asks for the next token: start tag, attribute, text, end tag) or
  push (a caller's callback per event); how a token longer than the caller's buffer is delivered (in
  pieces, or refused); attributes as separate events or with their start tag.
- **Limits:** which are fixed and which the caller sets (nesting depth, name length, token or
  attribute-value length). Exceeding one the caller set is `LIMIT_EXCEEDED` (D18).
- **Entities:** the draft position is the five predefined entities and numeric character references
  only, with no DTDs, so no external or user-defined entity is ever expanded. Still to settle: what an
  undefined entity is (an error, or passed through as text), and whether a DOCTYPE is refused or
  skipped.
- **Namespaces:** resolved by the tokenizer (prefixes mapped to URIs, which needs memory for the
  declarations in scope) or left to the caller (raw qualified names). The OPF, `container.xml` and
  navigation all use namespaces, and the navigation document needs `epub:type`.
- **Encoding:** UTF-8 only, or UTF-16 too (EPUB allows UTF-16 content documents), and whether invalid
  UTF-8 is an error.
- **Relation to Q17:** whether the same tokenizer is exposed for content documents (Q17 B), which
  makes its event model public API and its limits part of the contract, or stays internal to the
  package and navigation layers.
