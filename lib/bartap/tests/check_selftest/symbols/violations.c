// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Planted violations for symbol_check_selftest (check_symbols.cmake): one piece of writable static
 * state and one call into the C library. Compiled into a library of its own, never linked. */

#include <stdlib.h>

static unsigned calls;

void *bartap_selftest_allocate(size_t size)
{
    calls++;
    return calls > 1000U ? NULL : malloc(size);
}
