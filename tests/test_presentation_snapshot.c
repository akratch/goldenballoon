/*
 * Presentation snapshot unit (spec §7, §12.2.5, §12.4; Phase 3 Wave A).
 *
 * The module is compiled standalone here — it deliberately contains no game
 * header — so this test can stage lifecycle races that are impossible to
 * provoke reliably inside a running race: freeing a slot and respawning into
 * the exact same address inside one tick, an overflowing capture, a level
 * transition between two ticks.
 *
 * Coverage:
 *   1. identity + generation: a freed slot reused by a new spawn is never an
 *      interpolation pair, even at the identical address;
 *   2. shortest-arc angle wrap, including the 0x7FFF/0x8000 boundary and the
 *      ambiguous half turn;
 *   3. exact rational alpha endpoints: alpha 0 returns previous bits, alpha 1
 *      returns current bits, bit for bit;
 *   4. the discrete previous-until-complete selection rule;
 *   5. discontinuity suppression (spawn and teleport draw current, unblended);
 *   6. published-pair immutability while the next capture is in flight;
 *   7. atomic overflow: a capture that overruns publishes nothing and the
 *      previous pair is retained.
 */
#include <stdio.h>
#include <string.h>

#include "presentation_snapshot.h"

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int bits_equal(float a, float b) {
    return memcmp(&a, &b, sizeof(float)) == 0;
}

/* Synthetic object addresses. Only their identity matters to the module. */
static char s_slot_a[4];
static char s_slot_b[4];

static PresentationObjectEntry make_object(const void *address, float x,
                                           float y, float z) {
    PresentationObjectEntry sample;

    memset(&sample, 0, sizeof(sample));
    sample.address = address;
    sample.position[0] = x;
    sample.position[1] = y;
    sample.position[2] = z;
    sample.scale = 1.0f;
    sample.model_index = 0;
    sample.animation_id = 0;
    sample.animation_frame = 0;
    sample.opacity = 255;
    return sample;
}

static PresentationCameraEntry make_camera(int camera_id, float x, float y,
                                           float z) {
    PresentationCameraEntry sample;

    memset(&sample, 0, sizeof(sample));
    sample.camera_id = camera_id;
    sample.position[0] = x;
    sample.position[1] = y;
    sample.position[2] = z;
    sample.fov = 60.0f;
    sample.vertical_fov = 60.0f;
    sample.aspect = 4.0f / 3.0f;
    sample.near_plane = 10.0f;
    sample.far_plane = 15000.0f;
    sample.viewport[2] = 320.0f;
    sample.viewport[3] = 240.0f;
    return sample;
}

static void begin(void) {
    presentation_snapshot_shutdown();
    presentation_snapshot_set_enabled(true);
}

/* ---- 2. shortest-arc angle interpolation --------------------------------- */

static void test_angle_shortest_arc(void) {
    /* Plain interior case: no wrap involved. */
    expect(presentation_lerp_angle(0, 1000, 1, 2) == 500,
           "angle: interior midpoint");

    /*
     * The 0x7FFF/0x8000 boundary. 32767 -> -32768 is ONE step forward, not
     * 65535 steps backward: the wrapped difference is +1, so the midpoint
     * must stay at 32767 (truncation) and the endpoint must land exactly on
     * -32768.
     */
    expect(presentation_lerp_angle(32767, -32768, 1, 2) == 32767,
           "angle: 0x7FFF->0x8000 midpoint takes the +1 arc");
    expect(presentation_lerp_angle(32767, -32768, 1, 1) == -32768,
           "angle: 0x7FFF->0x8000 endpoint is exact");

    /* And the same boundary crossed the other way: -32768 -> 32767 is one
     * step BACKWARD, so the result wraps below 0x8000. */
    expect(presentation_lerp_angle(-32768, 32767, 1, 2) == -32768,
           "angle: 0x8000->0x7FFF midpoint takes the -1 arc");
    expect(presentation_lerp_angle(-32768, 32767, 1, 1) == 32767,
           "angle: 0x8000->0x7FFF endpoint is exact");

    /*
     * Crossing zero: 0x8000 apart is the ambiguous half turn and resolves
     * negative; 0xC000 (-16384) is unambiguously the short way round
     * backwards, NOT +49152 forwards.
     */
    expect(presentation_lerp_angle(0, (int16_t)0xC000, 1, 2) == -8192,
           "angle: 0 -> 0xC000 takes the short negative arc");
    expect(presentation_lerp_angle((int16_t)0xC000, 0, 1, 2) == -8192,
           "angle: 0xC000 -> 0 takes the short positive arc");
    /* The ambiguous half turn (delta == INT16_MIN, both arcs equal) is past
     * the rotation snap threshold (see test_rotation_snap): it now snaps to
     * current rather than picking an arbitrary blend arc. */
    expect(presentation_lerp_angle(0, (int16_t)0x8000, 1, 2) ==
               (int16_t)0x8000,
           "angle: exact half turn snaps instead of picking an arc");

    /* Wrap through 0 the short way: 0xFF00 -> 0x0100 is +512, not -65024. */
    expect(presentation_lerp_angle((int16_t)0xFF00, 0x0100, 1, 2) == 0,
           "angle: wrap through zero interpolates the short way");

    /* Endpoints of every case return the endpoint bits untouched. */
    expect(presentation_lerp_angle(1234, -4321, 0, 2) == 1234,
           "angle: alpha 0 is previous");
    expect(presentation_lerp_angle(1234, -4321, 2, 2) == -4321,
           "angle: alpha 1 is current");
    expect(presentation_lerp_angle(1234, -4321, 9, 2) == -4321,
           "angle: alpha > 1 clamps to current");
    expect(presentation_lerp_angle(1234, -4321, 1, 0) == 1234,
           "angle: zero denominator is previous");
}

static void test_rotation_snap(void) {
    /* > 0x4000 shortest-arc delta: any intermediate alpha snaps to current.
     * Calibrated to Ghostship's proven threshold: a quarter turn per tick is
     * beyond any legitimate smooth motion; blending across it draws the
     * model swinging through poses the simulation never held. */
    expect(presentation_lerp_angle(0, 0x4001, 1, 2) == 0x4001,
           "delta just past a quarter turn snaps to current");
    expect(presentation_lerp_angle(0, 0x4000, 1, 2) == 0x2000,
           "exactly a quarter turn still blends");
    /* The ambiguous half turn (delta == INT16_MIN) now snaps too. */
    expect(presentation_lerp_angle(0, (int16_t)0x8000, 1, 2) ==
               (int16_t)0x8000,
           "ambiguous half turn snaps instead of picking an arc");
    /* Wrap case: 0x7000 -> 0x9000 is a +0x2000 shortest arc; still blends. */
    expect(presentation_lerp_angle((int16_t)0x7000, (int16_t)0x9000, 1, 2) ==
               (int16_t)0x8000,
           "wrap-crossing small delta still blends");
    /* Endpoint exactness is untouched by the snap. */
    expect(presentation_lerp_angle(0, 0x4001, 0, 2) == 0,
           "alpha 0 returns previous even past the snap threshold");
    expect(presentation_lerp_angle(0, 0x4001, 2, 2) == 0x4001,
           "alpha 1 returns current");
}

/* ---- 3. exact rational alpha endpoints ----------------------------------- */

