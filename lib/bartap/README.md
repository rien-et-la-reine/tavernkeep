<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Bartap

Bartap is an allocation-free, platform-agnostic EPUB reader library for small devices: the ZIP
container and its DEFLATE entries, the OCF container, the OPF package and spine, the table of contents
(EPUB 3 navigation document or EPUB 2 NCX), any entry by its path, and chapters as a stream of words and
blocks. Layout and rendering are left to the caller.

**Status: implemented, API still draft.** Host fixtures, token/position regressions, zlib differential
cases and portability checks run through CTest. The reference book is walked in stream and memory
modes. See [DECISIONS.md](DECISIONS.md) for the design and [VALIDATION.md](VALIDATION.md) for the checked
configurations and memory costs. Device latency and full firmware stack use still need measurement.

## Shape

- **Synchronous open and lookup.** `bartap_open` and the `*_open` calls run to completion. Only
  resource bytes (`bartap_stream_*`) and text tokens (`bartap_text_*`) are streamed. Real-time work on
  the same core (audio) is the caller's to do, inside its byte source's read callback if need be.
- **Lenient only.** Damaged packaging and markup are recovered from and reported as `BARTAP_WARN_*`
  bits; invalid UTF-8 is read as Windows-1252.
- **Positions are `bartap_loc_t`:** a spine index, raw XHTML chunk offset, token index and raw anchor.
  Seeking replays formatting from the chapter start. With PSRAM (`bartap_text_open_memory`), replay
  reads memory without inflating again. A tail buffer for stream mode is decided but not yet
  implemented (D16). `bartap_text_context` supplies open blocks after a seek;
  `bartap_text_seek_fragment` resolves a TOC's decoded fragment on demand.
- **State the caller declares by type.** `bartap.h` defines every state struct, so the caller declares
  a `bartap_book_t` (and so on) and Bartap never casts a byte array to a struct. The fields are
  Bartap's.

## Rules the library keeps

These hold for everything in `include/` and `src/`. The tests check the header and memory rules
mechanically.

- **Headers:** only `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` and `<string.h>`.
- **Names:** every symbol and type starts with `bartap_`, and every macro with `BARTAP_`, internal ones
  included: a static library's internal symbols are still global.
- **Memory:** no dynamic allocation and no global or static mutable state. The caller owns all state
  and provides all working memory: `bartap_open`'s work buffer (whose window then serves streams and
  iterators), the spine array.
- **I/O:** only through the caller's `bartap_source_t` read callback.
- **Errors:** `bartap_err_t`. Nothing in the library aborts or asserts.
- **Untrusted input:** every offset and length from a file is bounds-checked.

## Small contracts worth knowing

- Adjacent WORD tokens join without an inserted space. Bartap emits SPACE and LINE_BREAK explicitly.
- Formatting uses at most 32 recognized nested elements. Deeper ones are ignored with
  `BARTAP_WARN_MARKUP`; their text stays in the enclosing block and style.
- Table cells and structural containers separate words without being blocks themselves; images and
  text outside any block open an implicit paragraph.
- Read `bartap_text_warnings` / `bartap_toc_warnings` as well as `bartap_warnings`.
- Memory-mode opening takes `book` before `spine_index` to resolve relative images: it reads the
  chapter's name from its local header once. Iteration and seeking read no archive; only the supplied
  XHTML must remain alive afterward.
- The TOC is the nav document's `epub:type="toc"` nav, else the NCX; a nav document with no toc nav
  falls back to its first untyped nav.
- The identifier is the one `unique-identifier` names, else the first non-empty `dc:identifier` (with
  `BARTAP_WARN_MARKUP`).
- Source callbacks and live iterators must not share an active inflate window. After opening, reuse
  the work buffer's window, but do not place the work buffer itself on a small embedded stack.
- The new position format is draft and tied to tokenizer behavior. Version saved bookmarks in the
  application; positions from the earlier implementation are not compatible.

## Building

From this directory:

```sh
cmake -S . -B build -G Ninja
cmake --build build
(cd build && ctest)                 # host tests and the portability checks
python3 tests/run_matrix.py         # the same under gcc and clang, 64- and 32-bit, as available
```

The libFuzzer harnesses build separately, under clang; see [tests/fuzz/README.md](tests/fuzz/README.md).

From another CMake project:

```cmake
add_subdirectory(path/to/bartap)
target_link_libraries(your_target PRIVATE bartap)
```

Inside another project, the tests are off (`BARTAP_BUILD_TESTS`) and warnings are not errors
(`BARTAP_WARNINGS_AS_ERRORS`). Both default to on when Bartap is built by itself. The library builds as
ISO C11 without compiler extensions, whatever the including project uses.

## Layout

| Path | What it is |
|---|---|
| `include/bartap.h` | The public API, and every state struct's layout |
| `src/bartap_internal.h` | The interfaces between the modules |
| `src/byte_source.c` | Reads through the caller's source; the memory source; shared helpers |
| `src/ocf.c` | ZIP end record and central directory; exact entry names |
| `src/inflate.c` | Streaming DEFLATE, with an optional fast Huffman table |
| `src/xml.c` | Lenient pull tokenizer, entities, Windows-1252 fallback |
| `src/epub.c` | Opening the book, metadata, spine, streams, the TOC |
| `src/text.c` | Chapters as words, spaces, line breaks, blocks and images |
| `tests/` | Host tests, fixture and book generators, fuzz harnesses, portability checks, the compiler-matrix runner |
| `DECISIONS.md` | Decisions that shape Bartap, with their reasoning |

## Licence

MIT; see [LICENSE](LICENSE). Every file carries SPDX tags, with two exceptions:
- `LICENSE` itself, which is the licence text.
- The test fixture `tests/fixtures/pride-and-prejudice.epub`, which is not Bartap's work. Its licensing
  is described in [tests/README.md](tests/README.md#fixtures).
