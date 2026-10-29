// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* A lenient pull tokenizer for container.xml, the OPF, the TOC and XHTML: start and end tags, text with
 * the XML and XHTML named entities (all 253) and numeric references expanded, comments, processing
 * instructions and DOCTYPEs skipped, CDATA as text. Malformed markup is recovered from and reported as
 * BARTAP_WARN_MARKUP; a byte that is not valid UTF-8 is read as Windows-1252 (BARTAP_WARN_ENCODING). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

typedef struct {
    char name[9];
    uint16_t codepoint;
} bartap_entity_t;

/* All 253 XML 1.0 and XHTML 1.0 named character entities (plus &apos;).
 * Every codepoint fits in uint16_t (largest is 0x2666, &diams;). */
static const bartap_entity_t bartap_entities[253] = {
    { "AElig", 0x00C6 },  { "Aacute", 0x00C1 }, { "Acirc", 0x00C2 },  { "Agrave", 0x00C0 },
    { "Alpha", 0x0391 },  { "Aring", 0x00C5 },  { "Atilde", 0x00C3 }, { "Auml", 0x00C4 },
    { "Beta", 0x0392 },   { "Ccedil", 0x00C7 }, { "Chi", 0x03A7 },    { "Dagger", 0x2021 },
    { "Delta", 0x0394 },  { "ETH", 0x00D0 },    { "Eacute", 0x00C9 }, { "Ecirc", 0x00CA },
    { "Egrave", 0x00C8 }, { "Epsilon", 0x0395 },{ "Eta", 0x0397 },    { "Euml", 0x00CB },
    { "Gamma", 0x0393 },  { "Iacute", 0x00CD }, { "Icirc", 0x00CE },  { "Igrave", 0x00CC },
    { "Iota", 0x0399 },   { "Iuml", 0x00CF },   { "Kappa", 0x039A },  { "Lambda", 0x039B },
    { "Mu", 0x039C },     { "Ntilde", 0x00D1 }, { "Nu", 0x039D },     { "OElig", 0x0152 },
    { "Oacute", 0x00D3 }, { "Ocirc", 0x00D4 },  { "Ograve", 0x00D2 }, { "Omega", 0x03A9 },
    { "Omicron", 0x039F },{ "Oslash", 0x00D8 }, { "Otilde", 0x00D5 }, { "Ouml", 0x00D6 },
    { "Phi", 0x03A6 },    { "Pi", 0x03A0 },     { "Prime", 0x2033 },  { "Psi", 0x03A8 },
    { "Rho", 0x03A1 },    { "Scaron", 0x0160 }, { "Sigma", 0x03A3 },  { "THORN", 0x00DE },
    { "Tau", 0x03A4 },    { "Theta", 0x0398 },  { "Uacute", 0x00DA }, { "Ucirc", 0x00DB },
    { "Ugrave", 0x00D9 }, { "Upsilon", 0x03A5 },{ "Uuml", 0x00DC },   { "Xi", 0x039E },
    { "Yacute", 0x00DD }, { "Yuml", 0x0178 },   { "Zeta", 0x0396 },   { "aacute", 0x00E1 },
    { "acirc", 0x00E2 },  { "acute", 0x00B4 },  { "aelig", 0x00E6 },  { "agrave", 0x00E0 },
    { "alefsym", 0x2135 },{ "alpha", 0x03B1 },  { "amp", 0x0026 },    { "and", 0x2227 },
    { "ang", 0x2220 },    { "apos", 0x0027 },   { "aring", 0x00E5 },  { "asymp", 0x2248 },
    { "atilde", 0x00E3 }, { "auml", 0x00E4 },   { "bdquo", 0x201E },  { "beta", 0x03B2 },
    { "brvbar", 0x00A6 }, { "bull", 0x2022 },   { "cap", 0x2229 },    { "ccedil", 0x00E7 },
    { "cedil", 0x00B8 },  { "cent", 0x00A2 },   { "chi", 0x03C7 },    { "circ", 0x02C6 },
    { "clubs", 0x2663 },  { "cong", 0x2245 },   { "copy", 0x00A9 },   { "crarr", 0x21B5 },
    { "cup", 0x222A },    { "curren", 0x00A4 }, { "dArr", 0x21D3 },   { "dagger", 0x2020 },
    { "darr", 0x2193 },   { "deg", 0x00B0 },    { "delta", 0x03B4 },  { "diams", 0x2666 },
    { "divide", 0x00F7 }, { "eacute", 0x00E9 }, { "ecirc", 0x00EA },  { "egrave", 0x00E8 },
    { "empty", 0x2205 },  { "emsp", 0x2003 },   { "ensp", 0x2002 },   { "epsilon", 0x03B5 },
    { "equiv", 0x2261 },  { "eta", 0x03B7 },    { "eth", 0x00F0 },    { "euml", 0x00EB },
    { "euro", 0x20AC },   { "exist", 0x2203 },  { "fnof", 0x0192 },   { "forall", 0x2200 },
    { "frac12", 0x00BD }, { "frac14", 0x00BC }, { "frac34", 0x00BE }, { "frasl", 0x2044 },
    { "gamma", 0x03B3 },  { "ge", 0x2265 },     { "gt", 0x003E },     { "hArr", 0x21D4 },
    { "harr", 0x2194 },   { "hearts", 0x2665 }, { "hellip", 0x2026 }, { "iacute", 0x00ED },
    { "icirc", 0x00EE },  { "iexcl", 0x00A1 },  { "igrave", 0x00EC }, { "image", 0x2111 },
    { "infin", 0x221E },  { "int", 0x222B },    { "iota", 0x03B9 },   { "iquest", 0x00BF },
    { "isin", 0x2208 },   { "iuml", 0x00EF },   { "kappa", 0x03BA },  { "lArr", 0x21D0 },
    { "lambda", 0x03BB }, { "lang", 0x2329 },   { "laquo", 0x00AB },  { "larr", 0x2190 },
    { "lceil", 0x2308 },  { "ldquo", 0x201C },  { "le", 0x2264 },     { "lfloor", 0x230A },
    { "lowast", 0x2217 }, { "loz", 0x25CA },    { "lrm", 0x200E },    { "lsaquo", 0x2039 },
    { "lsquo", 0x2018 },  { "lt", 0x003C },     { "macr", 0x00AF },   { "mdash", 0x2014 },
    { "micro", 0x00B5 },  { "middot", 0x00B7 }, { "minus", 0x2212 },  { "mu", 0x03BC },
    { "nabla", 0x2207 },  { "nbsp", 0x00A0 },   { "ndash", 0x2013 },  { "ne", 0x2260 },
    { "ni", 0x220B },     { "not", 0x00AC },    { "notin", 0x2209 },  { "nsub", 0x2284 },
    { "ntilde", 0x00F1 }, { "nu", 0x03BD },     { "oacute", 0x00F3 }, { "ocirc", 0x00F4 },
    { "oelig", 0x0153 },  { "ograve", 0x00F2 }, { "oline", 0x203E },  { "omega", 0x03C9 },
    { "omicron", 0x03BF },{ "oplus", 0x2295 },  { "or", 0x2228 },     { "ordf", 0x00AA },
    { "ordm", 0x00BA },   { "oslash", 0x00F8 }, { "otilde", 0x00F5 }, { "otimes", 0x2297 },
    { "ouml", 0x00F6 },   { "para", 0x00B6 },   { "part", 0x2202 },   { "permil", 0x2030 },
    { "perp", 0x22A5 },   { "phi", 0x03C6 },    { "pi", 0x03C0 },     { "piv", 0x03D6 },
    { "plusmn", 0x00B1 }, { "pound", 0x00A3 },  { "prime", 0x2032 },  { "prod", 0x220F },
    { "prop", 0x221D },   { "psi", 0x03C8 },    { "quot", 0x0022 },   { "rArr", 0x21D2 },
    { "radic", 0x221A },  { "rang", 0x232A },   { "raquo", 0x00BB },  { "rarr", 0x2192 },
    { "rceil", 0x2309 },  { "rdquo", 0x201D },  { "real", 0x211C },   { "reg", 0x00AE },
    { "rfloor", 0x230B }, { "rho", 0x03C1 },    { "rlm", 0x200F },    { "rsaquo", 0x203A },
    { "rsquo", 0x2019 },  { "sbquo", 0x201A },  { "scaron", 0x0161 }, { "sdot", 0x22C5 },
    { "sect", 0x00A7 },   { "shy", 0x00AD },    { "sigma", 0x03C3 },  { "sigmaf", 0x03C2 },
    { "sim", 0x223C },    { "spades", 0x2660 }, { "sub", 0x2282 },    { "sube", 0x2286 },
    { "sum", 0x2211 },    { "sup", 0x2283 },    { "sup1", 0x00B9 },   { "sup2", 0x00B2 },
    { "sup3", 0x00B3 },   { "supe", 0x2287 },   { "szlig", 0x00DF },  { "tau", 0x03C4 },
    { "there4", 0x2234 }, { "theta", 0x03B8 },  { "thetasym", 0x03D1 },{ "thinsp", 0x2009 },
    { "thorn", 0x00FE },  { "tilde", 0x02DC },  { "times", 0x00D7 },  { "trade", 0x2122 },
    { "uArr", 0x21D1 },   { "uacute", 0x00FA }, { "uarr", 0x2191 },   { "ucirc", 0x00FB },
    { "ugrave", 0x00F9 }, { "uml", 0x00A8 },    { "upsih", 0x03D2 },  { "upsilon", 0x03C5 },
    { "uuml", 0x00FC },   { "weierp", 0x2118 }, { "xi", 0x03BE },     { "yacute", 0x00FD },
    { "yen", 0x00A5 },    { "yuml", 0x00FF },   { "zeta", 0x03B6 },   { "zwj", 0x200D },
    { "zwnj", 0x200C },
};

