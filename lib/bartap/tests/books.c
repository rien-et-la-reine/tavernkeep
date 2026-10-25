// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include "books.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool test_book_load(test_book_t *book, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, name[0] == '/' ? "%s" : BARTAP_TESTS_BOOKS_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    book->data = malloc(size > 0 ? (size_t)size : 1U);
    book->size = size > 0 ? (size_t)size : 0U;
    const bool ok = size >= 0 && fread(book->data, 1, book->size, f) == book->size;
    fclose(f);
    bartap_byte_source_memory_init(&book->memory, book->data, (bartap_offset_t)book->size, &book->source);
    return ok;
}

bartap_epub_result_t test_book_open(test_book_t *book, uint8_t mode, uint32_t flags)
{
    const bartap_epub_memory_t memory = { book->scratch, sizeof book->scratch, book->window, sizeof book->window,
        book->input, sizeof book->input, book->spine, sizeof book->spine / sizeof book->spine[0], 1U << 20, mode,
        flags };
    return bartap_epub_open(&book->opener, &book->book, &book->source, &memory);
}

test_book_t *test_book_new(const char *name, uint8_t mode, uint32_t flags, bartap_epub_result_t *result)
{
    test_book_t *book = calloc(1, sizeof *book);
    if (!test_book_load(book, name)) {
        test_book_free(book);
        return NULL;
    }
    *result = test_book_open(book, mode, flags);
    return book;
}

void test_book_free(test_book_t *book)
{
    if (book != NULL) {
        free(book->data);
        free(book);
    }
}

bartap_epub_result_t test_book_read(test_book_t *book, const bartap_epub_location_t *location, uint8_t **data,
    size_t *size)
{
    *data = NULL;
    *size = 0;
    bartap_epub_stream_t stream;
    bartap_epub_result_t r = bartap_epub_stream_open_at(&stream, &book->book, location, 1U << 24, book->window,
        sizeof book->window, book->input, sizeof book->input);
    size_t capacity = 0;
    bool end = false;
    while (r == BARTAP_EPUB_RESULT_OK && !end) {
        if (*size + 4096U + 1U > capacity) {
            capacity = 2U * capacity + 8192U;
            *data = realloc(*data, capacity);
        }
        size_t got;
        r = bartap_epub_stream_read(&stream, *data + *size, 4096, &got, &end);
        *size += got;
    }
    if (*data == NULL) {
        *data = malloc(1);
    }
    (*data)[*size] = 0;
    return r;
}
