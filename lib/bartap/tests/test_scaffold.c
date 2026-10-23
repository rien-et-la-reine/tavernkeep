//placeholder case for the EPUB host tests: it checks only that the build wiring is in place, that is,
//the committed fixture and the generated inputs are where the build says they are. replace or delete it
//once real cases exist

#include <stdio.h>

#include "harness.h"

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    fclose(file);
    return true;
}

static void scaffold_fixtures_are_in_place(void)
{
    EXPECT(file_exists(EPUB_TESTS_FIXTURE_DIR "/pride-and-prejudice.epub"));
    EXPECT(file_exists(EPUB_TESTS_GENERATED_DIR "/manifest.json"));
}

const test_case_t scaffold_tests[] = {
    TEST_CASE(scaffold_fixtures_are_in_place),
};
const size_t scaffold_test_count = sizeof scaffold_tests / sizeof scaffold_tests[0];