static void test_exact_endpoints(void) {
    /* A value that cannot survive a round trip through a sloppy blend. */
    const float previous = 1.0000001f;
    const float current = 16777217.0f;
    const float a3[3] = { 0.1f, -12345.678f, 3.0e7f };
    const float b3[3] = { 99999.9f, 0.0f, -1.0e-7f };
    float out[3];

    expect(bits_equal(presentation_lerp1(previous, current, 0, 3), previous),
           "lerp1: alpha 0 returns previous bits exactly");
    expect(bits_equal(presentation_lerp1(previous, current, 3, 3), current),
           "lerp1: alpha 1 returns current bits exactly");
    expect(bits_equal(presentation_lerp1(previous, current, 4, 3), current),
           "lerp1: alpha > 1 clamps to current bits exactly");
    expect(bits_equal(presentation_lerp1(previous, current, 1, 0), previous),
           "lerp1: zero denominator returns previous bits exactly");

    presentation_lerp3(a3, b3, 0, 7, out);
    expect(bits_equal(out[0], a3[0]) && bits_equal(out[1], a3[1]) &&
               bits_equal(out[2], a3[2]),
           "lerp3: alpha 0 is previous, bit for bit, on all three axes");
    presentation_lerp3(a3, b3, 7, 7, out);
    expect(bits_equal(out[0], b3[0]) && bits_equal(out[1], b3[1]) &&
               bits_equal(out[2], b3[2]),
           "lerp3: alpha 1 is current, bit for bit, on all three axes");

    /* The rational is honoured exactly, not via a pre-rounded float alpha:
     * 1/3 of the way from 0 to 3 is 1. */
    expect(presentation_lerp1(0.0f, 3.0f, 1, 3) == 1.0f,
           "lerp1: exact rational thirds");
    expect(presentation_alpha(1, 3) > 0.333333 &&
               presentation_alpha(1, 3) < 0.333334,
           "alpha: rational converted once, in double");
    expect(presentation_alpha(0, 0) == 0.0,
           "alpha: zero denominator is 0");
    expect(presentation_alpha(5, 2) == 1.0, "alpha: clamps at 1");

    /* Interior monotonicity across a large-magnitude interval. */
    expect(presentation_lerp1(-1000.0f, 1000.0f, 1, 4) == -500.0f,
           "lerp1: interior quarter");

    expect(presentation_lerp_u8(10u, 20u, 0u, 2u) == 10u &&
               presentation_lerp_u8(10u, 20u, 1u, 2u) == 15u &&
               presentation_lerp_u8(10u, 20u, 2u, 2u) == 20u,
           "lerp u8: endpoints and rounded midpoint are exact");
    expect(presentation_particle_opacity_u8(0) == 0u &&
               presentation_particle_opacity_u8((int16_t)0x1234) == 0x12u &&
               presentation_particle_opacity_u8((int16_t)0x7f00) == 0x7fu &&
               presentation_particle_opacity_u8((int16_t)0xff00) == 0xffu,
           "particle opacity: signed storage preserves unsigned 8.8 high byte");
    expect(presentation_scale_opacity_u8(200u, 200u, 100u) == 100u,
           "opacity scale: alpha-zero source denominator preserves midpoint fade");
    expect(presentation_scale_opacity_u8(100u, 200u, 100u) == 50u &&
               presentation_scale_opacity_u8(255u, 100u, 200u) == 255u &&
               presentation_scale_opacity_u8(77u, 0u, 200u) == 77u &&
               presentation_scale_opacity_u8(77u, 200u, 200u) == 77u,
           "opacity scale: preserves modifiers, clamps, and fails closed");
}

/* ---- 4. the discrete selection rule -------------------------------------- */

static void test_discrete_rule(void) {
    expect(!presentation_discrete_use_current(0, 2),
           "discrete: alpha 0 shows previous");
    expect(!presentation_discrete_use_current(1, 2),
           "discrete: alpha 0.5 STILL shows previous (not nearest-neighbour)");
    expect(!presentation_discrete_use_current(999, 1000),
           "discrete: alpha 0.999 still shows previous");
    expect(presentation_discrete_use_current(1000, 1000),
           "discrete: alpha 1 switches to current");
    expect(presentation_discrete_use_current(3, 2),
           "discrete: alpha > 1 shows current");
    expect(!presentation_discrete_use_current(1, 0),
           "discrete: zero denominator shows previous");
}

/* ---- 1. identity + generation across slot reuse -------------------------- */

static void test_identity_generation_reuse(void) {
    PresentationObjectPose pose;
    PresentationObjectEntry sample;
    uint64_t first_generation = 0;
    uint64_t second_generation = 0;

    begin();

    /* Tick 1: object A at the origin. */
    presentation_snapshot_note_spawn(s_slot_a);
    expect(presentation_snapshot_identity_generation(
               s_slot_a, &first_generation) && first_generation != 0u,
           "identity: lifecycle generation is readable without live object access");
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 0.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();

    /* Tick 2: A moved 10 units. A normal, interpolable pair. */
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 10.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();

    expect(presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose),
           "identity: live object resolves");
    expect(pose.interpolated == 1, "identity: matched pair interpolates");
    expect(pose.position[0] == 5.0f, "identity: matched pair blends position");

    /*
     * THE CASE THIS MODULE EXISTS FOR. Between tick 2 and tick 3, A is freed
     * and a new object B is allocated at the EXACT same address (DKR's pool
     * recycles constantly). Keyed on the address alone, tick 2's pose and
     * tick 3's pose look like one object that jumped; keyed on
     * (address, generation) they are two different lives and must not pair.
     */
    presentation_snapshot_note_free(s_slot_a);
    expect(!presentation_snapshot_identity_generation(
               s_slot_a, &second_generation),
           "identity: freed lifetime is no longer registrable by the renderer");
    presentation_snapshot_note_spawn(s_slot_a); /* same address, new object */
    expect(presentation_snapshot_identity_generation(
               s_slot_a, &second_generation) &&
               second_generation != first_generation,
           "identity: recycled address receives a distinct renderer generation");

    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 20.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();

    expect(presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose),
           "identity: reused slot resolves as an object");
    expect(pose.interpolated == 0,
           "identity: reused slot does NOT interpolate from the dead object");
    expect(pose.position[0] == 20.0f,
           "identity: reused slot draws its own spawn pose verbatim");
    expect(!presentation_snapshot_resolve_object_generation(
               s_slot_a, first_generation, 1, 2, &pose),
           "identity: stale matrix generation cannot resolve the recycled address");
    expect(presentation_snapshot_resolve_object_generation(
               s_slot_a, second_generation, 1, 2, &pose),
           "identity: current matrix generation resolves its exact lifetime");

    /*
     * And the generation really is what did it: the two published frames
     * hold the same address with different generations.
     */
    {
        const PresentationSnapshot *current = presentation_snapshot_current();
        const PresentationSnapshot *previous =
            presentation_snapshot_previous();
        expect(current != NULL && previous != NULL &&
                   current->object_count == 1 && previous->object_count == 1,
               "identity: both frames hold the one entry");
        if (current != NULL && previous != NULL && current->object_count == 1 &&
            previous->object_count == 1) {
            expect(current->objects[0].address == previous->objects[0].address,
                   "identity: the address really is identical");
            expect(current->objects[0].generation !=
                       previous->objects[0].generation,
                   "identity: the generation is what separates them");
        }
    }

    /* Once the new object has a tick of history of its own it interpolates
     * normally — the suppression lasts exactly one tick. */
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 30.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose),
           "identity: respawned object resolves on its second tick");
    expect(pose.interpolated == 1 && pose.position[0] == 25.0f,
           "identity: respawned object interpolates from its own history");

    /* A destroyed object is simply not resolvable — no gameplay presence and
     * no visual tail owned by this module (spec §7). */
    presentation_snapshot_note_free(s_slot_a);
    presentation_snapshot_capture_begin();
    presentation_snapshot_capture_commit();
    expect(!presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose),
           "identity: destroyed object does not resolve");
}

