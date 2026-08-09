#include "enhancement_registry.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void expect(int c, const char *w) {
    if (!c) { printf("FAIL %s\n", w); failures++; } else printf("ok   %s\n", w);
}

static void test_every_row_is_complete(void) {
    for (int i = 0; i < mdkr_enhancement_count(); i++) {
        const MdkrEnhancement *e = mdkr_enhancement_at(i);
        char what[128];
        snprintf(what, sizeof what, "row %d has a label", i);
        expect(e->label && e->label[0], what);
        snprintf(what, sizeof what, "row %d has help text", i);
        expect(e->help && e->help[0], what);
        snprintf(what, sizeof what, "row %d declares an authority class", i);
        expect(e->authority == MDKR_ENH_PRESENTATION ||
               e->authority == MDKR_ENH_GAMEPLAY, what);
    }
}

static void test_keys_are_unique(void) {
    for (int i = 0; i < mdkr_enhancement_count(); i++)
        for (int j = i + 1; j < mdkr_enhancement_count(); j++)
            expect(mdkr_enhancement_at(i)->key != mdkr_enhancement_at(j)->key,
                   "no two enhancements share a config key");
}

static void test_lookup_round_trips(void) {
    for (int i = 0; i < mdkr_enhancement_count(); i++) {
        const MdkrEnhancement *e = mdkr_enhancement_at(i);
        expect(mdkr_enhancement_for_key(e->key) == e,
               "for_key returns the same row at() did");
    }
    expect(mdkr_enhancement_for_key(MDKR_VIDEO_RENDER_SCALE) == NULL,
           "a non-enhancement key returns NULL rather than a neighbour");
}

/* The registry is one of two independent lists of the same set of keys — the
 * other is mdkr_video_key_is_enhancement(). Nothing enforces they agree except
 * this test, so every key from 0 to MDKR_VIDEO_KEY_COUNT is checked both ways:
 * present in the registry if and only if the predicate says so. */
static void test_registry_matches_is_enhancement_predicate(void) {
    for (int k = 0; k < MDKR_VIDEO_KEY_COUNT; k++) {
        const MdkrVideoKey key = (MdkrVideoKey)k;
        const int in_registry = mdkr_enhancement_for_key(key) != NULL;
        const int predicate = mdkr_video_key_is_enhancement(key) != 0;
        char what[128];
        snprintf(what, sizeof what,
                 "key %d: registry membership agrees with "
                 "mdkr_video_key_is_enhancement()", k);
        expect(in_registry == predicate, what);
    }
}

/* Each row must point at a real Enhancements.* schema entry, so a future
 * append that mis-targets the wrong enum member is caught here rather than by
 * a subtler failure downstream. */
static void test_every_row_key_is_a_real_enhancement_schema_key(void) {
    for (int i = 0; i < mdkr_enhancement_count(); i++) {
        const MdkrEnhancement *e = mdkr_enhancement_at(i);
        const MdkrVideoSchema *schema = mdkr_video_schema(e->key);
        char what[128];
        snprintf(what, sizeof what, "row %d's key resolves to a schema entry",
                 i);
        expect(schema != NULL, what);
        if (schema == NULL)
            continue;
        snprintf(what, sizeof what,
                 "row %d's schema name starts with \"Enhancements.\"", i);
        expect(strncmp(schema->name, "Enhancements.",
                        strlen("Enhancements.")) == 0, what);
    }
}

int main(void) {
    test_every_row_is_complete();
    test_keys_are_unique();
    test_lookup_round_trips();
    test_registry_matches_is_enhancement_predicate();
    test_every_row_key_is_a_real_enhancement_schema_key();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all ok\n");
    return 0;
}
