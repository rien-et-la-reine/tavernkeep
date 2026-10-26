<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Bartap

Bartap is an allocation-free, platform-agnostic EPUB reader library for small devices: the ZIP
container and its DEFLATE entries, the OCF container, the OPF package and spine, the table of contents
(EPUB 3 navigation document or EPUB 2 NCX), any entry by its path, and chapters as a stream of words and
blocks. Layout and rendering are left to the caller.

**Status: skeleton.** A rewrite toward roughly 2,800 lines. `include/bartap.h` and
`src/bartap_internal.h` are drafted, and the decisions behind them are in [DECISIONS.md](DECISIONS.md); every function in `src/` is a stub marked `TODO(rewrite)`. The
previous implementation is in this branch's history.

## Shape

- **Synchronous open and lookup.** `bartap_open` and the `*_open` calls run to completion. Only
  resource bytes (`bartap_stream_*`) and text tokens (`bartap_text_*`) are streamed. Real-time work on
  the same core (audio) is the caller's to do, inside its byte source's read callback if need be.
- **Lenient only.** Damaged packaging and markup are recovered from and reported as `BARTAP_WARN_*`
  bits; invalid UTF-8 is read as Windows-1252.
- **Positions are `bartap_loc_t`:** a spine index and a byte offset in that chapter's uncompressed
  XHTML. Paging backwards re-reads the chapter from its start, or seeks in memory when the chapter has
  been inflated into PSRAM once (`bartap_text_open_memory`).
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
| `src/ocf.c` | ZIP end record and central directory; entries by name or hash |
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