static void test_identity_ensure_generation(void) {
    uint64_t first_generation = 0;
    uint64_t repeated_generation = 0;
    uint64_t replacement_generation = 0;

    begin();

    expect(!presentation_snapshot_identity_generation(
               s_slot_a, &first_generation),
           "identity ensure: an unregistered presentation object has no lifetime");
    expect(presentation_snapshot_identity_ensure_generation(
               s_slot_a, &first_generation) && first_generation != 0u,
           "identity ensure: a presentation-only object gets a lifetime lazily");
    expect(presentation_snapshot_identity_ensure_generation(
               s_slot_a, &repeated_generation) &&
               repeated_generation == first_generation,
           "identity ensure: repeated render observation preserves the lifetime");

    presentation_snapshot_note_free(s_slot_a);
    expect(!presentation_snapshot_identity_generation(
               s_slot_a, &replacement_generation),
           "identity ensure: freeing retires a presentation-only lifetime");
    expect(presentation_snapshot_identity_ensure_generation(
               s_slot_a, &replacement_generation) &&
               replacement_generation != first_generation,
           "identity ensure: address reuse receives a fresh lifetime");
}

/* ---- 5. discontinuity suppression ---------------------------------------- */

static void test_discontinuity(void) {
    PresentationObjectPose pose;
    PresentationObjectEntry sample;
    PresentationSnapshotStats stats;

    begin();

    /* Spawn tick: no previous pose exists, so nothing to blend from. */
    presentation_snapshot_note_spawn(s_slot_a);
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 100.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose),
           "discontinuity: spawned object resolves");
    expect(pose.interpolated == 0 && pose.position[0] == 100.0f,
           "discontinuity: spawn draws the authoritative spawn pose");

    /* Ordinary motion: interpolates. */
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 140.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose);
    expect(pose.interpolated == 1 && pose.position[0] == 120.0f,
           "discontinuity: ordinary motion still interpolates");

    /* Just under the threshold is still motion. */
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 140.0f + 1999.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose);
    expect(pose.interpolated == 1,
           "discontinuity: 1999 units in one tick is (barely) motion");

    /* Over the threshold on a single axis is a teleport: current pose, no
     * blend, and it is counted. */
    presentation_snapshot_get_stats(&stats);
    {
        const uint64_t before = stats.discontinuities;
        presentation_snapshot_capture_begin();
        sample = make_object(s_slot_a, 40000.0f, 0.0f, 0.0f);
        presentation_snapshot_capture_object(&sample);
        presentation_snapshot_capture_commit();
        presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose);
        expect(pose.interpolated == 0,
               "discontinuity: teleport is not interpolated");
        expect(pose.position[0] == 40000.0f,
               "discontinuity: teleport draws the current pose");
        presentation_snapshot_get_stats(&stats);
        expect(stats.discontinuities == before + 1,
               "discontinuity: the teleport is counted once");
    }

    /* The threshold is on the 3D distance, not any single axis: three
     * components of 1500 each is 2598 units and must trip it. */
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 40000.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 41500.0f, 1500.0f, 1500.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose);
    expect(pose.interpolated == 0,
           "discontinuity: threshold applies to the 3D distance");

    /*
     * Level transition (spec §5): the stage reset drops history so the first
     * tick of the new scene cannot blend from the old one, even for an
     * address that happens to survive.
     */
    begin();
    presentation_snapshot_note_spawn(s_slot_a);
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 0.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 4.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose);
    expect(pose.interpolated == 1, "stage: interpolating before the reset");

    presentation_snapshot_stage_reset();
    expect(presentation_snapshot_current() == NULL &&
               presentation_snapshot_previous() == NULL,
           "stage: reset drops the published pair");
    presentation_snapshot_note_spawn(s_slot_a);
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 8.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 12.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose);
    expect(pose.interpolated == 1 && pose.position[0] == 10.0f,
           "stage: the new scene builds its own history");
    presentation_snapshot_get_stats(&stats);
    expect(stats.resets == 1, "stage: the reset is counted");

    /*
     * A tick an object sits out is a gap, not motion: the pair is no longer
     * adjacent and must not be blended across.
     */
    begin();
    presentation_snapshot_note_spawn(s_slot_a);
    presentation_snapshot_note_spawn(s_slot_b);
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 0.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    sample = make_object(s_slot_b, 0.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_capture_begin(); /* A absent this tick */
    sample = make_object(s_slot_b, 2.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 4.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    sample = make_object(s_slot_b, 4.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose);
    expect(pose.interpolated == 0,
           "discontinuity: an object that missed a tick has no adjacent pair");
    presentation_snapshot_resolve_object(s_slot_b, 1, 2, &pose);
    expect(pose.interpolated == 1 && pose.position[0] == 3.0f,
           "discontinuity: its neighbour is unaffected");
}

/* ---- 4b. discrete fields through the resolver ---------------------------- */

static void test_resolved_fields(void) {
    PresentationObjectPose pose;
    PresentationObjectEntry sample;
    PresentationCameraPose camera;
    PresentationCameraEntry camera_sample;

    begin();
    presentation_snapshot_note_spawn(s_slot_a);

    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 0.0f, 0.0f, 0.0f);
    sample.scale = 1.0f;
    sample.rotation_y = 0;
    sample.opacity = 0;
    sample.model_index = 0;
    sample.animation_id = 7;
    sample.animation_frame = 11;
    presentation_snapshot_capture_object(&sample);
    camera_sample = make_camera(0, 0.0f, 0.0f, 0.0f);
    camera_sample.rotation_y = 0;
    camera_sample.apply_shake = 0;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 8.0f, 16.0f, -8.0f);
    sample.scale = 3.0f;
    sample.rotation_y = 4000;
    sample.opacity = 200;
    sample.model_index = 2;
    sample.animation_id = 9;
    sample.animation_frame = 13;
    presentation_snapshot_capture_object(&sample);
    camera_sample = make_camera(0, 40.0f, 0.0f, 0.0f);
    camera_sample.rotation_y = 2000;
    camera_sample.fov = 80.0f;
    camera_sample.apply_shake = 1;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    /* Half way through the tick. */
    expect(presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose),
           "fields: resolves");
    expect(pose.position[0] == 4.0f && pose.position[1] == 8.0f &&
               pose.position[2] == -4.0f,
           "fields: position is interpolated on all three axes");
    expect(pose.scale == 2.0f, "fields: scale is a continuous scalar");
    expect(pose.rotation_y == 2000, "fields: rotation is shortest-arc blended");
    expect(pose.opacity == 100, "fields: opacity is a continuous scalar");
    expect(pose.model_index == 0 && pose.animation_id == 7 &&
               pose.animation_frame == 11,
           "fields: discrete state holds PREVIOUS at alpha 0.5");

    /* At alpha 1 every field is the current tick's, exactly. */
    expect(presentation_snapshot_resolve_object(s_slot_a, 2, 2, &pose),
           "fields: resolves at alpha 1");
    expect(bits_equal(pose.position[0], 8.0f) &&
               bits_equal(pose.position[1], 16.0f) &&
               bits_equal(pose.position[2], -8.0f) &&
               bits_equal(pose.scale, 3.0f) && pose.rotation_y == 4000 &&
               pose.opacity == 200 && pose.model_index == 2 &&
               pose.animation_id == 9 && pose.animation_frame == 13,
           "fields: alpha 1 is the current tick, bit for bit");

    /* At alpha 0 every field is the previous tick's, exactly. */
    expect(presentation_snapshot_resolve_object(s_slot_a, 0, 2, &pose),
           "fields: resolves at alpha 0");
    expect(bits_equal(pose.position[0], 0.0f) &&
               bits_equal(pose.scale, 1.0f) && pose.rotation_y == 0 &&
               pose.opacity == 0 && pose.model_index == 0 &&
               pose.animation_id == 7 && pose.animation_frame == 11,
           "fields: alpha 0 is the previous tick, bit for bit");

    /* Cameras use the same pair and the same alpha (spec §7). */
    expect(presentation_snapshot_resolve_camera(0, 1, 2, &camera),
           "camera: viewport 0 resolves");
    expect(camera.interpolated == 1, "camera: matched viewport interpolates");
    expect(camera.position[0] == 20.0f, "camera: position blends");
    expect(camera.rotation_y == 1000, "camera: rotation is shortest-arc");
    expect(camera.fov == 70.0f, "camera: FOV is a continuous projection input");
    expect(camera.apply_shake == 0,
           "camera: the shake flag is discrete, previous until complete");
    expect(presentation_snapshot_resolve_camera(0, 2, 2, &camera) &&
               camera.camera_id == 0 &&
               bits_equal(camera.position[0], 40.0f) &&
               bits_equal(camera.fov, 80.0f) && camera.apply_shake == 1,
           "camera: alpha 1 is the current tick exactly");
    expect(!presentation_snapshot_resolve_camera(3, 1, 2, &camera),
           "camera: an unpublished viewport does not resolve");

    /* Region ownership is a discrete presentation-layout boundary. Blending
     * across it can put a safe-aperture camera into a wide draw region (or vice
     * versa) for one intermediate present. */
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 44.0f, 0.0f, 0.0f);
    camera_sample.aspect = 21.0f / 9.0f;
    camera_sample.world_region = 1;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_resolve_camera(0, 1, 2, &camera) &&
               camera.interpolated == 0 &&
               bits_equal(camera.aspect, 21.0f / 9.0f),
           "camera: world-region boundary holds the current projection exactly");

    /* Once the discrete policy is stable, both camera motion and an authored
     * viewport resize must immediately resume midpoint interpolation. */
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 48.0f, 0.0f, 0.0f);
    camera_sample.aspect = 21.0f / 9.0f;
    camera_sample.world_region = 1;
    camera_sample.viewport[0] = 20.0f;
    camera_sample.viewport[2] = 280.0f;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_resolve_camera(0, 1, 2, &camera) &&
               camera.interpolated == 1 &&
               bits_equal(camera.position[0], 46.0f) &&
               bits_equal(camera.viewport[0], 10.0f) &&
               bits_equal(camera.viewport[2], 300.0f),
           "camera: stable region resumes motion and viewport midpoints");

    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 52.0f, 0.0f, 0.0f);
    camera_sample.aspect = 21.0f / 9.0f;
    camera_sample.world_region = 1;
    camera_sample.viewport[0] = 80.0f;
    camera_sample.viewport[2] = 160.0f;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_resolve_camera(0, 1, 2, &camera) &&
               camera.interpolated == 1 &&
               bits_equal(camera.position[0], 50.0f) &&
               bits_equal(camera.viewport[0], 50.0f) &&
               bits_equal(camera.viewport[2], 220.0f),
           "camera: animated viewport keeps interpolating inside one region");

    /* A viewport that switches which gCameras[] entry it draws is a cut. */
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(4, 900.0f, 0.0f, 0.0f); /* cutscene bank */
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_resolve_camera(0, 1, 2, &camera),
           "camera: cut viewport still resolves");
    expect(camera.camera_id == 4 && camera.interpolated == 0 &&
               camera.position[0] == 900.0f,
           "camera: wrong-bank history is rejected and the new authored bank "
           "is held exactly");
}

