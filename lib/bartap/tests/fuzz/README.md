<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Fuzzing Bartap

libFuzzer harnesses, one per entry point, under AddressSanitizer and UndefinedBehaviorSanitizer (D8).
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
| `fuzz_inflate` | A byte for the input piece size, one for the output piece size (1 to 256), then raw DEFLATE | Differential against zlib's raw inflate (windowBits -15): the same verdict (decodes, truncated, corrupt), since `inflate.h` accepts exactly what zlib does, and the same output when both decode; what both produce agrees as far as both go |
| `fuzz_xml` | A byte each for the flags, the depth limit, the token buffer's size and the piece sizes, then a document | Depth within the limit; every name and value inside the token buffer; positions never decrease or pass the bytes taken; at DONE every byte taken exactly once; nothing CORRUPT in lenient mode |
| `fuzz_ocf` | A zip archive | Open (with a 64-byte or a 4 KiB scratch buffer); every entry read to its end with 1-byte buffers and with large ones, which must end the same way and agree on the bytes; the scratch pointer cleared once open finishes |
| `fuzz_epub` | An EPUB | The whole-book flow (`../common/flow.h`): open, every metadata field, the table of contents, every spine item through the text layer in mixed piece sizes; every item's position formatted, parsed back (it must round-trip) and resolved (it must succeed); a snapshot of each spine item restored into a fresh text layer must deliver the same items. The input's hash picks strict or lenient markup and whether element items are on |

Every step loop has an iteration cap derived from the input's size, and fails if a call returns OK without
making progress (nothing taken, nothing delivered, and its state unchanged byte for byte) more than a few
times in a row: D5 forbids livelocks, and a call that changes nothing would do the same for ever.

## Corpora

`corpus/NAME/` holds the seeds, named by their SHA-1 as libFuzzer names what it adds.
`make_seeds.py` writes them from the committed fixture (`tests/fixtures/pride-and-prejudice.epub`: some
of its entries' raw DEFLATE, its XML documents, and a cut-down rebuild of the book under 16 KiB) and from
the books `tests/make_books.py` generates. Keep every crash a fuzzer finds: once fixed, copy the input
into its corpus directory, where `ctest` runs it from then on.
