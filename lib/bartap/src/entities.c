// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The named character entities of XHTML 1.x (xml.h, internal): the 253 that the XHTML 1.0 and 1.1 DTDs
 * declare through HTMLlat1, HTMLsymbol and HTMLspecial (the HTML 4.01 set, plus apos). EPUB 2 content
 * documents declare those DTDs, so their entities belong to the document; EPUB 3 ones declare none, but
 * books use them anyway.
 *
 * Every one is a single code point in the Basic Multilingual Plane, and no name is longer than 8 bytes.
 * Names are kept in 8-byte fields, NUL-padded, in byte order for a binary search, with the code points in
 * a parallel array: 2530 bytes of read-only data, consulted only for a reference that is not one of XML's
 * five. Generated from Python's html.entities.name2codepoint (the HTML 4.01 set) with apos added. */

#include "xml.h"

#include <string.h>

#define NAME_SIZE 8U
#define COUNT 253U

//one string of fixed-width fields, so no field needs room for a terminator
static const char names[COUNT * NAME_SIZE + 1U] =
    "AElig\0\0\0" "Aacute\0\0" "Acirc\0\0\0" "Agrave\0\0" "Alpha\0\0\0" "Aring\0\0\0" "Atilde\0\0"
    "Auml\0\0\0\0" "Beta\0\0\0\0" "Ccedil\0\0" "Chi\0\0\0\0\0" "Dagger\0\0" "Delta\0\0\0" "ETH\0\0\0\0\0"
    "Eacute\0\0" "Ecirc\0\0\0" "Egrave\0\0" "Epsilon\0" "Eta\0\0\0\0\0" "Euml\0\0\0\0" "Gamma\0\0\0"
    "Iacute\0\0" "Icirc\0\0\0" "Igrave\0\0" "Iota\0\0\0\0" "Iuml\0\0\0\0" "Kappa\0\0\0" "Lambda\0\0"
    "Mu\0\0\0\0\0\0" "Ntilde\0\0" "Nu\0\0\0\0\0\0" "OElig\0\0\0" "Oacute\0\0" "Ocirc\0\0\0" "Ograve\0\0"
    "Omega\0\0\0" "Omicron\0" "Oslash\0\0" "Otilde\0\0" "Ouml\0\0\0\0" "Phi\0\0\0\0\0" "Pi\0\0\0\0\0\0"
    "Prime\0\0\0" "Psi\0\0\0\0\0" "Rho\0\0\0\0\0" "Scaron\0\0" "Sigma\0\0\0" "THORN\0\0\0" "Tau\0\0\0\0\0"
    "Theta\0\0\0" "Uacute\0\0" "Ucirc\0\0\0" "Ugrave\0\0" "Upsilon\0" "Uuml\0\0\0\0" "Xi\0\0\0\0\0\0"
    "Yacute\0\0" "Yuml\0\0\0\0" "Zeta\0\0\0\0" "aacute\0\0" "acirc\0\0\0" "acute\0\0\0" "aelig\0\0\0"
    "agrave\0\0" "alefsym\0" "alpha\0\0\0" "amp\0\0\0\0\0" "and\0\0\0\0\0" "ang\0\0\0\0\0" "apos\0\0\0\0"
    "aring\0\0\0" "asymp\0\0\0" "atilde\0\0" "auml\0\0\0\0" "bdquo\0\0\0" "beta\0\0\0\0" "brvbar\0\0"
    "bull\0\0\0\0" "cap\0\0\0\0\0" "ccedil\0\0" "cedil\0\0\0" "cent\0\0\0\0" "chi\0\0\0\0\0" "circ\0\0\0\0"
    "clubs\0\0\0" "cong\0\0\0\0" "copy\0\0\0\0" "crarr\0\0\0" "cup\0\0\0\0\0" "curren\0\0" "dArr\0\0\0\0"
    "dagger\0\0" "darr\0\0\0\0" "deg\0\0\0\0\0" "delta\0\0\0" "diams\0\0\0" "divide\0\0" "eacute\0\0"
    "ecirc\0\0\0" "egrave\0\0" "empty\0\0\0" "emsp\0\0\0\0" "ensp\0\0\0\0" "epsilon\0" "equiv\0\0\0"
    "eta\0\0\0\0\0" "eth\0\0\0\0\0" "euml\0\0\0\0" "euro\0\0\0\0" "exist\0\0\0" "fnof\0\0\0\0" "forall\0\0"
    "frac12\0\0" "frac14\0\0" "frac34\0\0" "frasl\0\0\0" "gamma\0\0\0" "ge\0\0\0\0\0\0" "gt\0\0\0\0\0\0"
    "hArr\0\0\0\0" "harr\0\0\0\0" "hearts\0\0" "hellip\0\0" "iacute\0\0" "icirc\0\0\0" "iexcl\0\0\0"
    "igrave\0\0" "image\0\0\0" "infin\0\0\0" "int\0\0\0\0\0" "iota\0\0\0\0" "iquest\0\0" "isin\0\0\0\0"
    "iuml\0\0\0\0" "kappa\0\0\0" "lArr\0\0\0\0" "lambda\0\0" "lang\0\0\0\0" "laquo\0\0\0" "larr\0\0\0\0"
    "lceil\0\0\0" "ldquo\0\0\0" "le\0\0\0\0\0\0" "lfloor\0\0" "lowast\0\0" "loz\0\0\0\0\0" "lrm\0\0\0\0\0"
    "lsaquo\0\0" "lsquo\0\0\0" "lt\0\0\0\0\0\0" "macr\0\0\0\0" "mdash\0\0\0" "micro\0\0\0" "middot\0\0"
    "minus\0\0\0" "mu\0\0\0\0\0\0" "nabla\0\0\0" "nbsp\0\0\0\0" "ndash\0\0\0" "ne\0\0\0\0\0\0" "ni\0\0\0\0\0\0"
    "not\0\0\0\0\0" "notin\0\0\0" "nsub\0\0\0\0" "ntilde\0\0" "nu\0\0\0\0\0\0" "oacute\0\0" "ocirc\0\0\0"
    "oelig\0\0\0" "ograve\0\0" "oline\0\0\0" "omega\0\0\0" "omicron\0" "oplus\0\0\0" "or\0\0\0\0\0\0"
    "ordf\0\0\0\0" "ordm\0\0\0\0" "oslash\0\0" "otilde\0\0" "otimes\0\0" "ouml\0\0\0\0" "para\0\0\0\0"
    "part\0\0\0\0" "permil\0\0" "perp\0\0\0\0" "phi\0\0\0\0\0" "pi\0\0\0\0\0\0" "piv\0\0\0\0\0" "plusmn\0\0"
    "pound\0\0\0" "prime\0\0\0" "prod\0\0\0\0" "prop\0\0\0\0" "psi\0\0\0\0\0" "quot\0\0\0\0" "rArr\0\0\0\0"
    "radic\0\0\0" "rang\0\0\0\0" "raquo\0\0\0" "rarr\0\0\0\0" "rceil\0\0\0" "rdquo\0\0\0" "real\0\0\0\0"
    "reg\0\0\0\0\0" "rfloor\0\0" "rho\0\0\0\0\0" "rlm\0\0\0\0\0" "rsaquo\0\0" "rsquo\0\0\0" "sbquo\0\0\0"
    "scaron\0\0" "sdot\0\0\0\0" "sect\0\0\0\0" "shy\0\0\0\0\0" "sigma\0\0\0" "sigmaf\0\0" "sim\0\0\0\0\0"
    "spades\0\0" "sub\0\0\0\0\0" "sube\0\0\0\0" "sum\0\0\0\0\0" "sup\0\0\0\0\0" "sup1\0\0\0\0" "sup2\0\0\0\0"
    "sup3\0\0\0\0" "supe\0\0\0\0" "szlig\0\0\0" "tau\0\0\0\0\0" "there4\0\0" "theta\0\0\0" "thetasym"
    "thinsp\0\0" "thorn\0\0\0" "tilde\0\0\0" "times\0\0\0" "trade\0\0\0" "uArr\0\0\0\0" "uacute\0\0"
    "uarr\0\0\0\0" "ucirc\0\0\0" "ugrave\0\0" "uml\0\0\0\0\0" "upsih\0\0\0" "upsilon\0" "uuml\0\0\0\0"
    "weierp\0\0" "xi\0\0\0\0\0\0" "yacute\0\0" "yen\0\0\0\0\0" "yuml\0\0\0\0" "zeta\0\0\0\0" "zwj\0\0\0\0\0"
    "zwnj\0\0\0\0";

