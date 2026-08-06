#include "camera_obstruction_resolver.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_EPSILON 0.0005f

static int s_failures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_near(const char *name, float actual, float expected, float tolerance) {
    if (!isfinite(actual) || fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "FAIL %s actual=%f expected=%f (+/-%f)\n",
                name, actual, expected, tolerance);
        s_failures++;
    }
}

static MdkrCameraProjection test_projection(uint64_t generation) {
    const MdkrCameraProjection projection = {
        .near_plane = 1.0f,
        .far_plane = 1000.0f,
        .vertical_fov = 28.6478897565f,
        .aspect = 1.0f,
        .generation = generation,
    };
    return projection;
}

static MdkrCameraObstructionResolverConfig test_config(
    MdkrCameraObstructionPolicy policy) {
    const MdkrCameraObstructionResolverConfig config = {
        .policy = policy,
        .clearance_skin = 0.1f,
        .recovery_speed = 4.0f,
        .max_recovery_step = 0.5f,
    };
    return config;
}

static MdkrCameraOcclusionWorld one_wall_world(
    const MdkrCameraVec3 *vertices,
    const uint32_t *indices,
    const MdkrCameraOcclusionTriangle *triangles) {
    const MdkrCameraOcclusionWorld world = {
        .vertices = vertices,
        .vertex_count = 3U,
        .indices = indices,
        .triangles = triangles,
        .triangle_count = 1U,
    };
    return world;
}

static MdkrCameraObstructionResolverInput resolver_input(
    const MdkrCameraOcclusionWorld *world,
    const MdkrCameraProjection *projection,
    MdkrCameraVec3 start_safe_eye,
    MdkrCameraVec3 desired_eye) {
    const MdkrCameraObstructionResolverInput input = {
        .query = {
            .sweep = mdkr_camera_occlusion_world_sweep,
            .context = world,
        },
        .projection = projection,
        .pivot = { 0.0f, 0.0f, 0.0f },
        .start_safe_eye = start_safe_eye,
        .desired_eye = desired_eye,
        .fixed_delta_seconds = 0.1f,
    };
    return input;
}