/* A quarter-turn-plus camera pan in one tick is a cut or a whip; task 1.1's
 * rotation snap must apply to the camera path exactly like the object path.
 * The snap is per-field, not per-camera: fov and position still blend while
 * rotation_y jumps straight to current. */
/*
 * Before Task 6, a fast yaw pan past PRESENTATION_SNAPSHOT_ROTATION_SNAP
 * (0x4000, a quarter turn) was invisible to capture: the position clause saw
 * nothing to catch (position did not move), so the pair still reached
 * resolve_camera_pair as ordinary motion. Only the per-axis rotation-arc
 * snap inside presentation_lerp_angle forced yaw to its endpoint; FOV, on a
 * different axis, kept right on blending across the same "cut" -- a shear
 * where two axes of the very same camera disagreed about whether the tick
 * was continuous (docs/evidence/smoothing-artifact-repro-2026-08.md §2.1).
 *
 * 0x5000 (112.5 deg) is past both PRESENTATION_SNAPSHOT_ROTATION_SNAP (90
 * deg) and the new MDKR_CUT_YAW_DEG (67.5 deg), so Task 6's capture-time
 * yaw clause now catches this before any per-axis blending decision is
 * made: the whole camera holds, FOV included.
 */
static void test_camera_fast_pan_snaps(void) {
    PresentationCameraPose pose;
    PresentationCameraEntry camera_sample;

    begin();

    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 0.0f, 0.0f, 0.0f);
    camera_sample.rotation_y = 0;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 0.0f, 0.0f, 0.0f);
    camera_sample.rotation_y = 0x5000; /* > 0x4000 shortest-arc delta */
    camera_sample.fov = 70.0f;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    expect(presentation_snapshot_resolve_camera(0, 1, 2, &pose),
           "camera fast pan: viewport 0 resolves");
    expect(pose.interpolated == 0,
           "camera fast pan: the yaw-delta cut clause holds the whole "
           "camera instead of letting resolve_camera_pair blend it");
    expect(pose.rotation_y == 0x5000,
           "camera fast pan: rotation holds at the authored endpoint");
    expect(pose.fov == 70.0f,
           "camera fast pan: fov holds too now -- Task 6 closes the shear "
           "where fov kept blending while only rotation snapped");
}

/*
 * The per-axis 0x4000 rotation snap is a backstop Task 6 must not remove:
 * pitch, rotation_x and rotation_z are not covered by MDKR_CUT_YAW_DEG (it
 * grades yaw only, via mdkr_yaw_delta_deg), so a hard pitch snap with yaw and
 * FOV both quiet must still take the OLD per-axis path -- capture does not
 * flag a cut, resolve_camera_pair blends, and only the pitch axis snaps to
 * its endpoint while position and FOV keep blending across the same tick.
 */
