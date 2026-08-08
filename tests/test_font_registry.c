#include "fast3d/gfx_font_registry.h"

#include <stdint.h>
#include <stdio.h>

static int s_failures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void test_lifecycle_and_region_union(void) {
    static GfxFontRegistry registry;
    uint8_t source_a;
    uint8_t source_b;
    const GfxFontAtlasRegion first[] = {
        { 0, 0, 8, 12 },
        { 8, 0, 9, 12 },
        { 8, 0, 9, 12 },
        { 99, 99, 0, 12 },
    };
    const GfxFontAtlasRegion second[] = {
        { 8, 0, 9, 12 },
        { 17, 0, 6, 12 },
    };
    const GfxFontRegistryEntry *entry;

    gfx_font_registry_init(&registry);
    expect_true("empty after init",
                gfx_font_registry_active_count(&registry) == 0);
    expect_true("register first font",
                gfx_font_registry_register(
                    &registry, &source_a, GFX_FONT_FACE_SMALL, first,
                    sizeof(first) / sizeof(first[0])));
    entry = gfx_font_registry_find(&registry, &source_a);
    expect_true("first source found", entry != NULL);
    expect_true("duplicates and empty regions removed",
                entry != NULL && entry->region_count == 2);
    expect_true("first reference recorded",
                entry != NULL && entry->references == 1);

    expect_true("register shared atlas",
                gfx_font_registry_register(
                    &registry, &source_a, GFX_FONT_FACE_SMALL, second,
                    sizeof(second) / sizeof(second[0])));
    entry = gfx_font_registry_find(&registry, &source_a);
    expect_true("shared references merged",
                entry != NULL && entry->references == 2);
    expect_true("region union merged",
                entry != NULL && entry->region_count == 3);

    expect_true("second source registered",
                gfx_font_registry_register(&registry, &source_b,
                                           GFX_FONT_FACE_NONE, NULL, 0));
    expect_true("two active sources",
                gfx_font_registry_active_count(&registry) == 2);
    expect_true("first unregister decrements only",
                gfx_font_registry_unregister(&registry, &source_a));
    entry = gfx_font_registry_find(&registry, &source_a);
    expect_true("shared source remains",
                entry != NULL && entry->references == 1);
    expect_true("final unregister clears entry",
                gfx_font_registry_unregister(&registry, &source_a));
    expect_true("source absent at zero",
                gfx_font_registry_find(&registry, &source_a) == NULL);
    expect_true("unknown unregister rejected",
                !gfx_font_registry_unregister(&registry, &source_a));
}

static void test_capacity_failure_is_atomic(void) {
    static GfxFontRegistry registry;
    static uint8_t sources[GFX_FONT_REGISTRY_CAPACITY + 1];

    gfx_font_registry_init(&registry);
    for (size_t index = 0; index < GFX_FONT_REGISTRY_CAPACITY; index++) {
        expect_true("fill registry",
                    gfx_font_registry_register(
                        &registry, &sources[index], GFX_FONT_FACE_NONE,
                        NULL, 0));
    }
    expect_true("registry reports full",
                gfx_font_registry_active_count(&registry) ==
                    GFX_FONT_REGISTRY_CAPACITY);
    expect_true("overflow registration rejected",
                !gfx_font_registry_register(
                    &registry, &sources[GFX_FONT_REGISTRY_CAPACITY],
                    GFX_FONT_FACE_NONE, NULL, 0));
    expect_true("overflow source absent",
                gfx_font_registry_find(
                    &registry, &sources[GFX_FONT_REGISTRY_CAPACITY]) == NULL);
}

/*
 * One cell legitimately serves two characters -- FunFont draws 'a' from the
 * 'A' cell -- and the outline path has to be able to ask which glyph a region
 * belongs to. Regions are therefore keyed by rectangle AND character, so an
 * identical rectangle under a different character is a distinct region while a
 * genuine repeat still collapses.
 */
static void test_regions_are_keyed_by_character(void) {
    static GfxFontRegistry registry;
    uint8_t source;
    const GfxFontAtlasRegion regions[] = {
        { 0, 0, 8, 12, 33 },   /* 'A' */
        { 0, 0, 8, 12, 65 },   /* 'a', same cell */
        { 0, 0, 8, 12, 33 },   /* exact repeat of the first */
    };
    const GfxFontRegistryEntry *entry;

    gfx_font_registry_init(&registry);
    expect_true("register aliased cells",
                gfx_font_registry_register(
                    &registry, &source, GFX_FONT_FACE_NONE, regions,
                    sizeof(regions) / sizeof(regions[0])));
    entry = gfx_font_registry_find(&registry, &source);
    expect_true("same cell under two characters kept",
                entry != NULL && entry->region_count == 2);
    expect_true("exact repeat still collapsed",
                entry != NULL && entry->regions[0].character == 33 &&
                    entry->regions[1].character == 65);
}

/*
 * If two fonts claim one atlas with different faces neither claim can be
 * trusted, and keeping the ROM pixels is always the safe answer.
 */
static void test_face_conflict_degrades_to_none(void) {
    static GfxFontRegistry registry;
    uint8_t source;
    const GfxFontRegistryEntry *entry;

    gfx_font_registry_init(&registry);
    expect_true("first face claim",
                gfx_font_registry_register(&registry, &source,
                                           GFX_FONT_FACE_SMALL, NULL, 0));
    entry = gfx_font_registry_find(&registry, &source);
    expect_true("face recorded",
                entry != NULL && entry->face == GFX_FONT_FACE_SMALL);

    expect_true("conflicting face claim accepted",
                gfx_font_registry_register(&registry, &source,
                                           GFX_FONT_FACE_SUBTITLE, NULL, 0));
    entry = gfx_font_registry_find(&registry, &source);
    expect_true("conflict degrades to none",
                entry != NULL && entry->face == GFX_FONT_FACE_NONE);

    /* An agreeing re-registration must not disturb the recorded face. */
    gfx_font_registry_init(&registry);
    (void)gfx_font_registry_register(&registry, &source,
                                     GFX_FONT_FACE_SUBTITLE, NULL, 0);
    (void)gfx_font_registry_register(&registry, &source,
                                     GFX_FONT_FACE_SUBTITLE, NULL, 0);
    entry = gfx_font_registry_find(&registry, &source);
    expect_true("agreeing claims keep the face",
                entry != NULL && entry->face == GFX_FONT_FACE_SUBTITLE);
}

int main(void) {
    test_lifecycle_and_region_union();
    test_capacity_failure_is_atomic();
    test_regions_are_keyed_by_character();
    test_face_conflict_degrades_to_none();

    if (s_failures != 0) {
        fprintf(stderr, "%d font registry test(s) failed\n", s_failures);
        return 1;
    }
    printf("font registry tests passed\n");
    return 0;
}
