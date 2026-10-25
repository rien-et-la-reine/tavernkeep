// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Byte sources (bartap/byte_source.h). */

#include "bartap/byte_source.h"

#include <string.h>

bartap_byte_source_result_t bartap_byte_source_read(
    const bartap_byte_source_t *source,
    bartap_offset_t offset,
    void *buffer,
    size_t size,
    size_t *bytes_read)
{
    if (bytes_read == NULL) {
        return BARTAP_BYTE_SOURCE_RESULT_INVALID_ARGUMENT;
    }
    *bytes_read = 0;
    if (source == NULL || source->read == NULL || (buffer == NULL && size != 0U)) {
        return BARTAP_BYTE_SOURCE_RESULT_INVALID_ARGUMENT;
    }
    if (offset >= source->size || size == 0U) {
        return BARTAP_BYTE_SOURCE_RESULT_OK;    //nothing to read: not an error, and no callback
    }
    //clip to the source: the remaining length fits in size_t's range only after the comparison
    const bartap_offset_t remaining = source->size - offset;
    const size_t length = size > (size_t)remaining ? (size_t)remaining : size;
    if (source->read(source->context, offset, buffer, length) != BARTAP_BYTE_SOURCE_RESULT_OK) {
        return BARTAP_BYTE_SOURCE_RESULT_IO_ERROR;  //all or nothing (D14): any failure is IO_ERROR
    }
    *bytes_read = length;
    return BARTAP_BYTE_SOURCE_RESULT_OK;
}

static bartap_byte_source_result_t memory_read(void *context, bartap_offset_t offset, void *buffer, size_t size)
{
    const bartap_byte_source_memory_t *memory = context;
    memcpy(buffer, memory->data + offset, size);
    return BARTAP_BYTE_SOURCE_RESULT_OK;
}

void bartap_byte_source_memory_init(
    bartap_byte_source_memory_t *memory,
    const void *data,
    bartap_offset_t size,
    bartap_byte_source_t *source)
{
    if (memory == NULL || source == NULL || (data == NULL && size != 0U)) {
        return;
    }
    memory->data = data;
    source->read = memory_read;
    source->context = memory;
    source->size = size;
}