static void test_camera_pitch_axis_snap_still_backstops(void) {
    PresentationCameraPose pose;
    PresentationCameraEntry camera_sample;

    begin();

    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 0.0f, 0.0f, 0.0f);
    camera_sample.pitch = 0;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 0.0f, 0.0f, 0.0f);
    camera_sample.pitch = 0x5000; /* > 0x4000: pitch-only snap */
    camera_sample.fov = 70.0f;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    expect(presentation_snapshot_resolve_camera(0, 1, 2, &pose),
           "camera pitch snap: viewport 0 resolves");
    expect(pose.interpolated == 1,
           "camera pitch snap: no capture-time clause fires for pitch, so "
           "the pair still blends");
    expect(pose.pitch == 0x5000,
           "camera pitch snap: pitch still snaps to its endpoint via the "
           "per-axis backstop");
    expect(pose.fov > 60.0f && pose.fov < 70.0f,
           "camera pitch snap: fov still blends -- this axis is untouched "
           "by Task 6's yaw/FOV clauses");
}

/*
 * Task 6: camera-cut clauses for angle and FOV.
 *
 * The position clause only catches a cut that MOVES the camera. The TT-cam
 * spectate switch and the post-race spectator handoff both swap to a camera
 * a few units from the last one but pointed somewhere else, or with a
 * different authored FOV -- exactly what these two clauses exist to catch
 * at capture time, before resolve_camera_pair ever decides to blend.
 */
static void test_camera_cut_angle_and_fov(void) {
    PresentationCameraPose pose;
    PresentationCameraEntry camera_sample;
    PresentationSnapshotStats before_stats;
    PresentationSnapshotStats after_stats;

    /* ---- yaw cut: position barely moves, yaw jumps past MDKR_CUT_YAW_DEG */
    begin();
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 0.0f, 0.0f, 0.0f);
    camera_sample.rotation_y = 0;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    presentation_snapshot_get_stats(&before_stats);

    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 1.0f, 0.0f, 0.0f); /* 1 unit: not a
                                                         * teleport */
    camera_sample.rotation_y = 20000; /* ~109.9 deg: past MDKR_CUT_YAW_DEG */
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    expect(presentation_snapshot_resolve_camera(0, 1, 2, &pose) &&
               pose.interpolated == 0 && pose.rotation_y == 20000 &&
               pose.position[0] == 1.0f,
           "camera cut: a yaw jump past MDKR_CUT_YAW_DEG holds the whole "
           "camera even though position barely moved");

    presentation_snapshot_get_stats(&after_stats);
    expect(after_stats.rotation_arc_checks == before_stats.rotation_arc_checks &&
               after_stats.rotation_arc_snaps == before_stats.rotation_arc_snaps,
           "camera cut: the yaw cut fires at capture, before "
           "rotation_arc_audit ever runs for this tick");

    /* Negative control: a yaw delta safely under threshold still blends. */
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 2.0f, 0.0f, 0.0f);
    camera_sample.rotation_y = 20000 + 4000; /* ~22 more degrees: well under
                                               * MDKR_CUT_YAW_DEG */
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_resolve_camera(0, 1, 2, &pose) &&
               pose.interpolated == 1,
           "camera cut: an ordinary pan under MDKR_CUT_YAW_DEG still blends");

    /* ---- FOV cut: position and yaw hold, FOV jumps past MDKR_CUT_FOV_DEG */
    begin();
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 0.0f, 0.0f, 0.0f); /* fov 60 */
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 0.0f, 0.0f, 0.0f);
    camera_sample.fov = 90.0f; /* +30: past MDKR_CUT_FOV_DEG */
    camera_sample.vertical_fov = 90.0f;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    expect(presentation_snapshot_resolve_camera(0, 1, 2, &pose) &&
               pose.interpolated == 0 && pose.fov == 90.0f,
           "camera cut: an FOV jump past MDKR_CUT_FOV_DEG holds the whole "
           "camera even though position and yaw did not move");

    /* Negative control: an ordinary FOV kick under threshold still blends. */
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, 0.0f, 0.0f, 0.0f);
    camera_sample.fov = 95.0f; /* +5 from 90: well under MDKR_CUT_FOV_DEG */
    camera_sample.vertical_fov = 95.0f;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_resolve_camera(0, 1, 2, &pose) &&
               pose.interpolated == 1,
           "camera cut: an ordinary FOV change under MDKR_CUT_FOV_DEG still "
           "blends");
}

/*
 * Code-review fix for Task 6: the two clauses above must stay QUIET on an
 * ordinary racing route, not merely fire when handed a hand-picked cut.
 * check_camera_snapshot_coverage.py's floor checks (counts >= wanted) only
 * catch a DECREASE in cuts and are read from a pose-distance classifier that
 * is structurally independent of the `discontinuity` flag these clauses
 * set -- neither one would notice a future change that made the clauses
 * over-fire on ordinary camera-follow panning.
 *
 * This drives a plausible sustained camera-follow pan: constant forward
 * motion, yaw ramping at a fixed rate just UNDER MDKR_CUT_YAW_DEG every
 * tick (12000 raw units == ~65.9 deg/tick, versus the 12288-unit/67.5-deg
 * threshold), FOV drifting +-15 deg/tick (under the 20 deg MDKR_CUT_FOV_DEG
 * threshold) across 40 consecutive ticks -- a tight hairpin's worth of
 * continuous pan -- and asserts every single one of those ticks blends
 * (`interpolated == 1`) with zero new discontinuities. The positive control
 * appends one more tick whose yaw delta crosses the threshold (13000 raw
 * units, ~71.1 deg) and asserts THAT tick, and only that tick, cuts -- so
 * the 40-tick quiet stretch is not quiet merely because the clause can
 * never fire in this test.
 */
