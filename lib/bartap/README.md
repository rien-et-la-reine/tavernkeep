<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Bartap

Bartap is an allocation-free, streaming, platform-agnostic EPUB container and package library for small
devices.

It covers the zip container and its DEFLATE-compressed entries, the OCF container, the OPF package and
spine, any entry by its path (stylesheets, images, the cover), navigation (the EPUB 3 navigation
document or the EPUB 2 NCX), and EPUB CFI parsing and resolution. An optional layer, `bartap/text.h`,
reads XHTML content documents as a stream of styled words with their positions. Layout and rendering
are left to the caller.

**Status: draft implementation.** Everything in `include/bartap/` is implemented and tested, but the
interfaces are still drafts: Q17, Q18 and Q20 are open, and the current shape follows the reader-facing
API proposal (push-fed text and CFI resolution with snapshots, element items and overrides, lenient
markup with warnings, a transient opener with open modes, entry lookup and the cover). Decisions are in
[DECISIONS.md](DECISIONS.md), open questions in [DESIGN-QUESTIONS.md](DESIGN-QUESTIONS.md).

The book is read through bounded calls, each making at most one read, so a reader with no more RAM
than a few KiB of state and a 32 KiB inflate window can stream a chapter from its zip entry while it
plays audio. The text layer and the CFI resolver take the document's bytes from the caller, so a
reader with PSRAM can decompress a chapter, or the whole book, once and read from memory instead:
seeks and snapshots then jump straight to any offset, which makes paging backwards cheap. The headers
describe both ways.

## Rules the library keeps

These hold for everything in `include/` and `src/`; the reasons are in DECISIONS.md. The tests check
the header and memory rules mechanically.

- **Headers:** only `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` and `<string.h>` (for `memcpy`, `memset`
  and `memcmp`).
- **Names:** every symbol and type starts with `bartap_`, and every macro with `BARTAP_`, internal ones
  included: a static library's internal symbols are still global.
- **Memory:** no dynamic allocation and no global or static mutable state. The caller owns all state
  and provides all working memory and output buffers (D3).
- **I/O:** only through callbacks the caller supplies (D4).
- **Errors:** result codes, with fixed values that are only ever appended to (D18). Nothing in the
  library aborts or asserts.
- **Bytes:** bytes read from files are `uint8_t`, never plain `char`, and values in files are decoded
  byte by byte, with no assumptions about byte order or alignment. Strings a caller passes in only to
  be matched are `const char *`, compared with `memcmp` (D7).
- **Bounded work:** every streaming call does a bounded amount of work and returns. Each header says
  which of its calls run to completion instead (D5).

## Building

From this directory:

```sh
cmake -S . -B build -G Ninja
cmake --build build
(cd build && ctest)                 # host tests, the CFI oracle and the portability checks
python3 tests/run_matrix.py         # the same under gcc and clang, 64- and 32-bit, as available
```

The libFuzzer harnesses build separately, under clang; see
[tests/fuzz/README.md](tests/fuzz/README.md). A corpus runner reads a directory of books of your own
(never committed) through the whole flow; see [tests/README.md](tests/README.md#the-corpus-runner).

From another CMake project:

```cmake
add_subdirectory(path/to/bartap)
target_link_libraries(your_target PRIVATE bartap)
```

Inside another project, the tests are off (`BARTAP_BUILD_TESTS`) and warnings are not errors
(`BARTAP_WARNINGS_AS_ERRORS`). Both default to on when Bartap is built by itself. The library builds as
ISO C11 without compiler extensions, whatever the including project uses.

See [tests/README.md](tests/README.md) for the tests and their fixtures.

## Layout

| Path | What it is |
|---|---|
| `include/bartap/` | Public headers: `epub.h` (the book-level API), `text.h` (content documents as styled text) and `byte_source.h` (D9) |
| `src/` | Implementation, and the internal headers: `ocf.h` (the OCF container), `inflate.h` (DEFLATE), `xml.h` (the XML tokenizer), `crc.h`, and the helpers `names.h` and `snapshot.h` |
| `tests/` | Host tests, fixture and book generators, the CFI oracle, the corpus runner, fuzz harnesses, portability checks, the compiler-matrix runner |
| `DECISIONS.md` | Decisions that shape Bartap, with their reasoning |
| `DESIGN-QUESTIONS.md` | Open questions about the draft interfaces |

## Licence

MIT; see [LICENSE](LICENSE). Every file carries SPDX tags, with two exceptions:
- `LICENSE` itself, which is the licence text.
- The test fixture `tests/fixtures/pride-and-prejudice.epub`, which is not Bartap's work. Its licensing
  is described in [tests/README.md](tests/README.md#fixtures).