static const uint16_t code_points[COUNT] = {
    0x00C6, 0x00C1, 0x00C2, 0x00C0, 0x0391, 0x00C5, 0x00C3, 0x00C4, 0x0392, 0x00C7, 0x03A7, 0x2021, 0x0394,
    0x00D0, 0x00C9, 0x00CA, 0x00C8, 0x0395, 0x0397, 0x00CB, 0x0393, 0x00CD, 0x00CE, 0x00CC, 0x0399, 0x00CF,
    0x039A, 0x039B, 0x039C, 0x00D1, 0x039D, 0x0152, 0x00D3, 0x00D4, 0x00D2, 0x03A9, 0x039F, 0x00D8, 0x00D5,
    0x00D6, 0x03A6, 0x03A0, 0x2033, 0x03A8, 0x03A1, 0x0160, 0x03A3, 0x00DE, 0x03A4, 0x0398, 0x00DA, 0x00DB,
    0x00D9, 0x03A5, 0x00DC, 0x039E, 0x00DD, 0x0178, 0x0396, 0x00E1, 0x00E2, 0x00B4, 0x00E6, 0x00E0, 0x2135,
    0x03B1, 0x0026, 0x2227, 0x2220, 0x0027, 0x00E5, 0x2248, 0x00E3, 0x00E4, 0x201E, 0x03B2, 0x00A6, 0x2022,
    0x2229, 0x00E7, 0x00B8, 0x00A2, 0x03C7, 0x02C6, 0x2663, 0x2245, 0x00A9, 0x21B5, 0x222A, 0x00A4, 0x21D3,
    0x2020, 0x2193, 0x00B0, 0x03B4, 0x2666, 0x00F7, 0x00E9, 0x00EA, 0x00E8, 0x2205, 0x2003, 0x2002, 0x03B5,
    0x2261, 0x03B7, 0x00F0, 0x00EB, 0x20AC, 0x2203, 0x0192, 0x2200, 0x00BD, 0x00BC, 0x00BE, 0x2044, 0x03B3,
    0x2265, 0x003E, 0x21D4, 0x2194, 0x2665, 0x2026, 0x00ED, 0x00EE, 0x00A1, 0x00EC, 0x2111, 0x221E, 0x222B,
    0x03B9, 0x00BF, 0x2208, 0x00EF, 0x03BA, 0x21D0, 0x03BB, 0x2329, 0x00AB, 0x2190, 0x2308, 0x201C, 0x2264,
    0x230A, 0x2217, 0x25CA, 0x200E, 0x2039, 0x2018, 0x003C, 0x00AF, 0x2014, 0x00B5, 0x00B7, 0x2212, 0x03BC,
    0x2207, 0x00A0, 0x2013, 0x2260, 0x220B, 0x00AC, 0x2209, 0x2284, 0x00F1, 0x03BD, 0x00F3, 0x00F4, 0x0153,
    0x00F2, 0x203E, 0x03C9, 0x03BF, 0x2295, 0x2228, 0x00AA, 0x00BA, 0x00F8, 0x00F5, 0x2297, 0x00F6, 0x00B6,
    0x2202, 0x2030, 0x22A5, 0x03C6, 0x03C0, 0x03D6, 0x00B1, 0x00A3, 0x2032, 0x220F, 0x221D, 0x03C8, 0x0022,
    0x21D2, 0x221A, 0x232A, 0x00BB, 0x2192, 0x2309, 0x201D, 0x211C, 0x00AE, 0x230B, 0x03C1, 0x200F, 0x203A,
    0x2019, 0x201A, 0x0161, 0x22C5, 0x00A7, 0x00AD, 0x03C3, 0x03C2, 0x223C, 0x2660, 0x2282, 0x2286, 0x2211,
    0x2283, 0x00B9, 0x00B2, 0x00B3, 0x2287, 0x00DF, 0x03C4, 0x2234, 0x03B8, 0x03D1, 0x2009, 0x00FE, 0x02DC,
    0x00D7, 0x2122, 0x21D1, 0x00FA, 0x2191, 0x00FB, 0x00F9, 0x00A8, 0x03D2, 0x03C5, 0x00FC, 0x2118, 0x03BE,
    0x00FD, 0x00A5, 0x00FF, 0x03B6, 0x200D, 0x200C,
};

bool bartap_xml_entity_lookup(const uint8_t *name, size_t length, uint32_t *code_point)
{
    if (length == 0U || length > NAME_SIZE) {
        return false;
    }
    size_t low = 0;
    size_t high = COUNT;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        const char *field = names + middle * NAME_SIZE;
        //byte order, as the table is sorted; a name that is a prefix of the field comes first
        int order = memcmp(name, field, length);
        if (order == 0 && length < NAME_SIZE && field[length] != '\0') {
            order = -1;
        }
        if (order == 0) {
            *code_point = code_points[middle];
            return true;
        }
        if (order < 0) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return false;
}