static void test_camera_cut_clauses_quiet_on_ordinary_pan(void) {
    PresentationCameraPose pose;
    PresentationCameraEntry camera_sample;
    PresentationSnapshotStats stats;
    uint16_t rotation_raw = 0;
    float fov = 60.0f;
    float position_x = 0.0f;
    int tick;
    /* 12000/65536*360 = 65.918 deg/tick: safely under MDKR_CUT_YAW_DEG's
     * 12288-unit (67.5 deg) threshold. */
    const uint16_t yaw_step_quiet = 12000;
    /* 13000/65536*360 = 71.398 deg/tick: past the threshold. */
    const uint16_t yaw_step_cut = 13000;

    begin();

    /* Seed tick: always a viewport-entry discontinuity, not part of the
     * quiet claim below. */
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(0, position_x, 0.0f, 0.0f);
    camera_sample.rotation_y = (int16_t)rotation_raw;
    camera_sample.fov = fov;
    camera_sample.vertical_fov = fov;
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    presentation_snapshot_get_stats(&stats);
    for (tick = 0; tick < 40; tick++) {
        uint64_t discontinuities_before = stats.discontinuities;

        position_x += 10.0f;                 /* ordinary forward motion */
        rotation_raw = (uint16_t)(rotation_raw + yaw_step_quiet);
        fov += (tick % 2 == 0) ? 15.0f : -15.0f; /* drift, under threshold */

        presentation_snapshot_capture_begin();
        camera_sample = make_camera(0, position_x, 0.0f, 0.0f);
        camera_sample.rotation_y = (int16_t)rotation_raw;
        camera_sample.fov = fov;
        camera_sample.vertical_fov = fov;
        presentation_snapshot_capture_camera(&camera_sample);
        presentation_snapshot_capture_commit();

        expect(presentation_snapshot_resolve_camera(0, 1, 2, &pose) &&
                   pose.interpolated == 1,
               "quiet pan: an ordinary camera-follow tick under both "
               "thresholds must blend, not cut");

        presentation_snapshot_get_stats(&stats);
        expect(stats.discontinuities == discontinuities_before,
               "quiet pan: an ordinary camera-follow tick must not add a "
               "new discontinuity");
    }

    /* Positive control: one tick over the yaw threshold, everything else
     * unremarkable, must cut -- proving the 40-tick quiet stretch above was
     * not quiet merely because these clauses cannot fire in this test. */
    {
        uint64_t discontinuities_before = stats.discontinuities;

        position_x += 10.0f;
        rotation_raw = (uint16_t)(rotation_raw + yaw_step_cut);

        presentation_snapshot_capture_begin();
        camera_sample = make_camera(0, position_x, 0.0f, 0.0f);
        camera_sample.rotation_y = (int16_t)rotation_raw;
        camera_sample.fov = fov;
        camera_sample.vertical_fov = fov;
        presentation_snapshot_capture_camera(&camera_sample);
        presentation_snapshot_capture_commit();

        expect(presentation_snapshot_resolve_camera(0, 1, 2, &pose) &&
                   pose.interpolated == 0,
               "quiet pan: a yaw delta past MDKR_CUT_YAW_DEG on this same "
               "route still cuts -- the clause is armed, not disabled");
        presentation_snapshot_get_stats(&stats);
        expect(stats.discontinuities == discontinuities_before + 1,
               "quiet pan: exactly one new discontinuity for the one tick "
               "that crossed the threshold");
    }
}

/*
 * A game-side camera cut is filed in VIEWPORT space, and it must still be
 * found when that viewport is drawing its CUTSCENE-bank camera.
 *
 * This is the exact shape the shipped defect had: the note sites all pass a
 * player index (== the viewport), while camSetProjMtx records the gCameras[]
 * slot `viewport + (gCutsceneCameraActive ? 4 : 0)`. Keyed on the slot, a note
 * raised as bit 0 was looked for at bit 4 on every tick a cutscene camera owned
 * viewport 0 — it missed, and the old commit then destroyed it unconditionally,
 * so the camera blended straight across a hard cut. Nothing about the pose says
 * "cut" here on purpose: the move is 300 units, well inside the 2000-unit
 * teleport threshold, exactly like two adjacent spectate points.
 */
