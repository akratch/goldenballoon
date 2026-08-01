#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx_shadow_frame.h"

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void identity(float matrix[4][4]) {
    memset(matrix, 0, 16 * sizeof(float));
    for (int index = 0; index < 4; index++) {
        matrix[index][index] = 1.0f;
    }
}

int main(void) {
    float world[4][4];
    float view_projection[4][4];
    float viewport_a[4] = {0.0f, 0.0f, 320.0f, 240.0f};
    float viewport_b[4] = {0.0f, 0.0f, 160.0f, 240.0f};
    float positions[9] = {
        -2.0f, 1.0f, 4.0f,
         3.0f, 2.0f, 5.0f,
         1.0f, 7.0f, -6.0f,
    };
    float uv[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f};
    GfxShadowMaterial opaque = {
        .texture_id = 0,
        .alpha_mode = GFX_SHADOW_ALPHA_OPAQUE,
        .two_sided = false,
    };
    GfxShadowMaterial masked = {
        .texture_id = 42,
        .alpha_mode = GFX_SHADOW_ALPHA_MASKED,
        .two_sided = true,
    };
    GfxShadowMatrixBinding binding;
    GfxPresentationMatrixOwner owner;
    GfxPresentationOwnerStats owner_stats;
    GfxWorldFxStats stats;
    const GfxShadowFrame *frame;
    int key_a;
    int key_b;
    int owner_key;
    int consumed_key;
    int static_source;
    int static_source_b;
    int view_a;
    int view_b;

    identity(world);
    identity(view_projection);
    world[3][0] = 17.0f;
    view_projection[2][3] = -1.0f;

    gfx_shadow_stage_begin(7);
    expect(
        !gfx_shadow_matrix_register(
            NULL, world, view_projection, GFX_SHADOW_MOBILITY_STATIC),
        "NULL matrix key fails closed");
    expect(
        gfx_shadow_matrix_register(
            &key_a, world, view_projection, GFX_SHADOW_MOBILITY_STATIC),
        "matrix binding registers");
    memset(&binding, 0, sizeof(binding));
    expect(
        gfx_shadow_matrix_lookup(&key_a, &binding) &&
        memcmp(binding.world, world, sizeof(world)) == 0 &&
        memcmp(binding.view_projection, view_projection,
               sizeof(view_projection)) == 0 &&
        binding.mobility == GFX_SHADOW_MOBILITY_STATIC,
        "matrix binding round trips by exact pointer");
    expect(
        !gfx_shadow_matrix_lookup(&key_b, &binding),
        "unknown matrix key fails closed");

    memset(&owner, 0, sizeof(owner));
    owner.address = &owner_key;
    owner.generation = 17;
    owner.matrix_class = GFX_PRESENTATION_MATRIX_ROOT;
    owner.source_scale = 1.0f;
    owner.scale_y = 1.0f;
    identity(owner.parent_world);
    owner.valid = true;
    gfx_shadow_matrix_set_presentation_owner(&owner);
    expect(gfx_shadow_matrix_register(
               &owner_key, world, view_projection,
               GFX_SHADOW_MOBILITY_DYNAMIC),
           "presentation owner registers with its matrix");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_shadow_matrix_lookup(&owner_key, &binding) &&
               binding.presentation_owner.valid &&
               binding.presentation_owner.address == &owner_key &&
               binding.presentation_owner.generation == 17 &&
               binding.presentation_owner.matrix_class ==
                   GFX_PRESENTATION_MATRIX_ROOT,
           "presentation owner metadata round trips by value");

    /* Per-call tags must clear on failure: a rejected key cannot lend its
     * object identity to the following untagged matrix. */
    gfx_shadow_matrix_set_presentation_owner(&owner);
    expect(!gfx_shadow_matrix_register(
               NULL, world, view_projection, GFX_SHADOW_MOBILITY_DYNAMIC),
           "presentation owner failure control rejects the null key");
    expect(gfx_shadow_matrix_register(
               &consumed_key, world, view_projection,
               GFX_SHADOW_MOBILITY_DYNAMIC),
           "matrix after failed owner registration still registers");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_shadow_matrix_lookup(&consumed_key, &binding) &&
               !binding.presentation_owner.valid,
           "failed registration consumes presentation owner metadata");
    memset(&owner_stats, 0, sizeof(owner_stats));
    gfx_shadow_presentation_owner_get_stats(&owner_stats);
    expect(owner_stats.registrations == 3 && owner_stats.roots == 1 &&
               owner_stats.unowned == 2,
           "presentation owner census classifies owned and unowned matrices");

    gfx_shadow_capture_begin();
    frame = gfx_shadow_frame_previous();
    expect(!frame->valid, "begin does not expose a partial write frame");
    view_a = gfx_shadow_capture_view(viewport_a, view_projection);
    view_b = gfx_shadow_capture_view(viewport_b, view_projection);
    expect(view_a == 0 && view_b == 1, "distinct viewports get stable slots");
    expect(
        gfx_shadow_capture_view(viewport_a, view_projection) == view_a,
        "repeated viewport reuses its slot");
    expect(
        gfx_shadow_capture_triangle(
            view_a, &static_source, GFX_SHADOW_MOBILITY_STATIC,
            positions, uv, &opaque) &&
        gfx_shadow_capture_triangle(
            view_a, &static_source, GFX_SHADOW_MOBILITY_STATIC,
            positions, uv, &opaque) &&
        gfx_shadow_capture_triangle(
            view_b, &key_b, GFX_SHADOW_MOBILITY_DYNAMIC,
            positions, uv, &masked),
        "static opaque and dynamic masked triangles capture");
    gfx_shadow_capture_commit();

    frame = gfx_shadow_frame_previous();
    expect(
        frame->valid && !frame->failed && frame->generation == 1 &&
        frame->stage_generation == 7,
        "complete frame publishes atomically");
    expect(
        frame->view_count == 2 &&
        frame->static_vertex_count == 3 &&
        frame->static_range_count == 1 &&
        frame->vertex_count == 3 &&
        frame->range_count == 1,
        "frame separates stage-stable and dynamic ownership");
    expect(
        gfx_shadow_previous_view_index(viewport_a) == 0 &&
        gfx_shadow_previous_view_index(viewport_b) == 1 &&
        gfx_shadow_previous_view_index(
            (const float[4]) {1.0f, 2.0f, 3.0f, 4.0f}) == -1,
        "published viewport identity resolves exact receiver ownership");
    expect(
        frame->views[0].triangle_count == 2 &&
        frame->views[1].triangle_count == 1,
        "per-view triangle counts are exact");
    expect(
        frame->views[0].bounds_min[0] == -2.0f &&
        frame->views[0].bounds_min[2] == -6.0f &&
        frame->views[0].bounds_max[1] == 7.0f,
        "world bounds cover every captured vertex");
    expect(
        frame->static_ranges[0].vertex_count == 3 &&
        frame->static_ranges[0].view_index == UINT8_MAX &&
        frame->static_ranges[0].alpha_mode == GFX_SHADOW_ALPHA_OPAQUE &&
        frame->ranges[0].texture_id == 42 &&
        frame->ranges[0].alpha_mode == GFX_SHADOW_ALPHA_MASKED,
        "ranges retain shared-static and per-view dynamic classification");

    gfx_shadow_capture_begin();
    view_a = gfx_shadow_capture_view(viewport_a, view_projection);
    expect(
        gfx_shadow_capture_triangle(
            view_a, &static_source, GFX_SHADOW_MOBILITY_STATIC,
            positions, uv, &opaque),
        "stage cache accepts a repeated source in a later frame");
    gfx_shadow_capture_commit();
    frame = gfx_shadow_frame_previous();
    expect(
        frame->valid && frame->static_vertex_count == 3 &&
        frame->vertex_count == 0 && frame->views[0].triangle_count == 1,
        "stage cache persists without duplicating static geometry");

    /*
     * Admit a second same-material static source, then fail the write frame.
     * The cache is append-only, but the retained published frame must preserve
     * its counts and its final range metadata exactly.
     */
    {
        uint64_t retained_generation = frame->generation;
        size_t retained_vertices = frame->static_vertex_count;
        size_t retained_ranges = frame->static_range_count;
        size_t retained_range_vertices =
            frame->static_ranges[frame->static_range_count - 1].vertex_count;

        gfx_shadow_capture_begin();
        view_a = gfx_shadow_capture_view(viewport_a, view_projection);
        expect(
            gfx_shadow_capture_triangle(
                view_a, &static_source_b, GFX_SHADOW_MOBILITY_STATIC,
                positions, uv, &opaque),
            "new static source can be staged before a later frame failure");
        for (int index = 1; index <= GFX_SHADOW_MAX_VIEWS; index++) {
            float viewport[4] = {
                (float)(index * 10), 0.0f, 8.0f, 8.0f
            };
            (void)gfx_shadow_capture_view(viewport, view_projection);
        }
        gfx_shadow_capture_commit();
        frame = gfx_shadow_frame_previous();
        expect(
            frame->generation == retained_generation &&
            frame->static_vertex_count == retained_vertices &&
            frame->static_range_count == retained_ranges &&
            frame->static_ranges[retained_ranges - 1].vertex_count ==
                retained_range_vertices,
            "failed static admission cannot mutate the retained frame");
    }

    gfx_shadow_capture_begin();
    view_a = gfx_shadow_capture_view(viewport_a, view_projection);
    expect(
        gfx_shadow_capture_triangle(
            view_a, &static_source_b, GFX_SHADOW_MOBILITY_STATIC,
            positions, uv, &opaque),
        "staged static source remains reusable after frame rollback");
    gfx_shadow_capture_commit();
    frame = gfx_shadow_frame_previous();
    expect(
        frame->valid && frame->generation == 4 &&
        frame->static_vertex_count == 6 &&
        frame->static_range_count == 2 &&
        frame->static_ranges[0].vertex_count == 3 &&
        frame->static_ranges[1].first_vertex == 3 &&
        frame->static_ranges[1].vertex_count == 3,
        "next successful frame atomically publishes staged static geometry");

    gfx_shadow_capture_begin();
    for (int index = 0; index <= GFX_SHADOW_MAX_VIEWS; index++) {
        float viewport[4] = {
            (float)(index * 10), 0.0f, 8.0f, 8.0f
        };
        (void)gfx_shadow_capture_view(viewport, view_projection);
    }
    gfx_shadow_capture_commit();
    frame = gfx_shadow_frame_previous();
    expect(
        frame->valid && frame->generation == 4 &&
        frame->static_vertex_count == 6,
        "failed construction retains the previous complete frame");

    gfx_shadow_stage_begin(8);
    frame = gfx_shadow_frame_previous();
    expect(
        !frame->valid && frame->static_vertex_count == 0,
        "stage transition invalidates all prior static geometry");
    gfx_shadow_capture_begin();
    gfx_shadow_capture_commit();
    frame = gfx_shadow_frame_previous();
    expect(
        !frame->valid && frame->vertex_count == 0 &&
        frame->static_vertex_count == 0 && frame->view_count == 0,
        "empty stage frame replaces prior content instead of ghost casters");

    gfx_world_fx_get_stats(&stats);
    expect(
        stats.frames_begun == 6 &&
        stats.frames_committed == 4 &&
        stats.frames_failed == 2 &&
        stats.triangles_captured == 6 &&
        stats.opaque_triangles == 5 &&
        stats.masked_triangles == 1 &&
        stats.static_cache_hits == 3 &&
        stats.static_cache_misses == 2 &&
        stats.matrix_lookup_hits == 3 &&
        stats.matrix_lookup_misses == 1,
        "telemetry records both directions");

    gfx_shadow_matrix_registry_reset();
    expect(
        !gfx_shadow_matrix_lookup(&key_a, &binding),
        "matrix registry reset expires the frame's bindings");

    expect(
        !gfx_shadow_projected_range_contains(&key_a) &&
        gfx_shadow_projected_range_mark(&key_a) &&
        gfx_shadow_projected_range_mark(&key_a) &&
        gfx_shadow_projected_range_contains(&key_a),
        "projected fallback markers are exact and idempotent");
    gfx_shadow_projected_ranges_reset();
    expect(
        !gfx_shadow_projected_range_contains(&key_a),
        "projected fallback markers expire at the task boundary");

    /* Caster-exclusion markers share the projected seam's lifecycle. */
    expect(
        gfx_shadow_caster_exclude_mark(&key_b) &&
        gfx_shadow_caster_exclude_mark(&key_b) &&
        gfx_shadow_caster_excluded(&key_b),
        "caster exclusion markers are exact and idempotent");
    gfx_shadow_projected_ranges_reset();
    expect(
        !gfx_shadow_caster_excluded(&key_b),
        "caster exclusion markers expire at the task boundary");

    /* Wave "shadowplay" hardening: plausibility clamp, masked bounds
     * exclusion, and the committed-watermark static range merge. */
    gfx_shadow_stage_begin(9);
    gfx_shadow_capture_begin();
    view_a = gfx_shadow_capture_view(viewport_a, view_projection);
    {
        float wild[9];
        memcpy(wild, positions, sizeof(wild));
        wild[3] = 1.0e9f;
        expect(
            !gfx_shadow_capture_triangle(
                view_a, &static_source,
                GFX_SHADOW_MOBILITY_STATIC, wild, uv, &opaque),
            "finite but implausible vertices are rejected");
    }
    gfx_world_fx_get_stats(&stats);
    expect(
        stats.implausible_triangles == 1,
        "implausible rejection is counted");
    expect(
        gfx_shadow_capture_triangle(
            view_a, &static_source,
            GFX_SHADOW_MOBILITY_STATIC, positions, uv, &opaque) &&
        gfx_shadow_capture_triangle(
            view_a, &static_source_b,
            GFX_SHADOW_MOBILITY_STATIC, positions, uv, &opaque),
        "plausible static admissions still succeed");
    gfx_shadow_capture_commit();
    frame = gfx_shadow_frame_previous();
    expect(
        frame->valid &&
        frame->static_range_count == 1 &&
        frame->static_vertex_count == 6,
        "same-material static admissions before a commit share one range");
    {
        int static_source_c = 0;
        size_t committed_ranges = frame->static_range_count;
        gfx_shadow_capture_begin();
        view_a = gfx_shadow_capture_view(viewport_a, view_projection);
        expect(
            gfx_shadow_capture_triangle(
                view_a, &static_source_c,
                GFX_SHADOW_MOBILITY_STATIC, positions, uv, &opaque),
            "post-commit static admission succeeds");
        expect(
            frame->static_range_count == committed_ranges,
            "published static range counts stay immutable past the watermark");
        gfx_shadow_capture_commit();
        frame = gfx_shadow_frame_previous();
        expect(
            frame->static_range_count == 2,
            "post-watermark admissions open their own range");
    }
    gfx_shadow_capture_begin();
    view_a = gfx_shadow_capture_view(viewport_a, view_projection);
    {
        float tall[9];
        memcpy(tall, positions, sizeof(tall));
        tall[1] = 2000.0f;
        expect(
            gfx_shadow_capture_triangle(
                view_a, &key_a,
                GFX_SHADOW_MOBILITY_DYNAMIC, tall, uv, &masked),
            "masked capture is still classified");
    }
    gfx_shadow_capture_commit();
    frame = gfx_shadow_frame_previous();
    expect(
        frame->valid && frame->views[0].bounds_max[1] < 1999.0f,
        "masked ranges do not extend the caster depth bounds");

    /*
     * Replay freeze/restore/release state machine (Phase 3 Wave B slice 2).
     *
     * The restore takes CUSTODY of the live registry: it copies it aside, drops
     * the frozen copy on top, and gfx_shadow_replay_release is what puts the
     * live one back. The custody flag is what gfx_shadow_replay_restore refuses
     * on, so if any path ever leaves it set the replay is dead for the rest of
     * the process and the registry keeps serving the frozen set. This asserts
     * the round trip end to end and that both re-entry refusals are in place.
     */
    {
        uint64_t freezes = 0, restores = 0, freeze_fail = 0, restore_fail = 0;
        float frozen_world[4][4];
        int frozen_key;
        int live_key;

        gfx_shadow_matrix_registry_reset();
        identity(frozen_world);
        frozen_world[3][0] = 101.0f;
        expect(
            gfx_shadow_matrix_register(
                &frozen_key, frozen_world, view_projection,
                GFX_SHADOW_MOBILITY_STATIC),
            "replay: the frozen tick registers its matrix");
        gfx_shadow_replay_freeze();
        expect(gfx_shadow_replay_frozen(), "replay: freeze succeeds");
        expect(gfx_shadow_replay_frozen_matrix_count() == 1,
               "replay: freeze captured exactly the registered matrix");

        /* The next tick's display list is built and registered before any
         * interpolated present runs -- that is the whole reason the restore
         * has to displace rather than overwrite. */
        gfx_shadow_matrix_registry_reset();
        expect(
            gfx_shadow_matrix_register(
                &live_key, world, view_projection,
                GFX_SHADOW_MOBILITY_STATIC),
            "replay: the next tick registers its own matrix");

        expect(gfx_shadow_replay_restore(NULL, 0),
               "replay: restore installs the frozen registry");
        memset(&binding, 0, sizeof(binding));
        expect(gfx_shadow_matrix_lookup(&frozen_key, &binding) &&
                   memcmp(binding.world, frozen_world,
                          sizeof(frozen_world)) == 0,
               "replay: the frozen tick's matrix is what the replay walks");
        expect(!gfx_shadow_matrix_lookup(&live_key, &binding),
               "replay: the live registration is displaced, not merged");
        expect(!gfx_shadow_replay_restore(NULL, 0),
               "replay: a second restore refuses while custody is held");

        expect(gfx_shadow_replay_release(),
               "replay: release returns custody");
        memset(&binding, 0, sizeof(binding));
        expect(gfx_shadow_matrix_lookup(&live_key, &binding) &&
                   memcmp(binding.world, world, sizeof(world)) == 0,
               "replay: release puts the live registration back verbatim");
        expect(!gfx_shadow_matrix_lookup(&frozen_key, &binding),
               "replay: the frozen copy does not survive the release");
        expect(!gfx_shadow_replay_release(),
               "replay: release without custody is a no-op");
        expect(gfx_shadow_replay_restore(NULL, 0),
               "replay: restore is usable again after a clean release");
        expect(gfx_shadow_replay_release(), "replay: and releases again");

        gfx_shadow_replay_get_stats(&freezes, &restores, &freeze_fail,
                                    &restore_fail);
        expect(freezes == 1 && restores == 2 && freeze_fail == 0 &&
                   restore_fail == 0,
               "replay: the round trip reports no freeze or unwind failures");
        gfx_shadow_matrix_registry_reset();
    }

    gfx_shadow_frame_shutdown();
    gfx_world_fx_get_stats(&stats);
    expect(
        stats.frames_begun == 0 && stats.peak_vertices == 0,
        "shutdown releases and resets all ownership");

    if (failures != 0) {
        fprintf(stderr, "%d shadow-frame test(s) failed\n", failures);
        return 1;
    }
    puts("PASS shadow frame");
    return 0;
}
