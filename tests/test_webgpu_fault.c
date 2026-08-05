#include "gfx_webgpu_fault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #condition,       \
                    __FILE__, __LINE__);                                     \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

static void test_registry(void) {
    size_t count = gfx_webgpu_fault_point_count();
    CHECK(count == GFX_WEBGPU_FAULT_COUNT);
    /* Truncation floor. GFX_WEBGPU_FAULT_POINT_LIST is a backslash-continued
     * X-macro: a lost continuation silently shortens the vocabulary while the
     * enum and the table stay consistent, so the equality above cannot see it.
     * The registry held 102 points when the fault boundaries were gated; 80
     * leaves room for deliberate consolidation but not for a dropped tail. */
    CHECK(count > 80);
    for (size_t i = 0; i < count; i++) {
        const char *name = gfx_webgpu_fault_point_name(i);
        CHECK(name != NULL);
        CHECK(name[0] != '\0');
        for (size_t j = i + 1; j < count; j++) {
            CHECK(strcmp(name, gfx_webgpu_fault_point_name(j)) != 0);
        }
    }
    CHECK(gfx_webgpu_fault_point_name(count) == NULL);
}

static void test_default_occurrence(void) {
    gfx_webgpu_fault_set_spec_for_test("frame.encoder");
    CHECK(gfx_webgpu_fault_configure());
    CHECK(gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_FRAME_ENCODER));
    CHECK(!gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_FRAME_ENCODER));
    CHECK(!gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_FRAME_PASS));
}

static void test_selected_occurrence(void) {
    gfx_webgpu_fault_set_spec_for_test("surface.timeout@3");
    CHECK(gfx_webgpu_fault_configure());
    CHECK(gfx_webgpu_fault_selected(GFX_WEBGPU_FAULT_SURFACE_TIMEOUT));
    CHECK(!gfx_webgpu_fault_selected(GFX_WEBGPU_FAULT_SURFACE_LOST));
    CHECK(!gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_TIMEOUT));
    CHECK(!gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_TIMEOUT));
    CHECK(gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_TIMEOUT));
    CHECK(!gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_TIMEOUT));
}

static void test_none(void) {
    gfx_webgpu_fault_set_spec_for_test("none");
    CHECK(gfx_webgpu_fault_configure());
    for (size_t i = 0; i < gfx_webgpu_fault_point_count(); i++) {
        CHECK(!gfx_webgpu_fault_hit((enum GfxWebgpuFaultPoint)i));
    }
}

static void test_all_occurrences(void) {
    gfx_webgpu_fault_set_spec_for_test("surface.lost@all");
    CHECK(gfx_webgpu_fault_configure());
    for (int i = 0; i < 4; i++) {
        CHECK(gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_LOST));
    }
    CHECK(!gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_SURFACE_ERROR));
}

static void test_rejects_bad_specs(void) {
    const char *bad[] = {
        "not-a-point",
        "frame.encoder@",
        "frame.encoder@0",
        "frame.encoder@-1",
        "frame.encoder@1x",
        "@2",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        gfx_webgpu_fault_set_spec_for_test(bad[i]);
        CHECK(!gfx_webgpu_fault_configure());
        CHECK(gfx_webgpu_fault_error() != NULL);
        CHECK(!gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_FRAME_ENCODER));
    }
}

int main(void) {
    test_registry();
    test_default_occurrence();
    test_selected_occurrence();
    test_none();
    test_all_occurrences();
    test_rejects_bad_specs();
    gfx_webgpu_fault_set_spec_for_test(NULL);
    puts("test_webgpu_fault: PASS");
    return 0;
}
