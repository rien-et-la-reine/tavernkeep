// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_TESTS_BOOKS_H
#define BARTAP_TESTS_BOOKS_H

//helpers for the book-level cases: a book from make_books.py (BARTAP_TESTS_BOOKS_DIR) or the committed
//fixture, opened from memory with heap-allocated working memory, and a content document read whole

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bartap/byte_source.h"
#include "bartap/epub.h"

typedef struct {
    uint8_t *data;
    size_t size;
    bartap_byte_source_memory_t memory;
    bartap_byte_source_t source;
    bartap_epub_t book;
    bartap_epub_opener_t opener;
    bartap_epub_spine_item_t spine[64];
    uint8_t window[32768];
    uint8_t input[512];
    uint8_t scratch[512];
} test_book_t;

//load path (relative to the books directory, or absolute) into book->data; false if it cannot be read
bool test_book_load(test_book_t *book, const char *name);

//open the loaded book in mode with flags; the result of bartap_epub_open
bartap_epub_result_t test_book_open(test_book_t *book, uint8_t mode, uint32_t flags);

//load and open in FULL mode: a fresh heap object the caller frees with test_book_free, or NULL
test_book_t *test_book_new(const char *name, uint8_t mode, uint32_t flags, bartap_epub_result_t *result);
void test_book_free(test_book_t *book);

//read an entry whole into a heap buffer (NUL-terminated); the stream's result, *size its length
bartap_epub_result_t test_book_read(test_book_t *book, const bartap_epub_location_t *location, uint8_t **data,
    size_t *size);

#endif
