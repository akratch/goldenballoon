#include "camera_obstruction_query.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;
static MdkrCameraSweepInput s_last_sphere_input;
static MdkrCameraRoundedLensSweepInput s_last_rounded_input;
static size_t s_sphere_callback_count;
static size_t s_rounded_callback_count;

typedef struct TestSource {
    MdkrCameraSweepStatus status;
    MdkrCameraSweepHit hit;
    uint32_t required_mask;
    uint32_t required_ignored_generation;
    unsigned int source_tag;
    unsigned int *visit_log;
    size_t *visit_count;
} TestSource;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static MdkrCameraSweepInput valid_input(void) {
    MdkrCameraSweepInput input;
    memset(&input, 0, sizeof(input));
    input.guard.kind = MDKR_CAMERA_LENS_GUARD_SPHERE;
    input.guard.radius = 1.0f;
    input.desired_eye.x = 10.0f;
    return input;
}

static MdkrCameraRoundedLensSweepInput valid_rounded_input(void) {
    MdkrCameraRoundedLensSweepInput input;

    memset(&input, 0, sizeof(input));
    input.guard.forward.z = 1.0f;
    input.guard.right.x = -1.0f;
    input.guard.up.y = 1.0f;
    input.guard.near_distance = 2.0f;
    input.guard.half_width = 1.0f;
    input.guard.half_height = 0.75f;
    input.guard.skin = 0.1f;
    input.guard.broadphase_radius = 2.4585f;
    input.desired_eye.z = 10.0f;
    return input;
}

static MdkrCameraSweepHit valid_hit(float fraction, uint32_t stable_id, uint32_t kind) {
    MdkrCameraSweepHit hit;
    memset(&hit, 0, sizeof(hit));
    hit.fraction = fraction;
    hit.point.x = fraction * 10.0f;
    hit.normal.x = -1.0f;
    hit.kind = kind;
    hit.stable_id = stable_id;
    hit.feature = MDKR_CAMERA_SWEEP_FEATURE_FACE;
    return hit;
}

