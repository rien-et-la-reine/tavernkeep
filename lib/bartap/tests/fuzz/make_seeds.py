#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
"""Write the fuzzers' seed corpora into tests/fuzz/corpus/NAME/, from the committed fixture
(tests/fixtures/pride-and-prejudice.epub) and the books tests/make_books.py generates.

    python3 tests/fuzz/make_seeds.py

Each seed is named by the SHA-1 of its contents, as libFuzzer names what it adds, so running this again
adds nothing new and the fuzzers' own additions sit beside the seeds. Seeds are kept small: whole books
over 64 KiB are replaced by a cut-down rebuild holding the package, navigation and a few documents.
Then commit the corpus directories.
"""

import hashlib
import io
import os
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
FIXTURE = os.path.join(TESTS, "fixtures", "pride-and-prejudice.epub")
sys.path.insert(0, TESTS)
import make_books  # noqa: E402

SMALL_ENTRIES = ("mimetype", "META-INF/container.xml", "epub/content.opf", "epub/toc.xhtml",
                 "epub/text/titlepage.xhtml", "epub/text/chapter-1.xhtml", "epub/css/local.css")


def save(kind, data):
    directory = os.path.join(HERE, "corpus", kind)
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, hashlib.sha1(data).hexdigest())
    if not os.path.exists(path):
        with open(path, "wb") as f:
            f.write(data)


def raw_deflate(info, archive_bytes):
    """An entry's compressed bytes, straight from the archive."""
    offset = info.header_offset
    name_length = int.from_bytes(archive_bytes[offset + 26:offset + 28], "little")
    extra_length = int.from_bytes(archive_bytes[offset + 28:offset + 30], "little")
    start = offset + 30 + name_length + extra_length
    return archive_bytes[start:start + info.compress_size]


def cut_down(archive_bytes):
    """The fixture rebuilt with only SMALL_ENTRIES: the OPF names more, so spine items go missing."""
    out = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(archive_bytes)) as source, zipfile.ZipFile(out, "w") as z:
        for name in SMALL_ENTRIES:
            info = source.getinfo(name)
            copy = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            copy.compress_type = info.compress_type
            z.writestr(copy, source.read(name))
    return out.getvalue()


def main():
    with open(FIXTURE, "rb") as f:
        fixture = f.read()
    books = []
    with zipfile.ZipFile(io.BytesIO(fixture)) as z:
        for name in SMALL_ENTRIES:
            info = z.getinfo(name)
            content = z.read(name)
            if info.compress_type == zipfile.ZIP_DEFLATED:
                # piece sizes 1/1, 7/13 and 255/255
                for prefix in (b"\x00\x00", b"\x06\x0c", b"\xfe\xfe"):
                    save("inflate", prefix + raw_deflate(info, fixture))
            if name.endswith((".xml", ".opf", ".xhtml")):
                # lenient, strict with XHTML entities; deep and shallow limits; small and large buffers
                for prefix in (b"\x01\x3f\xff\x00", b"\x03\x3f\x00\x05", b"\x00\x07\x10\x09"):
                    save("xml", prefix + content)
    books.append(cut_down(fixture))
    with tempfile.TemporaryDirectory() as directory:
        for name, (entries, _) in sorted(make_books.books().items()):
            path = os.path.join(directory, name)
            make_books.write_epub(path, entries)
            with open(path, "rb") as f:
                books.append(f.read())
            with zipfile.ZipFile(path) as z:
                for entry in z.namelist():
                    if entry.endswith((".xhtml", ".opf", ".ncx", ".html")):
                        save("xml", b"\x01\x3f\xff\x00" + z.read(entry))
    for book in books:
        save("ocf", book)
        save("epub", book)


if __name__ == "__main__":
    main()
