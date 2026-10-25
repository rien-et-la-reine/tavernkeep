// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The CFI dumper for cfi_oracle.py: runs the whole-book flow (../common/flow.h) over one EPUB and prints,
 * for every spine item and every item the text layer delivers, a tab-separated line:
 *
 *   SPINE  index  archive-path
 *   ITEM   spine-index  kind  cfi  text-as-hex
 *
 * kind is a bartap_text_item_kind_t; text is the word, or for element items the element's name. A last
 * line, RESULT, gives the flow's results and any invariant it found broken.
 *
 *   bartap_cfi_dump [--elements] [--strict] book.epub */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flow.h"

static void on_spine(void *context, uint32_t spine_index, const char *path)
{
    (void)context;
    printf("SPINE\t%u\t%s\n", spine_index, path);
}

static void on_item(void *context, uint32_t spine_index, const bartap_text_item_t *item, const char *cfi)
{
    (void)context;
    printf("ITEM\t%u\t%u\t%s\t", spine_index, item->kind, cfi);
    for (size_t i = 0; i < item->length; i++) {
        printf("%02x", item->text[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    flow_options_t options;
    flow_options_default(&options);
    options.resolve_every = 0;
    options.snapshot_check = false;
    options.on_item = on_item;
    options.on_spine = on_spine;
    int i = 1;
    for (; i < argc - 1; i++) {
        if (strcmp(argv[i], "--elements") == 0) {
            options.text_flags |= BARTAP_TEXT_DELIVER_ELEMENTS;
        } else if (strcmp(argv[i], "--strict") == 0) {
            options.text_flags |= BARTAP_TEXT_STRICT;
        } else {
            break;
        }
    }
    if (i != argc - 1) {
        fprintf(stderr, "usage: %s [--elements] [--strict] book.epub\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[i], "rb");
    if (f == NULL) {
        perror(argv[i]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(size > 0 ? (size_t)size : 1U);
    if (size < 0 || fread(data, 1, (size_t)size, f) != (size_t)size) {
        perror(argv[i]);
        return 2;
    }
    fclose(f);
    flow_report_t report;
    flow_run(data, (size_t)size, &options, &report);
    printf("RESULT\topen=%s\ttext=%s\tfailure=%s\n", flow_result_name(report.open), flow_result_name(report.text),
        report.failure);
    free(data);
    return report.failure[0] == '\0' ? 0 : 1;
}