static MdkrCameraSweepStatus test_source_sweep(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    const TestSource *source = context;

    if (source == NULL || input == NULL || out_hit == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    s_last_sphere_input = *input;
    s_sphere_callback_count++;
    if (source->visit_log != NULL && source->visit_count != NULL) {
        source->visit_log[(*source->visit_count)++] = source->source_tag;
    }
    if ((source->required_mask != 0U && input->mask != source->required_mask) ||
        (source->required_ignored_generation != 0U &&
         input->ignored_object_generation != source->required_ignored_generation)) {
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    *out_hit = source->hit;
    return source->status;
}

static MdkrCameraSweepStatus test_rounded_source_sweep(
    const void *context,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    const TestSource *source = context;

    if (source == NULL || input == NULL || out_hit == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    s_last_rounded_input = *input;
    s_rounded_callback_count++;
    if (source->visit_log != NULL && source->visit_count != NULL) {
        source->visit_log[(*source->visit_count)++] = source->source_tag;
    }
    if ((source->required_mask != 0U && input->mask != source->required_mask) ||
        (source->required_ignored_generation != 0U &&
         input->ignored_object_generation != source->required_ignored_generation)) {
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    *out_hit = source->hit;
    return source->status;
}

static MdkrCameraObstructionCombinedQuery make_query(
    const MdkrCameraObstructionQuerySource *sources,
    size_t count) {
    MdkrCameraObstructionCombinedQuery query = { sources, count, count };
    return query;
}

static MdkrCameraObstructionRoundedLensCombinedQuery make_rounded_query(
    const MdkrCameraObstructionRoundedLensQuerySource *sources,
    size_t count) {
    MdkrCameraObstructionRoundedLensCombinedQuery query = { sources, count, count };
    return query;
}

static void expect_canonical_clear(const char *name, const MdkrCameraSweepHit *hit) {
    MdkrCameraSweepHit expected;

    memset(&expected, 0, sizeof(expected));
    expected.fraction = 1.0f;
    expected.clearance = INFINITY;
    expect_true(name, memcmp(hit, &expected, sizeof(expected)) == 0);
}

static void expect_canonical_invalid(const char *name, const MdkrCameraSweepHit *hit) {
    MdkrCameraSweepHit expected;

    memset(&expected, 0, sizeof(expected));
    expect_true(name, memcmp(hit, &expected, sizeof(expected)) == 0);
}

static void test_permutation_and_stable_order(void) {
    unsigned int visit_log[4] = { 0U };
    size_t visit_count = 0U;
    TestSource first = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.5f, 80U, 1U), 0U, 0U, 10U, visit_log, &visit_count };
    TestSource second = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.5f, 20U, 2U), 0U, 0U, 20U, visit_log, &visit_count };
    MdkrCameraObstructionQuerySource sources[] = {
        { test_source_sweep, &first }, { test_source_sweep, &second },
    };
    MdkrCameraObstructionQuerySource permuted[] = {
        { test_source_sweep, &second }, { test_source_sweep, &first },
    };
    MdkrCameraObstructionCombinedQuery query = make_query(sources, 2U);
    MdkrCameraSweepInput input = valid_input();
    MdkrCameraSweepHit hit;
    MdkrCameraSweepHit permuted_hit;

    expect_true("stable order query hit", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("sources visit caller order", visit_count == 2U && visit_log[0] == 10U && visit_log[1] == 20U);
    expect_true("exact tie chooses global lowest id", hit.stable_id == 20U && hit.kind == 2U);
    query = make_query(permuted, 2U);
    expect_true("permuted source query hit", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &permuted_hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("permuted unique IDs preserve result", memcmp(&hit, &permuted_hit, sizeof(hit)) == 0);
}

static void test_earliest_overlap_filters_and_adapter(void) {
    TestSource later_lower_id = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.7f, 1U, 1U), 0U, 0U, 0U, NULL, NULL };
    TestSource earlier_higher_id = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.3f, 900U, 2U), 0U, 0U, 0U, NULL, NULL };
    TestSource overlap = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.0f, 500U, 3U), 0U, 0U, 0U, NULL, NULL };
    TestSource filtered = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.2f, 3U, 4U), 0xA5U, 77U, 0U, NULL, NULL };
    MdkrCameraObstructionQuerySource sources[] = {
        { test_source_sweep, &later_lower_id }, { test_source_sweep, &earlier_higher_id },
    };
    MdkrCameraObstructionQuerySource overlap_source[] = { { test_source_sweep, &overlap } };
    MdkrCameraObstructionQuerySource filtered_source[] = { { test_source_sweep, &filtered } };
    MdkrCameraObstructionCombinedQuery query = make_query(sources, 2U);
    MdkrCameraSweepInput input = valid_input();
    MdkrCameraSweepHit hit;

    expect_true("earlier time beats lower id", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 900U);
    overlap.hit.started_overlapping = 1U;
    overlap.hit.clearance = -0.5f;
    overlap.hit.penetration_depth = 0.5f;
    query = make_query(overlap_source, 1U);
    expect_true("overlap preserved", mdkr_camera_obstruction_combined_sweep_adapter(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("overlap metadata preserved", hit.started_overlapping && hit.penetration_depth == 0.5f && hit.kind == 3U);

    query = make_query(filtered_source, 1U);
    expect_true("filtered source clears", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_CLEAR);
    expect_canonical_clear("filtered clear canonical", &hit);
    input.mask = 0xA5U;
    input.ignored_object_generation = 77U;
    expect_true("unchanged input reaches filtered source", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 3U);
}

static void test_invalid_zero_capacity_purity_and_determinism(void) {
    TestSource valid = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.4f, 9U, 1U), 0U, 0U, 0U, NULL, NULL };
    TestSource invalid = { MDKR_CAMERA_SWEEP_INVALID, valid_hit(0.2f, 1U, 1U), 0U, 0U, 0U, NULL, NULL };
    TestSource duplicate = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.4f, 9U, 2U), 0U, 0U, 0U, NULL, NULL };
    MdkrCameraObstructionQuerySource valid_source[] = { { test_source_sweep, &valid } };
    MdkrCameraObstructionQuerySource invalid_source[] = { { test_source_sweep, &invalid } };
    MdkrCameraObstructionQuerySource duplicate_sources[] = {
        { test_source_sweep, &valid }, { test_source_sweep, &duplicate },
    };
    MdkrCameraObstructionQuerySource null_source[] = { { NULL, &valid } };
    MdkrCameraObstructionCombinedQuery query = make_query(valid_source, 1U);
    MdkrCameraObstructionCombinedQuery before_query;
    MdkrCameraObstructionQuerySource before_sources[1];
    MdkrCameraSweepInput input = valid_input();
    MdkrCameraSweepInput before_input;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepHit repeat_hit;

    before_query = query;
    memcpy(before_sources, valid_source, sizeof(before_sources));
    before_input = input;
    expect_true("valid source hit", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("query purity", memcmp(&query, &before_query, sizeof(query)) == 0);
    expect_true("source array purity", memcmp(valid_source, before_sources, sizeof(before_sources)) == 0);
    expect_true("input purity", memcmp(&input, &before_input, sizeof(input)) == 0);
    expect_true("repeat hit", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &repeat_hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("repeat bytes", memcmp(&hit, &repeat_hit, sizeof(hit)) == 0);

    query = (MdkrCameraObstructionCombinedQuery){ NULL, 0U, 0U };
    expect_true("zero source clear", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_CLEAR);
    expect_canonical_clear("zero source clear canonical", &hit);
    query = (MdkrCameraObstructionCombinedQuery){ valid_source, 2U, 1U };
    expect_true("count capacity rejected", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("count capacity invalid canonical", &hit);
    query = (MdkrCameraObstructionCombinedQuery){ valid_source, SIZE_MAX, SIZE_MAX };
    expect_true("capacity byte overflow rejected", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("capacity overflow invalid canonical", &hit);
    query = make_query(invalid_source, 1U);
    expect_true("invalid child fail closed", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("invalid child canonical", &hit);
    query = make_query(null_source, 1U);
    expect_true("null callback contract rejected", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("null callback canonical", &hit);
    query = make_query(duplicate_sources, 2U);
    expect_true("duplicate exact tie fail closed", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("duplicate exact tie canonical", &hit);
}

static void test_sphere_time_tie_epsilon(void) {
    const float within_epsilon = (float)(0.5 * MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON);
    const float outside_epsilon = (float)(2.0 * MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON);
    TestSource first_tied = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.0f, 70U, 1U), 0U, 0U, 0U, NULL, NULL };
    TestSource later_tied_lower_id = { MDKR_CAMERA_SWEEP_HIT, valid_hit(within_epsilon, 3U, 2U), 0U, 0U, 0U, NULL, NULL };
    TestSource later_outside_lower_id = { MDKR_CAMERA_SWEEP_HIT, valid_hit(outside_epsilon, 1U, 3U), 0U, 0U, 0U, NULL, NULL };
    TestSource earlier_outside_higher_id = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.0f, 90U, 4U), 0U, 0U, 0U, NULL, NULL };
    TestSource duplicate_within_epsilon = { MDKR_CAMERA_SWEEP_HIT, valid_hit(within_epsilon, 70U, 5U), 0U, 0U, 0U, NULL, NULL };
    MdkrCameraObstructionQuerySource tied_sources[] = {
        { test_source_sweep, &first_tied }, { test_source_sweep, &later_tied_lower_id },
    };
    MdkrCameraObstructionQuerySource outside_sources[] = {
        { test_source_sweep, &later_outside_lower_id }, { test_source_sweep, &earlier_outside_higher_id },
    };
    MdkrCameraObstructionQuerySource duplicate_sources[] = {
        { test_source_sweep, &first_tied }, { test_source_sweep, &duplicate_within_epsilon },
    };
    MdkrCameraObstructionCombinedQuery query = make_query(tied_sources, 2U);
    MdkrCameraSweepInput input = valid_input();
    MdkrCameraSweepHit hit;

    expect_true("sphere within epsilon ties by stable ID", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 3U);
    query = make_query(outside_sources, 2U);
    expect_true("sphere beyond epsilon chooses earlier", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 90U);
    query = make_query(duplicate_sources, 2U);
    expect_true("sphere duplicate within epsilon fails closed", mdkr_camera_obstruction_combined_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("sphere duplicate within epsilon canonical", &hit);
}

static void test_rounded_multiple_sources_and_permutation(void) {
    unsigned int visit_log[4] = { 0U };
    size_t visit_count = 0U;
    TestSource clear = { MDKR_CAMERA_SWEEP_CLEAR, { 0 }, 0U, 0U, 10U, visit_log, &visit_count };
    TestSource later = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.8f, 90U, 1U), 0U, 0U, 20U, visit_log, &visit_count };
    TestSource earliest = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.25f, 60U, 2U), 0U, 0U, 30U, visit_log, &visit_count };
    TestSource exact_tie_lower_id = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.25f, 5U, 3U), 0U, 0U, 40U, visit_log, &visit_count };
    MdkrCameraObstructionRoundedLensQuerySource sources[] = {
        { test_rounded_source_sweep, &clear },
        { test_rounded_source_sweep, &later },
        { test_rounded_source_sweep, &earliest },
        { test_rounded_source_sweep, &exact_tie_lower_id },
    };
    MdkrCameraObstructionRoundedLensQuerySource permuted[] = {
        { test_rounded_source_sweep, &exact_tie_lower_id },
        { test_rounded_source_sweep, &earliest },
        { test_rounded_source_sweep, &later },
        { test_rounded_source_sweep, &clear },
    };
    MdkrCameraObstructionRoundedLensCombinedQuery query = make_rounded_query(sources, 4U);
    MdkrCameraRoundedLensSweepInput input = valid_rounded_input();
    MdkrCameraSweepHit hit;
    MdkrCameraSweepHit permuted_hit;

    expect_true("rounded source after clear still blocks", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("rounded earliest exact tie chooses lowest ID", hit.stable_id == 5U && hit.kind == 3U);
    expect_true("rounded sources visit caller order", visit_count == 4U &&
        visit_log[0] == 10U && visit_log[1] == 20U && visit_log[2] == 30U && visit_log[3] == 40U);

    visit_count = 0U;
    query = make_rounded_query(permuted, 4U);
    expect_true("rounded permutation hit", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &permuted_hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("rounded permutation preserves unique result",
        memcmp(&hit, &permuted_hit, sizeof(hit)) == 0);
}

static void test_rounded_invalid_contracts_and_purity(void) {
    TestSource valid = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.4f, 9U, 1U), 0U, 0U, 0U, NULL, NULL };
    TestSource invalid_child = { MDKR_CAMERA_SWEEP_INVALID, valid_hit(0.2f, 1U, 1U), 0U, 0U, 0U, NULL, NULL };
    TestSource invalid_payload = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.2f, 2U, 1U), 0U, 0U, 0U, NULL, NULL };
    TestSource duplicate = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.4f, 9U, 2U), 0U, 0U, 0U, NULL, NULL };
    MdkrCameraObstructionRoundedLensQuerySource valid_source[] = { { test_rounded_source_sweep, &valid } };
    MdkrCameraObstructionRoundedLensQuerySource invalid_child_source[] = { { test_rounded_source_sweep, &invalid_child } };
    MdkrCameraObstructionRoundedLensQuerySource invalid_payload_source[] = { { test_rounded_source_sweep, &invalid_payload } };
    MdkrCameraObstructionRoundedLensQuerySource duplicate_sources[] = {
        { test_rounded_source_sweep, &valid }, { test_rounded_source_sweep, &duplicate },
    };
    MdkrCameraObstructionRoundedLensQuerySource null_source[] = { { NULL, &valid } };
    MdkrCameraObstructionRoundedLensCombinedQuery query = make_rounded_query(valid_source, 1U);
    MdkrCameraObstructionRoundedLensCombinedQuery before_query;
    MdkrCameraObstructionRoundedLensQuerySource before_sources[1];
    MdkrCameraRoundedLensSweepInput input = valid_rounded_input();
    MdkrCameraRoundedLensSweepInput before_input;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepHit repeat_hit;

    before_query = query;
    memcpy(before_sources, valid_source, sizeof(before_sources));
    before_input = input;
    expect_true("rounded valid source hit", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("rounded query purity", memcmp(&query, &before_query, sizeof(query)) == 0);
    expect_true("rounded source array purity", memcmp(valid_source, before_sources, sizeof(before_sources)) == 0);
    expect_true("rounded input purity", memcmp(&input, &before_input, sizeof(input)) == 0);
    expect_true("rounded repeat hit", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &repeat_hit) == MDKR_CAMERA_SWEEP_HIT);
    expect_true("rounded repeat bytes", memcmp(&hit, &repeat_hit, sizeof(hit)) == 0);
    expect_true("rounded adapter hit", mdkr_camera_obstruction_combined_rounded_lens_sweep_adapter(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 9U);

    query = (MdkrCameraObstructionRoundedLensCombinedQuery){ NULL, 0U, 0U };
    expect_true("rounded zero source clear", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_CLEAR);
    expect_canonical_clear("rounded zero source canonical", &hit);
    expect_true("rounded null query rejected", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        NULL, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded null query canonical", &hit);
    expect_true("rounded null input rejected", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, NULL, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded null input canonical", &hit);
    query = (MdkrCameraObstructionRoundedLensCombinedQuery){ NULL, 1U, 1U };
    expect_true("rounded null sources rejected", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded null sources canonical", &hit);
    query = (MdkrCameraObstructionRoundedLensCombinedQuery){ valid_source, 2U, 1U };
    expect_true("rounded count capacity rejected", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded count capacity canonical", &hit);
    query = (MdkrCameraObstructionRoundedLensCombinedQuery){ valid_source, SIZE_MAX, SIZE_MAX };
    expect_true("rounded capacity overflow rejected", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded capacity overflow canonical", &hit);
    query = make_rounded_query(invalid_child_source, 1U);
    expect_true("rounded invalid child fail closed", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded invalid child canonical", &hit);
    invalid_payload.hit.normal.x = NAN;
    query = make_rounded_query(invalid_payload_source, 1U);
    expect_true("rounded invalid payload fail closed", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded invalid payload canonical", &hit);
    query = make_rounded_query(null_source, 1U);
    expect_true("rounded null callback rejected", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded null callback canonical", &hit);
    query = make_rounded_query(duplicate_sources, 2U);
    expect_true("rounded duplicate exact tie fail closed", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded duplicate exact tie canonical", &hit);
}

static void test_rounded_time_tie_epsilon(void) {
    const float within_epsilon = (float)(0.5 * MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON);
    const float outside_epsilon = (float)(2.0 * MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON);
    TestSource first_tied = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.0f, 70U, 1U), 0U, 0U, 0U, NULL, NULL };
    TestSource later_tied_lower_id = { MDKR_CAMERA_SWEEP_HIT, valid_hit(within_epsilon, 3U, 2U), 0U, 0U, 0U, NULL, NULL };
    TestSource later_outside_lower_id = { MDKR_CAMERA_SWEEP_HIT, valid_hit(outside_epsilon, 1U, 3U), 0U, 0U, 0U, NULL, NULL };
    TestSource earlier_outside_higher_id = { MDKR_CAMERA_SWEEP_HIT, valid_hit(0.0f, 90U, 4U), 0U, 0U, 0U, NULL, NULL };
    TestSource duplicate_within_epsilon = { MDKR_CAMERA_SWEEP_HIT, valid_hit(within_epsilon, 70U, 5U), 0U, 0U, 0U, NULL, NULL };
    MdkrCameraObstructionRoundedLensQuerySource tied_sources[] = {
        { test_rounded_source_sweep, &first_tied }, { test_rounded_source_sweep, &later_tied_lower_id },
    };
    MdkrCameraObstructionRoundedLensQuerySource outside_sources[] = {
        { test_rounded_source_sweep, &later_outside_lower_id }, { test_rounded_source_sweep, &earlier_outside_higher_id },
    };
    MdkrCameraObstructionRoundedLensQuerySource duplicate_sources[] = {
        { test_rounded_source_sweep, &first_tied }, { test_rounded_source_sweep, &duplicate_within_epsilon },
    };
    MdkrCameraObstructionRoundedLensCombinedQuery query = make_rounded_query(tied_sources, 2U);
    MdkrCameraRoundedLensSweepInput input = valid_rounded_input();
    MdkrCameraSweepHit hit;

    expect_true("rounded within epsilon ties by stable ID", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 3U);
    query = make_rounded_query(outside_sources, 2U);
    expect_true("rounded beyond epsilon chooses earlier", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_HIT && hit.stable_id == 90U);
    query = make_rounded_query(duplicate_sources, 2U);
    expect_true("rounded duplicate within epsilon fails closed", mdkr_camera_obstruction_combined_rounded_lens_sweep(
        &query, &input, &hit) == MDKR_CAMERA_SWEEP_INVALID);
    expect_canonical_invalid("rounded duplicate within epsilon canonical", &hit);
}

static void test_two_phase_dispatch(void) {
    TestSource sphere = {
        MDKR_CAMERA_SWEEP_CLEAR, { 0 }, 0U, 0U, 0U, NULL, NULL,
    };
    TestSource exact = {
        MDKR_CAMERA_SWEEP_HIT, valid_hit(0.6f, 700U, 7U), 0U, 0U, 0U, NULL, NULL,
    };
    MdkrCameraObstructionQuerySource sphere_sources[] = {
        { test_source_sweep, &sphere },
    };
    MdkrCameraObstructionRoundedLensQuerySource exact_sources[] = {
        { test_rounded_source_sweep, &exact },
    };
    MdkrCameraObstructionCombinedQuery sphere_query = make_query(sphere_sources, 1U);
    MdkrCameraObstructionRoundedLensCombinedQuery exact_query =
        make_rounded_query(exact_sources, 1U);
    MdkrCameraSweepInput sphere_input = valid_input();
    MdkrCameraRoundedLensSweepInput rounded_input = valid_rounded_input();
    MdkrCameraObstructionTwoPhaseDecision decision;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;
    double exact_radius;

    sphere_input.mask = 0xA5U;
    sphere_input.ignored_object_generation = 77U;
    sphere.required_mask = sphere_input.mask;
    sphere.required_ignored_generation = sphere_input.ignored_object_generation;
    exact.required_mask = sphere_input.mask;
    exact.required_ignored_generation = sphere_input.ignored_object_generation;
    s_sphere_callback_count = 0U;
    s_rounded_callback_count = 0U;
    status = mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
        &sphere_query, &exact_query, &sphere_input, &rounded_input.guard, &hit, &decision);
    expect_true("two phase sphere clear", status == MDKR_CAMERA_SWEEP_CLEAR);
    expect_true("two phase sphere clear outcome",
                decision.outcome == MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_SPHERE_CLEAR &&
                decision.sphere_status == MDKR_CAMERA_SWEEP_CLEAR &&
                !decision.exact_invoked && !decision.degraded);
    expect_true("two phase sphere clear skips exact",
                s_sphere_callback_count == 1U && s_rounded_callback_count == 0U);
    expect_canonical_clear("two phase sphere clear canonical", &hit);
    expect_true("two phase sphere promoted outward",
                mdkr_camera_rounded_lens_guard_conservative_radius(
                    &rounded_input.guard, &exact_radius) &&
                (double)s_last_sphere_input.guard.radius >= exact_radius);

    sphere.status = MDKR_CAMERA_SWEEP_HIT;
    sphere.hit = valid_hit(0.25f, 100U, 1U);
    exact.status = MDKR_CAMERA_SWEEP_CLEAR;
    s_rounded_callback_count = 0U;
    status = mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
        &sphere_query, &exact_query, &sphere_input, &rounded_input.guard, &hit, &decision);
    expect_true("two phase exact rejects false positive", status == MDKR_CAMERA_SWEEP_CLEAR);
    expect_true("two phase exact clear outcome",
                decision.outcome == MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_CLEAR &&
                decision.exact_invoked && !decision.degraded);
    expect_true("two phase exact called once", s_rounded_callback_count == 1U);
    expect_true("two phase exact forwards corridor",
                memcmp(&s_last_rounded_input.start_eye, &sphere_input.start_eye,
                       sizeof(sphere_input.start_eye)) == 0 &&
                memcmp(&s_last_rounded_input.desired_eye, &sphere_input.desired_eye,
                       sizeof(sphere_input.desired_eye)) == 0 &&
                s_last_rounded_input.mask == sphere_input.mask &&
                s_last_rounded_input.ignored_object_generation ==
                    sphere_input.ignored_object_generation);
    expect_canonical_clear("two phase exact clear canonical", &hit);

    exact.status = MDKR_CAMERA_SWEEP_HIT;
    exact.hit = valid_hit(0.5f, 900U, 9U);
    status = mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
        &sphere_query, &exact_query, &sphere_input, &rounded_input.guard, &hit, &decision);
    expect_true("two phase exact hit", status == MDKR_CAMERA_SWEEP_HIT &&
                hit.stable_id == 900U && hit.fraction == 0.5f);
    expect_true("two phase exact hit replaces sphere",
                decision.outcome == MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_HIT &&
                decision.exact_status == MDKR_CAMERA_SWEEP_HIT && !decision.degraded);

    exact.status = MDKR_CAMERA_SWEEP_INVALID;
    status = mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
        &sphere_query, &exact_query, &sphere_input, &rounded_input.guard, &hit, &decision);
    expect_true("two phase invalid exact keeps sphere", status == MDKR_CAMERA_SWEEP_HIT &&
                hit.stable_id == sphere.hit.stable_id && hit.fraction == sphere.hit.fraction);
    expect_true("two phase invalid exact degrades",
                decision.outcome == MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_CONSERVATIVE_FALLBACK &&
                decision.exact_status == MDKR_CAMERA_SWEEP_INVALID &&
                decision.exact_invoked && decision.degraded);

    sphere.status = MDKR_CAMERA_SWEEP_INVALID;
    s_rounded_callback_count = 0U;
    status = mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
        &sphere_query, &exact_query, &sphere_input, &rounded_input.guard, &hit, &decision);
    expect_true("two phase invalid sphere fails closed", status == MDKR_CAMERA_SWEEP_INVALID);
    expect_true("two phase invalid sphere skips exact", !decision.exact_invoked &&
                s_rounded_callback_count == 0U);
    expect_canonical_invalid("two phase invalid sphere canonical", &hit);

    sphere.status = MDKR_CAMERA_SWEEP_CLEAR;
    rounded_input.guard.forward.x = NAN;
    status = mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
        &sphere_query, &exact_query, &sphere_input, &rounded_input.guard, &hit, &decision);
    expect_true("two phase invalid guard rejected before proof",
                status == MDKR_CAMERA_SWEEP_INVALID && !decision.exact_invoked);
    expect_canonical_invalid("two phase invalid guard canonical", &hit);
}

int main(void) {
    test_permutation_and_stable_order();
    test_earliest_overlap_filters_and_adapter();
    test_invalid_zero_capacity_purity_and_determinism();
    test_sphere_time_tie_epsilon();
    test_rounded_multiple_sources_and_permutation();
    test_rounded_invalid_contracts_and_purity();
    test_rounded_time_tie_epsilon();
    test_two_phase_dispatch();

    if (s_failures != 0) {
        fprintf(stderr, "%d camera-obstruction-query assertion(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    puts("camera-obstruction-query: all assertions passed");
    return EXIT_SUCCESS;
}