/* Windows-1252 mapping for 0x80..0x9F; undefined slots (0x81, 0x8D, 0x8F, 0x90, 0x9D) map to U+FFFD. */
static const uint16_t bartap_cp1252_table[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

static char bartap_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool bartap_is_xml_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bartap_err_t bartap_xml_ensure(bartap_xml_t *xml, size_t min_bytes)
{
    if (xml->fill == NULL) {
        return BARTAP_OK;
    }
    if ((size_t)(xml->in_len - xml->in_pos) >= min_bytes) {
        return BARTAP_OK;
    }
    if (xml->in_pos > 0U) {
        const size_t rem = (size_t)(xml->in_len - xml->in_pos);
        if (rem > 0U) {
            memmove(xml->in, xml->in + xml->in_pos, rem);
        }
        xml->in_len = (uint16_t)rem;
        xml->in_pos = 0U;
    }
    while ((size_t)xml->in_len < min_bytes && xml->in_len < BARTAP_XML_IN_SIZE) {
        size_t got = 0U;
        const bartap_err_t err = xml->fill(xml->user, xml->in + xml->in_len,
            (size_t)(BARTAP_XML_IN_SIZE - xml->in_len), &got);
        if (err != BARTAP_OK) {
            return err;
        }
        if (got == 0U) {
            break;
        }
        xml->in_len = (uint16_t)(xml->in_len + (uint16_t)got);
    }
    return BARTAP_OK;
}

static int bartap_xml_peek(bartap_xml_t *xml, size_t lookahead, bartap_err_t *err)
{
    if ((size_t)(xml->in_len - xml->in_pos) <= lookahead) {
        const size_t want = lookahead + 1U > BARTAP_ANCHOR_SIZE * 2U
            ? lookahead + 1U
            : BARTAP_ANCHOR_SIZE * 2U;
        *err = bartap_xml_ensure(xml, want);
        if (*err != BARTAP_OK) {
            return -1;
        }
    }
    if ((size_t)(xml->in_len - xml->in_pos) <= lookahead) {
        return -1;
    }
    return (int)xml->in[xml->in_pos + lookahead];
}

static uint8_t bartap_xml_take(bartap_xml_t *xml)
{
    const uint8_t b = xml->in[xml->in_pos++];
    xml->offset++;
    return b;
}

static size_t bartap_xml_encode_utf8(uint32_t cp, char *out)
{
    if (cp == 0U || (cp >= 0xD800UL && cp <= 0xDFFFUL) || cp > 0x10FFFFUL) {
        cp = 0xFFFDUL;
    }
    if (cp < 0x80UL) {
        out[0] = (char)cp;
        return 1U;
    }
    if (cp < 0x800UL) {
        out[0] = (char)(0xC0U | (cp >> 6U));
        out[1] = (char)(0x80U | (cp & 0x3FU));
        return 2U;
    }
    if (cp < 0x10000UL) {
        out[0] = (char)(0xE0U | (cp >> 12U));
        out[1] = (char)(0x80U | ((cp >> 6U) & 0x3FU));
        out[2] = (char)(0x80U | (cp & 0x3FU));
        return 3U;
    }
    out[0] = (char)(0xF0U | (cp >> 18U));
    out[1] = (char)(0x80U | ((cp >> 12U) & 0x3FU));
    out[2] = (char)(0x80U | ((cp >> 6U) & 0x3FU));
    out[3] = (char)(0x80U | (cp & 0x3FU));
    return 4U;
}

static uint32_t bartap_cp1252_to_codepoint(uint8_t b)
{
    if (b >= 0x80U && b <= 0x9FU) {
        return bartap_cp1252_table[b - 0x80U];
    }
    return b;
}

static bool bartap_xml_lookup_entity(const char *body, size_t len, uint32_t *cp)
{
    if (len == 0U) {
        return false;
    }
    if (body[0] == '#') {
        if (len < 2U) {
            return false;
        }
        uint32_t val = 0U;
        if (body[1] == 'x' || body[1] == 'X') {
            if (len < 3U) {
                return false;
            }
            for (size_t i = 2U; i < len; i++) {
                const char c = body[i];
                uint32_t d;
                if (c >= '0' && c <= '9') {
                    d = (uint32_t)(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    d = (uint32_t)(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    d = (uint32_t)(c - 'A' + 10);
                } else {
                    return false;
                }
                val = (val << 4U) | d;
                if (val > 0x10FFFFUL) {
                    val = 0x110000UL;
                }
            }
        } else {
            for (size_t i = 1U; i < len; i++) {
                const char c = body[i];
                if (c < '0' || c > '9') {
                    return false;
                }
                val = val * 10U + (uint32_t)(c - '0');
                if (val > 0x10FFFFUL) {
                    val = 0x110000UL;
                }
            }
        }
        *cp = val;
        return true;
    }

    size_t lo = 0U;
    size_t hi = sizeof(bartap_entities) / sizeof(bartap_entities[0]);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const char *const name = bartap_entities[mid].name;
        const size_t nlen = strlen(name);
        const size_t cmp_len = len < nlen ? len : nlen;
        int cmp = memcmp(body, name, cmp_len);
        if (cmp == 0) {
            if (len < nlen) {
                cmp = -1;
            } else if (len > nlen) {
                cmp = 1;
            }
        }
        if (cmp == 0) {
            *cp = bartap_entities[mid].codepoint;
            return true;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1U;
        }
    }
    return false;
}

static void bartap_xml_local_lower(char *dst, size_t cap, const char *raw, size_t len)
{
    size_t start = 0U;
    for (size_t i = 0U; i < len; i++) {
        if (raw[i] == ':') {
            start = i + 1U;
        }
    }
    size_t out_len = 0U;
    for (size_t i = start; i < len && out_len + 1U < cap; i++) {
        dst[out_len++] = bartap_ascii_lower(raw[i]);
    }
    dst[out_len] = '\0';
}

void bartap_xml_init(bartap_xml_t *xml, bartap_xml_fill_fn fill, void *user)
{
    if (xml == NULL) {
        return;
    }
    memset(xml, 0, sizeof *xml);
    xml->fill = fill;
    xml->user = user;
}

static bartap_err_t bartap_xml_read_char(bartap_xml_t *xml, bool expand_entities, char out[4],
    size_t *out_len)
{
    bartap_err_t err = BARTAP_OK;
    const int c0 = bartap_xml_peek(xml, 0U, &err);
    if (err != BARTAP_OK) {
        return err;
    }
    if (c0 < 0) {
        *out_len = 0U;
        return BARTAP_OK;
    }

    if (expand_entities && c0 == '&') {
        char body[12];
        size_t blen = 0U;
        bool has_semi = false;
        for (size_t i = 1U; i <= sizeof(body); i++) {
            const int ch = bartap_xml_peek(xml, i, &err);
            if (err != BARTAP_OK) {
                return err;
            }
            if (ch < 0 || ch == '<' || ch == '&' || bartap_is_xml_space(ch)) {
                break;
            }
            if (ch == ';') {
                has_semi = true;
                break;
            }
            body[blen++] = (char)ch;
        }
        uint32_t cp = 0U;
        if (has_semi && bartap_xml_lookup_entity(body, blen, &cp)) {
            for (size_t i = 0U; i < blen + 2U; i++) {
                (void)bartap_xml_take(xml);
            }
            *out_len = bartap_xml_encode_utf8(cp, out);
            return BARTAP_OK;
        }
        xml->warnings |= BARTAP_WARN_MARKUP;
        (void)bartap_xml_take(xml);
        out[0] = '&';
        *out_len = 1U;
        return BARTAP_OK;
    }

    const uint8_t b0 = (uint8_t)c0;
    if (b0 < 0x80U) {
        (void)bartap_xml_take(xml);
        *out_len = bartap_xml_encode_utf8(b0, out);
        return BARTAP_OK;
    }

    size_t seq = 0U;
    uint32_t cp = 0U;
    uint32_t min_cp = 0U;
    if ((b0 & 0xE0U) == 0xC0U) {
        seq = 2U;
        cp = b0 & 0x1FU;
        min_cp = 0x80UL;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        seq = 3U;
        cp = b0 & 0x0FU;
        min_cp = 0x800UL;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        seq = 4U;
        cp = b0 & 0x07U;
        min_cp = 0x10000UL;
    }

    bool valid = (seq >= 2U);
    for (size_t i = 1U; valid && i < seq; i++) {
        const int ci = bartap_xml_peek(xml, i, &err);
        if (err != BARTAP_OK) {
            return err;
        }
        if (ci < 0 || ((uint8_t)ci & 0xC0U) != 0x80U) {
            valid = false;
            break;
        }
        cp = (cp << 6U) | ((uint8_t)ci & 0x3FU);
    }
    if (valid && (cp < min_cp || (cp >= 0xD800UL && cp <= 0xDFFFUL) || cp > 0x10FFFFUL)) {
        valid = false;
    }

    if (valid) {
        for (size_t i = 0U; i < seq; i++) {
            out[i] = (char)bartap_xml_take(xml);
        }
        *out_len = seq;
        return BARTAP_OK;
    }

    xml->warnings |= BARTAP_WARN_ENCODING;
    (void)bartap_xml_take(xml);
    *out_len = bartap_xml_encode_utf8(bartap_cp1252_to_codepoint(b0), out);
    return BARTAP_OK;
}

/* Attribute IDs are stored as one byte followed by a NUL-terminated raw value. Unknown
 * attributes are scanned but never stored, so a long namespace/prefix declaration cannot hide id. */
static const char bartap_attr_names[][18] = {
    "full-path", "unique-identifier", "id", "name", "content", "toc", "idref", "linear",
    "href", "properties", "media-type", "type", "src"
};

static unsigned bartap_attr_id(const char *name)
{
    for (unsigned i = 0U; i < sizeof bartap_attr_names / sizeof bartap_attr_names[0]; i++) {
        if (strcmp(name, bartap_attr_names[i]) == 0) {
            return i + 1U;
        }
    }
    return 0U;
}

static bartap_err_t bartap_xml_read_attrs(bartap_xml_t *xml, bool is_end, bool *self_closing)
{
    bartap_err_t err = BARTAP_OK;
    xml->attrs_len = 0U;
    xml->attrs_overflow = 0U;
    for (;;) {
        int c = bartap_xml_peek(xml, 0U, &err);
        if (err != BARTAP_OK) {
            return err;
        }
        if (c < 0 || c == '<') {
            xml->warnings |= BARTAP_WARN_MARKUP;
            return BARTAP_OK;
        }
        if (c == '>') {
            (void)bartap_xml_take(xml);
            return BARTAP_OK;
        }
        if (c == '/' && bartap_xml_peek(xml, 1U, &err) == '>') {
            (void)bartap_xml_take(xml);
            (void)bartap_xml_take(xml);
            *self_closing = !is_end;
            return BARTAP_OK;
        }
        if (bartap_is_xml_space(c) || c == '/') {
            (void)bartap_xml_take(xml);
            continue;
        }

        char key[BARTAP_XML_NAME_MAX + 1];
        size_t klen = 0U;
        bool key_long = false;
        while ((c = bartap_xml_peek(xml, 0U, &err)) >= 0 && err == BARTAP_OK
                && !bartap_is_xml_space(c) && c != '=' && c != '>' && c != '<' && c != '/') {
            (void)bartap_xml_take(xml);
            if (c == ':') {             // local name, as elsewhere in this tokenizer
                klen = 0U;
                key_long = false;
            } else if (klen < BARTAP_XML_NAME_MAX) {
                key[klen++] = bartap_ascii_lower((char)c);
            } else {
                key_long = true;
            }
        }
        key[klen] = '\0';
        while (bartap_is_xml_space(c) && err == BARTAP_OK) {
            (void)bartap_xml_take(xml);
            c = bartap_xml_peek(xml, 0U, &err);
        }
        if (err != BARTAP_OK) {
            return err;
        }
        if (c != '=') {
            xml->warnings |= BARTAP_WARN_MARKUP;
            continue;
        }
        (void)bartap_xml_take(xml);
        while (bartap_is_xml_space(c = bartap_xml_peek(xml, 0U, &err)) && err == BARTAP_OK) {
            (void)bartap_xml_take(xml);
        }
        const int quote = c == '\'' || c == '"' ? c : 0;
        if (quote != 0) {
            (void)bartap_xml_take(xml);
        } else {
            xml->warnings |= BARTAP_WARN_MARKUP;
        }
        const unsigned id = is_end || key_long ? 0U : bartap_attr_id(key);
        const size_t start = xml->attrs_len;
        size_t used = start;
        bool overflow = false;
        if (id != 0U && used < BARTAP_XML_ATTRS_MAX) {
            xml->attrs[used++] = (char)id;
        } else if (id != 0U) {
            overflow = true;
        }
        size_t value_len = 0U;
        while ((c = bartap_xml_peek(xml, 0U, &err)) >= 0 && err == BARTAP_OK) {
            if ((quote != 0 && c == quote) || (quote == 0 &&
                    (bartap_is_xml_space(c) || c == '>' || c == '<'
                     || (c == '/' && bartap_xml_peek(xml, 1U, &err) == '>')))) {
                break;
            }
            (void)bartap_xml_take(xml);
            if (id != 0U) {
                if (used < BARTAP_XML_ATTRS_MAX && value_len < BARTAP_PATH_MAX && c != 0) {
                    xml->attrs[used++] = (char)c;
                    value_len++;
                } else {
                    overflow = true;
                }
            }
        }
        if (err != BARTAP_OK) {
            return err;
        }
        if (quote != 0 && c == quote) {
            (void)bartap_xml_take(xml);
        } else if (c < 0) {
            xml->warnings |= BARTAP_WARN_MARKUP;
            overflow = true;
        }
        if (id != 0U) {
            if (overflow) {
                xml->warnings |= BARTAP_WARN_MARKUP;
                xml->attrs_overflow |= 1UL << id;
            } else {
                xml->attrs[used++] = '\0';
                xml->attrs_len = (uint16_t)used;
            }
        }
    }
}

bartap_err_t bartap_xml_next(bartap_xml_t *xml, bartap_xml_token_t *token)
{
    if (xml == NULL || token == NULL) {
        return BARTAP_ERR_ARG;
    }

    if (xml->pending_end_tag) {
        xml->pending_end_tag = false;
        token->type = BARTAP_XML_END_TAG;
        token->offset = xml->pending_offset;
        token->text = NULL;
        token->text_len = 0U;
        return BARTAP_OK;
    }

    for (;;) {
        bartap_err_t err = bartap_xml_ensure(xml, BARTAP_ANCHOR_SIZE * 2U);
        if (err != BARTAP_OK) {
            return err;
        }

        const size_t available = (size_t)(xml->in_len - xml->in_pos);
        xml->anchor_len = (uint8_t)(available < BARTAP_ANCHOR_SIZE ? available : BARTAP_ANCHOR_SIZE);
        memcpy(xml->anchor, xml->in + xml->in_pos, xml->anchor_len);

        if (xml->in_cdata) {
            const uint32_t cdata_off = xml->offset;
            size_t tlen = 0U;
            int d0;
            while (tlen + 4U <= BARTAP_WORD_MAX
                    && (d0 = bartap_xml_peek(xml, 0U, &err)) >= 0 && err == BARTAP_OK) {
                if (d0 == ']' && bartap_xml_peek(xml, 1U, &err) == ']'
                        && bartap_xml_peek(xml, 2U, &err) == '>') {
                    (void)bartap_xml_take(xml);
                    (void)bartap_xml_take(xml);
                    (void)bartap_xml_take(xml);
                    xml->in_cdata = false;
                    break;
                }
                char enc[4];
                size_t elen = 0U;
                err = bartap_xml_read_char(xml, false, enc, &elen);
                if (err != BARTAP_OK) {
                    return err;
                }
                memcpy(xml->text + tlen, enc, elen);
                tlen += elen;
                if (tlen >= 32U && (enc[0] == ' ' || enc[0] == '\t' || enc[0] == '\n')) {
                    break;
                }
            }
            if (err != BARTAP_OK) {
                return err;
            }
            if (d0 < 0) {
                xml->warnings |= BARTAP_WARN_MARKUP;
                xml->in_cdata = false;
            }
            if (tlen == 0U && !xml->in_cdata) {
                continue;
            }
            xml->text[tlen] = '\0';
            token->type = BARTAP_XML_TEXT;
            token->offset = cdata_off;
            token->text = xml->text;
            token->text_len = tlen;
            return BARTAP_OK;
        }

        if (xml->offset == 0U && (size_t)(xml->in_len - xml->in_pos) >= 3U
                && xml->in[xml->in_pos] == 0xEFU
                && xml->in[xml->in_pos + 1U] == 0xBBU
                && xml->in[xml->in_pos + 2U] == 0xBFU) {
            (void)bartap_xml_take(xml);
            (void)bartap_xml_take(xml);
            (void)bartap_xml_take(xml);
            continue;
        }

        const int c0 = bartap_xml_peek(xml, 0U, &err);
        if (err != BARTAP_OK) {
            return err;
        }
        if (c0 < 0) {
            token->type = BARTAP_XML_END;
            token->offset = xml->offset;
            token->text = NULL;
            token->text_len = 0U;
            return BARTAP_OK;
        }

        const uint32_t tok_off = xml->offset;

        if (c0 == '<') {
            const int c1 = bartap_xml_peek(xml, 1U, &err);
            if (err != BARTAP_OK) {
                return err;
            }

            if (c1 == '?') {
                (void)bartap_xml_take(xml);
                (void)bartap_xml_take(xml);
                bool closed = false;
                int ch;
                while ((ch = bartap_xml_peek(xml, 0U, &err)) >= 0 && err == BARTAP_OK) {
                    (void)bartap_xml_take(xml);
                    if (ch == '?' && bartap_xml_peek(xml, 0U, &err) == '>') {
                        (void)bartap_xml_take(xml);
                        closed = true;
                        break;
                    }
                    if (ch == '>') {
                        xml->warnings |= BARTAP_WARN_MARKUP;
                        closed = true;
                        break;
                    }
                }
                if (err != BARTAP_OK) {
                    return err;
                }
                if (!closed) {
                    xml->warnings |= BARTAP_WARN_MARKUP;
                }
                continue;
            }

            if (c1 == '!') {
                (void)bartap_xml_take(xml);
                (void)bartap_xml_take(xml);
                int d0 = bartap_xml_peek(xml, 0U, &err);
                const int d1 = bartap_xml_peek(xml, 1U, &err);
                if (err != BARTAP_OK) {
                    return err;
                }
                if (d0 == '-' && d1 == '-') {
                    (void)bartap_xml_take(xml);
                    (void)bartap_xml_take(xml);
                    bool closed = false;
                    while ((d0 = bartap_xml_peek(xml, 0U, &err)) >= 0 && err == BARTAP_OK) {
                        (void)bartap_xml_take(xml);
                        if (d0 == '-' && bartap_xml_peek(xml, 0U, &err) == '-'
                                && bartap_xml_peek(xml, 1U, &err) == '>') {
                            (void)bartap_xml_take(xml);
                            (void)bartap_xml_take(xml);
                            closed = true;
                            break;
                        }
                    }
                    if (err != BARTAP_OK) {
                        return err;
                    }
                    if (!closed) {
                        xml->warnings |= BARTAP_WARN_MARKUP;
                    }
                    continue;
                }

                if (d0 == '[' && d1 == 'C'
                        && bartap_xml_peek(xml, 2U, &err) == 'D'
                        && bartap_xml_peek(xml, 3U, &err) == 'A'
                        && bartap_xml_peek(xml, 4U, &err) == 'T'
                        && bartap_xml_peek(xml, 5U, &err) == 'A'
                        && bartap_xml_peek(xml, 6U, &err) == '[') {
                    for (int i = 0; i < 7; i++) {
                        (void)bartap_xml_take(xml);
                    }
                    xml->in_cdata = true;
                    continue;
                }

                int bracket_depth = 0;
                while ((d0 = bartap_xml_peek(xml, 0U, &err)) >= 0 && err == BARTAP_OK) {
                    (void)bartap_xml_take(xml);
                    if (d0 == '[') {
                        bracket_depth++;
                    } else if (d0 == ']' && bracket_depth > 0) {
                        bracket_depth--;
                    } else if (d0 == '>' && bracket_depth == 0) {
                        break;
                    }
                }
                if (err != BARTAP_OK) {
                    return err;
                }
                continue;
            }

            const bool is_end = (c1 == '/');
            const int name_first = is_end ? bartap_xml_peek(xml, 2U, &err) : c1;
            if (err != BARTAP_OK) {
                return err;
            }
            const bool valid_start = (name_first >= 'a' && name_first <= 'z')
                || (name_first >= 'A' && name_first <= 'Z')
                || name_first == '_';
            if (!valid_start) {
                xml->warnings |= BARTAP_WARN_MARKUP;
                (void)bartap_xml_take(xml);
                xml->text[0] = '<';
                xml->text[1] = '\0';
                token->type = BARTAP_XML_TEXT;
                token->offset = tok_off;
                token->text = xml->text;
                token->text_len = 1U;
                return BARTAP_OK;
            }

            (void)bartap_xml_take(xml);
            if (is_end) {
                (void)bartap_xml_take(xml);
            }

            char raw_name[64];
            size_t rlen = 0U;
            int ch;
            while ((ch = bartap_xml_peek(xml, 0U, &err)) >= 0 && err == BARTAP_OK) {
                if (ch == '>' || ch == '/' || ch == '<' || bartap_is_xml_space(ch)) {
                    break;
                }
                if (rlen + 1U < sizeof(raw_name)) {
                    raw_name[rlen++] = (char)ch;
                }
                (void)bartap_xml_take(xml);
            }
            if (err != BARTAP_OK) {
                return err;
            }

            bartap_xml_local_lower(xml->name, sizeof(xml->name), raw_name, rlen);
            bool self_closing = false;
            err = bartap_xml_read_attrs(xml, is_end, &self_closing);
            if (err != BARTAP_OK) {
                return err;
            }

            token->type = is_end ? BARTAP_XML_END_TAG : BARTAP_XML_START_TAG;
            token->offset = tok_off;
            token->text = NULL;
            token->text_len = 0U;

            if (self_closing) {
                xml->pending_end_tag = true;
                xml->pending_offset = tok_off;
            }
            return BARTAP_OK;
        }

        size_t tlen = 0U;
        bool in_word = false;
        while (tlen < BARTAP_WORD_MAX) {
            const int pk = bartap_xml_peek(xml, 0U, &err);
            if (err != BARTAP_OK) {
                return err;
            }
            if (pk < 0 || pk == '<') {
                break;
            }
            if (in_word && bartap_is_xml_space(pk)) {
                break;
            }
            if (tlen + 4U > BARTAP_WORD_MAX && (pk >= 0x80 || pk == '&')) {
                break;
            }
            char enc[4];
            size_t elen = 0U;
            err = bartap_xml_read_char(xml, true, enc, &elen);
            if (err != BARTAP_OK) {
                return err;
            }
            if (elen == 0U) {
                break;
            }
            memcpy(xml->text + tlen, enc, elen);
            tlen += elen;
            if (!bartap_is_xml_space((uint8_t)enc[0])) {
                in_word = true;
            } else if (tlen >= 32U) {
                break;
            }
        }
        xml->text[tlen] = '\0';
        token->type = BARTAP_XML_TEXT;
        token->offset = tok_off;
        token->text = xml->text;
        token->text_len = tlen;
        return BARTAP_OK;
    }
}

static size_t bartap_xml_decode_attr_value(const char *val, size_t vlen, char *buf, size_t size)
{
    size_t out_pos = 0U;
    size_t i = 0U;
    while (i < vlen) {
        char enc[4];
        size_t elen = 0U;
        if (val[i] == '&') {
            size_t semi = i + 1U;
            while (semi < vlen && semi - i <= 12U && val[semi] != ';') {
                semi++;
            }
            uint32_t cp = 0U;
            if (semi < vlen && val[semi] == ';'
                    && bartap_xml_lookup_entity(val + i + 1U, semi - (i + 1U), &cp)) {
                elen = bartap_xml_encode_utf8(cp, enc);
                i = semi + 1U;
            } else {
                enc[0] = '&';
                elen = 1U;
                i++;
            }
        } else {
            enc[0] = val[i++];
            elen = 1U;
        }
        if (buf != NULL && size > 0U && out_pos + elen < size) {
            memcpy(buf + out_pos, enc, elen);
        }
        out_pos += elen;
    }
    if (buf != NULL && size > 0U) {
        buf[out_pos < size ? out_pos : 0U] = '\0';
    }
    return out_pos;
}

bartap_err_t bartap_xml_attr(bartap_xml_t *xml, const char *name, char *buf, size_t size,
    size_t *len)
{
    if (xml == NULL || name == NULL || len == NULL || (buf == NULL && size > 0U)) {
        return BARTAP_ERR_ARG;
    }
    *len = 0U;
    if (size > 0U) {
        buf[0] = '\0';
    }
    const unsigned id = bartap_attr_id(name);
    if (id == 0U) {
        return BARTAP_ERR_NOT_FOUND;
    }
    if ((xml->attrs_overflow & (1UL << id)) != 0U) {
        return BARTAP_ERR_LIMIT;
    }
    size_t pos = 0U;
    while (pos < xml->attrs_len) {
        const unsigned found = (uint8_t)xml->attrs[pos++];
        const size_t n = strlen(xml->attrs + pos);
        if (found == id) {
            *len = bartap_xml_decode_attr_value(xml->attrs + pos, n, buf, size);
            if (*len >= size) {
                xml->warnings |= BARTAP_WARN_MARKUP;
                return BARTAP_ERR_LIMIT;
            }
            return BARTAP_OK;
        }
        pos += n + 1U;
    }
    return BARTAP_ERR_NOT_FOUND;
}
