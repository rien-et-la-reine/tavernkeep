<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Fuzzing Bartap

libFuzzer harnesses, one per entry point, under AddressSanitizer and UndefinedBehaviorSanitizer.
They need clang and zlib's headers, and build only when asked:

```sh
cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DBARTAP_BUILD_FUZZERS=ON -DBARTAP_BUILD_TESTS=OFF
cmake --build build-fuzz
(cd build-fuzz && ctest)            # every committed seed and crash, run once
```

To fuzz, give each harness a working copy of its corpus, so that what it adds stays out of the tree until
you choose to keep it:

```sh
mkdir -p /tmp/fuzz/epub && cp tests/fuzz/corpus/epub/* /tmp/fuzz/epub/
./build-fuzz/tests/fuzz/fuzz_epub -max_total_time=600 -max_len=65536 -timeout=30 /tmp/fuzz/epub
```

| Harness | Input | Checks |
|---|---|---|
| `fuzz_inflate` | A byte for the input piece size, one for the output piece size (1 to 256), then raw DEFLATE | TODO(rewrite): differential against zlib's raw inflate (windowBits -15) |
| `fuzz_xml` | A byte for the piece size, then a document | TODO(rewrite): offsets never decrease; every byte consumed once |
| `fuzz_ocf` | A zip archive | TODO(rewrite): every entry read to its end with small and large buffers, agreeing |
| `fuzz_epub` | An EPUB | Open, metadata, the TOC and every spine item's text. TODO(rewrite): seek to every token's location and resume at the same token |

The harnesses are skeletons for the rewrite. The seed corpora predate it: the `xml` seeds were written
for the old harness, whose first bytes were flags, a depth limit and buffer sizes, so their leading bytes
now act as noise until `make_seeds.py` is updated.

Every loop has an iteration cap derived from the input's size, so a call that makes no progress ends the
run rather than hanging it.

## Corpora

`corpus/NAME/` holds the seeds, named by their SHA-1 as libFuzzer names what it adds.
`make_seeds.py` writes them from the committed fixture (`tests/fixtures/pride-and-prejudice.epub`: some
of its entries' raw DEFLATE, its XML documents, and a cut-down rebuild of the book under 16 KiB) and from
the books `tests/make_books.py` generates. Keep every crash a fuzzer finds: once fixed, copy the input
into its corpus directory, where `ctest` runs it from then on.
