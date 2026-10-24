// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT
// No include guard: this is a never-compiled fixture, exempt from the header-guard rule.

/* Planted violations for include_check_selftest (check_includes.cmake): five disallowed includes, one
 * of each kind, among allowed ones that must not be counted. Never compiled. A mention of #include
 * <stdio.h> inside a comment, like this one, is not a directive and is not counted either. */

#include <stdint.h>
#include "bartap/zip.h"

#include <stdio.h>                  // another standard header
  #  include <stdlib.h>            // the same, spaced out
#include "platform/debug.h"         // a quoted header outside bartap/
#include BARTAP_PLATFORM_HEADER     // through a macro, so it cannot be checked
#include_next <assert.h>            // include_next is an include too
