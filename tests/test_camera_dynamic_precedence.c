/*
 * The dynamic narrow-phase winner ordering, driven through the PRODUCTION
 * comparator.
 *
 * The structural gate used to assert this contract against a Python
 * reimplementation of the same rule, which proves only that two copies of the
 * rule agree. This target includes the production translation unit so the
 * ordering under test is the one the sweeps actually call.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DKR_ANGLE_TO_RAD (6.28318530717958647692f / 65536.0f)

#include "../game/src/camera_dynamic_occlusion.c"

/* The included translation unit retains its native pointer resolver reference;
 * no test here loads a model. */
void *dkr_lo32_to_ptr(uint32_t lo32) {
    (void)lo32;
    return NULL;
}

/* Only the comparator is under test. An empty authoritative object list keeps
 * the census a no-op rather than smuggling a second object model in. */
Object **objGetObjList(s32 *first, s32 *count) {
    if (first != NULL) *first = 0;
    if (count != NULL) *count = 0;
    return NULL;
}

/* The engine's fixed-angle table lives behind ROM setup this target does not
 * perform. The production matrix recipe still runs; only its angle source is
 * this libm stand-in, which is the same one the other camera unit tests use. */
static s32 test_sine(s16 angle) {
    const uint16_t turn = (uint16_t)angle;

    if (turn == 0U || turn == 0x8000U) return 0;
    if (turn == 0x4000U) return 0x10000;
    if (turn == 0xC000U) return -0x10000;
    return (s32)lroundf(sinf((float)turn * DKR_ANGLE_TO_RAD) * 65536.0f);
}

s32 sins_s16(s16 angle) {
    return test_sine(angle);
}

s32 coss_s16(s16 angle) {
    return test_sine((s16)(angle + 0x4000));
}

static int failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static MdkrCameraDynamicOcclusionHit make_hit(
    float fraction,
    uint64_t spawn_generation,
    uint32_t model_generation,
    uint32_t source_triangle_stable_id,
    uint32_t stable_id,
    size_t authoritative_list_index) {
    MdkrCameraDynamicOcclusionHit hit;

    memset(&hit, 0, sizeof(hit));
    hit.hit.fraction = fraction;
    hit.hit.stable_id = stable_id;
    hit.object_spawn_generation = spawn_generation;
    hit.model_generation = model_generation;
    hit.source_triangle_stable_id = source_triangle_stable_id;
    hit.authoritative_list_index = authoritative_list_index;
    return hit;
}

/* Exercise one ordering key in isolation: everything ahead of it is equal, and
 * the loser differs only in that key. Both directions are asserted so a
 * comparator that returns a constant fails. */
static void expect_key_orders(
    const char *name,
    MdkrCameraDynamicOcclusionHit lower,
    MdkrCameraDynamicOcclusionHit higher) {
    char label[96];

    snprintf(label, sizeof(label), "%s: lower precedes higher", name);
    expect(label, mdkr_camera_dynamic_hit_precedes(&lower, &higher));
    snprintf(label, sizeof(label), "%s: higher does not precede lower", name);
    expect(label, !mdkr_camera_dynamic_hit_precedes(&higher, &lower));
}

int main(void) {
    const double epsilon = MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON;
    const float within = (float)(0.5 * epsilon);
    const float beyond = (float)(2.0 * epsilon);
    MdkrCameraDynamicOcclusionHit a;
    MdkrCameraDynamicOcclusionHit b;

    /* Time dominates once the difference leaves the public tie window. */
    expect_key_orders(
        "earlier hit beyond the tie window",
        make_hit(0.0f, 9U, 9U, 9U, 9U, 9U),
        make_hit(beyond, 1U, 1U, 1U, 1U, 1U));

    /* Inside the window the fractions are a tie, so identity decides. */
    a = make_hit(within, 1U, 1U, 1U, 1U, 1U);
    b = make_hit(0.0f, 2U, 1U, 1U, 1U, 1U);
    expect("within the tie window a later hit still wins on spawn generation",
           mdkr_camera_dynamic_hit_precedes(&a, &b));
    a = make_hit(beyond, 1U, 1U, 1U, 1U, 1U);
    expect("beyond the tie window a later hit cannot win on identity",
           !mdkr_camera_dynamic_hit_precedes(&a, &b));

    /* The full precedence order, one key at a time, most significant first. */
    expect_key_orders(
        "spawn generation",
        make_hit(0.0f, 1U, 9U, 9U, 9U, 9U),
        make_hit(0.0f, 2U, 1U, 1U, 1U, 1U));
    expect_key_orders(
        "model generation",
        make_hit(0.0f, 3U, 1U, 9U, 9U, 9U),
        make_hit(0.0f, 3U, 2U, 1U, 1U, 1U));
    expect_key_orders(
        "source triangle stable id",
        make_hit(0.0f, 3U, 5U, 1U, 9U, 9U),
        make_hit(0.0f, 3U, 5U, 2U, 1U, 1U));
    expect_key_orders(
        "hit stable id",
        make_hit(0.0f, 3U, 5U, 7U, 1U, 9U),
        make_hit(0.0f, 3U, 5U, 7U, 2U, 1U));
    expect_key_orders(
        "authoritative list index",
        make_hit(0.0f, 3U, 5U, 7U, 11U, 1U),
        make_hit(0.0f, 3U, 5U, 7U, 11U, 2U));

    /* A fully equal pair is not "before" itself, so a first winner is never
     * displaced by an identical later candidate. */
    a = make_hit(0.25f, 3U, 5U, 7U, 11U, 13U);
    b = a;
    expect("an identical candidate does not displace the incumbent",
           !mdkr_camera_dynamic_hit_precedes(&a, &b));

    if (failures != 0) {
        fprintf(stderr, "%d camera-dynamic-precedence assertion(s) failed\n",
                failures);
        return 1;
    }
    printf("camera-dynamic-precedence: production ordering pinned\n");
    return 0;
}