static void test_immediate_retract_has_skin_and_no_penetration(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5.0f, -10.0f, -10.0f }, { 5.0f, 10.0f, -10.0f }, { 5.0f, 0.0f, 10.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 1U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraProjection projection = test_projection(1U);
    const MdkrCameraObstructionResolverConfig config = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state;
    MdkrCameraObstructionResolverResult result;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus sweep;

    mdkr_camera_obstruction_resolver_reset(&state);
    expect_true("wall retract status",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    expect_true("wall retract accepted", result.accepted && result.path_was_blocked);
    expect_true("wall retract identifies blocker",
                result.blocker_kind == 1U && result.blocker_stable_id == 1U);
    expect_true("wall retract is behind contact", result.resolved_eye.x < 3.9f);
    expect_true("wall retract is forward of start", result.resolved_eye.x >= 0.0f);
    sweep = mdkr_camera_sweep(&world, &(MdkrCameraSweepInput){
        .guard = result.guard,
        .start_eye = result.resolved_eye,
        .desired_eye = result.resolved_eye,
    }, &hit);
    expect_true("retracted eye is not penetrating", sweep == MDKR_CAMERA_SWEEP_CLEAR);

    input.desired_eye.x = 20.0f;
    expect_true("larger desired boom still retracts",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    expect_true("retract does not overshoot wall", result.resolved_eye.x < 3.9f);
}

static void test_recovery_is_monotonic_and_bounded(void) {
    const MdkrCameraOcclusionWorld empty_world = { 0 };
    const MdkrCameraProjection projection = test_projection(1U);
    const MdkrCameraObstructionResolverConfig config = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &empty_world, &projection, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state;
    MdkrCameraObstructionResolverResult result;
    float previous = 0.0f;
    int i;

    mdkr_camera_obstruction_resolver_reset(&state);
    for (i = 0; i < 8; i++) {
        expect_true("clear recovery status",
                    mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                        MDKR_CAMERA_OBSTRUCTION_RESOLVER_RECOVERING);
        expect_true("clear recovery is not a blocker", !result.path_was_blocked);
        expect_true("clear recovery clears blocker identity",
                    result.blocker_kind == 0U && result.blocker_stable_id == 0U);
        expect_true("recovery is monotonic", result.resolved_eye.x >= previous);
        expect_true("recovery has speed bound", result.resolved_eye.x - previous <= 0.4001f);
        expect_true("recovery has no overshoot", result.resolved_eye.x <= 10.0f);
        previous = result.resolved_eye.x;
        input.start_safe_eye = result.resolved_eye;
    }
    expect_near("fixed-step recovery distance", previous, 3.2f, TEST_EPSILON);
}

static void test_zero_time_revalidates_without_recovery_motion(void) {
    const MdkrCameraOcclusionWorld empty_world = { 0 };
    const MdkrCameraProjection projection = test_projection(2U);
    const MdkrCameraObstructionResolverConfig config = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &empty_world, &projection, (MdkrCameraVec3){ 3.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state = {
        .last_safe_eye = { 3.0f, 0.0f, 0.0f },
        .projection_generation = 1U,
        .last_safe_valid = 1U,
    };
    MdkrCameraObstructionResolverResult result;

    input.fixed_delta_seconds = 0.0f;
    expect_true("zero-time projection revalidation recovers",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RECOVERING);
    expect_true("zero-time projection was revalidated", result.projection_revalidated);
    expect_near("zero-time recovery x is frozen", result.resolved_eye.x, 3.0f, TEST_EPSILON);
}

static void test_projection_generation_revalidates_last_safe(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5.0f, -10.0f, -10.0f }, { 5.0f, 10.0f, -10.0f }, { 5.0f, 0.0f, 10.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 2U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraProjection projection = test_projection(2U);
    const MdkrCameraObstructionResolverConfig config = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 4.5f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state = {
        .last_safe_eye = { 0.0f, 0.0f, 0.0f },
        .projection_generation = 1U,
        .last_safe_valid = 1U,
    };
    MdkrCameraObstructionResolverResult result;

    expect_true("projection change resolves",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    expect_true("projection change revalidates", result.projection_revalidated);
    expect_true("projection change uses revalidated safe", result.used_last_safe);
    expect_true("projection generation advances", state.projection_generation == 2U);
    expect_true("projection output remains safe", result.resolved_eye.x < 3.9f);
}

static void test_discontinuity_drops_stale_fallback(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5.0f, -10.0f, -10.0f }, { 5.0f, 10.0f, -10.0f }, { 5.0f, 0.0f, 10.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 3U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraProjection projection = test_projection(1U);
    const MdkrCameraObstructionResolverConfig config = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 4.5f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state = {
        .last_safe_eye = { -10.0f, 0.0f, 0.0f },
        .projection_generation = 1U,
        .last_safe_valid = 1U,
    };
    MdkrCameraObstructionResolverResult result;

    input.discontinuity = 1U;
    expect_true("discontinuity uses new route",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    expect_true("discontinuity does not use stale fallback", !result.used_last_safe);
    expect_true("discontinuity publishes new safe", state.last_safe_valid && state.last_safe_eye.x >= 0.0f);
}

static void test_invalid_world_fails_closed_without_state_advance(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 0.0f }, { 6.0f, 0.0f, 0.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 4U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraProjection projection = test_projection(1U);
    const MdkrCameraObstructionResolverConfig config = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    const MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state = {
        .last_safe_eye = { -2.0f, 0.0f, 0.0f }, .projection_generation = 9U, .last_safe_valid = 1U,
    };
    const MdkrCameraObstructionResolverState before = state;
    MdkrCameraProjection zero_generation = projection;
    MdkrCameraObstructionResolverInput zero_generation_input = input;
    MdkrCameraObstructionResolverResult result;

    expect_true("invalid world fails closed",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID);
    expect_true("invalid world not accepted", !result.accepted);
    expect_true("invalid world retains state", memcmp(&state, &before, sizeof(state)) == 0);

    zero_generation.generation = 0U;
    zero_generation_input.projection = &zero_generation;
    expect_true("zero projection generation is rejected",
                mdkr_camera_obstruction_resolve(&config, &zero_generation_input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID);
    expect_true("zero generation does not advance state", memcmp(&state, &before, sizeof(state)) == 0);
}

static void test_overlap_without_revalidated_fallback_fails_closed(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5.0f, -10.0f, -10.0f }, { 5.0f, 10.0f, -10.0f }, { 5.0f, 0.0f, 10.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 9U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraProjection projection = test_projection(1U);
    const MdkrCameraObstructionResolverConfig config = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 4.5f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state;
    MdkrCameraObstructionResolverResult result;

    mdkr_camera_obstruction_resolver_reset(&state);
    input.pivot = input.start_safe_eye;
    expect_true("unrecoverable overlap fails closed",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_FAILSAFE);
    expect_true("unrecoverable overlap is not accepted", !result.accepted);
    expect_true("unrecoverable overlap does not manufacture safe state", !state.last_safe_valid);
}

static void test_repeat_is_deterministic(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5.0f, -10.0f, -10.0f }, { 5.0f, 10.0f, -10.0f }, { 5.0f, 0.0f, 10.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 5U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraProjection projection = test_projection(1U);
    const MdkrCameraObstructionResolverConfig config = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    const MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState first_state;
    MdkrCameraObstructionResolverState second_state;
    MdkrCameraObstructionResolverResult first;
    MdkrCameraObstructionResolverResult second;

    mdkr_camera_obstruction_resolver_reset(&first_state);
    mdkr_camera_obstruction_resolver_reset(&second_state);
    expect_true("first deterministic resolve", mdkr_camera_obstruction_resolve(
        &config, &input, &first_state, &first) == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    expect_true("second deterministic resolve", mdkr_camera_obstruction_resolve(
        &config, &input, &second_state, &second) == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    expect_true("deterministic result bytes", memcmp(&first, &second, sizeof(first)) == 0);
    expect_true("deterministic state bytes", memcmp(&first_state, &second_state, sizeof(first_state)) == 0);
}

static void test_center_ray_is_the_expected_near_plane_control(void) {
    const MdkrCameraVec3 vertices[] = {
        { 50.0f, 10.0f, -4.0f }, { 50.0f, 14.0f, -4.0f }, { 50.0f, 12.0f, 4.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 6U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraProjection projection = {
        .near_plane = 10.0f,
        .far_plane = 15000.0f,
        .vertical_fov = 60.0f,
        .aspect = 4.0f / 3.0f,
        .generation = 1U,
    };
    MdkrCameraObstructionResolverConfig modern = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverConfig center = test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_CENTER_RAY);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 100.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState modern_state;
    MdkrCameraObstructionResolverState center_state;
    MdkrCameraObstructionResolverResult result;

    modern.recovery_speed = center.recovery_speed = 1000.0f;
    modern.max_recovery_step = center.max_recovery_step = 1000.0f;
    input.fixed_delta_seconds = 1.0f;
    mdkr_camera_obstruction_resolver_reset(&modern_state);
    expect_true("modern near-plane guard catches corner", mdkr_camera_obstruction_resolve(
        &modern, &input, &modern_state, &result) == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    mdkr_camera_obstruction_resolver_reset(&center_state);
    expect_true("center ray misses corner", mdkr_camera_obstruction_resolve(
        &center, &input, &center_state, &result) == MDKR_CAMERA_OBSTRUCTION_RESOLVER_CLEAR);
    expect_near("center ray reaches desired", result.resolved_eye.x, 100.0f, TEST_EPSILON);
}

/*
 * Release hysteresis (MOTION-01). A swept lens volume grazing a wall reports
 * CLEAR for a few ticks at facet seams; expanding the boom on that report and
 * retracting again on the next tick is the measured "pop and snap" chatter.
 * The resolver must therefore engage on contact with zero latency but hold a
 * latched retraction until the corridor has been clear for a whole window.
 *
 * The world here has one wall and the clear ticks are produced by moving the
 * desired eye short of it, which is the same shape as the runtime's dropout:
 * an identical anchor, a corridor the sweep calls clear, and a re-block.
 */
static void test_release_hold_absorbs_a_transient_clear_corridor(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5.0f, -10.0f, -10.0f }, { 5.0f, 10.0f, -10.0f }, { 5.0f, 0.0f, 10.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 7U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraOcclusionWorld empty_world = { 0 };
    const MdkrCameraProjection projection = test_projection(1U);
    MdkrCameraObstructionResolverConfig config =
        test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state;
    MdkrCameraObstructionResolverResult result;
    float retracted_x;
    int tick;

    config.release_hold_ticks = 4U;
    mdkr_camera_obstruction_resolver_reset(&state);

    /* Contact. Retraction is immediate and the hold cannot delay it. */
    expect_true("hold: contact retracts on the contact tick",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    expect_true("hold: contact tick is a real blocker",
                result.path_was_blocked && !result.release_held &&
                result.blocker_stable_id == 7U);
    retracted_x = result.resolved_eye.x;
    input.start_safe_eye = result.resolved_eye;

    /* The wall stops being reported. The boom must not grow. */
    input.query.context = &empty_world;
    for (tick = 1; tick < 4; tick++) {
        expect_true("hold: dropout keeps the correction engaged",
                    mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                        MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
        expect_true("hold: dropout is flagged as held, not as a contact",
                    result.release_held && !result.path_was_blocked);
        expect_true("hold: dropout reports no blocker it did not measure",
                    result.blocker_stable_id == 0U);
        expect_near("hold: boom does not expand while held",
                    result.resolved_eye.x, retracted_x, TEST_EPSILON);
        input.start_safe_eye = result.resolved_eye;
    }

    /* The window is served: recovery starts, bounded as before. */
    expect_true("hold: release lets recovery start",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RECOVERING);
    expect_true("hold: released tick is not marked held", !result.release_held);
    expect_true("hold: recovery is still speed bound",
                result.resolved_eye.x - retracted_x <= 0.4001f);
    expect_true("hold: recovery actually moves",
                result.resolved_eye.x > retracted_x);
}

static void test_release_hold_reengages_without_latency_and_resets(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5.0f, -10.0f, -10.0f }, { 5.0f, 10.0f, -10.0f }, { 5.0f, 0.0f, 10.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 8U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraOcclusionWorld empty_world = { 0 };
    const MdkrCameraProjection projection = test_projection(1U);
    MdkrCameraObstructionResolverConfig config =
        test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state;
    MdkrCameraObstructionResolverResult result;

    config.release_hold_ticks = 4U;
    mdkr_camera_obstruction_resolver_reset(&state);
    (void)mdkr_camera_obstruction_resolve(&config, &input, &state, &result);
    input.start_safe_eye = result.resolved_eye;

    input.query.context = &empty_world;
    (void)mdkr_camera_obstruction_resolve(&config, &input, &state, &result);
    input.start_safe_eye = result.resolved_eye;
    expect_true("hold: partial run is held", result.release_held);

    /* A contact inside the window restarts the whole window. */
    input.query.context = &world;
    expect_true("hold: contact inside the window retracts immediately",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    expect_true("hold: re-contact is a contact, not a held tick",
                result.path_was_blocked && !result.release_held);
    expect_true("hold: re-contact restarts the clear run",
                state.clear_run_ticks == 0U && state.retraction_latched);

    /* A published cut must not carry a latch from an unrelated shot. */
    input.query.context = &empty_world;
    input.discontinuity = 1U;
    expect_true("hold: a cut drops the latch instead of holding through it",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) !=
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED);
    expect_true("hold: a cut leaves nothing latched",
                !state.retraction_latched && state.clear_run_ticks == 0U);
}

static void test_zero_release_hold_is_the_pre_hysteresis_control(void) {
    const MdkrCameraVec3 vertices[] = {
        { 5.0f, -10.0f, -10.0f }, { 5.0f, 10.0f, -10.0f }, { 5.0f, 0.0f, 10.0f },
    };
    const uint32_t indices[] = { 0U, 1U, 2U };
    const MdkrCameraOcclusionTriangle triangles[] = { { 9U, 1U, 1U, 0U } };
    const MdkrCameraOcclusionWorld world = one_wall_world(vertices, indices, triangles);
    const MdkrCameraOcclusionWorld empty_world = { 0 };
    const MdkrCameraProjection projection = test_projection(1U);
    const MdkrCameraObstructionResolverConfig config =
        test_config(MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN);
    MdkrCameraObstructionResolverInput input = resolver_input(
        &world, &projection, (MdkrCameraVec3){ 0.0f, 0.0f, 0.0f },
        (MdkrCameraVec3){ 10.0f, 0.0f, 0.0f });
    MdkrCameraObstructionResolverState state;
    MdkrCameraObstructionResolverResult result;

    /* test_config leaves release_hold_ticks at 0: the defect must reproduce. */
    mdkr_camera_obstruction_resolver_reset(&state);
    (void)mdkr_camera_obstruction_resolve(&config, &input, &state, &result);
    input.start_safe_eye = result.resolved_eye;
    input.query.context = &empty_world;
    expect_true("no hold: a single clear tick releases immediately",
                mdkr_camera_obstruction_resolve(&config, &input, &state, &result) ==
                    MDKR_CAMERA_OBSTRUCTION_RESOLVER_RECOVERING);
    expect_true("no hold: nothing is reported as held", !result.release_held);
}

int main(void) {
    test_immediate_retract_has_skin_and_no_penetration();
    test_recovery_is_monotonic_and_bounded();
    test_zero_time_revalidates_without_recovery_motion();
    test_projection_generation_revalidates_last_safe();
    test_discontinuity_drops_stale_fallback();
    test_invalid_world_fails_closed_without_state_advance();
    test_overlap_without_revalidated_fallback_fails_closed();
    test_repeat_is_deterministic();
    test_center_ray_is_the_expected_near_plane_control();
    test_release_hold_absorbs_a_transient_clear_corridor();
    test_release_hold_reengages_without_latency_and_resets();
    test_zero_release_hold_is_the_pre_hysteresis_control();

    if (s_failures != 0) {
        fprintf(stderr, "%d camera-obstruction-resolver assertion(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    puts("camera-obstruction-resolver: all policy assertions passed");
    return EXIT_SUCCESS;
}
