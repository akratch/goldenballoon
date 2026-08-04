#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gfx_presentation_packet.h"
#include "gfx_deformation_shape.h"

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static GfxPresentationMatrixOwner make_owner(const void *address,
                                             uint64_t generation) {
    GfxPresentationMatrixOwner owner;
    memset(&owner, 0, sizeof(owner));
    owner.address = address;
    owner.generation = generation;
    owner.matrix_class = GFX_PRESENTATION_MATRIX_BILLBOARD;
    owner.source_scale = 1.0f;
    owner.scale_y = 1.0f;
    owner.valid = true;
    return owner;
}

int main(void) {
    unsigned char matrix[64];
    unsigned char vertex[10];
    unsigned char next_matrix[64];
    unsigned char particle_vertex[20];
    unsigned char projected_shadow_vertex[20];
    unsigned char projected_shadow_future_vertex[20];
    unsigned char deform_previous[20];
    unsigned char deform_current[20];
    unsigned char deform_skipped[20];
    unsigned char effect_previous_a[20];
    unsigned char effect_previous_b[20];
    unsigned char effect_current_a[20];
    unsigned char effect_current_b[20];
    unsigned char retained_identity[64];
    unsigned char retained_observation[64];
    unsigned char future_previous[20];
    unsigned char future_current[20];
    GfxPresentationMatrixOwner owner;
    GfxPresentationMatrixOwner effect_a;
    GfxPresentationMatrixOwner effect_b;
    GfxPresentationMatrixOwner shadow_owner;
    GfxPresentationMatrixOwner future_shadow_owner;
    GfxPresentationPacketBinding binding;
    GfxPresentationDeformationBinding deformation;
    GfxPresentationPacketStats stats;
    uint64_t simulated_live_tick;

    memset(matrix, 0x11, sizeof(matrix));
    memset(vertex, 0x22, sizeof(vertex));
    memset(next_matrix, 0x33, sizeof(next_matrix));
    owner = make_owner(matrix, 7u);

    expect(!gfx_deformation_shape_matches(
               UINT32_MAX, 2u, 20u, UINT32_MAX) &&
               !gfx_deformation_shape_matches(
                   UINT32_MAX, UINT32_MAX, 20u, UINT32_MAX) &&
               gfx_deformation_shape_matches(2u, 10u, 20u, UINT32_MAX),
           "wasm32 deformation multiplication guard rejects wrapped shapes");

    expect(!gfx_presentation_packet_register_matrix(
               NULL, sizeof(matrix), 0, &owner),
           "null matrix key fails closed");
    expect(!gfx_presentation_packet_register_vertex(
               vertex, GFX_PRESENTATION_PACKET_MAX_KEY_BYTES + 1u, 0, &owner),
           "oversized retained key fails closed");
    expect(gfx_presentation_packet_register_matrix(
               matrix, sizeof(matrix), 2, &owner),
           "matrix dependency registers");
    owner.generation = 8u;
    expect(gfx_presentation_packet_register_matrix(
               matrix, sizeof(matrix), 3, &owner),
           "latest tenant replaces a repeated matrix address");
    expect(gfx_presentation_packet_register_vertex(
               vertex, sizeof(vertex), 1, &owner),
           "vertex dependency registers");
    owner = make_owner(particle_vertex, 10u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES;
    expect(gfx_presentation_packet_register_vertex_identity(
               particle_vertex, 2, &owner) &&
               gfx_presentation_packet_lookup_live_vertex(
                   particle_vertex, &binding) &&
               binding.owner.generation == 10u && binding.viewport == 2 &&
               binding.key_size == 0u,
           "stable particle vertex buffer registers by identity");
    expect(gfx_presentation_packet_has_live_vertex(vertex) &&
               !gfx_presentation_packet_has_frozen_vertex(vertex),
           "vertex recipe is identifiable on the live side before freeze");

    expect(!gfx_presentation_packet_frozen(),
           "live registrations are not visible before publication");
    expect(gfx_presentation_packet_note_walked_matrix(
               matrix, matrix, sizeof(matrix)) &&
               gfx_presentation_packet_note_walked_vertex(
                   vertex, vertex, sizeof(vertex)),
           "real-walk dependency bytes replace the earlier build-time image");
    gfx_presentation_packet_freeze();
    expect(gfx_presentation_packet_frozen(),
           "complete dependency packet publishes atomically");
    expect(!gfx_presentation_packet_has_live_vertex(vertex) &&
               gfx_presentation_packet_has_frozen_vertex(vertex) &&
               gfx_presentation_packet_has_frozen_vertex(particle_vertex),
           "vertex recipe identity rotates to the frozen side");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_matrix(matrix, &binding) &&
               binding.owner.generation == 8u && binding.viewport == 3,
           "frozen matrix carries the latest owner by value");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(vertex, &binding) &&
               binding.owner.generation == 8u && binding.viewport == 1,
           "frozen vertex carries owner and viewport by value");

    matrix[0] ^= 1u;
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_matrix(matrix, &binding) &&
               binding.stale && binding.key_size == sizeof(matrix) &&
               binding.key_bytes[0] == 0x11u,
           "rewritten transient matrix resolves the frozen real-walk bytes");
    gfx_presentation_packet_note_stale_hold(true, true);
    matrix[0] ^= 1u;
    vertex[0] ^= 1u;
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(vertex, &binding) &&
               binding.stale && binding.key_size == sizeof(vertex) &&
               binding.key_bytes[0] == 0x22u,
           "rewritten billboard anchor resolves the frozen real-walk bytes");
    gfx_presentation_packet_note_stale_hold(false, true);
    vertex[0] ^= 1u;

    owner = make_owner(next_matrix, 9u);
    expect(gfx_presentation_packet_register_matrix(
               next_matrix, sizeof(next_matrix), 0, &owner),
           "next tick can build while the frozen packet remains visible");
    expect(!gfx_presentation_packet_lookup_matrix(next_matrix, &binding),
           "live next-tick dependency does not leak into frozen lookup");
    expect(gfx_presentation_packet_lookup_matrix(matrix, &binding),
           "frozen dependency remains immutable during next-tick build");

    gfx_presentation_packet_freeze();
    expect(gfx_presentation_packet_lookup_matrix(next_matrix, &binding) &&
               !gfx_presentation_packet_lookup_matrix(matrix, &binding),
           "next atomic freeze replaces rather than merges packets");

    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.matrix_registrations == 3u &&
               stats.vertex_registrations == 2u &&
               stats.particle_vertex_registrations == 1u &&
               stats.freezes == 2u &&
               stats.freeze_failures == 0u && stats.stale_keys == 2u &&
               stats.stale_matrix_holds == 1u &&
               stats.stale_vertex_holds == 1u &&
               stats.unsafe_stale_fallbacks == 0u &&
               stats.matrix_peak == 1u && stats.vertex_peak == 2u,
           "packet census reports ownership, bounds and stale refusal");

    memset(projected_shadow_vertex, 0x2A, sizeof(projected_shadow_vertex));
    owner = make_owner(projected_shadow_vertex, 11u);
    owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    expect(gfx_presentation_packet_register_projected_shadow_vertex(
               projected_shadow_vertex, 0, 4u, &owner) &&
               gfx_presentation_packet_lookup_live_vertex(
                   projected_shadow_vertex, &binding) &&
               binding.owner.matrix_class ==
                   GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES &&
               binding.viewport == 0 && binding.ordinal == 4u &&
               binding.key_size == 0u,
           "projected shadow vertex batch retains object identity and ordinal");
    expect(!gfx_presentation_packet_register_projected_shadow_vertex(
               NULL, 0, 4u, &owner),
           "projected shadow registration rejects a missing vertex identity");
    gfx_presentation_packet_freeze();
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(
               projected_shadow_vertex, &binding) &&
               binding.ordinal == 4u && binding.owner.generation == 11u,
           "projected shadow ownership freezes by value");
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.projected_shadow_vertex_registrations == 1u,
           "projected shadow registration has dedicated telemetry");

    /* Deformation history is indexed by stable object generation plus the
     * deterministic viewport/batch ordinal—not by the transient vertex
     * pointer. It only resolves an adjacent pair with identical topology. */
    memset(deform_previous, 0x41, sizeof(deform_previous));
    memset(deform_current, 0x52, sizeof(deform_current));
    memset(deform_skipped, 0x63, sizeof(deform_skipped));
    owner = make_owner(next_matrix, 19u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_ROOT;
    gfx_presentation_packet_capture_begin(11u);
    expect(!gfx_presentation_packet_capture_deformation(
               &owner, 2, 2u, deform_previous, sizeof(deform_previous),
               UINT32_MAX, 2u) &&
               !gfx_presentation_packet_capture_deformation(
                   &owner, 2, 2u, deform_previous, sizeof(deform_previous),
                   UINT32_MAX, UINT32_MAX),
           "hostile deformation count-stride products fail closed");
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 2, 3u, deform_previous, sizeof(deform_previous),
               2u, 10u),
           "first deformation batch captures by value");
    gfx_presentation_packet_freeze();
    memset(&deformation, 0, sizeof(deformation));
    expect(!gfx_presentation_packet_lookup_deformation(
               &owner, 2, 3u, 11u, 2u, 10u, &deformation),
           "one captured tick cannot manufacture deformation history");

    gfx_presentation_packet_capture_begin(12u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 2, 3u, deform_current, sizeof(deform_current),
               2u, 10u),
           "second deformation batch captures");
    gfx_presentation_packet_freeze();
    memset(&deformation, 0, sizeof(deformation));
    expect(gfx_presentation_packet_lookup_deformation(
               &owner, 2, 3u, 12u, 2u, 10u, &deformation) &&
               deformation.byte_size == sizeof(deform_current) &&
               memcmp(deformation.previous_bytes, deform_previous,
                      sizeof(deform_previous)) == 0 &&
               memcmp(deformation.current_bytes, deform_current,
                      sizeof(deform_current)) == 0,
           "adjacent compatible deformation pair resolves exact retained bytes");
    /* The host counter may advance before this older task is walked. Lookup
     * uses the immutable task token, not that mutable counter. */
    simulated_live_tick = 99u;
    memset(&deformation, 0, sizeof(deformation));
    expect(gfx_presentation_packet_lookup_deformation_hold(
               &owner, 2, 3u, 12u, 2u, 10u, &deformation) &&
               deformation.previous_bytes == deformation.current_bytes &&
               memcmp(deformation.current_bytes, deform_current,
                      sizeof(deform_current)) == 0,
           "task-authored token resolves after the live counter advances");
    expect(!gfx_presentation_packet_lookup_deformation_hold(
               &owner, 2, 3u, simulated_live_tick, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation_hold(
                   &owner, 2, 3u, 11u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation_hold(
                   &owner, 2, 3u, 13u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation_hold(
                   &owner, 2, 4u, 12u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation_hold(
                   &owner, 2, 3u, 12u, 1u, 10u, &deformation),
           "retained hold refuses wrong task tokens and nonmatching topology");
    expect(!gfx_presentation_packet_lookup_deformation_hold(
               &owner, 2, 3u, 12u, UINT32_MAX, UINT32_MAX, &deformation),
           "retained hold rejects hostile count-stride products");
    gfx_presentation_packet_note_deformation_override();
    gfx_presentation_packet_note_particle_deformation(false);
    gfx_presentation_packet_note_particle_deformation(true);
    gfx_presentation_packet_note_deformation_color(false, false);
    gfx_presentation_packet_note_deformation_color(false, true);
    gfx_presentation_packet_note_deformation_color(true, true);
    gfx_presentation_packet_note_primitive_alpha(false, false);
    gfx_presentation_packet_note_primitive_alpha(false, true);
    gfx_presentation_packet_note_primitive_alpha(true, true);
    gfx_presentation_packet_note_projected_shadow_primitive_alpha(false);
    gfx_presentation_packet_note_projected_shadow_primitive_alpha(true);
    gfx_presentation_packet_note_phase_hold(false);
    gfx_presentation_packet_note_endpoint_semantic(
        deform_current, deform_current, sizeof(deform_current));
    expect(!gfx_presentation_packet_lookup_deformation(
               &owner, 2, 4u, 12u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation(
                   &owner, 2, 3u, 12u, 1u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation(
                   &owner, 2, 3u, 13u, 2u, 10u, &deformation),
           "ordinal, topology and tick mismatches fail closed");

    gfx_presentation_packet_capture_begin(14u); /* intentionally skip 13 */
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 2, 3u, deform_skipped, sizeof(deform_skipped),
               2u, 10u),
           "post-gap deformation batch still captures");
    gfx_presentation_packet_freeze();
    expect(!gfx_presentation_packet_lookup_deformation(
               &owner, 2, 3u, 14u, 2u, 10u, &deformation),
           "a skipped tick is never interpolated across");

    gfx_presentation_packet_capture_begin(15u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 2, 3u, deform_current, sizeof(deform_current),
               2u, 10u) &&
               !gfx_presentation_packet_capture_deformation(
                   &owner, 2, 3u, deform_skipped, sizeof(deform_skipped),
                   2u, 10u),
           "duplicate stable deformation key is detected, not overwritten");
    gfx_presentation_packet_freeze();
    expect(!gfx_presentation_packet_lookup_deformation(
               &owner, 2, 3u, 15u, 2u, 10u, &deformation),
           "ambiguous deformation key is poisoned for replay");

    owner = make_owner(particle_vertex, 21u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_PARTICLE_VERTICES;
    gfx_presentation_packet_capture_begin(16u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 0, 0u, deform_current, sizeof(deform_current),
               2u, 10u) &&
               gfx_presentation_packet_capture_deformation(
                   &owner, 0, 0u, deform_current, sizeof(deform_current),
                   2u, 10u),
           "identical multi-viewport particle submissions are idempotent");
    expect(!gfx_presentation_packet_capture_deformation(
               &owner, 0, 0u, deform_skipped, sizeof(deform_skipped),
               2u, 10u),
           "conflicting repeated particle bytes still poison the key");
    gfx_presentation_packet_freeze();

    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.deformation_registrations == 5u &&
               stats.deformation_hits == 1u &&
               stats.deformation_holds == 1u &&
               stats.deformation_phase_holds == 1u &&
               stats.deformation_overrides == 1u &&
               stats.deformation_misses == 6u &&
               stats.deformation_collisions == 2u &&
               stats.particle_deformation_hits == 2u &&
               stats.particle_deformation_overrides == 1u &&
               stats.deformation_color_hits == 3u &&
               stats.deformation_color_overrides == 2u &&
               stats.particle_color_hits == 1u &&
               stats.particle_color_overrides == 1u &&
               stats.primitive_alpha_hits == 3u &&
               stats.primitive_alpha_overrides == 2u &&
               stats.projected_shadow_primitive_alpha_hits == 2u &&
               stats.projected_shadow_primitive_alpha_overrides == 1u &&
               stats.particle_primitive_alpha_hits == 1u &&
               stats.particle_primitive_alpha_overrides == 1u &&
               stats.endpoint_vertex_checks == 1u &&
               stats.endpoint_vertex_mismatches == 0u &&
               stats.endpoint_expected_hash != 0u &&
               stats.endpoint_expected_hash == stats.endpoint_actual_hash &&
               stats.deformation_peak == 1u,
           "deformation census reports captures, exact hits and refusals");

    /* A shield and magnet can be drawn around the same racer in one tick.
     * Their stable recipe therefore has two lifetimes: the primary racer and
     * the secondary effect object. The secondary key must keep those histories
     * independent even at the same viewport and ordinal. */
    memset(effect_previous_a, 0x71, sizeof(effect_previous_a));
    memset(effect_previous_b, 0x72, sizeof(effect_previous_b));
    memset(effect_current_a, 0x81, sizeof(effect_current_a));
    memset(effect_current_b, 0x82, sizeof(effect_current_b));
    effect_a = make_owner(next_matrix, 30u);
    effect_a.matrix_class = GFX_PRESENTATION_MATRIX_EFFECT;
    effect_a.secondary_address = matrix;
    effect_a.secondary_generation = 31u;
    effect_b = effect_a;
    effect_b.secondary_address = vertex;
    effect_b.secondary_generation = 32u;

    gfx_presentation_packet_capture_begin(17u);
    expect(gfx_presentation_packet_capture_deformation(
               &effect_a, 0, 0u, effect_previous_a,
               sizeof(effect_previous_a), 1u, sizeof(effect_previous_a)) &&
               gfx_presentation_packet_capture_deformation(
                   &effect_b, 0, 0u, effect_previous_b,
                   sizeof(effect_previous_b), 1u, sizeof(effect_previous_b)),
           "effect history accepts two secondary identities on one racer");
    gfx_presentation_packet_freeze();

    gfx_presentation_packet_capture_begin(18u);
    expect(gfx_presentation_packet_capture_deformation(
               &effect_a, 0, 0u, effect_current_a,
               sizeof(effect_current_a), 1u, sizeof(effect_current_a)) &&
               gfx_presentation_packet_capture_deformation(
                   &effect_b, 0, 0u, effect_current_b,
                   sizeof(effect_current_b), 1u, sizeof(effect_current_b)),
           "effect history retains both secondary identities on the next tick");
    gfx_presentation_packet_freeze();
    expect(gfx_presentation_packet_lookup_deformation(
               &effect_a, 0, 0u, 18u, 1u, sizeof(effect_current_a),
               &deformation) &&
               memcmp(deformation.previous_bytes, effect_previous_a,
                      sizeof(effect_previous_a)) == 0 &&
               memcmp(deformation.current_bytes, effect_current_a,
                      sizeof(effect_current_a)) == 0,
           "effect history resolves the first secondary identity exactly");
    gfx_presentation_packet_note_effect_override();
    gfx_presentation_packet_note_phase_hold(true);
    expect(gfx_presentation_packet_lookup_deformation(
               &effect_b, 0, 0u, 18u, 1u, sizeof(effect_current_b),
               &deformation) &&
               memcmp(deformation.previous_bytes, effect_previous_b,
                      sizeof(effect_previous_b)) == 0 &&
               memcmp(deformation.current_bytes, effect_current_b,
                      sizeof(effect_current_b)) == 0,
           "effect history resolves the second secondary identity exactly");
    gfx_presentation_packet_note_effect_override();

    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.deformation_registrations == 5u &&
               stats.deformation_hits == 1u &&
               stats.deformation_holds == 1u &&
               stats.deformation_phase_holds == 1u &&
               stats.deformation_overrides == 1u &&
               stats.deformation_misses == 6u &&
               stats.deformation_collisions == 2u &&
               stats.effect_registrations == 4u &&
               stats.effect_hits == 2u && stats.effect_overrides == 2u &&
               stats.effect_phase_holds == 1u &&
               stats.effect_misses == 0u && stats.effect_collisions == 0u &&
               stats.endpoint_vertex_checks == 1u &&
               stats.endpoint_vertex_mismatches == 0u &&
               stats.endpoint_expected_hash == stats.endpoint_actual_hash &&
               stats.deformation_peak == 2u,
           "effect census is separate and two-identity histories do not collide");

    /* A future-only publication advances exact deformation history without
     * replacing the matrix/vertex ownership packet for the task being
     * replayed. The lookup key remains the real-walk address, while immutable
     * bytes from a private retained arena drive stale-tenancy validation. */
    memset(retained_identity, 0x91, sizeof(retained_identity));
    memcpy(retained_observation, retained_identity,
           sizeof(retained_observation));
    memset(future_previous, 0xA1, sizeof(future_previous));
    memset(future_current, 0xB2, sizeof(future_current));
    owner = make_owner(retained_identity, 40u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_ROOT;
    expect(gfx_presentation_packet_register_matrix(
               retained_identity, sizeof(retained_identity), 1, &owner),
           "forward-pair control matrix registers");
    gfx_presentation_packet_capture_begin(19u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 1, 7u, future_previous, sizeof(future_previous),
               2u, 10u),
           "task T deformation captures before the ordinary freeze");
    gfx_presentation_packet_freeze();
    retained_identity[0] ^= 1u;
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_matrix_observed(
               retained_identity, retained_observation, &binding) &&
               !binding.stale,
           "private observed bytes validate under the original ownership key");

    gfx_presentation_packet_capture_begin(20u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 1, 7u, future_current, sizeof(future_current),
               2u, 10u) &&
               gfx_presentation_packet_publish_deformation(),
           "already-authored task T+1 publishes deformation only");
    gfx_presentation_packet_note_future_capture(true);
    memset(&deformation, 0, sizeof(deformation));
    expect(gfx_presentation_packet_lookup_deformation(
               &owner, 1, 7u, 20u, 2u, 10u, &deformation) &&
               memcmp(deformation.previous_bytes, future_previous,
                      sizeof(future_previous)) == 0 &&
               memcmp(deformation.current_bytes, future_current,
                      sizeof(future_current)) == 0,
           "future publication exposes the exact adjacent T/T+1 pair");
    gfx_presentation_packet_capture_begin(21u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 1, 7u, deform_skipped, sizeof(deform_skipped),
               2u, 10u),
           "failed-scan control stages a partial future task");
    gfx_presentation_packet_capture_abort();
    expect(!gfx_presentation_packet_publish_deformation() &&
               gfx_presentation_packet_lookup_deformation(
                   &owner, 1, 7u, 20u, 2u, 10u, &deformation) &&
               memcmp(deformation.previous_bytes, future_previous,
                      sizeof(future_previous)) == 0 &&
               memcmp(deformation.current_bytes, future_current,
                      sizeof(future_current)) == 0,
           "aborted future scan leaves the last complete pair intact");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_matrix_observed(
               retained_identity, retained_observation, &binding) &&
               !binding.stale,
           "future publication leaves the replay task's owner packet intact");
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.future_captures == 1u && stats.future_failures == 0u,
           "future publication has explicit success telemetry");

    /* A racer's projected decal and its first model batch can both be ordinal
     * zero in viewport zero. Matrix class and topology signature namespace the
     * histories so neither poisons the other. */
    owner = make_owner(next_matrix, 22u);
    owner.matrix_class = GFX_PRESENTATION_MATRIX_ROOT;
    shadow_owner = owner;
    shadow_owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    shadow_owner.geometry_signature = UINT64_C(0x123456789abcdef0);
    gfx_presentation_packet_capture_begin(22u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 0, 0u, deform_previous, sizeof(deform_previous),
               2u, 10u) &&
               gfx_presentation_packet_capture_deformation(
                   &shadow_owner, 0, 0u, effect_previous_a,
                   sizeof(effect_previous_a), 2u, 10u),
           "model and projected-shadow ordinal zero capture independently");
    gfx_presentation_packet_freeze();
    gfx_presentation_packet_capture_begin(23u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 0, 0u, deform_current, sizeof(deform_current),
               2u, 10u) &&
               gfx_presentation_packet_capture_deformation(
                   &shadow_owner, 0, 0u, effect_current_a,
                   sizeof(effect_current_a), 2u, 10u),
           "both same-ordinal histories advance on the adjacent tick");
    gfx_presentation_packet_freeze();
    expect(gfx_presentation_packet_lookup_deformation(
               &owner, 0, 0u, 23u, 2u, 10u, &deformation) &&
               memcmp(deformation.previous_bytes, deform_previous,
                      sizeof(deform_previous)) == 0 &&
               memcmp(deformation.current_bytes, deform_current,
                      sizeof(deform_current)) == 0,
           "model ordinal zero resolves its own forward pair");
    expect(gfx_presentation_packet_lookup_deformation(
               &shadow_owner, 0, 0u, 23u, 2u, 10u, &deformation) &&
               memcmp(deformation.previous_bytes, effect_previous_a,
                      sizeof(effect_previous_a)) == 0 &&
               memcmp(deformation.current_bytes, effect_current_a,
                      sizeof(effect_current_a)) == 0,
           "projected-shadow ordinal zero resolves its own forward pair");
    shadow_owner.geometry_signature++;
    expect(!gfx_presentation_packet_lookup_deformation(
               &shadow_owner, 0, 0u, 23u, 2u, 10u, &deformation),
           "projected-shadow topology changes refuse interpolation");

    /* A receiver topology change between adjacent authored ticks must not
     * borrow the prior decal batch, even when its object and ordinal match. */
    shadow_owner = make_owner(next_matrix, 23u);
    shadow_owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    shadow_owner.geometry_signature = UINT64_C(0x1111111111111111);
    future_shadow_owner = shadow_owner;
    future_shadow_owner.geometry_signature = UINT64_C(0x2222222222222222);
    gfx_presentation_packet_capture_begin(24u);
    expect(gfx_presentation_packet_capture_deformation(
               &shadow_owner, 0, 1u, effect_previous_a,
               sizeof(effect_previous_a), 2u, 10u),
           "projected-shadow topology A captures at task T");
    gfx_presentation_packet_freeze();
    gfx_presentation_packet_capture_begin(25u);
    expect(gfx_presentation_packet_capture_deformation(
               &future_shadow_owner, 0, 1u, effect_current_a,
               sizeof(effect_current_a), 2u, 10u),
           "projected-shadow topology B captures at task T+1");
    gfx_presentation_packet_freeze();
    expect(!gfx_presentation_packet_lookup_deformation(
               &shadow_owner, 0, 1u, 25u, 2u, 10u, &deformation) &&
               !gfx_presentation_packet_lookup_deformation(
                   &future_shadow_owner, 0, 1u, 25u, 2u, 10u,
                   &deformation),
           "adjacent projected-shadow topology signatures never form a pair");

    /* Future direct-shadow VTX registrations use a fresh heap address, but
     * publishing their deformation history must not replace T's frozen owner
     * binding while that task is still being presented. */
    memset(projected_shadow_vertex, 0xC3, sizeof(projected_shadow_vertex));
    memset(projected_shadow_future_vertex, 0xD4,
           sizeof(projected_shadow_future_vertex));
    shadow_owner = make_owner(projected_shadow_vertex, 24u);
    shadow_owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    shadow_owner.geometry_signature = UINT64_C(0x3333333333333333);
    expect(gfx_presentation_packet_register_projected_shadow_vertex(
               projected_shadow_vertex, 0, 6u, &shadow_owner),
           "task T projected-shadow VTX binding registers");
    gfx_presentation_packet_capture_begin(26u);
    expect(gfx_presentation_packet_capture_deformation(
               &shadow_owner, 0, 6u, effect_previous_b,
               sizeof(effect_previous_b), 2u, 10u),
           "task T projected-shadow deformation captures");
    gfx_presentation_packet_freeze();
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(
               projected_shadow_vertex, &binding) &&
               binding.owner.generation == shadow_owner.generation &&
               binding.owner.geometry_signature ==
                   shadow_owner.geometry_signature &&
               binding.ordinal == 6u,
           "task T projected-shadow binding freezes by value");

    expect(gfx_presentation_packet_register_projected_shadow_vertex(
               projected_shadow_future_vertex, 0, 6u, &shadow_owner) &&
               gfx_presentation_packet_lookup_live_vertex(
                   projected_shadow_future_vertex, &binding),
           "task T+1 projected-shadow VTX uses its new live address");
    gfx_presentation_packet_capture_begin(27u);
    expect(gfx_presentation_packet_capture_deformation(
               &shadow_owner, 0, 6u, effect_current_b,
               sizeof(effect_current_b), 2u, 10u) &&
               gfx_presentation_packet_publish_deformation(),
           "task T+1 projected-shadow deformation publishes without a freeze");
    memset(&binding, 0, sizeof(binding));
    expect(gfx_presentation_packet_lookup_vertex(
               projected_shadow_vertex, &binding) &&
               binding.owner.generation == shadow_owner.generation &&
               binding.owner.geometry_signature ==
                   shadow_owner.geometry_signature &&
               binding.ordinal == 6u &&
               !gfx_presentation_packet_lookup_vertex(
                   projected_shadow_future_vertex, &binding),
           "future projected-shadow publication preserves task T's frozen binding");

    owner = make_owner(projected_shadow_vertex, 22u);
    owner.matrix_class =
        GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
    gfx_presentation_packet_capture_begin(21u);
    expect(gfx_presentation_packet_capture_deformation(
               &owner, 0, 5u, deform_current, sizeof(deform_current),
               2u, 10u) &&
               gfx_presentation_packet_capture_deformation(
                   &owner, 0, 5u, deform_current, sizeof(deform_current),
                   2u, 10u),
           "identical multi-viewport projected-shadow submissions are idempotent");
    gfx_presentation_packet_note_projected_shadow_deformation(true);
    gfx_presentation_packet_capture_abort();
    memset(&stats, 0, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.projected_shadow_deformation_hits == 1u &&
               stats.projected_shadow_deformation_overrides == 1u,
           "projected shadow interpolation has dedicated telemetry");

    gfx_presentation_packet_invalidate();
    expect(!gfx_presentation_packet_frozen() &&
               !gfx_presentation_packet_lookup_matrix(next_matrix, &binding),
           "lifecycle invalidation makes retained dependencies unavailable");
    gfx_presentation_packet_shutdown();
    memset(&stats, 0xFF, sizeof(stats));
    gfx_presentation_packet_get_stats(&stats);
    expect(stats.matrix_registrations == 0u && stats.freezes == 0u,
           "shutdown releases storage and resets ownership telemetry");

    if (failures != 0) {
        fprintf(stderr, "presentation_packet: %d failure(s)\n", failures);
        return 1;
    }
    puts("presentation_packet: all checks passed");
    return 0;
}
