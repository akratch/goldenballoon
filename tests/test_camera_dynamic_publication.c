#include "camera_dynamic_publication.h"

#include <stdio.h>

static int sFailures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        sFailures++;
    }
}

int main(void) {
    MdkrCameraDynamicPublicationState state = { 1U, 1U, 1U };
    int recovery;

    mdkr_camera_dynamic_publication_reset(&state);
    expect_true("reset attempt", !state.attempted);
    expect_true("reset current", !mdkr_camera_dynamic_publication_current_valid(&state));
    expect_true("reset previous", !mdkr_camera_dynamic_publication_previous_valid(&state));
    expect_true("reset does not cut",
                !mdkr_camera_dynamic_publication_requires_global_cut(&state));
    mdkr_camera_dynamic_publication_finish(&state, 1);
    expect_true("finish without begin cannot publish",
                !mdkr_camera_dynamic_publication_current_valid(&state));

    recovery = mdkr_camera_dynamic_publication_begin(&state);
    expect_true("first begin is not recovery", !recovery);
    expect_true("in-progress first publication is invalid",
                !mdkr_camera_dynamic_publication_current_valid(&state));
    mdkr_camera_dynamic_publication_finish(&state, 1);
    expect_true("first complete publication valid",
                mdkr_camera_dynamic_publication_current_valid(&state));
    expect_true("valid publication does not cut",
                !mdkr_camera_dynamic_publication_requires_global_cut(&state));

    recovery = mdkr_camera_dynamic_publication_begin(&state);
    expect_true("second begin after valid is not recovery", !recovery);
    expect_true("valid predecessor retained",
                mdkr_camera_dynamic_publication_previous_valid(&state));
    mdkr_camera_dynamic_publication_finish(&state, 0);
    expect_true("failed census invalid",
                !mdkr_camera_dynamic_publication_current_valid(&state));
    expect_true("failed census cuts all hard objects",
                mdkr_camera_dynamic_publication_requires_global_cut(&state));

    recovery = mdkr_camera_dynamic_publication_begin(&state);
    expect_true("first recovered census announces discontinuity", recovery);
    expect_true("failed predecessor cannot supply temporal envelope",
                !mdkr_camera_dynamic_publication_previous_valid(&state));
    mdkr_camera_dynamic_publication_finish(&state, 1);
    expect_true("recovered complete publication valid",
                mdkr_camera_dynamic_publication_current_valid(&state));
    expect_true("recovered completion retires global cut",
                !mdkr_camera_dynamic_publication_requires_global_cut(&state));

    recovery = mdkr_camera_dynamic_publication_begin(&state);
    expect_true("second valid tick resumes interpolation", !recovery);
    expect_true("recovered predecessor now authoritative",
                mdkr_camera_dynamic_publication_previous_valid(&state));
    mdkr_camera_dynamic_publication_finish(&state, 0);
    recovery = mdkr_camera_dynamic_publication_begin(&state);
    expect_true("repeated failure remains discontinuous", recovery);
    mdkr_camera_dynamic_publication_finish(&state, 0);
    recovery = mdkr_camera_dynamic_publication_begin(&state);
    expect_true("each failed retry remains discontinuous", recovery);

    expect_true("null begin safe", !mdkr_camera_dynamic_publication_begin(NULL));
    expect_true("null current invalid",
                !mdkr_camera_dynamic_publication_current_valid(NULL));
    expect_true("null previous invalid",
                !mdkr_camera_dynamic_publication_previous_valid(NULL));
    expect_true("null does not request cut",
                !mdkr_camera_dynamic_publication_requires_global_cut(NULL));
    mdkr_camera_dynamic_publication_reset(NULL);
    mdkr_camera_dynamic_publication_finish(NULL, 1);

    if (sFailures != 0) {
        fprintf(stderr, "camera-dynamic-publication: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("camera-dynamic-publication: invalid/recovery state machine passed\n");
    return 0;
}
