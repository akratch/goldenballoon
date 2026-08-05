#include "camera_target_visibility.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct SlabSource {
    float minimum_x;
    float maximum_x;
    int invalid;
} SlabSource;

static int sFailures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        sFailures++;
    }
}

static MdkrCameraSweepStatus slab_sweep(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    const SlabSource *slab = context;
    const double start = input->start_eye.x;
    const double end = input->desired_eye.x;
    const double minimum = slab->minimum_x - input->guard.radius;
    const double maximum = slab->maximum_x + input->guard.radius;
    const double delta = end - start;
    double fraction;

    memset(out_hit, 0, sizeof(*out_hit));
    if (slab->invalid) return MDKR_CAMERA_SWEEP_INVALID;
    if (start >= minimum && start <= maximum) {
        out_hit->fraction = 0.0f;
        out_hit->started_overlapping = 1U;
        out_hit->penetration_depth = (float)fmin(start - minimum, maximum - start);
        out_hit->normal.x = start - minimum < maximum - start ? -1.0f : 1.0f;
        out_hit->feature = MDKR_CAMERA_SWEEP_FEATURE_FACE;
        out_hit->stable_id = 77U;
        out_hit->kind = 5U;
        return MDKR_CAMERA_SWEEP_HIT;
    }
    if (delta == 0.0) return MDKR_CAMERA_SWEEP_CLEAR;
    fraction = ((delta > 0.0 ? minimum : maximum) - start) / delta;
    if (fraction < 0.0 || fraction > 1.0) {
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    out_hit->fraction = (float)fraction;
    out_hit->point.x = (float)(start + delta * fraction);
    out_hit->normal.x = delta > 0.0 ? -1.0f : 1.0f;
    out_hit->feature = MDKR_CAMERA_SWEEP_FEATURE_FACE;
    out_hit->stable_id = 77U;
    out_hit->kind = 5U;
    return MDKR_CAMERA_SWEEP_HIT;
}

static MdkrCameraObstructionCombinedQuery query_for(SlabSource *slab) {
    static MdkrCameraObstructionQuerySource source;
    MdkrCameraObstructionCombinedQuery query = { 0 };

    source.sweep = slab_sweep;
    source.context = slab;
    query.sources = &source;
    query.source_count = 1U;
    query.source_capacity = 1U;
    return query;
}

int main(void) {
    SlabSource slab;
    MdkrCameraObstructionCombinedQuery query;
    MdkrCameraSweepHit hit;
    MdkrCameraTargetVisibilityStatus status;
    const MdkrCameraVec3 target = { 0.0f, 0.0f, 0.0f };
    const MdkrCameraVec3 eye = { 100.0f, 0.0f, 0.0f };

    slab = (SlabSource) { -32.0f, 32.0f, 0 };
    query = query_for(&slab);
    status = mdkr_camera_target_visibility_query(&query, target, eye, 1U, &hit);
    expect_true("thick wall focus is embedded",
                status == MDKR_CAMERA_TARGET_VISIBILITY_EMBEDDED);
    expect_true("embedded witness remains overlap",
                hit.started_overlapping && hit.stable_id == 77U);

    slab = (SlabSource) { -0.1f, 0.1f, 0 };
    query = query_for(&slab);
    status = mdkr_camera_target_visibility_query(&query, target, eye, 1U, &hit);
    expect_true("bounded local skin exclusion becomes visible",
                status == MDKR_CAMERA_TARGET_VISIBILITY_VISIBLE);

    slab = (SlabSource) { 30.0f, 40.0f, 0 };
    query = query_for(&slab);
    status = mdkr_camera_target_visibility_query(&query, target, eye, 1U, &hit);
    expect_true("remote wall is hidden not embedded",
                status == MDKR_CAMERA_TARGET_VISIBILITY_HIDDEN);
    expect_true("hidden witness is non-overlap",
                !hit.started_overlapping && hit.stable_id == 77U);

    slab.invalid = 1;
    status = mdkr_camera_target_visibility_query(&query, target, eye, 1U, &hit);
    expect_true("invalid source is not hidden or embedded",
                status == MDKR_CAMERA_TARGET_VISIBILITY_INVALID);
    expect_true("null query invalid",
                mdkr_camera_target_visibility_query(
                    NULL, target, eye, 1U, &hit) ==
                    MDKR_CAMERA_TARGET_VISIBILITY_INVALID);
    slab.invalid = 0;
    expect_true("nonfinite eye invalid",
                mdkr_camera_target_visibility_query(
                    &query, target, (MdkrCameraVec3) { NAN, 0.0f, 0.0f },
                    1U, &hit) == MDKR_CAMERA_TARGET_VISIBILITY_INVALID);

    if (sFailures != 0) {
        fprintf(stderr, "camera-target-visibility: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("camera-target-visibility: visible/hidden/embedded/invalid policy passed\n");
    return 0;
}
