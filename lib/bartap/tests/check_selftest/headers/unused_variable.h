// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT
// No include guard: this is a planted fixture, exempt from the header-guard rule.

/* Planted violation for header_unit_selftest_unused_variable (tests/CMakeLists.txt): a static variable
 * defined in a header and never used, which a header unit must refuse to compile. */

static int bartap_selftest_unused_variable;
