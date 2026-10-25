// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//the library's headers, public and internal, included together as the tests use them, and the constants
//the specifications fix

#include "bartap/byte_source.h"
#include "bartap/epub.h"
#include "bartap/text.h"
#include "crc.h"
#include "inflate.h"
#include "names.h"
#include "ocf.h"
#include "snapshot.h"
#include "xml.h"

#include "harness.h"

//RFC 1951 3.2.5: distances reach back at most 32768 bytes, so a window smaller than that cannot decode
//every valid stream (D11)
static void headers_inflate_window_is_rfc1951_maximum_distance(void)
{
    EXPECT_EQ_U(BARTAP_INFLATE_WINDOW_SIZE, 32768U);
}

const test_case_t headers_tests[] = {
    TEST_CASE(headers_inflate_window_is_rfc1951_maximum_distance),
};
const size_t headers_test_count = sizeof headers_tests / sizeof headers_tests[0];
