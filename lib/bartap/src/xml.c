// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* A lenient pull tokenizer for container.xml, the OPF, the TOC and XHTML: start and end tags, text with
 * the XML and XHTML named entities (all 253) and numeric references expanded, comments, processing
 * instructions and DOCTYPEs skipped, CDATA as text. Malformed markup is recovered from and reported as
 * BARTAP_WARN_MARKUP; a byte that is not valid UTF-8 is read as Windows-1252 (BARTAP_WARN_ENCODING).
 *
 * TODO(rewrite): the entity table; the old one is lib/bartap/src/entities.c in this branch's history. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

void bartap_xml_init(bartap_xml_t *xml, bartap_xml_fill_fn fill, void *user)
{
    // TODO(rewrite)
    (void)xml;
    (void)fill;
    (void)user;
}

bartap_err_t bartap_xml_next(bartap_xml_t *xml, bartap_xml_token_t *token)
{
    // TODO(rewrite)
    (void)xml;
    (void)token;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_xml_attr(const bartap_xml_t *xml, const char *name, char *buf, size_t size,
    size_t *len)
{
    // TODO(rewrite)
    (void)xml;
    (void)name;
    (void)buf;
    (void)size;
    (void)len;
    return BARTAP_ERR_UNSUPPORTED;
}
