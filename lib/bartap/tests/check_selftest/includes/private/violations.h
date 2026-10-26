// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT
// No include guard: this is a never-compiled fixture, exempt from the header-guard rule.

/* Planted violations for include_check_selftest (check_includes.cmake), as an internal header: one
 * disallowed include among allowed ones that must not be counted. Never compiled. */

#include <stddef.h>
#include "bartap.h"
#include "internal.h"               // an internal header that exists here: allowed

#include "missing.h"                // a bare header that is not one of the library's internal ones