static void test_camera_cut_note_viewport_space(void) {
    PresentationCameraPose camera;
    PresentationCameraEntry camera_sample;
    PresentationSnapshotStats stats;

    begin();

    /* Viewport 0 on the cutscene bank (slot 4), two adjacent settled ticks. */
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(4, 0.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    presentation_snapshot_capture_begin();
    camera_sample = make_camera(4, 100.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    expect(presentation_snapshot_resolve_camera(0, 1, 2, &camera) &&
               camera.interpolated == 1 && camera.position[0] == 50.0f,
           "cut note: an unflagged cutscene-bank pair still interpolates");

    /* Now the game snaps the shot. The note names the VIEWPORT. */
    presentation_snapshot_note_camera_cut(0);
    presentation_snapshot_capture_begin();
    camera_sample = make_camera(4, 400.0f, 0.0f, 0.0f); /* 300 units: motion */
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();

    expect(presentation_snapshot_resolve_camera(0, 1, 2, &camera) &&
               camera.interpolated == 0 && camera.position[0] == 400.0f,
           "cut note: a viewport note is found while that viewport draws the "
           "cutscene bank, and holds the authored pose");

    presentation_snapshot_get_stats(&stats);
    expect(stats.camera_cut_notes == 1 && stats.camera_cut_consumed == 1 &&
               stats.camera_cut_unconsumed == 0,
           "cut note: the note was raised once and consumed by the capture "
           "of the viewport it names");

    /*
     * Carry, not destroy. A note raised on a tick whose camera capture never
     * ran survives to the next capture of that viewport. The old code cleared
     * the whole mask at every commit, so this note simply vanished.
     */
    presentation_snapshot_note_camera_cut(0);
    presentation_snapshot_capture_begin();   /* no camera captured this tick */
    presentation_snapshot_capture_commit();
    presentation_snapshot_get_stats(&stats);
    expect(stats.camera_cut_consumed == 1 && stats.camera_cut_unconsumed == 0,
           "cut note: a tick that captures no camera neither consumes nor "
           "discards the note");

    presentation_snapshot_capture_begin();
    camera_sample = make_camera(4, 800.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_camera(&camera_sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_get_stats(&stats);
    expect(stats.camera_cut_notes == 2 && stats.camera_cut_consumed == 2 &&
               stats.camera_cut_unconsumed == 0,
           "cut note: the carried note is spent by the next capture of that "
           "viewport, and nothing is left unconsumed");
}

/*
 * A lifecycle spawn the identity table cannot register fails the NEXT commit
 * whole instead of returning silently.
 *
 * The silent return left a recycled address carrying a dead object's
 * generation, last_position and last_capture, so the following capture skipped
 * the fresh-generation branch and published a new object under the dead one's
 * identity — a pair the generation check happily blends. Failing the frame is
 * the same direction presentation_snapshot_capture_object already takes on the
 * identical condition.
 */
static void test_identity_insert_failure_fails_closed(void) {
    /* Half load is identity_insert's probing budget, so the table refuses at
     * SLOTS/2 live entries. One address past that is the failure. */
    static char fill[PRESENTATION_SNAPSHOT_IDENTITY_SLOTS / 2 + 1];
    PresentationSnapshotStats stats;
    size_t index;

    begin();
    for (index = 0; index < PRESENTATION_SNAPSHOT_IDENTITY_SLOTS / 2; index++) {
        presentation_snapshot_note_spawn(&fill[index]);
    }
    presentation_snapshot_get_stats(&stats);
    expect(stats.identity_insert_failures == 0,
           "identity overflow: a table at exactly half load still registers");

    presentation_snapshot_note_spawn(
        &fill[PRESENTATION_SNAPSHOT_IDENTITY_SLOTS / 2]);
    presentation_snapshot_get_stats(&stats);
    expect(stats.identity_insert_failures == 1,
           "identity overflow: the refused spawn is counted, not swallowed");

    presentation_snapshot_capture_begin();
    presentation_snapshot_capture_commit();
    presentation_snapshot_get_stats(&stats);
    expect(stats.captures == 0 && stats.overflows == 1,
           "identity overflow: the next commit fails whole rather than "
           "publishing an object whose identity was never issued");

    presentation_snapshot_capture_begin();
    presentation_snapshot_capture_commit();
    presentation_snapshot_get_stats(&stats);
    expect(stats.captures == 1 && stats.overflows == 1,
           "identity overflow: the flag is spent by one frame, not sticky "
           "for the rest of the run");
}

static void test_authored_camera_latch(void) {
    PresentationCameraEntry cameras[4];
    PresentationCameraEntry camera0 = make_camera(4, 10.0f, 20.0f, 30.0f);
    PresentationCameraEntry camera1 = make_camera(5, 40.0f, 50.0f, 60.0f);
    PresentationCameraEntry conflicting;
    size_t count;

    memset(cameras, 0, sizeof(cameras));
    camera0.viewport_index = 0;
    camera1.viewport_index = 1;
    camera0.authored_view_projection[3][0] = 123.0f;

    begin();
    presentation_snapshot_authored_cameras_begin(41u);
    expect(presentation_snapshot_authored_camera_record(&camera0) &&
               presentation_snapshot_authored_camera_record(&camera1),
           "camera latch: complete authored recipes record beside the VP");
    count = presentation_snapshot_authored_cameras_copy(41u, cameras, 4u);
    expect(count == 2u && cameras[0].camera_id == 4 &&
               cameras[1].camera_id == 5 &&
               bits_equal(cameras[0].position[0], 10.0f) &&
               bits_equal(cameras[0].authored_view_projection[3][0], 123.0f),
           "camera latch: exact authored recipes survive later flag teardown");
    expect(presentation_snapshot_authored_cameras_copy(
               42u, cameras, 4u) == 0u,
           "camera latch: a different live tick cannot claim an older list");

    /* Pausing changes whether lifecycle code clears the cutscene flag, not
     * which camera the already-authored viewport used. Re-recording the same
     * paused viewport is idempotent and stays bank 4. */
    presentation_snapshot_authored_cameras_begin(42u);
    expect(presentation_snapshot_authored_camera_record(&camera0) &&
               presentation_snapshot_authored_camera_record(&camera0) &&
               presentation_snapshot_authored_cameras_copy(
                   42u, cameras, 4u) == 1u && cameras[0].camera_id == 4,
           "camera latch: paused cutscene retains its exact authored camera");

    presentation_snapshot_authored_cameras_begin(43u);
    expect(presentation_snapshot_authored_camera_record(&camera1) &&
               presentation_snapshot_authored_cameras_copy(
                   43u, cameras, 4u) == 0u,
           "camera latch: sparse viewport ownership fails closed");

    presentation_snapshot_authored_cameras_begin(44u);
    camera0.camera_id = 0;
    conflicting = camera0;
    conflicting.position[0] += 1.0f;
    expect(presentation_snapshot_authored_camera_record(&camera0) &&
               !presentation_snapshot_authored_camera_record(&conflicting) &&
               presentation_snapshot_authored_cameras_copy(
                   44u, cameras, 4u) == 0u,
           "camera latch: conflicting recipes fail closed");
}

static void test_authored_tick_pair(void) {
    uint64_t target = 0u;

    begin();
    presentation_snapshot_capture_begin_authored(70u);
    presentation_snapshot_capture_commit();
    expect(!presentation_snapshot_replay_target_tick(70u, &target),
           "authored ticks: one snapshot cannot manufacture a target pair");

    presentation_snapshot_capture_begin_authored(71u);
    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_replay_target_tick(70u, &target) &&
               target == 71u,
           "authored ticks: exact task-to-next snapshot pair resolves");
    expect(!presentation_snapshot_replay_target_tick(69u, &target) &&
               !presentation_snapshot_replay_target_tick(71u, &target),
           "authored ticks: wrong task token fails closed");

    presentation_snapshot_capture_begin_authored(73u);
    presentation_snapshot_capture_commit();
    expect(!presentation_snapshot_replay_target_tick(71u, &target),
           "authored ticks: a skipped snapshot tick fails closed");
}

/* ---- retained deformation compatibility -------------------------------- */

static void test_deformation_compatibility(void) {
    PresentationObjectEntry sample;
    uint64_t generation = 0;

    begin();
    presentation_snapshot_note_spawn(s_slot_a);
    expect(presentation_snapshot_identity_generation(
               s_slot_a, &generation) && generation != 0u,
           "deformation: renderer obtains the exact object generation");

    /* The spawn frame is deliberately not a deformation endpoint. A third
     * capture proves a continuous pair after the spawn discontinuity. */
    for (int tick = 0; tick < 3; tick++) {
        presentation_snapshot_capture_begin();
        sample = make_object(s_slot_a, (float)tick, 0.0f, 0.0f);
        sample.model_index = 4;
        sample.animation_id = 7;
        presentation_snapshot_capture_object(&sample);
        presentation_snapshot_capture_commit();
    }
    expect(presentation_snapshot_deformation_compatible(
               s_slot_a, generation),
           "deformation: continuous generation/model/animation pair is compatible");

    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 3.0f, 0.0f, 0.0f);
    sample.model_index = 4;
    sample.animation_id = 8;
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    expect(!presentation_snapshot_deformation_compatible(
               s_slot_a, generation),
           "deformation: animation transition holds the authored tick pose");

    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 4.0f, 0.0f, 0.0f);
    sample.model_index = 5;
    sample.animation_id = 8;
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    expect(!presentation_snapshot_deformation_compatible(
               s_slot_a, generation),
           "deformation: model/topology transition is incompatible");

    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 5.0f, 0.0f, 0.0f);
    sample.model_index = 5;
    sample.animation_id = 8;
    sample.is_particle = 1;
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    expect(!presentation_snapshot_deformation_compatible(
               s_slot_a, generation),
           "deformation: particle topology uses a separate future recipe");

    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 10000.0f, 0.0f, 0.0f);
    sample.model_index = 5;
    sample.animation_id = 8;
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    expect(!presentation_snapshot_deformation_compatible(
               s_slot_a, generation),
           "deformation: a teleport discontinuity never blends");

    presentation_snapshot_note_free(s_slot_a);
    presentation_snapshot_note_spawn(s_slot_a);
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 6.0f, 0.0f, 0.0f);
    sample.model_index = 5;
    sample.animation_id = 8;
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    expect(!presentation_snapshot_deformation_compatible(
               s_slot_a, generation),
           "deformation: stale generation cannot pair with a recycled address");

    begin();
    presentation_snapshot_note_spawn(s_slot_a);
    expect(presentation_snapshot_identity_generation(
               s_slot_a, &generation),
           "particle deformation: generation is registered");
    for (int tick = 0; tick < 3; tick++) {
        presentation_snapshot_capture_begin();
        sample = make_object(s_slot_a, (float)tick, 0.0f, 0.0f);
        sample.is_particle = 1;
        sample.model_index = 3; /* line */
        sample.animation_id = tick; /* texture frames do not change topology */
        presentation_snapshot_capture_object(&sample);
        presentation_snapshot_capture_commit();
    }
    expect(presentation_snapshot_particle_deformation_compatible(
               s_slot_a, generation) &&
               !presentation_snapshot_deformation_compatible(
                   s_slot_a, generation),
           "particle deformation: same kind pairs independently of texture frame");

    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 3.0f, 0.0f, 0.0f);
    sample.is_particle = 1;
    sample.model_index = 4; /* point: different topology */
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    expect(!presentation_snapshot_particle_deformation_compatible(
               s_slot_a, generation),
           "particle deformation: kind/topology transition holds");
}

/* ---- 6. published-pair immutability -------------------------------------- */

