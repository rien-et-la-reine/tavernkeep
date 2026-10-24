// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_EPUB_H
#define BARTAP_EPUB_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * EPUB: the book-level API, and with byte_source.h the whole of Bartap's
 * public surface (D9 in DECISIONS.md). The OCF container and DEFLATE are
 * internal layers beneath it (src/ocf.h, src/inflate.h).
 *
 * Intended responsibilities:
 * - opening a book from a byte source (byte_source.h);
 * - its metadata;
 * - its spine;
 * - its navigation: the EPUB 3 navigation document, or the EPUB 2 NCX;
 * - reading a content document;
 * - EPUB CFI parsing and resolution.
 *
 * TODO: the API itself is undesigned, so this header declares nothing yet.
 * Its shape is Q18. */

#endif