static void test_publish_immutability(void) {
    PresentationObjectEntry sample;
    PresentationSnapshot pinned_current;
    PresentationSnapshot pinned_previous;
    const PresentationSnapshot *current;
    const PresentationSnapshot *previous;
    PresentationObjectPose pose;

    begin();
    presentation_snapshot_note_spawn(s_slot_a);

    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 1.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 2.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();

    current = presentation_snapshot_current();
    previous = presentation_snapshot_previous();
    expect(current != NULL && previous != NULL,
           "publish: a pair is published after two commits");
    pinned_current = *current;
    pinned_previous = *previous;

    /*
     * Now stage a THIRD capture without committing it, the way a render
     * thread would be mid-frame when the next tick boundary arrives. The
     * published pair must not move a byte — which is why the ring is three
     * slots deep and not two.
     */
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 99.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);

    expect(presentation_snapshot_current() == current &&
               presentation_snapshot_previous() == previous,
           "publish: an in-flight capture does not move the published pair");
    expect(memcmp(&pinned_current, current, sizeof(pinned_current)) == 0,
           "publish: the published current is byte-identical mid-capture");
    expect(memcmp(&pinned_previous, previous, sizeof(pinned_previous)) == 0,
           "publish: the published previous is byte-identical mid-capture");
    expect(presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose) &&
               pose.position[0] == 1.5f,
           "publish: consumers keep resolving the old pair mid-capture");

    presentation_snapshot_capture_commit();
    expect(presentation_snapshot_previous() == current,
           "publish: commit promotes current to previous");
    expect(presentation_snapshot_current()->generation ==
               pinned_current.generation + 1,
           "publish: the generation counter advances by one per publish");
    expect(presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose) &&
               pose.position[0] == 50.5f,
           "publish: the new pair is live after commit");
}

/* ---- 7. atomic overflow with retention ----------------------------------- */

static void test_overflow_is_atomic(void) {
    static char slots[PRESENTATION_SNAPSHOT_MAX_OBJECTS + 8];
    PresentationObjectEntry sample;
    PresentationSnapshot pinned_current;
    PresentationSnapshot pinned_previous;
    PresentationSnapshotStats stats;
    const PresentationSnapshot *current;
    const PresentationSnapshot *previous;
    PresentationObjectPose pose;
    int index;

    begin();
    presentation_snapshot_note_spawn(s_slot_a);

    /* Two good ticks, so there is a pair worth retaining. */
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 0.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 10.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();

    current = presentation_snapshot_current();
    previous = presentation_snapshot_previous();
    pinned_current = *current;
    pinned_previous = *previous;
    presentation_snapshot_get_stats(&stats);
    expect(stats.captures == 2, "overflow: two good captures so far");
    expect(stats.objects_peak == 1, "overflow: peak tracks published frames");

    /* A capture with more objects than the table can hold. */
    presentation_snapshot_capture_begin();
    for (index = 0; index < PRESENTATION_SNAPSHOT_MAX_OBJECTS + 4; index++) {
        sample = make_object(&slots[index], (float)index, 0.0f, 0.0f);
        presentation_snapshot_capture_object(&sample);
    }
    presentation_snapshot_capture_commit();

    presentation_snapshot_get_stats(&stats);
    expect(stats.overflows == 1, "overflow: the failure is counted");
    expect(stats.captures == 2, "overflow: nothing new was published");
    expect(stats.objects_peak == 1,
           "overflow: a failed capture never touches the peak");
    expect(presentation_snapshot_current() == current &&
               presentation_snapshot_previous() == previous,
           "overflow: the published pair is retained");
    expect(memcmp(&pinned_current, current, sizeof(pinned_current)) == 0 &&
               memcmp(&pinned_previous, previous, sizeof(pinned_previous)) == 0,
           "overflow: the retained pair is byte-identical");
    expect(presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose) &&
               pose.interpolated == 1 && pose.position[0] == 5.0f,
           "overflow: consumers still resolve the retained pair");
    expect(!presentation_snapshot_resolve_object(&slots[0], 1, 2, &pose),
           "overflow: not one entry of the failed capture is visible");

    /*
     * Recovery: the next well-sized capture publishes, and because the
     * failed one never published, the object that WAS captured on both good
     * ticks has a non-adjacent pair and correctly refuses to interpolate
     * across the gap.
     */
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 20.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    presentation_snapshot_capture_commit();
    presentation_snapshot_get_stats(&stats);
    expect(stats.captures == 3, "overflow: capture resumes after the failure");
    presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose);
    expect(pose.interpolated == 0,
           "overflow: no blend across the tick the failed capture ate");

    /* A capture that overflows on CAMERAS fails just as whole. */
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 30.0f, 0.0f, 0.0f);
    presentation_snapshot_capture_object(&sample);
    for (index = 0; index < PRESENTATION_SNAPSHOT_MAX_VIEWPORTS + 1; index++) {
        PresentationCameraEntry camera_sample = make_camera(index, 0, 0, 0);
        presentation_snapshot_capture_camera(&camera_sample);
    }
    presentation_snapshot_capture_commit();
    presentation_snapshot_get_stats(&stats);
    expect(stats.overflows == 2 && stats.captures == 3,
           "overflow: a viewport overrun also fails the whole snapshot");
}

/* ---- seam ---------------------------------------------------------------- */

static void test_disabled_seam(void) {
    PresentationObjectEntry sample;
    PresentationObjectPose pose;
    PresentationSnapshotStats stats;

    presentation_snapshot_shutdown();
    presentation_snapshot_set_enabled(false);

    presentation_snapshot_note_spawn(s_slot_a);
    presentation_snapshot_capture_begin();
    sample = make_object(s_slot_a, 1.0f, 0.0f, 0.0f);
    expect(!presentation_snapshot_capture_object(&sample),
           "seam: capture is inert when the seam is off");
    presentation_snapshot_capture_commit();
    presentation_snapshot_stage_reset();

    expect(presentation_snapshot_current() == NULL &&
               presentation_snapshot_previous() == NULL,
           "seam: nothing is published when the seam is off");
    expect(!presentation_snapshot_resolve_object(s_slot_a, 1, 2, &pose),
           "seam: nothing resolves when the seam is off");
    presentation_snapshot_get_stats(&stats);
    expect(stats.captures == 0 && stats.overflows == 0 && stats.resets == 0,
           "seam: no statistic moves when the seam is off");
}

int main(void) {
    test_angle_shortest_arc();
    test_rotation_snap();
    test_exact_endpoints();
    test_discrete_rule();
    test_identity_generation_reuse();
    test_identity_ensure_generation();
    test_discontinuity();
    test_resolved_fields();
    test_camera_fast_pan_snaps();
    test_camera_pitch_axis_snap_still_backstops();
    test_camera_cut_angle_and_fov();
    test_camera_cut_clauses_quiet_on_ordinary_pan();
    test_camera_cut_note_viewport_space();
    test_identity_insert_failure_fails_closed();
    test_authored_camera_latch();
    test_authored_tick_pair();
    test_deformation_compatibility();
    test_publish_immutability();
    test_overflow_is_atomic();
    test_disabled_seam();

    if (failures != 0) {
        fprintf(stderr, "presentation_snapshot: %d failure(s)\n", failures);
        return 1;
    }
    printf("presentation_snapshot: all checks passed\n");
    return 0;
}
