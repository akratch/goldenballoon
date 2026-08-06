#include "camera_obstruction_runtime.h"

#ifdef NATIVE_PORT

#include "camera.h"
#include "camera_dynamic_occlusion.h"
#include "game.h"
#include "game_ui.h"
#include "math_util.h"
#include "objects.h"
#include "racer.h"
#include "tracks.h"
#include "thread3_main.h"
#include "platform_os.h"

#include "camera_obstruction.h"
#include "camera_obstruction_query.h"
#include "camera_target_visibility.h"
#include "camera_obstruction_resolver.h"

#include <ultra64.h>
#include <PRinternal/viint.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT 8
#define MDKR_CAMERA_OBSTRUCTION_DEG_TO_RAD 0.01745329251994329576923690768489f
#define MDKR_CAMERA_OBSTRUCTION_ANCHOR_STEPS 32
#define MDKR_CAMERA_OBSTRUCTION_ANCHOR_REFINE_STEPS 8
#define MDKR_CAMERA_OBSTRUCTION_EXACT_ANCHOR_REFINE_STEPS 3
#define MDKR_CAMERA_OBSTRUCTION_LENS_QUERY_CACHE_SIZE 8
#define MDKR_CAMERA_OBSTRUCTION_HARD_MASK 1U
/*
 * Release hysteresis on retraction (docs/architecture/camera-obstruction.md
 * section 7.3). Contact retracts the boom on the tick it is seen -- retract
 * latency stays 0 -- but a clear corridor does NOT start recovery until it has
 * been clear this many consecutive authored ticks.
 *
 * The value comes from the MOTION-01 traces, not from taste. On the 3P + T.T.
 * spectate route the corridor sweep drops out against a wall it is still
 * pressed against for runs of 1, 1, 3, 4 and 7 ticks before the same or an
 * adjacent facet of that wall blocks again -- twice with the identical blocker
 * id on both sides of the gap, which is what proves the gap is a false
 * negative rather than a wall the camera drove past. 8 is the smallest hold
 * that covers every measured run; 9 adds one tick of margin.
 *
 * 9 is deliberately still less than the census's 12-tick chatter window: a
 * clear run of 9, 10 or 11 ticks releases and, if the correction comes back,
 * is still counted as a re-engagement. The hold closes the measured defect
 * without hollowing out the assertion that measures it.
 */
#define MDKR_CAMERA_OBSTRUCTION_RELEASE_HOLD_TICKS 9U
#define MDKR_CAMERA_PERF_BIN_WIDTH_NS UINT64_C(10000)
#define MDKR_CAMERA_PERF_BIN_COUNT 1024U
#define MDKR_CAMERA_PERF_NTSC_P99_BUDGET_NS UINT64_C(833333)
#define MDKR_CAMERA_PERF_NTSC_TAIL_BUDGET_NS UINT64_C(1666667)

typedef enum MdkrCameraPerfSection {
    MDKR_CAMERA_PERF_FINALIZER = 0,
    MDKR_CAMERA_PERF_SLOT,
    MDKR_CAMERA_PERF_STATIC_QUERY,
    MDKR_CAMERA_PERF_DYNAMIC_QUERY,
    MDKR_CAMERA_PERF_DYNAMIC_PUBLICATION,
    MDKR_CAMERA_PERF_EXACT_STATIC_QUERY,
    MDKR_CAMERA_PERF_EXACT_DYNAMIC_QUERY,
    MDKR_CAMERA_PERF_SECTION_COUNT,
} MdkrCameraPerfSection;

typedef struct MdkrCameraPerfHistogram {
    uint64_t bins[MDKR_CAMERA_PERF_BIN_COUNT];
    uint64_t hits;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t overflow;
    uint64_t over_p99_budget;
    uint64_t over_tail_budget;
} MdkrCameraPerfHistogram;

static int sCameraPerfEnabled = -1;
static int sCameraExactShadowEnabled = -1;
static MdkrCameraPerfHistogram sCameraPerf[MDKR_CAMERA_PERF_SECTION_COUNT];
static uint64_t sCameraPerfSelectedTicks[5];

static int camera_obstruction_exact_shadow_enabled(void) {
    if (sCameraExactShadowEnabled < 0) {
        const char *value = getenv("MDKR_CAMERA_EXACT_SHADOW");
        sCameraExactShadowEnabled =
            value != NULL && value[0] != '\0' && value[0] != '0';
    }
    return sCameraExactShadowEnabled != 0;
}

static int camera_obstruction_perf_enabled(void) {
    if (sCameraPerfEnabled < 0) {
        const char *value = getenv("MDKR_CAMERA_PERF");
        sCameraPerfEnabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    return sCameraPerfEnabled != 0;
}

static uint64_t camera_obstruction_perf_begin(void) {
    if (!camera_obstruction_perf_enabled()) {
        return 0U;
    }
    return platform_perf_monotonic_ns();
}

static void camera_obstruction_perf_add(MdkrCameraPerfSection section, uint64_t start) {
    MdkrCameraPerfHistogram *histogram;
    uint64_t now;
    uint64_t elapsed;
    uint64_t bin;

    if (start == 0U || section < 0 || section >= MDKR_CAMERA_PERF_SECTION_COUNT) {
        return;
    }
    now = platform_perf_monotonic_ns();
    if (now < start) {
        return;
    }
    elapsed = now - start;
    histogram = &sCameraPerf[section];
    if (UINT64_MAX - histogram->total_ns < elapsed) {
        histogram->total_ns = UINT64_MAX;
    } else {
        histogram->total_ns += elapsed;
    }
    histogram->hits++;
    if (histogram->hits == 1U || elapsed < histogram->min_ns) {
        histogram->min_ns = elapsed;
    }
    if (elapsed > histogram->max_ns) {
        histogram->max_ns = elapsed;
    }
    if (elapsed > MDKR_CAMERA_PERF_NTSC_P99_BUDGET_NS) {
        histogram->over_p99_budget++;
    }
    if (elapsed > MDKR_CAMERA_PERF_NTSC_TAIL_BUDGET_NS) {
        histogram->over_tail_budget++;
    }
    bin = elapsed / MDKR_CAMERA_PERF_BIN_WIDTH_NS;
    if (bin >= MDKR_CAMERA_PERF_BIN_COUNT) {
        bin = MDKR_CAMERA_PERF_BIN_COUNT - 1U;
        histogram->overflow++;
    }
    histogram->bins[bin]++;
}

static uint64_t camera_obstruction_perf_percentile(
    const MdkrCameraPerfHistogram *histogram, uint64_t numerator) {
    uint64_t target;
    uint64_t seen = 0U;
    size_t bin;

    if (histogram->hits == 0U) {
        return 0U;
    }
    target = (histogram->hits * numerator + 99U) / 100U;
    for (bin = 0U; bin < MDKR_CAMERA_PERF_BIN_COUNT; bin++) {
        seen += histogram->bins[bin];
        if (seen >= target) {
            if (bin + 1U == MDKR_CAMERA_PERF_BIN_COUNT && histogram->overflow != 0U) {
                return histogram->max_ns;
            }
            return (bin + 1U) * MDKR_CAMERA_PERF_BIN_WIDTH_NS;
        }
    }
    return histogram->max_ns;
}

/* camera.c/tracks.c deliberately keep these globals out of their public ABI. */
extern Camera gCameras[MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT];
extern s32 gScenePlayerViewports;
extern s32 gNoCamShake;

typedef enum MdkrCameraObstructionRuntimePolicy {
    MDKR_CAMERA_RUNTIME_OBSERVE = 0,
    MDKR_CAMERA_RUNTIME_LEGACY,
    MDKR_CAMERA_RUNTIME_MODERN,
    MDKR_CAMERA_RUNTIME_CENTER_RAY,
} MdkrCameraObstructionRuntimePolicy;

/*
 * Mode policy (docs/architecture/camera-obstruction.md section 5.8). One
 * profile per authored camera family, naming exactly which correction stages
 * that family consents to. This is a table, not a ladder: a profile is a set of
 * enabled stages, and the enum order is only the order they were introduced.
 *
 *   FULL             sweep + retract + recovery + alternate shoulder/elevation
 *                    fan + emergency framing. The camera may leave the authored
 *                    pivot->eye ray to keep the racer readable.
 *   SAFETY_ONLY      sweep + retract + recovery + emergency framing, with no
 *                    alternate fan. The eye stays on the authored pivot->eye
 *                    ray and only moves along it. This is the racing profile:
 *                    a shot whose composition is already load-bearing for
 *                    steering may be shortened but must not be swung sideways.
 *   DEPENETRATE_ONLY the authored eye and orientation are kept; the eye is only
 *                    pushed out of geometry it has penetrated. Never retracts,
 *                    recovers, or considers an alternate shot.
 *
 * Only camera_obstruction_family_treatment() decides which family gets which
 * profile, so a route can be measured under a different profile by forcing that
 * one function (MDKR_CAMERA_PROFILE_FORCE) rather than by editing stages.
 */
typedef enum MdkrCameraObstructionTreatment {
    MDKR_CAMERA_OBSTRUCTION_TREATMENT_FULL = 0,
    MDKR_CAMERA_OBSTRUCTION_TREATMENT_SAFETY_ONLY,
    MDKR_CAMERA_OBSTRUCTION_TREATMENT_DEPENETRATE_ONLY,
    MDKR_CAMERA_OBSTRUCTION_TREATMENT_COUNT,
} MdkrCameraObstructionTreatment;

static const char *const kCameraObstructionTreatmentNames
    [MDKR_CAMERA_OBSTRUCTION_TREATMENT_COUNT] = {
    "full", "safety_only", "depenetrate_only",
};

static const char *const kCameraIntentFamilyNames[MDKR_CAMERA_INTENT_FAMILY_COUNT] = {
    "unknown", "car", "hovercraft", "plane", "loop", "fixed",
    "finish_challenge", "finish_race", "tt_spectate", "scripted_cutscene",
};

/*
 * Per-slot finite-difference and phase continuity. Lives inside the observe
 * slot, so a level reload (which memsets the runtime) correctly retires the
 * difference chain instead of differencing across a teleport.
 */
typedef struct MdkrCameraMotionSlot {
    MdkrCameraVec3 previous_eye;
    MdkrCameraVec3 previous_velocity;
    MdkrCameraVec3 previous_acceleration;
    MdkrCameraVec3 previous_forward;
    MdkrCameraVec3 blocker_normal;
    float previous_angular_velocity;
    float previous_angular_acceleration;
    uint64_t previous_tick;
    uint64_t block_onset_tick;
    uint64_t clear_onset_tick;
    uint64_t recovery_onset_tick;
    uint32_t blocker_stable_id;
    /* Consecutive continuous resolved ticks, saturating at 4 (jerk order). */
    uint8_t continuity_run;
    uint8_t phase;              /* MdkrCameraMotionPhase */
    uint8_t previous_phase;
    uint8_t recovering;
    uint8_t emergency;
    uint32_t emergency_run;
    uint8_t alternate;
    int8_t shoulder_side;
    uint8_t blocker_valid;
    uint8_t block_span_degenerate;
    uint8_t retract_pending;
    uint8_t release_held;
} MdkrCameraMotionSlot;

typedef enum MdkrCameraMotionPhase {
    MDKR_CAMERA_MOTION_PHASE_UNKNOWN = 0,
    MDKR_CAMERA_MOTION_PHASE_CLEAR,
    MDKR_CAMERA_MOTION_PHASE_BLOCKED,
} MdkrCameraMotionPhase;

typedef struct MdkrCameraObstructionObserveSlot {
    Camera authored;
    Camera last_validated_camera;
    MdkrCameraVec3 desired_eye;
    MdkrCameraVec3 effective_eye;
    MdkrCameraVec3 previous_desired_eye;
    /* Two lens channels for one viewport: `projection` is the presentation lens
     * every guard, resolver input, and continuity comparison is built from,
     * while `render_projection` is the latched record render draws. They differ
     * only where a framed view narrows the image, and only the render channel
     * participates in the authored-image generation handshake. */
    MdkrCameraProjection projection;
    MdkrCameraProjection render_projection;
    MdkrCameraProjection last_validated_projection;
    MdkrCameraProjection last_validated_render_projection;
    MdkrCameraLensGuard guard;
    MdkrCameraRoundedLensGuard exact_guard;
    MdkrCameraRoundedLensGuard published_exact_guard;
    MdkrCameraRoundedLensGuard last_validated_exact_guard;
    MdkrCameraLensGuard last_validated_guard;
    float last_validated_authored_fov;
    s32 last_validated_viewport_layout;
    uint8_t last_validated_world_region;
    MdkrCameraSweepHit stationary_hit;
    MdkrCameraSweepHit corridor_hit;
    MdkrCameraSweepHit target_visibility_hit;
    MdkrCameraSweepStatus stationary_status;
    MdkrCameraSweepStatus corridor_status;
    MdkrCameraSweepStatus resolved_stationary_status;
    MdkrCameraSweepStatus exact_shadow_status;
    MdkrCameraObstructionTwoPhaseOutcome exact_shadow_outcome;
    MdkrCameraIntent intent;
    MdkrCameraObstructionResolverState resolver;
    MdkrCameraObstructionResolverStatus resolver_status;
    MdkrCameraVec3 previous_pivot;
    uint64_t last_solve_tick;
    uint32_t solve_count;
    uint32_t intent_age_ticks;
    s16 logical_segment;
    s16 physical_slot;
    s16 viewport;
    uint8_t previous_desired_valid;
    uint8_t selected;
    uint8_t selected_previous_tick;
    uint8_t intent_fresh;
    uint8_t intent_missing_or_stale;
    uint8_t intent_usable;
    uint8_t previous_pivot_valid;
    uint8_t was_obstructed;
    uint8_t resolved_valid;
    uint8_t correction_applied;
    uint8_t alternate_active;
    uint8_t elevated_emergency;
    uint8_t orientation_retargeted;
    uint8_t presentation_discontinuity;
    uint8_t resolved_target_visible;
    uint8_t resolved_target_embedded;
    uint8_t query_source_degraded;
    uint8_t exact_guard_valid;
    uint8_t exact_shadow_invoked;
    uint8_t exact_shadow_degraded;
    uint8_t exact_runtime_invoked;
    uint8_t exact_runtime_sphere_clear;
    uint8_t exact_runtime_clear;
    uint8_t exact_runtime_hit;
    uint8_t exact_runtime_degraded;
    uint8_t exact_runtime_override;
    uint8_t published_pose_validated;
    uint8_t dynamic_source_hit;
    uint8_t exact_dynamic_source_hit;
    uint8_t transition_invoked;
    uint8_t transition_clear;
    uint8_t transition_cut;
    uint8_t transition_tuple_cut;
    uint8_t emergency_racer_opacity;
    uint8_t last_validated_camera_valid;
    uint8_t last_validated_exact_guard_valid;
    uint8_t last_validated_apply_shake;
    uint8_t last_validated_retargeted;
    uint8_t last_validated_gameplay_projection;
    uint32_t blocker_kind;
    uint32_t blocker_stable_id;
    MdkrCameraVec3 blocker_normal;
    uint8_t blocker_normal_valid;
    /* This tick was RETRACTED by the resolver's release hold, not by contact. */
    uint8_t release_held;
    /* MOTION-01 census state. Read and written only by the motion sampler. */
    MdkrCameraMotionSlot motion;
} MdkrCameraObstructionObserveSlot;

typedef struct MdkrCameraIntentRecord {
    MdkrCameraIntent intent;
    uint64_t capture_serial;
    uint64_t capture_tick;
    uint64_t consumed_serial;
    uint64_t last_consumed_tick;
    uint8_t valid;
} MdkrCameraIntentRecord;

typedef struct MdkrCameraObstructionRuntime {
    MdkrCameraObstructionObserveSlot slots[MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT];
    Camera resolved_cameras[MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT];
    uint64_t tick_serial;
    uint32_t duplicate_solve_violations;
    uint32_t projection_mismatch_violations;
    uint32_t presentation_depth_violations;
    /* Pushes refused past the depth ceiling. The matching pops consume these
     * first, so a refused scope cannot pull the depth below the scope that
     * still owns it. */
    uint32_t presentation_refused_depth;
    s16 last_physical_slot_by_viewport[4];
    uint8_t viewport_slot_valid[4];
    uint8_t presentation_depth;
} MdkrCameraObstructionRuntime;

typedef struct MdkrCameraObstructionLensQueryCacheEntry {
    MdkrCameraSweepInput input;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;
    uint8_t valid;
} MdkrCameraObstructionLensQueryCacheEntry;

typedef struct MdkrCameraObstructionLensQuery {
    const MdkrCameraObstructionCombinedQuery *sphere;
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact;
    const MdkrCameraRoundedLensGuard *guard;
    MdkrCameraObstructionObserveSlot *observe;
    MdkrCameraObstructionLensQueryCacheEntry
        cache[MDKR_CAMERA_OBSTRUCTION_LENS_QUERY_CACHE_SIZE];
    uint8_t cache_next;
} MdkrCameraObstructionLensQuery;

typedef struct MdkrCameraFinalPose {
    Camera camera;
    MdkrCameraVec3 rendered_eye;
    MdkrCameraRoundedLensGuard guard;
    MdkrCameraProjection projection;
    uint8_t apply_shake;
    uint8_t retargeted;
    uint8_t discontinuity;
    uint8_t validated;
} MdkrCameraFinalPose;

static MdkrCameraObstructionRuntime sCameraObstructionRuntime;
static MdkrCameraIntentRecord sCameraIntents[MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT];
static uint64_t sCameraIntentCaptureSerial;
static uint64_t sCameraObstructionResetSerial;

static int camera_obstruction_intent_is_current_tick(uint64_t capture_tick, uint64_t tick) {
    if (capture_tick == UINT64_MAX) {
        return tick == 1U;
    }
    return capture_tick + 1U == tick;
}

static int camera_obstruction_trace_level(void) {
    const char *value = getenv("MDKR_CAMERA_TRACE");

    if (value == NULL || value[0] == '\0' || value[0] == '0') {
        return 0;
    }
    return value[0] >= '2' ? 2 : 1;
}

static int camera_obstruction_test_projection_failure(uint64_t tick) {
    const char *value = getenv("MDKR_TEST_CAMERA_PROJECTION_FAIL_TICK");
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || value[0] == '\0') {
        return FALSE;
    }
    parsed = strtoull(value, &end, 10);
    return end != value && *end == '\0' && parsed != 0U && parsed == tick;
}

static int camera_obstruction_test_disable_alternate_shot(void) {
    const char *value = getenv("MDKR_TEST_CAMERA_DISABLE_ALTERNATE");
    return value != NULL && value[0] == '1' && value[1] == '\0';
}

/*
 * MOTION-01 measurement seam. MDKR_CAMERA_PROFILE_FORCE=car:safety_only,loop:full
 * overrides the shipped per-family profile table so a pinned route can be driven
 * under a different profile and compared. `all` sets every family at once.
 *
 * Diagnostic only, and gated exactly like the other diagnostic environment
 * seams above: read once, cached, absent means the shipped table. An unparsable
 * entry is reported and ignored rather than silently applied, because a
 * measurement run that did not get the profile it asked for is the one result a
 * reader cannot tell apart from the shipped table.
 */
static int8_t sCameraProfileForce[MDKR_CAMERA_INTENT_FAMILY_COUNT];
static int sCameraProfileForceParsed;

static int camera_obstruction_token_matches(
    const char *token, size_t length, const char *name) {
    return strlen(name) == length && strncmp(token, name, length) == 0;
}

static void camera_obstruction_parse_profile_force(void) {
    const char *cursor = getenv("MDKR_CAMERA_PROFILE_FORCE");
    size_t family;

    sCameraProfileForceParsed = TRUE;
    for (family = 0U; family < MDKR_CAMERA_INTENT_FAMILY_COUNT; family++) {
        sCameraProfileForce[family] = -1;
    }
    if (cursor == NULL || cursor[0] == '\0') {
        return;
    }
    while (*cursor != '\0') {
        const char *entry_end = strchr(cursor, ',');
        const char *colon;
        size_t entry_length;
        size_t family_length;
        size_t profile_length;
        const char *profile;
        int matched_family = -1;
        int matched_profile = -1;
        size_t index;

        if (entry_end == NULL) {
            entry_end = cursor + strlen(cursor);
        }
        entry_length = (size_t)(entry_end - cursor);
        colon = (const char *)memchr(cursor, ':', entry_length);
        if (entry_length == 0U) {
            cursor = *entry_end == '\0' ? entry_end : entry_end + 1;
            continue;
        }
        if (colon != NULL) {
            family_length = (size_t)(colon - cursor);
            profile = colon + 1;
            profile_length = (size_t)(entry_end - profile);
            for (index = 0U; index < MDKR_CAMERA_INTENT_FAMILY_COUNT; index++) {
                if (camera_obstruction_token_matches(
                        cursor, family_length, kCameraIntentFamilyNames[index])) {
                    matched_family = (int)index;
                    break;
                }
            }
            if (matched_family < 0 &&
                camera_obstruction_token_matches(cursor, family_length, "all")) {
                matched_family = (int)MDKR_CAMERA_INTENT_FAMILY_COUNT;
            }
            for (index = 0U; index < MDKR_CAMERA_OBSTRUCTION_TREATMENT_COUNT; index++) {
                if (camera_obstruction_token_matches(
                        profile, profile_length,
                        kCameraObstructionTreatmentNames[index])) {
                    matched_profile = (int)index;
                    break;
                }
            }
        }
        if (matched_family < 0 || matched_profile < 0) {
            fprintf(stderr,
                    "camera_obstruction: MDKR_CAMERA_PROFILE_FORCE entry \"%.*s\" "
                    "is not <family>:<profile>; ignoring it. Families: unknown, "
                    "car, hovercraft, plane, loop, fixed, finish_challenge, "
                    "finish_race, tt_spectate, scripted_cutscene, all. Profiles: "
                    "full, safety_only, depenetrate_only.\n",
                    (int)entry_length, cursor);
        } else if (matched_family == (int)MDKR_CAMERA_INTENT_FAMILY_COUNT) {
            for (index = 0U; index < MDKR_CAMERA_INTENT_FAMILY_COUNT; index++) {
                sCameraProfileForce[index] = (int8_t)matched_profile;
            }
        } else {
            sCameraProfileForce[matched_family] = (int8_t)matched_profile;
        }
        cursor = *entry_end == '\0' ? entry_end : entry_end + 1;
    }
}

static int sCameraObstructionPolicyFallbackReported;

static MdkrCameraObstructionRuntimePolicy camera_obstruction_runtime_policy(void) {
    const char *value = getenv("MDKR_CAMERA_OBSTRUCTION");

    /*
     * OBSERVE is the default: the authored camera is the shipped one, and
     * correction is opt-in. The launcher's Camera.Obstruction setting reaches
     * this arm by exporting this same variable, so there is one seam and one
     * spelling for the choice however a player made it.
     */
    if (value == NULL || value[0] == '\0' || strcmp(value, "observe") == 0) {
        return MDKR_CAMERA_RUNTIME_OBSERVE;
    }
    if (strcmp(value, "modern") == 0) {
        return MDKR_CAMERA_RUNTIME_MODERN;
    }
    if (strcmp(value, "center-ray") == 0) {
        return MDKR_CAMERA_RUNTIME_CENTER_RAY;
    }
    if (strcmp(value, "legacy") == 0) {
        return MDKR_CAMERA_RUNTIME_LEGACY;
    }
    /*
     * A misspelled value resolves to OBSERVE, the only arm that measures
     * without moving a camera: a typo means the caller asked for a policy and
     * did not get it, so the run may neither silently correct nor silently
     * select the known-unsafe LEGACY arm.
     *
     * Say so once. The value the caller asked for was dropped, and a fallback
     * that lands on the same behavior as an unset variable is exactly the
     * case a player cannot tell apart from their request being honoured. One
     * shot, because this resolves per slot per fixed tick.
     */
    if (!sCameraObstructionPolicyFallbackReported) {
        sCameraObstructionPolicyFallbackReported = TRUE;
        fprintf(stderr,
                "camera_obstruction: MDKR_CAMERA_OBSTRUCTION=\"%s\" is not a "
                "known policy; falling back to observe (authored camera, no "
                "correction). Valid values: observe (default), modern, "
                "center-ray, legacy.\n",
                value);
    }
    return MDKR_CAMERA_RUNTIME_OBSERVE;
}

static MdkrCameraVec3 camera_obstruction_lerp(
    MdkrCameraVec3 a,
    MdkrCameraVec3 b,
    float fraction) {
    const MdkrCameraVec3 result = {
        a.x + (b.x - a.x) * fraction,
        a.y + (b.y - a.y) * fraction,
        a.z + (b.z - a.z) * fraction,
    };
    return result;
}

static float camera_obstruction_distance(MdkrCameraVec3 a, MdkrCameraVec3 b) {
    const double x = (double)b.x - a.x;
    const double y = (double)b.y - a.y;
    const double z = (double)b.z - a.z;
    return (float)sqrt(x * x + y * y + z * z);
}

static MdkrCameraSweepStatus camera_obstruction_track_sweep_adapter(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraSweepStatus status;
    const uint64_t started = camera_obstruction_perf_begin();
    (void)context;
    status = mdkr_track_occlusion_sweep(input, out_hit);
    camera_obstruction_perf_add(MDKR_CAMERA_PERF_STATIC_QUERY, started);
    return status;
}

static MdkrCameraSweepStatus camera_obstruction_dynamic_sweep_adapter(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraObstructionObserveSlot *observe =
        (MdkrCameraObstructionObserveSlot *)context;
    const uint64_t started = camera_obstruction_perf_begin();
    MdkrCameraSweepStatus status =
        mdkr_camera_dynamic_occlusion_sweep(input, out_hit);

    camera_obstruction_perf_add(MDKR_CAMERA_PERF_DYNAMIC_QUERY, started);

    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        /* An incomplete dynamic source cannot prove a Modern pose. Preserve
         * INVALID so no later sphere-clear path can seal or publish it. */
        if (observe != NULL) {
            observe->query_source_degraded = TRUE;
        }
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (status == MDKR_CAMERA_SWEEP_HIT && observe != NULL) {
        observe->dynamic_source_hit = TRUE;
    }
    return status;
}

static MdkrCameraSweepStatus camera_obstruction_track_sweep(
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraSweepStatus status;
    const uint64_t started = camera_obstruction_perf_begin();

    status = mdkr_track_occlusion_sweep(input, out_hit);
    camera_obstruction_perf_add(MDKR_CAMERA_PERF_STATIC_QUERY, started);
    return status;
}

static MdkrCameraSweepStatus camera_obstruction_track_rounded_sweep_adapter(
    const void *context,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraSweepStatus status;
    const uint64_t started = camera_obstruction_perf_begin();

    (void)context;
    status = mdkr_track_occlusion_rounded_lens_sweep(input, out_hit);
    camera_obstruction_perf_add(MDKR_CAMERA_PERF_EXACT_STATIC_QUERY, started);
    return status;
}

static MdkrCameraSweepStatus camera_obstruction_dynamic_rounded_sweep_adapter(
    const void *context,
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraSweepStatus status;
    const uint64_t started = camera_obstruction_perf_begin();

    status = mdkr_camera_dynamic_occlusion_rounded_lens_sweep(input, out_hit);
    camera_obstruction_perf_add(MDKR_CAMERA_PERF_EXACT_DYNAMIC_QUERY, started);
    if (status == MDKR_CAMERA_SWEEP_INVALID && context != NULL) {
        ((MdkrCameraObstructionObserveSlot *)context)->query_source_degraded = TRUE;
    } else if (status == MDKR_CAMERA_SWEEP_HIT && context != NULL) {
        ((MdkrCameraObstructionObserveSlot *)context)->exact_dynamic_source_hit = TRUE;
    }
    return status;
}

static int camera_obstruction_sweep_input_equal(
    const MdkrCameraSweepInput *left,
    const MdkrCameraSweepInput *right) {
    return left->guard.kind == right->guard.kind &&
        left->guard.radius == right->guard.radius &&
        left->start_eye.x == right->start_eye.x &&
        left->start_eye.y == right->start_eye.y &&
        left->start_eye.z == right->start_eye.z &&
        left->desired_eye.x == right->desired_eye.x &&
        left->desired_eye.y == right->desired_eye.y &&
        left->desired_eye.z == right->desired_eye.z &&
        left->mask == right->mask &&
        left->ignored_object_generation == right->ignored_object_generation;
}

static MdkrCameraSweepStatus camera_obstruction_lens_sweep(
    MdkrCameraObstructionLensQuery *query,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraObstructionTwoPhaseDecision decision;
    MdkrCameraSweepStatus status;
    size_t cache_index;

    if (query == NULL || query->sphere == NULL || query->exact == NULL ||
        query->guard == NULL || query->observe == NULL || input == NULL ||
        out_hit == NULL) {
        if (out_hit != NULL) memset(out_hit, 0, sizeof(*out_hit));
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    for (cache_index = 0U;
         cache_index < MDKR_CAMERA_OBSTRUCTION_LENS_QUERY_CACHE_SIZE;
         cache_index++) {
        const MdkrCameraObstructionLensQueryCacheEntry *entry =
            &query->cache[cache_index];
        if (entry->valid &&
            camera_obstruction_sweep_input_equal(&entry->input, input)) {
            *out_hit = entry->hit;
            return entry->status;
        }
    }
    status = mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
        query->sphere, query->exact, input, query->guard, out_hit, &decision);
    query->observe->exact_runtime_invoked |= decision.exact_invoked;
    query->observe->exact_runtime_sphere_clear |=
        decision.outcome == MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_SPHERE_CLEAR;
    query->observe->exact_runtime_clear |=
        decision.outcome == MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_CLEAR;
    query->observe->exact_runtime_hit |=
        decision.outcome == MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_HIT;
    query->observe->exact_runtime_degraded |= decision.degraded;
    query->observe->exact_runtime_override |=
        decision.outcome == MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_CLEAR;
    if (status == MDKR_CAMERA_SWEEP_INVALID || decision.degraded) {
        query->observe->query_source_degraded = TRUE;
    } else {
        MdkrCameraObstructionLensQueryCacheEntry *entry =
            &query->cache[query->cache_next];
        entry->input = *input;
        entry->hit = *out_hit;
        entry->status = status;
        entry->valid = TRUE;
        query->cache_next = (uint8_t)(
            (query->cache_next + 1U) %
            MDKR_CAMERA_OBSTRUCTION_LENS_QUERY_CACHE_SIZE);
    }
    return status;
}

static MdkrCameraSweepStatus camera_obstruction_lens_sweep_adapter(
    const void *context,
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    return camera_obstruction_lens_sweep(
        (MdkrCameraObstructionLensQuery *)context, input, out_hit);
}

static s32 camera_obstruction_viewport_count(void) {
    if (gScenePlayerViewports < VIEWPORT_LAYOUT_1_PLAYER ||
        gScenePlayerViewports > VIEWPORT_LAYOUT_4_PLAYERS) {
        return 1;
    }
    return gScenePlayerViewports + 1;
}

static s32 camera_obstruction_normal_slot_for_viewport(s32 viewport, s32 viewport_count) {
    if (viewport == 0 && viewport_count == 1 && is_player_two_in_control()) {
        return PLAYER_TWO;
    }
    return viewport;
}

static int camera_obstruction_tt_viewport_selected(s32 viewport_count) {
    return viewport_count == 3 &&
           level_type() != RACETYPE_CHALLENGE_EGGS &&
           level_type() != RACETYPE_CHALLENGE_BATTLE &&
           level_type() != RACETYPE_CHALLENGE_BANANAS &&
           hud_setting() == 0;
}

static int camera_obstruction_intent_discontinuous(
    const MdkrCameraIntent *previous,
    const MdkrCameraIntent *next) {
    const float dx = next->desired_eye.x - previous->desired_eye.x;
    const float dy = next->desired_eye.y - previous->desired_eye.y;
    const float dz = next->desired_eye.z - previous->desired_eye.z;

    return previous->camera_id != next->camera_id ||
           previous->authored_mode != next->authored_mode ||
           previous->family != next->family ||
           (dx * dx) + (dy * dy) + (dz * dz) > 1000000.0f;
}

void camera_obstruction_intent_capture(const MdkrCameraIntent *intent) {
    MdkrCameraIntentRecord *record;
    MdkrCameraIntent captured;

    if (intent == NULL || intent->camera_id < 0 ||
        intent->camera_id >= MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT) {
        return;
    }
    record = &sCameraIntents[intent->camera_id];
    captured = *intent;
    if (record->valid && camera_obstruction_intent_discontinuous(&record->intent, &captured)) {
        captured.discontinuity = TRUE;
    }
    sCameraIntentCaptureSerial++;
    if (sCameraIntentCaptureSerial == 0U) {
        sCameraIntentCaptureSerial = 1U;
    }
    record->intent = captured;
    record->capture_serial = sCameraIntentCaptureSerial;
    /* Authors run during obj_update/scene_tt_camera_tick. The finalizer bumps
     * tick_serial immediately afterward, so this stamps the authored phase. */
    record->capture_tick = sCameraObstructionRuntime.tick_serial;
    record->valid = TRUE;
}

static void camera_obstruction_snapshot_authored_slots(void) {
    s32 slot;

    for (slot = 0; slot < MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT; slot++) {
        MdkrCameraObstructionObserveSlot *observe = &sCameraObstructionRuntime.slots[slot];

        observe->authored = gCameras[slot];
        sCameraObstructionRuntime.resolved_cameras[slot] = gCameras[slot];
        observe->desired_eye.x = observe->authored.trans.x_position;
        observe->desired_eye.y = observe->authored.trans.y_position;
        observe->desired_eye.z = observe->authored.trans.z_position;
        /* OBSERVE: effective remains authored until a later shake-aware gate. */
        observe->effective_eye = observe->desired_eye;
        observe->logical_segment = observe->authored.cameraSegmentID;
        observe->selected_previous_tick = observe->selected;
        observe->selected = FALSE;
        observe->solve_count = 0;
        observe->intent_fresh = FALSE;
        observe->intent_missing_or_stale = FALSE;
        observe->intent_usable = FALSE;
        observe->presentation_discontinuity = FALSE;
        observe->elevated_emergency = FALSE;
        observe->resolved_stationary_status = MDKR_CAMERA_SWEEP_INVALID;
        observe->exact_shadow_status = MDKR_CAMERA_SWEEP_INVALID;
        observe->exact_shadow_outcome = MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_INVALID;
        observe->resolved_target_visible = FALSE;
        observe->resolved_target_embedded = FALSE;
        observe->query_source_degraded = FALSE;
        observe->exact_guard_valid = FALSE;
        observe->exact_shadow_invoked = FALSE;
        observe->exact_shadow_degraded = FALSE;
        observe->exact_runtime_invoked = FALSE;
        observe->exact_runtime_sphere_clear = FALSE;
        observe->exact_runtime_clear = FALSE;
        observe->exact_runtime_hit = FALSE;
        observe->exact_runtime_degraded = FALSE;
        observe->exact_runtime_override = FALSE;
        observe->published_pose_validated = FALSE;
        observe->dynamic_source_hit = FALSE;
        observe->exact_dynamic_source_hit = FALSE;
        observe->transition_invoked = FALSE;
        observe->transition_clear = FALSE;
        observe->transition_cut = FALSE;
        observe->transition_tuple_cut = FALSE;
        observe->emergency_racer_opacity = 255U;
        observe->intent_age_ticks = 0;
    }
}

static void camera_obstruction_trace_detail(
    const MdkrCameraObstructionObserveSlot *observe,
    s32 normal_slot,
    int cutscene_bank) {
    const MdkrCameraSweepHit *hit = &observe->stationary_hit;
    const MdkrCameraSweepHit *corridor = &observe->corridor_hit;

    fprintf(stderr,
            "camera_obstruction_observe detail tick=%llu viewport=%d normal_slot=%d "
            "physical_slot=%d bank=%s desired=(%.3f,%.3f,%.3f) effective=(%.3f,%.3f,%.3f) "
            "segment=%d projection={gen=%llu aspect=%.6f vfov=%.3f near=%.3f} guard=%.3f "
            "intent={fresh=%u stale=%u usable=%u age=%u family=%d mode=%d pivot=(%.3f,%.3f,%.3f) "
            "target_valid=%u target=(%.3f,%.3f,%.3f) forward_valid=%u discontinuity=%u} "
            "resolve={status=%d valid=%u corrected=%u alternate=%u emergency=%u retargeted=%u "
            "discontinuity=%u resolved=(%.3f,%.3f,%.3f) clearance=%d "
            "target_visible=%u target_embedded=%u target_blocker=%u/%u target_overlap=%u "
            "source_degraded=%u racer_opacity=%u blocker=%u/%u} "
            "stationary={status=%d hit=%u overlap=%u blocker=%u/%u} "
            "corridor={status=%d hit=%u overlap=%u blocker=%u/%u} "
            "dynamic_source={sphere_hit=%u exact_hit=%u} "
            "transition={invoked=%u clear=%u cut=%u tuple_cut=%u} "
            "exact_runtime={invoked=%u sphere_clear=%u exact_clear=%u exact_hit=%u "
            "override=%u degraded=%u} "
            "exact_shadow={guard=%u invoked=%u status=%d outcome=%d degraded=%u}\n",
            (unsigned long long)sCameraObstructionRuntime.tick_serial,
            observe->viewport, normal_slot, observe->physical_slot,
            cutscene_bank ? "cutscene" : "normal",
            observe->desired_eye.x, observe->desired_eye.y, observe->desired_eye.z,
            observe->effective_eye.x, observe->effective_eye.y, observe->effective_eye.z,
            observe->logical_segment,
            (unsigned long long)observe->projection.generation,
            observe->projection.aspect, observe->projection.vertical_fov,
            observe->projection.near_plane, observe->guard.radius,
            observe->intent_fresh, observe->intent_missing_or_stale, observe->intent_usable,
            observe->intent_age_ticks,
            observe->intent.family, observe->intent.authored_mode,
            observe->intent.pivot.x, observe->intent.pivot.y, observe->intent.pivot.z,
            observe->intent.target_valid,
            observe->intent.target.x, observe->intent.target.y, observe->intent.target.z,
            observe->intent.forward_valid,
            observe->intent.discontinuity,
            observe->resolver_status, observe->resolved_valid, observe->correction_applied,
            observe->alternate_active, observe->elevated_emergency,
            observe->orientation_retargeted,
            observe->presentation_discontinuity,
            observe->effective_eye.x, observe->effective_eye.y, observe->effective_eye.z,
            observe->resolved_stationary_status,
            observe->resolved_target_visible,
            observe->resolved_target_embedded,
            observe->target_visibility_hit.kind,
            observe->target_visibility_hit.stable_id,
            observe->target_visibility_hit.started_overlapping,
            observe->query_source_degraded,
            observe->emergency_racer_opacity,
            observe->blocker_kind, observe->blocker_stable_id,
            observe->stationary_status,
            observe->stationary_status == MDKR_CAMERA_SWEEP_HIT,
            hit->started_overlapping, hit->kind, hit->stable_id,
            observe->corridor_status,
            observe->corridor_status == MDKR_CAMERA_SWEEP_HIT,
            corridor->started_overlapping, corridor->kind, corridor->stable_id,
            observe->dynamic_source_hit, observe->exact_dynamic_source_hit,
            observe->transition_invoked, observe->transition_clear,
            observe->transition_cut, observe->transition_tuple_cut,
            observe->exact_runtime_invoked, observe->exact_runtime_sphere_clear,
            observe->exact_runtime_clear, observe->exact_runtime_hit,
            observe->exact_runtime_override, observe->exact_runtime_degraded,
            observe->exact_guard_valid, observe->exact_shadow_invoked,
            observe->exact_shadow_status, observe->exact_shadow_outcome,
            observe->exact_shadow_degraded);
}

static int camera_obstruction_build_exact_guard_for_camera(
    const MdkrCameraObstructionObserveSlot *observe,
    const Camera *camera,
    MdkrCameraRoundedLensGuard *out_guard,
    MdkrCameraVec3 *out_eye) {
    MdkrCameraLensPose pose;
    MdkrCameraVec3 forward;
    MdkrCameraVec3 up;

    if (observe == NULL || camera == NULL || out_guard == NULL ||
        !cam_lens_pose_from_camera_snapshot(camera, gNoCamShake, &pose)) {
        return FALSE;
    }
    forward.x = pose.forward.x;
    forward.y = pose.forward.y;
    forward.z = pose.forward.z;
    up.x = pose.up.x;
    up.y = pose.up.y;
    up.z = pose.up.z;
    if (!mdkr_camera_rounded_lens_guard_from_projection(
        observe->projection.near_plane,
        observe->projection.vertical_fov * MDKR_CAMERA_OBSTRUCTION_DEG_TO_RAD,
        observe->projection.aspect, 0.0f, forward, up, out_guard)) {
        return FALSE;
    }
    if (out_eye != NULL) {
        out_eye->x = pose.eye.x;
        out_eye->y = pose.eye.y;
        out_eye->z = pose.eye.z;
    }
    return TRUE;
}

static int camera_obstruction_promote_sphere_guard_for_exact(
    MdkrCameraLensGuard *sphere_guard,
    const MdkrCameraRoundedLensGuard *exact_guard) {
    double radius;
    float outward_radius;

    if (sphere_guard == NULL || exact_guard == NULL ||
        sphere_guard->kind != MDKR_CAMERA_LENS_GUARD_SPHERE ||
        !mdkr_camera_rounded_lens_guard_conservative_radius(
            exact_guard, &radius) ||
        !isfinite(radius) || radius > FLT_MAX) {
        return FALSE;
    }
    outward_radius = (float)radius;
    if ((double)outward_radius < radius) {
        outward_radius = nextafterf(outward_radius, INFINITY);
    }
    if (!isfinite(outward_radius)) {
        return FALSE;
    }
    if (sphere_guard->radius < outward_radius) {
        sphere_guard->radius = outward_radius;
    }
    return TRUE;
}

static void camera_obstruction_shadow_exact_corridor(
    MdkrCameraObstructionObserveSlot *observe,
    const MdkrCameraSweepInput *sphere_input) {
    const MdkrCameraObstructionQuerySource sphere_sources[] = {
        { camera_obstruction_track_sweep_adapter, NULL },
        { camera_obstruction_dynamic_sweep_adapter, observe },
    };
    const MdkrCameraObstructionRoundedLensQuerySource sources[] = {
        { camera_obstruction_track_rounded_sweep_adapter, NULL },
        { camera_obstruction_dynamic_rounded_sweep_adapter, observe },
    };
    const MdkrCameraObstructionCombinedQuery sphere_query = {
        sphere_sources, ARRAY_COUNT(sphere_sources), ARRAY_COUNT(sphere_sources),
    };
    const MdkrCameraObstructionRoundedLensCombinedQuery query = {
        sources, ARRAY_COUNT(sources), ARRAY_COUNT(sources),
    };
    MdkrCameraObstructionTwoPhaseDecision decision;
    MdkrCameraSweepHit shadow_hit;

    if (observe == NULL || sphere_input == NULL ||
        !camera_obstruction_exact_shadow_enabled()) {
        return;
    }
    if (!observe->exact_guard_valid) {
        observe->exact_shadow_degraded = TRUE;
        observe->exact_shadow_outcome =
            MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_CONSERVATIVE_FALLBACK;
        return;
    }
    (void)mdkr_camera_obstruction_two_phase_rounded_lens_sweep(
        &sphere_query, &query, sphere_input, &observe->exact_guard,
        &shadow_hit, &decision);
    observe->exact_shadow_invoked = decision.exact_invoked;
    observe->exact_shadow_status = decision.exact_status;
    observe->exact_shadow_outcome = decision.outcome;
    if (decision.degraded || observe->query_source_degraded ||
        decision.outcome == MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_INVALID) {
        observe->exact_shadow_degraded = TRUE;
        observe->exact_shadow_outcome =
            MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_CONSERVATIVE_FALLBACK;
    }
}

static void camera_obstruction_apply_intent(
    MdkrCameraObstructionObserveSlot *observe,
    s32 physical_slot) {
    MdkrCameraIntentRecord *record = &sCameraIntents[physical_slot];

    memset(&observe->intent, 0, sizeof(observe->intent));
    if (!record->valid) {
        observe->intent_missing_or_stale = TRUE;
        return;
    }
    observe->intent = record->intent;
    if (record->capture_serial != record->consumed_serial &&
        camera_obstruction_intent_is_current_tick(
            record->capture_tick, sCameraObstructionRuntime.tick_serial) &&
        record->intent.desired_eye.x == observe->desired_eye.x &&
        record->intent.desired_eye.y == observe->desired_eye.y &&
        record->intent.desired_eye.z == observe->desired_eye.z) {
        observe->intent_fresh = TRUE;
        observe->intent_usable = TRUE;
        observe->intent_age_ticks = 0;
        /* A same-tick author intent is telemetry's desired eye. This does not
         * publish a pose or write Camera/gCameras. */
        observe->desired_eye = record->intent.desired_eye;
        observe->effective_eye = observe->desired_eye;
        record->consumed_serial = record->capture_serial;
        record->last_consumed_tick = sCameraObstructionRuntime.tick_serial;
    } else {
        observe->intent_missing_or_stale = TRUE;
        /* Paused/zero-author ticks may safely reuse the intent that this same
         * selected slot consumed previously, but an inactive-bank capture can
         * never become usable merely because the bank is selected later. */
        if (is_game_paused() && observe->selected_previous_tick &&
            record->consumed_serial == record->capture_serial &&
            record->intent.desired_eye.x == observe->desired_eye.x &&
            record->intent.desired_eye.y == observe->desired_eye.y &&
            record->intent.desired_eye.z == observe->desired_eye.z) {
            observe->intent_usable = TRUE;
        }
        if (sCameraObstructionRuntime.tick_serial >= record->last_consumed_tick) {
            observe->intent_age_ticks = (uint32_t)(
                sCameraObstructionRuntime.tick_serial - record->last_consumed_tick);
        }
    }
}

static MdkrCameraSweepStatus camera_obstruction_stationary_query(
    const MdkrCameraObstructionCombinedQuery *query,
    MdkrCameraLensGuard guard,
    MdkrCameraVec3 eye,
    MdkrCameraSweepHit *out_hit) {
    const MdkrCameraSweepInput input = {
        .guard = guard,
        .start_eye = eye,
        .desired_eye = eye,
        .mask = MDKR_CAMERA_OBSTRUCTION_HARD_MASK |
            MDKR_CAMERA_DYNAMIC_OCCLUSION_QUERY_CURRENT_POSE,
    };
    return mdkr_camera_obstruction_combined_sweep(query, &input, out_hit);
}

static MdkrCameraSweepStatus camera_obstruction_lens_stationary_query(
    MdkrCameraObstructionLensQuery *query,
    MdkrCameraLensGuard sphere_guard,
    MdkrCameraVec3 eye,
    MdkrCameraSweepHit *out_hit) {
    const MdkrCameraSweepInput input = {
        .guard = sphere_guard,
        .start_eye = eye,
        .desired_eye = eye,
        .mask = MDKR_CAMERA_OBSTRUCTION_HARD_MASK,
    };
    return camera_obstruction_lens_sweep(query, &input, out_hit);
}

static MdkrCameraSweepStatus camera_obstruction_query_stationary(
    const MdkrCameraObstructionQuery *query,
    MdkrCameraLensGuard guard,
    MdkrCameraVec3 eye,
    MdkrCameraSweepHit *out_hit) {
    const MdkrCameraSweepInput input = {
        .guard = guard,
        .start_eye = eye,
        .desired_eye = eye,
        .mask = MDKR_CAMERA_OBSTRUCTION_HARD_MASK,
    };
    if (query == NULL || query->sweep == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    return query->sweep(query->context, &input, out_hit);
}

/*
 * A focus point may legally touch scenery, so it is not automatically a valid
 * lens origin. Walk outward along the authored boom in a fixed, bounded order
 * and choose its first non-overlapping eye. The remaining corridor is still
 * swept in full; this only establishes an honest start for the sphere.
 */
static MdkrCameraSweepStatus camera_obstruction_find_boom_anchor(
    const MdkrCameraObstructionCombinedQuery *query,
    MdkrCameraLensGuard guard,
    MdkrCameraVec3 pivot,
    MdkrCameraVec3 desired,
    MdkrCameraVec3 *out_anchor) {
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;
    int step;

    status = camera_obstruction_stationary_query(query, guard, pivot, &hit);
    if (status == MDKR_CAMERA_SWEEP_CLEAR) {
        *out_anchor = pivot;
        return status;
    }
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        return status;
    }

    /* The usual racer pivot intersects the ground-sized lens while the desired
     * eye is clear. Bracket those two facts and retain a known-clear high
     * endpoint at every refinement; the full anchor-to-eye corridor is still
     * swept by the caller, so non-monotonic geometry cannot be skipped. */
    status = camera_obstruction_stationary_query(query, guard, desired, &hit);
    if (status == MDKR_CAMERA_SWEEP_CLEAR) {
        MdkrCameraVec3 overlapping = pivot;
        MdkrCameraVec3 clear = desired;

        for (step = 0; step < MDKR_CAMERA_OBSTRUCTION_ANCHOR_REFINE_STEPS; step++) {
            const MdkrCameraVec3 candidate =
                camera_obstruction_lerp(overlapping, clear, 0.5f);
            status = camera_obstruction_stationary_query(query, guard, candidate, &hit);
            if (status == MDKR_CAMERA_SWEEP_INVALID) {
                return status;
            }
            if (status == MDKR_CAMERA_SWEEP_CLEAR) {
                clear = candidate;
            } else {
                overlapping = candidate;
            }
        }
        *out_anchor = clear;
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        return status;
    }

    /* Both endpoints overlap. Preserve the exhaustive deterministic search
     * for a clear pocket; the desired endpoint was already tested above. */
    for (step = 1; step < MDKR_CAMERA_OBSTRUCTION_ANCHOR_STEPS; step++) {
        const float fraction = (float)step / (float)MDKR_CAMERA_OBSTRUCTION_ANCHOR_STEPS;
        const MdkrCameraVec3 candidate = camera_obstruction_lerp(pivot, desired, fraction);
        status = camera_obstruction_stationary_query(query, guard, candidate, &hit);
        if (status == MDKR_CAMERA_SWEEP_CLEAR) {
            *out_anchor = candidate;
            return status;
        }
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            return status;
        }
    }
    return MDKR_CAMERA_SWEEP_HIT;
}

static MdkrCameraSweepStatus camera_obstruction_refine_boom_anchor_lens(
    MdkrCameraObstructionLensQuery *query,
    MdkrCameraLensGuard sphere_guard,
    MdkrCameraVec3 pivot,
    MdkrCameraVec3 desired,
    MdkrCameraVec3 conservative_anchor,
    int conservative_anchor_clear,
    MdkrCameraVec3 *out_anchor) {
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;
    MdkrCameraVec3 overlapping;
    MdkrCameraVec3 clear;
    MdkrCameraVec3 candidate;
    int step;

    if (query == NULL || out_anchor == NULL) {
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    status = camera_obstruction_lens_stationary_query(
        query, sphere_guard, pivot, &hit);
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        return status;
    }
    if (status == MDKR_CAMERA_SWEEP_CLEAR) {
        *out_anchor = pivot;
        return status;
    }
    overlapping = pivot;
    status = camera_obstruction_lens_stationary_query(
        query, sphere_guard, desired, &hit);
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        return status;
    }
    /* Prefer the authored endpoint as the high side, matching the shot's
     * connected boom. If it overlaps, the outward-promoted sphere anchor is
     * still a proven exact-clear fallback high side. */
    if (status == MDKR_CAMERA_SWEEP_CLEAR) {
        clear = desired;
    } else if (conservative_anchor_clear) {
        clear = conservative_anchor;
    } else {
        /* Both endpoints overlap their respective probes. Preserve the
         * exhaustive stable search for an interior exact-clear pocket; this
         * is uncommon but avoids escalating wide-lens corner cases directly
         * to an elevated emergency shot. */
        for (step = 1; step < MDKR_CAMERA_OBSTRUCTION_ANCHOR_STEPS; step++) {
            const float fraction =
                (float)step / MDKR_CAMERA_OBSTRUCTION_ANCHOR_STEPS;
            candidate = camera_obstruction_lerp(pivot, desired, fraction);
            status = camera_obstruction_lens_stationary_query(
                query, sphere_guard, candidate, &hit);
            if (status == MDKR_CAMERA_SWEEP_INVALID) {
                return status;
            }
            if (status == MDKR_CAMERA_SWEEP_CLEAR) {
                *out_anchor = candidate;
                return status;
            }
        }
        return MDKR_CAMERA_SWEEP_HIT;
    }
    for (step = 0; step < MDKR_CAMERA_OBSTRUCTION_EXACT_ANCHOR_REFINE_STEPS; step++) {
        candidate = camera_obstruction_lerp(overlapping, clear, 0.5f);
        status = camera_obstruction_lens_stationary_query(
            query, sphere_guard, candidate, &hit);
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            return status;
        }
        if (status == MDKR_CAMERA_SWEEP_CLEAR) {
            clear = candidate;
        } else {
            overlapping = candidate;
        }
    }
    *out_anchor = clear;
    return MDKR_CAMERA_SWEEP_CLEAR;
}

static int camera_obstruction_build_final_pose(
    const MdkrCameraObstructionObserveSlot *observe,
    MdkrCameraVec3 resolved_effective_eye,
    int retarget_orientation,
    MdkrCameraFinalPose *out_pose) {
    MdkrCameraFinalPose pose;
    MdkrCameraVec3 derived_eye;
    float shake;

    if (observe == NULL || out_pose == NULL ||
        !isfinite(resolved_effective_eye.x) ||
        !isfinite(resolved_effective_eye.y) ||
        !isfinite(resolved_effective_eye.z)) {
        return FALSE;
    }
    shake = gNoCamShake ? observe->authored.shakeMagnitude : 0.0f;
    memset(&pose, 0, sizeof(pose));
    pose.camera = observe->authored;
    pose.camera.trans.x_position = resolved_effective_eye.x;
    pose.camera.trans.y_position = resolved_effective_eye.y - shake;
    pose.camera.trans.z_position = resolved_effective_eye.z;
    pose.camera.cameraSegmentID = get_level_segment_index_from_position(
        resolved_effective_eye.x, resolved_effective_eye.y, resolved_effective_eye.z);
    pose.retargeted = retarget_orientation && observe->intent.target_valid;
    if (pose.retargeted) {
        const float diff_x = resolved_effective_eye.x - observe->intent.target.x;
        const float diff_y = resolved_effective_eye.y - observe->intent.target.y;
        const float diff_z = resolved_effective_eye.z - observe->intent.target.z;
        const float horizontal = sqrtf(diff_x * diff_x + diff_z * diff_z);
        pose.camera.trans.rotation.y_rotation =
            0x8000 - atan2s((s32)diff_x, (s32)diff_z);
        pose.camera.trans.rotation.x_rotation =
            atan2s((s32)diff_y, (s32)horizontal) - pose.camera.pitch;
    }
    if (!camera_obstruction_build_exact_guard_for_camera(
            observe, &pose.camera, &pose.guard, &derived_eye) ||
        camera_obstruction_distance(derived_eye, resolved_effective_eye) > 1.0e-3f) {
        return FALSE;
    }
    pose.rendered_eye = derived_eye;
    pose.projection = observe->projection;
    pose.apply_shake = gNoCamShake != 0;
    pose.discontinuity = pose.retargeted;
    *out_pose = pose;
    return TRUE;
}

static void camera_obstruction_publish_final_pose(
    MdkrCameraObstructionObserveSlot *observe,
    s32 physical_slot,
    const MdkrCameraFinalPose *pose) {
    if (observe == NULL || pose == NULL || !pose->validated ||
        observe->query_source_degraded ||
        physical_slot < 0 ||
        physical_slot >= MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT) {
        if (observe != NULL) {
            observe->resolved_valid = FALSE;
            observe->query_source_degraded = TRUE;
        }
        return;
    }
    sCameraObstructionRuntime.resolved_cameras[physical_slot] = pose->camera;
    observe->published_exact_guard = pose->guard;
    observe->published_pose_validated = TRUE;
    observe->orientation_retargeted = pose->retargeted;
    observe->effective_eye = pose->rendered_eye;
    observe->resolved_valid = TRUE;
    observe->correction_applied =
        pose->camera.trans.x_position != observe->authored.trans.x_position ||
        pose->camera.trans.y_position != observe->authored.trans.y_position ||
        pose->camera.trans.z_position != observe->authored.trans.z_position;
    if (pose->discontinuity) observe->presentation_discontinuity = TRUE;
}

static int camera_obstruction_exact_basis_equal(
    const MdkrCameraRoundedLensGuard *left,
    const MdkrCameraRoundedLensGuard *right) {
    return left->forward.x == right->forward.x &&
        left->forward.y == right->forward.y &&
        left->forward.z == right->forward.z &&
        left->right.x == right->right.x &&
        left->right.y == right->right.y &&
        left->right.z == right->right.z &&
        left->up.x == right->up.x &&
        left->up.y == right->up.y &&
        left->up.z == right->up.z;
}

static int camera_obstruction_exact_guard_equal(
    const MdkrCameraRoundedLensGuard *left,
    const MdkrCameraRoundedLensGuard *right) {
    return camera_obstruction_exact_basis_equal(left, right) &&
        left->near_distance == right->near_distance &&
        left->half_width == right->half_width &&
        left->half_height == right->half_height &&
        left->skin == right->skin &&
        left->broadphase_radius == right->broadphase_radius;
}

static int camera_obstruction_projection_geometry_equal(
    const MdkrCameraProjection *left,
    const MdkrCameraProjection *right) {
    return left->aspect == right->aspect &&
        left->vertical_fov == right->vertical_fov &&
        left->horizontal_fov == right->horizontal_fov &&
        left->near_plane == right->near_plane &&
        left->far_plane == right->far_plane &&
        left->camera_id == right->camera_id &&
        left->camera_bank == right->camera_bank;
}

static void camera_obstruction_validate_presentation_transition(
    MdkrCameraObstructionObserveSlot *observe,
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    MdkrCameraFinalPose *pose) {
    MdkrCameraLensPose previous_lens_pose;
    MdkrCameraSweepInput transition;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;
    double previous_radius;
    double current_radius;
    double radius;
    float sphere_radius;
    const uint8_t degraded_before = observe->query_source_degraded;

    if (!observe->last_validated_camera_valid ||
        !observe->last_validated_exact_guard_valid ||
        observe->intent.discontinuity ||
        !camera_obstruction_projection_geometry_equal(
            &observe->last_validated_projection, &pose->projection) ||
        observe->last_validated_apply_shake != pose->apply_shake ||
        observe->last_validated_camera.shakeMagnitude !=
            pose->camera.shakeMagnitude ||
        !cam_lens_pose_from_camera_snapshot(
            &observe->last_validated_camera,
            observe->last_validated_apply_shake,
            &previous_lens_pose)) {
        pose->discontinuity = TRUE;
        observe->transition_cut = TRUE;
        observe->transition_tuple_cut = TRUE;
        return;
    }
    transition = (MdkrCameraSweepInput) {
        .guard = observe->guard,
        .start_eye = {
            previous_lens_pose.eye.x,
            previous_lens_pose.eye.y,
            previous_lens_pose.eye.z,
        },
        .desired_eye = pose->rendered_eye,
        .mask = MDKR_CAMERA_OBSTRUCTION_HARD_MASK,
    };
    /* Non-door hard solids publish their current fixed pose and explicitly
     * disable presentation interpolation. If the camera chord overlaps one
     * of their broadphases, cut the camera too. Querying a geometric sweep
     * against only the current object pose would not prove the interpolated
     * presentation interval and can also exhaust bounded model work for no
     * useful safety result. */
    if (mdkr_camera_dynamic_occlusion_discontinuous_sweep_candidate(
            &transition)) {
        pose->discontinuity = TRUE;
        observe->transition_cut = TRUE;
        return;
    }
    if (camera_obstruction_exact_basis_equal(
            &observe->last_validated_exact_guard, &pose->guard)) {
        MdkrCameraObstructionLensQuery lens_query = {
            sphere_query, exact_query, &pose->guard, observe,
        };
        status = camera_obstruction_lens_sweep(&lens_query, &transition, &hit);
    } else if (mdkr_camera_rounded_lens_guard_conservative_radius(
                   &observe->last_validated_exact_guard, &previous_radius) &&
               mdkr_camera_rounded_lens_guard_conservative_radius(
                   &pose->guard, &current_radius)) {
        radius = previous_radius > current_radius ? previous_radius : current_radius;
        if (!isfinite(radius) || radius > FLT_MAX) {
            observe->query_source_degraded = TRUE;
            pose->discontinuity = TRUE;
            observe->transition_cut = TRUE;
            return;
        }
        sphere_radius = (float)radius;
        if ((double)sphere_radius < radius) {
            sphere_radius = nextafterf(sphere_radius, INFINITY);
        }
        transition.guard.radius = sphere_radius;
        status = mdkr_camera_obstruction_combined_sweep(
            sphere_query, &transition, &hit);
    } else {
        observe->query_source_degraded = TRUE;
        pose->discontinuity = TRUE;
        observe->transition_cut = TRUE;
        return;
    }
    observe->transition_invoked = TRUE;
    if (status != MDKR_CAMERA_SWEEP_CLEAR ||
        observe->query_source_degraded != degraded_before) {
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            observe->query_source_degraded = TRUE;
        }
        pose->discontinuity = TRUE;
        observe->transition_cut = TRUE;
    } else {
        observe->transition_clear = TRUE;
    }
}

static int camera_obstruction_final_pose_stationary_clear(
    MdkrCameraObstructionObserveSlot *observe,
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    MdkrCameraFinalPose *pose,
    int use_exact,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraObstructionLensQuery lens_query = { 0 };

    if (observe == NULL || sphere_query == NULL ||
        pose == NULL || out_hit == NULL) {
        return FALSE;
    }
    pose->validated = FALSE;
    if (!use_exact) {
        pose->validated = camera_obstruction_stationary_query(
                   sphere_query, observe->guard, pose->rendered_eye, out_hit) ==
               MDKR_CAMERA_SWEEP_CLEAR;
        return pose->validated;
    }
    if (exact_query == NULL) {
        observe->query_source_degraded = TRUE;
        return FALSE;
    }
    lens_query.sphere = sphere_query;
    lens_query.exact = exact_query;
    lens_query.guard = &pose->guard;
    lens_query.observe = observe;
        pose->validated = camera_obstruction_lens_stationary_query(
               &lens_query, observe->guard, pose->rendered_eye, out_hit) ==
           MDKR_CAMERA_SWEEP_CLEAR && !observe->query_source_degraded;
    if (pose->validated) {
        camera_obstruction_validate_presentation_transition(
            observe, sphere_query, exact_query, pose);
    }
    return pose->validated;
}

static int camera_obstruction_publish_resolved(
    MdkrCameraObstructionObserveSlot *observe,
    s32 physical_slot,
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    int use_exact,
    MdkrCameraVec3 resolved_effective_eye,
    int retarget_orientation) {
    MdkrCameraFinalPose pose;
    MdkrCameraSweepHit hit;
    if (!camera_obstruction_build_final_pose(
            observe, resolved_effective_eye, retarget_orientation, &pose) ||
        !camera_obstruction_final_pose_stationary_clear(
            observe, sphere_query, exact_query, &pose, use_exact, &hit)) {
        observe->resolved_valid = FALSE;
        observe->query_source_degraded = TRUE;
        return FALSE;
    }
    camera_obstruction_publish_final_pose(observe, physical_slot, &pose);
    return observe->resolved_valid;
}

static int camera_obstruction_depenetrate_only_eye(
    MdkrCameraObstructionObserveSlot *observe,
    s32 physical_slot,
    const MdkrCameraObstructionQuery *query,
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    int use_exact,
    MdkrCameraVec3 desired) {
    MdkrCameraVec3 candidate = desired;
    MdkrCameraSweepHit hit;
    int iteration;

    if (observe->resolver.last_safe_valid && !observe->intent.discontinuity) {
        MdkrCameraSweepInput path = {
            .guard = observe->guard,
            .start_eye = observe->resolver.last_safe_eye,
            .desired_eye = desired,
            .mask = MDKR_CAMERA_OBSTRUCTION_HARD_MASK,
        };
        const MdkrCameraSweepStatus path_status =
            query->sweep(query->context, &path, &hit);

        if (path_status == MDKR_CAMERA_SWEEP_INVALID) {
            observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID;
            return FALSE;
        }
        if (path_status == MDKR_CAMERA_SWEEP_HIT) {
            const MdkrCameraSweepHit path_hit = hit;
            const MdkrCameraSweepStatus endpoint_status =
                camera_obstruction_query_stationary(
                    query, observe->guard, desired, &hit);

            observe->blocker_kind = path_hit.kind;
            observe->blocker_stable_id = path_hit.stable_id;
            observe->blocker_normal = path_hit.normal;
            observe->blocker_normal_valid = TRUE;
            if (endpoint_status == MDKR_CAMERA_SWEEP_INVALID) {
                observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID;
                return FALSE;
            }
            if (endpoint_status == MDKR_CAMERA_SWEEP_CLEAR) {
                /* Both endpoints are safe but interpolation between them is
                 * not. Publish the authored endpoint as an explicit cut;
                 * holding at the contact would strand a depenetrate-only
                 * camera on the wrong side of a wall indefinitely. */
                if (!camera_obstruction_publish_resolved(
                        observe, physical_slot, sphere_query, exact_query,
                        use_exact, desired, FALSE)) {
                    return FALSE;
                }
                observe->resolver.last_safe_eye = desired;
                observe->resolver.last_safe_valid = TRUE;
                observe->resolver.projection_generation = observe->projection.generation;
                observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_CLEAR;
                observe->was_obstructed = FALSE;
                observe->presentation_discontinuity = TRUE;
                return TRUE;
            }
            /* The endpoint itself overlaps. Fall through to iterative
             * depenetration around the authored destination, then mark that
             * corrected endpoint discontinuous below. */
        }
    }

    for (iteration = 0; iteration < 8; iteration++) {
        const MdkrCameraSweepStatus status = camera_obstruction_query_stationary(
            query, observe->guard, candidate, &hit);
        if (status == MDKR_CAMERA_SWEEP_CLEAR) {
            if (!camera_obstruction_publish_resolved(
                    observe, physical_slot, sphere_query, exact_query,
                    use_exact, candidate, FALSE)) {
                return FALSE;
            }
            observe->resolver.last_safe_eye = candidate;
            observe->resolver.last_safe_valid = TRUE;
            observe->resolver.projection_generation = observe->projection.generation;
            observe->resolver_status = observe->correction_applied ?
                MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED :
                MDKR_CAMERA_OBSTRUCTION_RESOLVER_CLEAR;
            if (observe->correction_applied && !observe->was_obstructed) {
                observe->presentation_discontinuity = TRUE;
            }
            observe->was_obstructed = observe->correction_applied;
            return TRUE;
        }
        if (status == MDKR_CAMERA_SWEEP_INVALID || !isfinite(hit.penetration_depth)) {
            observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID;
            return FALSE;
        }
        /* The kernel's overlap normal is an outward depenetration direction.
         * Iteration handles corners where leaving one plane enters another. */
        candidate.x += hit.normal.x * (hit.penetration_depth + 1.0f);
        candidate.y += hit.normal.y * (hit.penetration_depth + 1.0f);
        candidate.z += hit.normal.z * (hit.penetration_depth + 1.0f);
    }
    observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_FAILSAFE;
    return FALSE;
}

static int camera_obstruction_target_visible(
    const MdkrCameraObstructionCombinedQuery *query,
    MdkrCameraVec3 target,
    MdkrCameraVec3 eye) {
    const MdkrCameraTargetVisibilityStatus status =
        mdkr_camera_target_visibility_query(
            query, target, eye, MDKR_CAMERA_OBSTRUCTION_HARD_MASK, NULL);

    /* Solver candidates cannot satisfy a line-of-sight constraint whose focus
     * point remains embedded. Camera safety stays enforceable; final telemetry
     * still reports the target as not visible and distinctly embedded. */
    return status == MDKR_CAMERA_TARGET_VISIBILITY_VISIBLE ||
        status == MDKR_CAMERA_TARGET_VISIBILITY_EMBEDDED;
}

static int camera_obstruction_try_emergency_candidate(
    MdkrCameraObstructionObserveSlot *observe,
    s32 physical_slot,
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    int use_exact,
    MdkrCameraVec3 candidate) {
    MdkrCameraFinalPose pose;
    MdkrCameraSweepHit hit;

    if (!camera_obstruction_build_final_pose(observe, candidate, TRUE, &pose) ||
        !camera_obstruction_final_pose_stationary_clear(
            observe, sphere_query, exact_query, &pose, use_exact, &hit) ||
        !camera_obstruction_target_visible(
            sphere_query, observe->intent.target, pose.rendered_eye)) {
        return FALSE;
    }
    pose.discontinuity = TRUE;
    camera_obstruction_publish_final_pose(observe, physical_slot, &pose);
    observe->resolver.last_safe_eye = pose.rendered_eye;
    observe->resolver.last_safe_valid = TRUE;
    observe->resolver.projection_generation = observe->projection.generation;
    observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED;
    observe->alternate_active = TRUE;
    observe->elevated_emergency = TRUE;
    observe->was_obstructed = TRUE;
    observe->previous_pivot = observe->intent.pivot;
    observe->previous_pivot_valid = TRUE;
    /* The authored-to-emergency chord may cross the shell that made the
     * ordinary boom impossible. Publish only a safe endpoint. */
    observe->presentation_discontinuity = TRUE;
    return TRUE;
}

static int camera_obstruction_publish_elevated_emergency(
    MdkrCameraObstructionObserveSlot *observe,
    s32 physical_slot,
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    int use_exact,
    MdkrCameraVec3 desired) {
    static const float lift_factors[] = {
        1.25f, 2.0f, 3.0f, 4.5f, 6.0f, 10.0f, 20.0f,
    };
    static const float ring_directions[][2] = {
        { 1.0f, 0.0f }, { 0.70710678f, 0.70710678f },
        { 0.0f, 1.0f }, { -0.70710678f, 0.70710678f },
        { -1.0f, 0.0f }, { -0.70710678f, -0.70710678f },
        { 0.0f, -1.0f }, { 0.70710678f, -0.70710678f },
    };
    const MdkrCameraVec3 bases[] = { desired, observe->intent.pivot };
    size_t factor_index;
    size_t base_index;
    size_t direction_index;

    if (!observe->intent.target_valid || !isfinite(observe->guard.radius)) {
        return FALSE;
    }
    for (factor_index = 0U;
         factor_index < sizeof(lift_factors) / sizeof(lift_factors[0]);
         factor_index++) {
        float lift = observe->guard.radius * lift_factors[factor_index] + 20.0f;
        if (lift < 60.0f) {
            lift = 60.0f;
        }
        for (base_index = 0U; base_index < sizeof(bases) / sizeof(bases[0]); base_index++) {
            MdkrCameraVec3 candidate = bases[base_index];
            candidate.y += lift;
            if (camera_obstruction_try_emergency_candidate(
                    observe, physical_slot, sphere_query, exact_query,
                    use_exact, candidate)) {
                return TRUE;
            }
        }
        for (direction_index = 0U;
             direction_index < ARRAY_COUNT(ring_directions);
             direction_index++) {
            MdkrCameraVec3 candidate = observe->intent.pivot;
            const float radius = observe->guard.radius * 2.0f + 40.0f;
            candidate.x += ring_directions[direction_index][0] * radius;
            candidate.y += lift;
            candidate.z += ring_directions[direction_index][1] * radius;
            if (camera_obstruction_try_emergency_candidate(
                    observe, physical_slot, sphere_query, exact_query,
                    use_exact, candidate)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static int camera_obstruction_publish_safe_fallback(
    MdkrCameraObstructionObserveSlot *observe,
    s32 physical_slot,
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    int use_exact,
    const MdkrCameraVec3 *known_safe) {
    MdkrCameraVec3 candidate;
    MdkrCameraFinalPose pose;
    MdkrCameraSweepHit hit;

    if (observe->resolver.last_safe_valid) {
        candidate = observe->resolver.last_safe_eye;
    } else if (known_safe != NULL) {
        candidate = *known_safe;
    } else {
        return FALSE;
    }
    if (!camera_obstruction_build_final_pose(
            observe, candidate, observe->alternate_active, &pose) ||
        !camera_obstruction_final_pose_stationary_clear(
            observe, sphere_query, exact_query, &pose, use_exact, &hit) ||
        (observe->intent.target_valid &&
         !camera_obstruction_target_visible(
             sphere_query, observe->intent.target, pose.rendered_eye))) {
        return FALSE;
    }
    pose.discontinuity = TRUE;
    camera_obstruction_publish_final_pose(observe, physical_slot, &pose);
    observe->resolver.last_safe_eye = pose.rendered_eye;
    observe->resolver.last_safe_valid = TRUE;
    observe->resolver.projection_generation = observe->projection.generation;
    observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_FAILSAFE;
    observe->was_obstructed = TRUE;
    observe->presentation_discontinuity = TRUE;
    return TRUE;
}

static void camera_obstruction_validate_resolved(
    MdkrCameraObstructionObserveSlot *observe,
    MdkrCameraObstructionRuntimePolicy runtime_policy) {
    const MdkrCameraObstructionQuerySource sources[] = {
        { camera_obstruction_track_sweep_adapter, NULL },
        { camera_obstruction_dynamic_sweep_adapter, observe },
    };
    const MdkrCameraObstructionCombinedQuery query = {
        sources, ARRAY_COUNT(sources), ARRAY_COUNT(sources),
    };
    const MdkrCameraObstructionRoundedLensQuerySource exact_sources[] = {
        { camera_obstruction_track_rounded_sweep_adapter, NULL },
        { camera_obstruction_dynamic_rounded_sweep_adapter, observe },
    };
    const MdkrCameraObstructionRoundedLensCombinedQuery exact_query = {
        exact_sources, ARRAY_COUNT(exact_sources), ARRAY_COUNT(exact_sources),
    };
    MdkrCameraVec3 eye = observe->effective_eye;
    MdkrCameraSweepHit hit;

    if (runtime_policy != MDKR_CAMERA_RUNTIME_MODERN &&
        runtime_policy != MDKR_CAMERA_RUNTIME_CENTER_RAY) {
        eye.y += gNoCamShake ? observe->authored.shakeMagnitude : 0.0f;
        observe->effective_eye = eye;
    }
    if (runtime_policy == MDKR_CAMERA_RUNTIME_MODERN) {
        MdkrCameraRoundedLensGuard final_guard;
        MdkrCameraVec3 derived_eye;
        MdkrCameraObstructionLensQuery lens_query = { 0 };

        if (!observe->resolved_valid ||
            !camera_obstruction_build_exact_guard_for_camera(
                observe,
                &sCameraObstructionRuntime.resolved_cameras[observe->physical_slot],
                &final_guard, &derived_eye) ||
            camera_obstruction_distance(derived_eye, eye) > 1.0e-3f) {
            observe->resolved_stationary_status = MDKR_CAMERA_SWEEP_INVALID;
            observe->query_source_degraded = TRUE;
        } else if (observe->published_pose_validated &&
                   camera_obstruction_exact_guard_equal(
                       &final_guard, &observe->published_exact_guard)) {
            /* Publication accepts only a pose sealed by the same endpoint
             * query. Re-derive the renderer tuple here to detect mutation,
             * without repeating the expensive immutable geometry sweep. */
            observe->resolved_stationary_status = MDKR_CAMERA_SWEEP_CLEAR;
        } else {
            lens_query.sphere = &query;
            lens_query.exact = &exact_query;
            lens_query.guard = &final_guard;
            lens_query.observe = observe;
            observe->resolved_stationary_status =
                camera_obstruction_lens_stationary_query(
                    &lens_query, observe->guard, derived_eye, &hit);
        }
    } else {
        observe->resolved_stationary_status = camera_obstruction_stationary_query(
            &query, observe->guard, eye, &hit);
    }
    memset(&observe->target_visibility_hit, 0,
           sizeof(observe->target_visibility_hit));
    observe->resolved_target_embedded = FALSE;
    if (!observe->intent.target_valid) {
        observe->resolved_target_visible = TRUE;
    } else {
        const MdkrCameraTargetVisibilityStatus target_status =
            mdkr_camera_target_visibility_query(
                &query, observe->intent.target, eye,
                MDKR_CAMERA_OBSTRUCTION_HARD_MASK,
                &observe->target_visibility_hit);

        observe->resolved_target_visible =
            target_status == MDKR_CAMERA_TARGET_VISIBILITY_VISIBLE;
        observe->resolved_target_embedded =
            target_status == MDKR_CAMERA_TARGET_VISIBILITY_EMBEDDED;
        if (target_status == MDKR_CAMERA_TARGET_VISIBILITY_INVALID) {
            observe->query_source_degraded = TRUE;
        }
    }
}

static void camera_obstruction_update_emergency_racer_opacity(
    MdkrCameraObstructionObserveSlot *observe,
    MdkrCameraObstructionRuntimePolicy runtime_policy) {
    float desired_distance;
    float resolved_distance;
    float ratio;

    observe->emergency_racer_opacity = 255U;
    if (runtime_policy != MDKR_CAMERA_RUNTIME_MODERN ||
        observe->alternate_active || !observe->intent.target_valid ||
        (observe->intent.family != MDKR_CAMERA_INTENT_FAMILY_CAR &&
         observe->intent.family != MDKR_CAMERA_INTENT_FAMILY_HOVERCRAFT &&
         observe->intent.family != MDKR_CAMERA_INTENT_FAMILY_PLANE)) {
        return;
    }
    desired_distance = camera_obstruction_distance(
        observe->intent.pivot, observe->desired_eye);
    resolved_distance = camera_obstruction_distance(
        observe->intent.pivot, observe->effective_eye);
    if (!isfinite(desired_distance) || !isfinite(resolved_distance) ||
        desired_distance <= 0.0f) {
        return;
    }
    ratio = resolved_distance / desired_distance;
    if (ratio < 0.45f) {
        int opacity;
        if (ratio < 0.0f) ratio = 0.0f;
        opacity = 96 + (int)(ratio * (159.0f / 0.45f));
        if (opacity < 96) opacity = 96;
        if (opacity > 255) opacity = 255;
        observe->emergency_racer_opacity = (uint8_t)opacity;
    }
}

static int camera_obstruction_alternate_candidate_safe(
    MdkrCameraObstructionObserveSlot *observe,
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    MdkrCameraLensGuard guard,
    MdkrCameraVec3 pivot,
    MdkrCameraVec3 target,
    MdkrCameraVec3 transition_start,
    MdkrCameraVec3 candidate,
    int use_exact) {
    MdkrCameraVec3 anchor;
    MdkrCameraFinalPose pose;
    MdkrCameraSweepInput sweep;
    MdkrCameraSweepHit hit;

    if (camera_obstruction_find_boom_anchor(
            sphere_query, guard, pivot, candidate, &anchor) != MDKR_CAMERA_SWEEP_CLEAR) {
        return FALSE;
    }
    sweep = (MdkrCameraSweepInput) {
        .guard = guard,
        .start_eye = anchor,
        .desired_eye = candidate,
        .mask = MDKR_CAMERA_OBSTRUCTION_HARD_MASK,
    };
    if (mdkr_camera_obstruction_combined_sweep(sphere_query, &sweep, &hit) !=
        MDKR_CAMERA_SWEEP_CLEAR ||
        !camera_obstruction_target_visible(sphere_query, target, candidate)) {
        return FALSE;
    }
    /* A fallback may cut, but never cut through a hard shell. */
    sweep.start_eye = transition_start;
    if (mdkr_camera_obstruction_combined_sweep(sphere_query, &sweep, &hit) !=
        MDKR_CAMERA_SWEEP_CLEAR ||
        !camera_obstruction_build_final_pose(observe, candidate, TRUE, &pose)) {
        return FALSE;
    }
    return camera_obstruction_final_pose_stationary_clear(
        observe, sphere_query, exact_query, &pose, use_exact, &hit);
}

static int camera_obstruction_select_alternate_shot(
    MdkrCameraObstructionObserveSlot *observe,
    const MdkrCameraObstructionCombinedQuery *sphere_query,
    const MdkrCameraObstructionRoundedLensCombinedQuery *exact_query,
    MdkrCameraLensGuard guard,
    MdkrCameraVec3 pivot,
    MdkrCameraVec3 target,
    MdkrCameraVec3 desired,
    MdkrCameraVec3 transition_start,
    const MdkrCameraVec3 *preferred,
    MdkrCameraVec3 *out_candidate,
    int use_exact) {
    static const float yaw_cos[] = {
        0.9659258263f, 0.9659258263f, 0.8660254038f, 0.8660254038f,
        0.7071067812f, 0.7071067812f, 0.5f, 0.5f,
        1.0f, 0.8660254038f, 0.8660254038f,
    };
    static const float yaw_sin[] = {
        0.2588190451f, -0.2588190451f, 0.5f, -0.5f,
        0.7071067812f, -0.7071067812f, 0.8660254038f, -0.8660254038f,
        0.0f, 0.5f, -0.5f,
    };
    static const uint8_t lifted[] = {
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE,
    };
    const MdkrCameraVec3 boom = {
        desired.x - pivot.x, desired.y - pivot.y, desired.z - pivot.z,
    };
    const float boom_distance = camera_obstruction_distance(pivot, desired);
    double best_cost = INFINITY;
    int found = FALSE;
    size_t index;

    if (!isfinite(boom_distance) || boom_distance <= 0.0f || out_candidate == NULL) {
        return FALSE;
    }
    /* Keep the translated prior shoulder while it remains safe. This is the
     * candidate-identity hysteresis: facet jitter cannot flip sides merely
     * because two fan scores trade by a few float ULPs. */
    if (preferred != NULL && camera_obstruction_alternate_candidate_safe(
            observe, sphere_query, exact_query, guard, pivot, target,
            transition_start, *preferred, use_exact)) {
        *out_candidate = *preferred;
        return TRUE;
    }
    for (index = 0U; index < sizeof(yaw_cos) / sizeof(yaw_cos[0]); index++) {
        MdkrCameraVec3 candidate = {
            pivot.x + boom.x * yaw_cos[index] + boom.z * yaw_sin[index],
            desired.y,
            pivot.z - boom.x * yaw_sin[index] + boom.z * yaw_cos[index],
        };
        double dx;
        double dy;
        double dz;
        double cost;

        if (lifted[index]) {
            float lift = boom_distance * 0.35f;
            if (lift < 40.0f) lift = 40.0f;
            if (lift > 100.0f) lift = 100.0f;
            candidate.y += lift;
        }
        if (!camera_obstruction_alternate_candidate_safe(
                observe, sphere_query, exact_query, guard, pivot, target,
                transition_start, candidate, use_exact)) {
            continue;
        }
        dx = (double)candidate.x - desired.x;
        dy = (double)candidate.y - desired.y;
        dz = (double)candidate.z - desired.z;
        cost = dx * dx + dy * dy + dz * dz +
               (lifted[index] ? (double)boom_distance * boom_distance * 0.25 : 0.0);
        if (!found || cost < best_cost) {
            *out_candidate = candidate;
            best_cost = cost;
            found = TRUE;
        }
    }
    return found;
}

/*
 * MOTION-01 (docs/architecture/camera-obstruction.md sections 6.3 and 7.3).
 *
 * The motion census measures the RESOLVED camera -- the pose render actually
 * consumes -- on the authored fixed-tick grid, because that is the only signal
 * a player can feel. Everything here is allocation-free and lives outside
 * sCameraObstructionRuntime so a level reload retires per-slot continuity
 * without discarding the run's distribution.
 *
 * Percentiles come from a log-spaced histogram: these metrics span several
 * decades (sub-unit jerk on an open straight, hundreds of units across a
 * published cut) and a linear bin width honest for one end is useless at the
 * other. Bin edges are exact powers of two subdivided
 * MDKR_CAMERA_MOTION_BINS_PER_OCTAVE ways, so a reported p95 is within ~4.4% of
 * the true one; min, max, and mean are exact.
 */
#define MDKR_CAMERA_MOTION_BINS_PER_OCTAVE 16
#define MDKR_CAMERA_MOTION_MIN_LOG2 (-16)
#define MDKR_CAMERA_MOTION_MAX_LOG2 16
#define MDKR_CAMERA_MOTION_BIN_COUNT \
    (((MDKR_CAMERA_MOTION_MAX_LOG2) - (MDKR_CAMERA_MOTION_MIN_LOG2)) * \
     MDKR_CAMERA_MOTION_BINS_PER_OCTAVE)
/*
 * The doc's chatter window: a correction that clears and re-engages, or engages
 * and clears, inside this many authored ticks is oscillation rather than a
 * distinct obstruction.
 */
#define MDKR_CAMERA_MOTION_OSCILLATION_TICKS 12U
/*
 * Two surfaces whose contact normals agree this closely are treated as one
 * continuous surface. Static track blockers carry a per-triangle stable ID (see
 * tracks.c: source_stable_id increments per source face), so the ID alone
 * cannot answer "same wall?"; the normal can.
 */
#define MDKR_CAMERA_MOTION_CONTINUOUS_SURFACE_DOT 0.985f
/* Lateral offset below this is not a shoulder choice, it is numerical noise. */
#define MDKR_CAMERA_MOTION_SHOULDER_EPSILON 1.0f

typedef enum MdkrCameraMotionStatId {
    MDKR_CAMERA_MOTION_STAT_RETRACT_LATENCY = 0,
    MDKR_CAMERA_MOTION_STAT_RECOVERY_DURATION,
    MDKR_CAMERA_MOTION_STAT_BLOCKED_SPAN,
    MDKR_CAMERA_MOTION_STAT_EMERGENCY_DWELL,
    MDKR_CAMERA_MOTION_STAT_POS_VELOCITY,
    MDKR_CAMERA_MOTION_STAT_POS_ACCEL,
    MDKR_CAMERA_MOTION_STAT_POS_JERK,
    MDKR_CAMERA_MOTION_STAT_ANG_VELOCITY,
    MDKR_CAMERA_MOTION_STAT_ANG_ACCEL,
    MDKR_CAMERA_MOTION_STAT_ANG_JERK,
    MDKR_CAMERA_MOTION_STAT_COUNT,
} MdkrCameraMotionStatId;

static const char *const kCameraMotionStatNames[MDKR_CAMERA_MOTION_STAT_COUNT] = {
    "retract_latency", "recovery_duration", "blocked_span", "emergency_dwell",
    "pos_velocity", "pos_accel", "pos_jerk",
    "ang_velocity", "ang_accel", "ang_jerk",
};

static const char *const kCameraMotionStatUnits[MDKR_CAMERA_MOTION_STAT_COUNT] = {
    "ticks", "ticks", "ticks", "ticks",
    "wu/tick", "wu/tick^2", "wu/tick^3",
    "deg/tick", "deg/tick^2", "deg/tick^3",
};

typedef struct MdkrCameraMotionStat {
    uint64_t bins[MDKR_CAMERA_MOTION_BIN_COUNT];
    uint64_t count;
    uint64_t zero_count;
    double total;
    double min;
    double max;
} MdkrCameraMotionStat;

typedef struct MdkrCameraMotionCensus {
    MdkrCameraMotionStat stats[MDKR_CAMERA_MOTION_STAT_COUNT];
    /* Resolved slot-ticks the census actually sampled. */
    uint64_t sampled_slot_ticks;
    /* Distinct authored ticks the census saw at least one slot on. */
    uint64_t sampled_ticks;
    uint64_t last_sampled_tick;
    uint8_t last_sampled_tick_valid;
    /* Slot-ticks per profile, so a reader knows what the run measured. */
    uint64_t profile_slot_ticks[MDKR_CAMERA_OBSTRUCTION_TREATMENT_COUNT];
    uint64_t discontinuities;
    uint64_t retract_events;
    uint64_t recovery_events;
    /*
     * Ticks the resolver kept a latched retraction engaged over a corridor it
     * reported clear (MDKR_CAMERA_OBSTRUCTION_RELEASE_HOLD_TICKS), and the
     * number of separate dropouts those ticks were spent on. This is the whole
     * cost of the release hysteresis, stated as a number rather than asserted.
     */
    uint64_t release_hold_ticks;
    uint64_t release_hold_spans;
    /*
     * Authored ticks on which at least one sampled slot published a cut, and
     * the last such tick so slots inside one tick are not double counted.
     * `discontinuities` counts slot ticks and is the right denominator for a
     * per-slot rate; this one is the right numerator for "how often does the
     * frame a player is shown jump", which is a per-tick question.
     */
    uint64_t cut_ticks;
    uint64_t last_cut_tick;
    uint8_t last_cut_tick_valid;
    uint64_t alternate_entries;
    uint64_t alternate_exits;
    uint64_t emergency_entries;
    uint64_t degenerate_ticks;
    /* Blocker identity changes while continuously blocked, split by whether the
     * contact normal stayed on one continuous surface. */
    uint64_t blocker_changes;
    uint64_t blocker_changes_same_surface;
    uint64_t blocker_changes_new_surface;
    /* Alternate-shot side changes, split the same way. */
    uint64_t shoulder_flips;
    uint64_t shoulder_flips_continuous_surface;
    uint64_t shoulder_flips_new_surface;
    /* clear -> blocked -> clear inside the chatter window. */
    uint64_t oscillation_cycles;
    uint64_t oscillation_cycles_excused;
    /* blocked -> clear -> blocked inside the chatter window. */
    uint64_t correction_reengagements;
    uint64_t max_emergency_dwell;
} MdkrCameraMotionCensus;

static int sCameraMotionEnabled = -1;
static MdkrCameraMotionCensus sCameraMotion;

static int camera_obstruction_motion_enabled(void) {
    if (sCameraMotionEnabled < 0) {
        const char *value = getenv("MDKR_CAMERA_MOTION");

        if (value != NULL && value[0] != '\0') {
            sCameraMotionEnabled = value[0] != '0';
        } else {
            /* No separate opt-in required: anything already asking for camera
             * trace output wants the motion census with it. A release build
             * with tracing off never reaches the sampler. */
            sCameraMotionEnabled = camera_obstruction_trace_level() >= 1;
        }
    }
    return sCameraMotionEnabled != 0;
}

static void camera_obstruction_motion_record(
    MdkrCameraMotionStatId id, double value) {
    MdkrCameraMotionStat *stat;
    int exponent;
    double mantissa;
    int bin;

    if (id < 0 || id >= MDKR_CAMERA_MOTION_STAT_COUNT || !isfinite(value) ||
        value < 0.0) {
        return;
    }
    stat = &sCameraMotion.stats[id];
    if (stat->count == 0U || value < stat->min) {
        stat->min = value;
    }
    if (stat->count == 0U || value > stat->max) {
        stat->max = value;
    }
    stat->total += value;
    stat->count++;
    if (value <= 0.0) {
        stat->zero_count++;
        return;
    }
    /* frexp gives value == mantissa * 2^exponent with mantissa in [0.5, 1). */
    mantissa = frexp(value, &exponent);
    bin = (exponent - 1 - MDKR_CAMERA_MOTION_MIN_LOG2) *
              MDKR_CAMERA_MOTION_BINS_PER_OCTAVE +
          (int)((mantissa * 2.0 - 1.0) * MDKR_CAMERA_MOTION_BINS_PER_OCTAVE);
    if (bin < 0) {
        bin = 0;
    }
    if (bin >= MDKR_CAMERA_MOTION_BIN_COUNT) {
        bin = MDKR_CAMERA_MOTION_BIN_COUNT - 1;
    }
    stat->bins[bin]++;
}

static double camera_obstruction_motion_percentile(
    const MdkrCameraMotionStat *stat, unsigned numerator) {
    uint64_t target;
    uint64_t seen;
    int bin;

    if (stat->count == 0U) {
        return 0.0;
    }
    target = (stat->count * numerator + 99U) / 100U;
    if (target == 0U) {
        target = 1U;
    }
    seen = stat->zero_count;
    if (seen >= target) {
        return 0.0;
    }
    for (bin = 0; bin < MDKR_CAMERA_MOTION_BIN_COUNT; bin++) {
        seen += stat->bins[bin];
        if (seen >= target) {
            /* Upper edge of the containing bin, clamped to the observed max so
             * the report can never claim a value the run did not produce. */
            const double edge = ldexp(
                1.0 + (double)((bin % MDKR_CAMERA_MOTION_BINS_PER_OCTAVE) + 1) /
                          MDKR_CAMERA_MOTION_BINS_PER_OCTAVE,
                MDKR_CAMERA_MOTION_MIN_LOG2 +
                    bin / MDKR_CAMERA_MOTION_BINS_PER_OCTAVE);
            return edge > stat->max ? stat->max : edge;
        }
    }
    return stat->max;
}

static MdkrCameraObstructionTreatment camera_obstruction_shipped_family_treatment(
    MdkrCameraIntentFamily family) {
    switch (family) {
        /* Scripted cutscenes 4-7 have no boom to retract along; the shot
         * contract owns framing and only wants an emergency eye push. Fixed
         * door cameras are authored to present a door/hub subject and must
         * never retract, recover, or swing onto an alternate shot that stops
         * presenting it. */
        case MDKR_CAMERA_INTENT_FAMILY_SCRIPTED_CUTSCENE:
        case MDKR_CAMERA_INTENT_FAMILY_FIXED:
            return MDKR_CAMERA_OBSTRUCTION_TREATMENT_DEPENETRATE_ONLY;
        /* A loop-mode shot is already on rails around the racer; swinging it
         * onto a shoulder candidate would fight the authored orbit rather than
         * clarify it. It still retracts, recovers, and takes emergency framing.
         * This spelling replaces the family test that used to sit inline on the
         * alternate-fan gate; the resulting behavior is the same one. */
        case MDKR_CAMERA_INTENT_FAMILY_LOOP:
            return MDKR_CAMERA_OBSTRUCTION_TREATMENT_SAFETY_ONLY;
        default:
            return MDKR_CAMERA_OBSTRUCTION_TREATMENT_FULL;
    }
}

static MdkrCameraObstructionTreatment camera_obstruction_family_treatment(
    MdkrCameraIntentFamily family) {
    if (!sCameraProfileForceParsed) {
        camera_obstruction_parse_profile_force();
    }
    if (family >= 0 && family < MDKR_CAMERA_INTENT_FAMILY_COUNT &&
        sCameraProfileForce[family] >= 0) {
        return (MdkrCameraObstructionTreatment)sCameraProfileForce[family];
    }
    return camera_obstruction_shipped_family_treatment(family);
}

static void camera_obstruction_resolve_slot(
    MdkrCameraObstructionObserveSlot *observe,
    s32 physical_slot,
    MdkrCameraObstructionRuntimePolicy runtime_policy,
    float fixed_delta_seconds) {
    const MdkrCameraObstructionQuerySource sources[] = {
        { camera_obstruction_track_sweep_adapter, NULL },
        { camera_obstruction_dynamic_sweep_adapter, observe },
    };
    const MdkrCameraObstructionCombinedQuery combined = {
        sources, sizeof(sources) / sizeof(sources[0]), sizeof(sources) / sizeof(sources[0]),
    };
    const MdkrCameraObstructionRoundedLensQuerySource exact_sources[] = {
        { camera_obstruction_track_rounded_sweep_adapter, NULL },
        { camera_obstruction_dynamic_rounded_sweep_adapter, observe },
    };
    const MdkrCameraObstructionRoundedLensCombinedQuery exact_combined = {
        exact_sources, ARRAY_COUNT(exact_sources), ARRAY_COUNT(exact_sources),
    };
    MdkrCameraObstructionLensQuery lens_query = {
        &combined, &exact_combined, &observe->exact_guard, observe,
    };
    const int use_exact = runtime_policy == MDKR_CAMERA_RUNTIME_MODERN;
    const MdkrCameraObstructionQuery active_query = {
        use_exact ? camera_obstruction_lens_sweep_adapter :
                    mdkr_camera_obstruction_combined_sweep_adapter,
        use_exact ? (const void *)&lens_query : (const void *)&combined,
    };
    MdkrCameraObstructionResolverConfig config = {
        .policy = runtime_policy == MDKR_CAMERA_RUNTIME_CENTER_RAY ?
            MDKR_CAMERA_OBSTRUCTION_POLICY_CENTER_RAY : MDKR_CAMERA_OBSTRUCTION_POLICY_MODERN,
        .clearance_skin = 1.0f,
        .recovery_speed = 600.0f,
        .max_recovery_step = 20.0f,
        .release_hold_ticks = MDKR_CAMERA_OBSTRUCTION_RELEASE_HOLD_TICKS,
    };
    MdkrCameraObstructionResolverInput input;
    MdkrCameraObstructionResolverResult result;
    MdkrCameraSweepInput corridor;
    MdkrCameraSweepHit hit;
    MdkrCameraSweepStatus status;
    MdkrCameraVec3 boom_anchor;
    MdkrCameraVec3 start;
    MdkrCameraVec3 desired = observe->desired_eye;
    const MdkrCameraVec3 prior_safe = observe->resolver.last_safe_eye;
    const int prior_safe_valid = observe->resolver.last_safe_valid;
    const int previously_obstructed = observe->was_obstructed;
    const int prior_alternate = observe->alternate_active;
    const float shake = gNoCamShake ? observe->authored.shakeMagnitude : 0.0f;
    MdkrCameraObstructionTreatment treatment;

    observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_CLEAR;
    observe->resolved_valid = TRUE;
    observe->correction_applied = FALSE;
    observe->blocker_kind = 0U;
    observe->blocker_stable_id = 0U;
    observe->blocker_normal_valid = FALSE;
    observe->release_held = FALSE;
    if (observe->intent.discontinuity) {
        observe->resolver.last_safe_valid = FALSE;
        observe->previous_pivot_valid = FALSE;
        observe->alternate_active = FALSE;
        observe->was_obstructed = FALSE;
    }
    if (runtime_policy == MDKR_CAMERA_RUNTIME_OBSERVE ||
        runtime_policy == MDKR_CAMERA_RUNTIME_LEGACY || !observe->intent_usable) {
        return;
    }
    if (use_exact && !observe->exact_guard_valid) {
        observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID;
        observe->resolved_valid = FALSE;
        observe->query_source_degraded = TRUE;
        observe->presentation_discontinuity = TRUE;
        return;
    }
    if (observe->intent.discontinuity) {
        observe->presentation_discontinuity = TRUE;
    }
    desired.y += shake;
    treatment = camera_obstruction_family_treatment(observe->intent.family);
    if (treatment == MDKR_CAMERA_OBSTRUCTION_TREATMENT_DEPENETRATE_ONLY) {
        if (!camera_obstruction_depenetrate_only_eye(
                observe, physical_slot, &active_query, &combined,
                &exact_combined, use_exact, desired)) {
            observe->resolved_valid = camera_obstruction_publish_safe_fallback(
                observe, physical_slot, &combined, &exact_combined,
                use_exact, NULL);
        }
        return;
    }
    /* The anchor is only a known-clear origin for later authoritative exact
     * sweeps. Its enclosing sphere is sufficient and avoids an exact
     * stationary fan against the ground at every racer pivot. */
    status = camera_obstruction_find_boom_anchor(
        &combined, observe->guard, observe->intent.pivot, desired, &boom_anchor);
    if (status != MDKR_CAMERA_SWEEP_INVALID && use_exact) {
        const int conservative_anchor_clear = status == MDKR_CAMERA_SWEEP_CLEAR;
        if (!conservative_anchor_clear) {
            boom_anchor = desired;
        }
        status = camera_obstruction_refine_boom_anchor_lens(
            &lens_query, observe->guard, observe->intent.pivot,
            desired, boom_anchor, conservative_anchor_clear, &boom_anchor);
    }
    if (status != MDKR_CAMERA_SWEEP_CLEAR) {
        if (status == MDKR_CAMERA_SWEEP_HIT &&
            camera_obstruction_publish_elevated_emergency(
                observe, physical_slot, &combined, &exact_combined,
                use_exact, desired)) {
            return;
        }
        observe->resolver_status = status == MDKR_CAMERA_SWEEP_INVALID ?
            MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID :
            MDKR_CAMERA_OBSTRUCTION_RESOLVER_FAILSAFE;
        observe->resolved_valid = camera_obstruction_publish_safe_fallback(
            observe, physical_slot, &combined, &exact_combined,
            use_exact, NULL);
        return;
    }

    corridor = (MdkrCameraSweepInput) {
        .guard = observe->guard,
        .start_eye = boom_anchor,
        .desired_eye = desired,
        .mask = MDKR_CAMERA_OBSTRUCTION_HARD_MASK,
    };
    status = active_query.sweep(active_query.context, &corridor, &hit);
    camera_obstruction_shadow_exact_corridor(observe, &corridor);
    if (status == MDKR_CAMERA_SWEEP_INVALID) {
        observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID;
        observe->resolved_valid = camera_obstruction_publish_safe_fallback(
            observe, physical_slot, &combined, &exact_combined,
            use_exact, &boom_anchor);
        return;
    }

    start = boom_anchor;
    if (status == MDKR_CAMERA_SWEEP_CLEAR) {
        if (observe->was_obstructed && observe->previous_pivot_valid &&
            observe->resolver.last_safe_valid && !observe->intent.discontinuity) {
            const float desired_distance = camera_obstruction_distance(observe->intent.pivot, desired);
            const float previous_distance = camera_obstruction_distance(
                observe->previous_pivot, observe->resolver.last_safe_eye);
            if (observe->alternate_active) {
                MdkrCameraVec3 translated = {
                    observe->resolver.last_safe_eye.x +
                        observe->intent.pivot.x - observe->previous_pivot.x,
                    observe->resolver.last_safe_eye.y +
                        observe->intent.pivot.y - observe->previous_pivot.y,
                    observe->resolver.last_safe_eye.z +
                        observe->intent.pivot.z - observe->previous_pivot.z,
                };
                MdkrCameraSweepInput translated_path = {
                    .guard = observe->guard,
                    .start_eye = boom_anchor,
                    .desired_eye = translated,
                    .mask = MDKR_CAMERA_OBSTRUCTION_HARD_MASK,
                };
                if (active_query.sweep(
                        active_query.context, &translated_path, &hit) ==
                    MDKR_CAMERA_SWEEP_CLEAR) {
                    start = translated;
                }
            } else if (isfinite(desired_distance) && isfinite(previous_distance) &&
                       desired_distance > 0.0f) {
                float fraction = previous_distance / desired_distance;
                if (fraction > 1.0f) fraction = 1.0f;
                start = camera_obstruction_lerp(observe->intent.pivot, desired, fraction);
            }
        } else {
            /* Open-space invariant: never introduce lag where no correction
             * was active and the entire authored boom is clear. */
            start = desired;
        }
    }

    memset(&input, 0, sizeof(input));
    input.query = active_query;
    input.projection = &observe->projection;
    input.pivot = boom_anchor;
    input.start_safe_eye = start;
    input.desired_eye = desired;
    input.fixed_delta_seconds = fixed_delta_seconds;
    input.mask = MDKR_CAMERA_OBSTRUCTION_HARD_MASK;
    input.discontinuity = observe->intent.discontinuity;
    observe->resolver_status = mdkr_camera_obstruction_resolve(
        &config, &input, &observe->resolver, &result);
    observe->blocker_kind = result.blocker_kind;
    observe->blocker_stable_id = result.blocker_stable_id;
    observe->blocker_normal = result.blocker_normal;
    observe->blocker_normal_valid = result.blocker_stable_id != 0U;
    observe->release_held = (uint8_t)(result.release_held != 0);
    if (result.accepted) {
        MdkrCameraVec3 alternate;
        MdkrCameraVec3 preferred_alternate;
        const MdkrCameraVec3 *preferred_alternate_ptr = NULL;
        MdkrCameraVec3 transition_start = result.resolved_eye;
        const float desired_distance = camera_obstruction_distance(observe->intent.pivot, desired);
        const float resolved_distance = camera_obstruction_distance(
            observe->intent.pivot, result.resolved_eye);
        const int direct_target_visible = !observe->intent.target_valid ||
            camera_obstruction_target_visible(
                &combined, observe->intent.target, result.resolved_eye);
        int selected_alternate = FALSE;

        if (prior_alternate && prior_safe_valid && observe->previous_pivot_valid) {
            transition_start.x = prior_safe.x + observe->intent.pivot.x - observe->previous_pivot.x;
            transition_start.y = prior_safe.y + observe->intent.pivot.y - observe->previous_pivot.y;
            transition_start.z = prior_safe.z + observe->intent.pivot.z - observe->previous_pivot.z;
            preferred_alternate = transition_start;
            preferred_alternate_ptr = &preferred_alternate;
        }

        /* Only FULL consents to leaving the authored pivot->eye ray. The
         * DEPENETRATE_ONLY families never reach here (they returned above), so
         * this test is exactly the old `family != LOOP` one. */
        /*
         * A tick held by the release hysteresis is RETRACTED without having
         * measured a blocker, and it must not open the fan on its own. The
         * 0.55 clause asks "is the authored ray obstructed badly enough to be
         * worth leaving?", and a held tick's answer to "obstructed?" is a
         * sweep result the resolver has just decided not to trust. Swinging
         * onto a shoulder there commits a shot change to a false negative --
         * measured: on the 3P route it moved the boom to a lifted candidate
         * for two ticks and then dropped it again. Contact ticks and a target
         * that is genuinely not visible still drive the fan unchanged, and the
         * 0.70 clause still holds an ALREADY-active alternate shot across the
         * hold, which is that shot's own hysteresis.
         */
        if (observe->intent.target_valid &&
            !camera_obstruction_test_disable_alternate_shot() &&
            treatment == MDKR_CAMERA_OBSTRUCTION_TREATMENT_FULL &&
            isfinite(desired_distance) && isfinite(resolved_distance) &&
            desired_distance > 0.0f &&
            ((result.status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED &&
              !result.release_held &&
              resolved_distance < desired_distance * 0.55f) ||
             (prior_alternate && resolved_distance < desired_distance * 0.70f) ||
            !direct_target_visible) &&
            camera_obstruction_select_alternate_shot(
                observe, &combined, &exact_combined,
                observe->guard, observe->intent.pivot,
                observe->intent.target, desired,
                transition_start, preferred_alternate_ptr, &alternate,
                use_exact)) {
            result.resolved_eye = alternate;
            observe->resolver.last_safe_eye = alternate;
            observe->resolver.last_safe_valid = TRUE;
            observe->resolver.projection_generation = observe->projection.generation;
            observe->alternate_active = TRUE;
            selected_alternate = TRUE;
            result.status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED;
            observe->resolver_status = result.status;
            if (!prior_alternate ||
                camera_obstruction_distance(transition_start, alternate) > 100.0f) {
                observe->presentation_discontinuity = TRUE;
            }
        }
        if (observe->intent.target_valid && !direct_target_visible &&
            !selected_alternate &&
            camera_obstruction_publish_elevated_emergency(
                observe, physical_slot, &combined, &exact_combined,
                use_exact, desired)) {
            /* The ordinary boom is lens-safe but cannot see its target and no
             * shoulder candidate was available (or the fault-injection seam
             * disabled that fan). Never publish a target-blind gameplay shot
             * when a validated emergency composition exists. */
            return;
        }
        if (result.status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED &&
            !selected_alternate) {
            observe->alternate_active = FALSE;
            if (prior_alternate) {
                observe->presentation_discontinuity = TRUE;
            }
        }
        if (result.status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_CLEAR) {
            observe->alternate_active = FALSE;
        }
        {
            MdkrCameraFinalPose final_pose;
            MdkrCameraSweepHit final_hit;

            if (!camera_obstruction_build_final_pose(
                    observe, result.resolved_eye, observe->alternate_active,
                    &final_pose) ||
                !camera_obstruction_final_pose_stationary_clear(
                    observe, &combined, &exact_combined, &final_pose,
                    use_exact, &final_hit)) {
                if (camera_obstruction_publish_elevated_emergency(
                        observe, physical_slot, &combined, &exact_combined,
                        use_exact, desired)) {
                    return;
                }
                observe->resolved_valid = camera_obstruction_publish_safe_fallback(
                    observe, physical_slot, &combined, &exact_combined,
                    use_exact, &boom_anchor);
                return;
            }
            camera_obstruction_publish_final_pose(
                observe, physical_slot, &final_pose);
        }
        observe->was_obstructed =
            result.status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED ||
            result.status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RECOVERING;
        if (result.status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED &&
            !previously_obstructed) {
            observe->presentation_discontinuity = TRUE;
        }
        observe->previous_pivot = observe->intent.pivot;
        observe->previous_pivot_valid = TRUE;
    } else {
        observe->resolved_valid = camera_obstruction_publish_safe_fallback(
            observe, physical_slot, &combined, &exact_combined,
            use_exact, &boom_anchor);
    }
}

/*
 * MOTION-01 sampler. Runs once per resolved slot per authored tick, after the
 * pose that render will consume has been published and validated. It reads
 * only published outputs and its own history: nothing here can influence a
 * camera, a query, or any authority state.
 */
static void camera_obstruction_motion_sample(
    MdkrCameraObstructionObserveSlot *observe,
    s32 physical_slot,
    s32 normal_slot,
    int trace_level) {
    MdkrCameraMotionSlot *motion;
    MdkrCameraLensPose pose;
    MdkrCameraObstructionTreatment treatment;
    const uint64_t tick = sCameraObstructionRuntime.tick_serial;
    MdkrCameraVec3 eye = { 0.0f, 0.0f, 0.0f };
    MdkrCameraVec3 forward = { 0.0f, 0.0f, 0.0f };
    MdkrCameraVec3 velocity = { 0.0f, 0.0f, 0.0f };
    MdkrCameraVec3 acceleration = { 0.0f, 0.0f, 0.0f };
    MdkrCameraVec3 jerk = { 0.0f, 0.0f, 0.0f };
    double angular_velocity = 0.0;
    double angular_acceleration = 0.0;
    double angular_jerk = 0.0;
    int pose_valid;
    int continuous;
    int degenerate;
    int emergency;
    int alternate;
    int retracted;
    int recovering;
    int held;
    int blocked;
    int cut;
    int side = 0;
    int phase;
    int churn = 0;
    int churn_same_surface = 0;
    int flipped = 0;

    if (observe == NULL || physical_slot < 0 ||
        physical_slot >= MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT) {
        return;
    }
    motion = &observe->motion;
    treatment = camera_obstruction_family_treatment(observe->intent.family);
    pose_valid = observe->resolved_valid &&
        cam_lens_pose_from_camera_snapshot(
            &sCameraObstructionRuntime.resolved_cameras[physical_slot],
            gNoCamShake != 0, &pose);
    if (pose_valid) {
        eye.x = pose.eye.x;
        eye.y = pose.eye.y;
        eye.z = pose.eye.z;
        forward.x = pose.forward.x;
        forward.y = pose.forward.y;
        forward.z = pose.forward.z;
    }

    /*
     * "Degenerate" is the doc's geometrically-invalid intermediate pose: the
     * tick published no validated safe image. Oscillation across such a tick is
     * excused, because the camera had no correct alternative to hold.
     */
    degenerate = !observe->resolved_valid ||
        observe->query_source_degraded ||
        observe->resolved_stationary_status != MDKR_CAMERA_SWEEP_CLEAR ||
        observe->resolver_status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID ||
        observe->resolver_status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_FAILSAFE;
    emergency = observe->elevated_emergency != 0;
    alternate = observe->alternate_active != 0;
    retracted =
        observe->resolver_status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RETRACTED;
    recovering =
        observe->resolver_status == MDKR_CAMERA_OBSTRUCTION_RESOLVER_RECOVERING;
    held = retracted && observe->release_held != 0;
    /* Correction is engaged whenever the published eye is not free to sit on
     * the authored desired pose, however that came about. */
    blocked = retracted || alternate || emergency;
    cut = observe->presentation_discontinuity != 0;
    continuous = pose_valid && !cut && motion->continuity_run != 0U &&
        motion->previous_tick + 1U == tick;

    sCameraMotion.sampled_slot_ticks++;
    if (!sCameraMotion.last_sampled_tick_valid ||
        sCameraMotion.last_sampled_tick != tick) {
        sCameraMotion.sampled_ticks++;
        sCameraMotion.last_sampled_tick = tick;
        sCameraMotion.last_sampled_tick_valid = TRUE;
    }
    if (treatment >= 0 && treatment < MDKR_CAMERA_OBSTRUCTION_TREATMENT_COUNT) {
        sCameraMotion.profile_slot_ticks[treatment]++;
    }
    if (cut) {
        sCameraMotion.discontinuities++;
        if (!sCameraMotion.last_cut_tick_valid ||
            sCameraMotion.last_cut_tick != tick) {
            sCameraMotion.cut_ticks++;
            sCameraMotion.last_cut_tick = tick;
            sCameraMotion.last_cut_tick_valid = TRUE;
        }
    }
    if (degenerate) {
        sCameraMotion.degenerate_ticks++;
        motion->block_span_degenerate = TRUE;
    }

    /*
     * Finite differences on the authored tick grid. A published cut, a dropped
     * tick, or an unvalidated pose retires the chain rather than differencing
     * across it: a cut is a discontinuity by construction, and reporting its
     * step as jerk would drown the smoothness signal the metric exists for.
     */
    if (!pose_valid) {
        motion->continuity_run = 0U;
    } else if (!continuous) {
        motion->continuity_run = 1U;
    } else {
        double dot;

        velocity.x = eye.x - motion->previous_eye.x;
        velocity.y = eye.y - motion->previous_eye.y;
        velocity.z = eye.z - motion->previous_eye.z;
        dot = (double)forward.x * motion->previous_forward.x +
              (double)forward.y * motion->previous_forward.y +
              (double)forward.z * motion->previous_forward.z;
        if (dot > 1.0) {
            dot = 1.0;
        }
        if (dot < -1.0) {
            dot = -1.0;
        }
        angular_velocity = acos(dot) / MDKR_CAMERA_OBSTRUCTION_DEG_TO_RAD;
        camera_obstruction_motion_record(
            MDKR_CAMERA_MOTION_STAT_POS_VELOCITY,
            sqrt((double)velocity.x * velocity.x +
                 (double)velocity.y * velocity.y +
                 (double)velocity.z * velocity.z));
        camera_obstruction_motion_record(
            MDKR_CAMERA_MOTION_STAT_ANG_VELOCITY, angular_velocity);
        if (motion->continuity_run >= 2U) {
            acceleration.x = velocity.x - motion->previous_velocity.x;
            acceleration.y = velocity.y - motion->previous_velocity.y;
            acceleration.z = velocity.z - motion->previous_velocity.z;
            angular_acceleration =
                fabs(angular_velocity - motion->previous_angular_velocity);
            camera_obstruction_motion_record(
                MDKR_CAMERA_MOTION_STAT_POS_ACCEL,
                sqrt((double)acceleration.x * acceleration.x +
                     (double)acceleration.y * acceleration.y +
                     (double)acceleration.z * acceleration.z));
            camera_obstruction_motion_record(
                MDKR_CAMERA_MOTION_STAT_ANG_ACCEL, angular_acceleration);
            if (motion->continuity_run >= 3U) {
                jerk.x = acceleration.x - motion->previous_acceleration.x;
                jerk.y = acceleration.y - motion->previous_acceleration.y;
                jerk.z = acceleration.z - motion->previous_acceleration.z;
                angular_jerk = fabs(
                    angular_acceleration - motion->previous_angular_acceleration);
                camera_obstruction_motion_record(
                    MDKR_CAMERA_MOTION_STAT_POS_JERK,
                    sqrt((double)jerk.x * jerk.x +
                         (double)jerk.y * jerk.y +
                         (double)jerk.z * jerk.z));
                camera_obstruction_motion_record(
                    MDKR_CAMERA_MOTION_STAT_ANG_JERK, angular_jerk);
            }
            motion->previous_acceleration = acceleration;
            motion->previous_angular_acceleration = (float)angular_acceleration;
        }
        motion->previous_velocity = velocity;
        motion->previous_angular_velocity = (float)angular_velocity;
        if (motion->continuity_run < 4U) {
            motion->continuity_run++;
        }
    }

    /* Correction phase machine: retract latency, blocked spans, oscillation. */
    phase = blocked ? (int)MDKR_CAMERA_MOTION_PHASE_BLOCKED :
                      (int)MDKR_CAMERA_MOTION_PHASE_CLEAR;
    if ((int)motion->phase != phase) {
        if (phase == (int)MDKR_CAMERA_MOTION_PHASE_BLOCKED) {
            sCameraMotion.retract_events++;
            if (motion->phase == (uint8_t)MDKR_CAMERA_MOTION_PHASE_CLEAR &&
                motion->previous_phase ==
                    (uint8_t)MDKR_CAMERA_MOTION_PHASE_BLOCKED &&
                tick - motion->clear_onset_tick <=
                    MDKR_CAMERA_MOTION_OSCILLATION_TICKS) {
                /* blocked -> clear -> blocked: the correction dropped out and
                 * came straight back. This is the chatter a player sees as the
                 * camera "pumping" against one wall. */
                sCameraMotion.correction_reengagements++;
            }
            motion->block_onset_tick = tick;
            motion->block_span_degenerate = (uint8_t)(degenerate != 0);
            motion->retract_pending = TRUE;
        } else {
            if (motion->phase == (uint8_t)MDKR_CAMERA_MOTION_PHASE_BLOCKED) {
                const uint64_t span = tick - motion->block_onset_tick;

                camera_obstruction_motion_record(
                    MDKR_CAMERA_MOTION_STAT_BLOCKED_SPAN, (double)span);
                if (span <= MDKR_CAMERA_MOTION_OSCILLATION_TICKS) {
                    /* clear -> blocked -> clear inside the window. */
                    sCameraMotion.oscillation_cycles++;
                    if (motion->block_span_degenerate) {
                        sCameraMotion.oscillation_cycles_excused++;
                    }
                }
                motion->retract_pending = FALSE;
            }
            motion->clear_onset_tick = tick;
        }
        motion->previous_phase = motion->phase;
        motion->phase = (uint8_t)phase;
    }
    if (motion->retract_pending && !degenerate) {
        /* Ticks from obstruction onset to a fully resolved, validated pose. */
        camera_obstruction_motion_record(
            MDKR_CAMERA_MOTION_STAT_RETRACT_LATENCY,
            (double)(tick - motion->block_onset_tick));
        motion->retract_pending = FALSE;
    }

    /*
     * Release-hold accounting. A held tick is one the resolver reported
     * RETRACTED over a corridor its own sweep called clear, so it is exactly
     * the tick that used to disengage the correction and pop the boom.
     */
    if (held) {
        sCameraMotion.release_hold_ticks++;
        if (!motion->release_held) {
            sCameraMotion.release_hold_spans++;
        }
    }
    motion->release_held = (uint8_t)(held != 0);

    if (recovering && !motion->recovering) {
        sCameraMotion.recovery_events++;
        motion->recovery_onset_tick = tick;
    } else if (!recovering && motion->recovering) {
        camera_obstruction_motion_record(
            MDKR_CAMERA_MOTION_STAT_RECOVERY_DURATION,
            (double)(tick - motion->recovery_onset_tick));
    }
    motion->recovering = (uint8_t)(recovering != 0);

    if (emergency) {
        if (!motion->emergency) {
            sCameraMotion.emergency_entries++;
            motion->emergency_run = 0U;
        }
        motion->emergency_run++;
        if (motion->emergency_run > sCameraMotion.max_emergency_dwell) {
            sCameraMotion.max_emergency_dwell = motion->emergency_run;
        }
    } else if (motion->emergency) {
        camera_obstruction_motion_record(
            MDKR_CAMERA_MOTION_STAT_EMERGENCY_DWELL,
            (double)motion->emergency_run);
        motion->emergency_run = 0U;
    }
    motion->emergency = (uint8_t)(emergency != 0);

    /*
     * Blocker-identity churn. Static track blockers carry a per-triangle stable
     * ID, so an ID change alone does not mean a new obstruction owner; the
     * contact normal decides whether the camera is still on one continuous
     * surface. The resolver never reads blocker identity, so churn cannot reset
     * recovery -- this metric measures how noisy the reported identity is, and
     * gives the shoulder-flip gate its continuous-surface test.
     */
    if (blocked && observe->blocker_stable_id != 0U) {
        if (motion->blocker_valid &&
            motion->blocker_stable_id != observe->blocker_stable_id) {
            const double normal_dot =
                (double)observe->blocker_normal.x * motion->blocker_normal.x +
                (double)observe->blocker_normal.y * motion->blocker_normal.y +
                (double)observe->blocker_normal.z * motion->blocker_normal.z;

            churn = TRUE;
            churn_same_surface = observe->blocker_normal_valid &&
                normal_dot >= MDKR_CAMERA_MOTION_CONTINUOUS_SURFACE_DOT;
            sCameraMotion.blocker_changes++;
            if (churn_same_surface) {
                sCameraMotion.blocker_changes_same_surface++;
            } else {
                sCameraMotion.blocker_changes_new_surface++;
            }
        }
        motion->blocker_stable_id = observe->blocker_stable_id;
        motion->blocker_normal = observe->blocker_normal;
        motion->blocker_valid = observe->blocker_normal_valid;
    } else if (!blocked) {
        motion->blocker_stable_id = 0U;
        motion->blocker_valid = FALSE;
    }

    /*
     * Shoulder side of the published eye, measured against the authored
     * pivot->desired ray. The authored ray has zero lateral offset by
     * construction, so this is exactly the alternate fan's shoulder choice.
     */
    if (alternate && pose_valid) {
        /* right = worldUp x boom, so only the horizontal boom terms survive. */
        const double right_x = (double)observe->desired_eye.z -
            observe->intent.pivot.z;
        const double right_z = -((double)observe->desired_eye.x -
            observe->intent.pivot.x);
        const double length = sqrt(right_x * right_x + right_z * right_z);

        if (isfinite(length) && length > 0.0) {
            const double lateral =
                (((double)eye.x - observe->intent.pivot.x) * right_x +
                 ((double)eye.z - observe->intent.pivot.z) * right_z) / length;

            if (lateral > MDKR_CAMERA_MOTION_SHOULDER_EPSILON) {
                side = 1;
            } else if (lateral < -MDKR_CAMERA_MOTION_SHOULDER_EPSILON) {
                side = -1;
            }
        }
    }
    if (alternate && !motion->alternate) {
        sCameraMotion.alternate_entries++;
    } else if (!alternate && motion->alternate) {
        sCameraMotion.alternate_exits++;
        motion->shoulder_side = 0;
    }
    if (alternate && motion->alternate && side != 0 &&
        motion->shoulder_side != 0 && side != motion->shoulder_side) {
        flipped = TRUE;
        sCameraMotion.shoulder_flips++;
        if (churn_same_surface || (!churn && motion->blocker_valid)) {
            /* The alternate side changed while the camera was still against
             * one continuous surface. Nothing about the geometry asked for the
             * other shoulder; this is the flapping the doc forbids. */
            sCameraMotion.shoulder_flips_continuous_surface++;
        } else {
            sCameraMotion.shoulder_flips_new_surface++;
        }
    }
    if (side != 0) {
        motion->shoulder_side = (int8_t)side;
    }
    motion->alternate = (uint8_t)(alternate != 0);

    if (pose_valid) {
        motion->previous_eye = eye;
        motion->previous_forward = forward;
    }
    motion->previous_tick = tick;

    if (trace_level >= 2) {
        fprintf(stderr,
                "camera_motion detail tick=%llu viewport=%d normal_slot=%d "
                "physical_slot=%d family=%d profile=%s "
                "eye=(%.4f,%.4f,%.4f) forward=(%.5f,%.5f,%.5f) "
                "continuity=%u pose_valid=%u continuous=%u "
                "pos={velocity=%.5f accel=%.5f jerk=%.5f} "
                "ang={velocity=%.5f accel=%.5f jerk=%.5f} "
                "state={blocked=%u retracted=%u recovering=%u held=%u alternate=%u "
                "emergency=%u degenerate=%u discontinuity=%u} "
                "blocker={id=%u kind=%u churn=%u same_surface=%u} "
                "shoulder={side=%d flip=%u} "
                "span={blocked=%llu emergency=%u}\n",
                (unsigned long long)tick, observe->viewport, normal_slot,
                observe->physical_slot, (int)observe->intent.family,
                kCameraObstructionTreatmentNames[treatment],
                eye.x, eye.y, eye.z, forward.x, forward.y, forward.z,
                motion->continuity_run, (unsigned)(pose_valid != 0),
                (unsigned)(continuous != 0),
                sqrt((double)velocity.x * velocity.x +
                     (double)velocity.y * velocity.y +
                     (double)velocity.z * velocity.z),
                sqrt((double)acceleration.x * acceleration.x +
                     (double)acceleration.y * acceleration.y +
                     (double)acceleration.z * acceleration.z),
                sqrt((double)jerk.x * jerk.x + (double)jerk.y * jerk.y +
                     (double)jerk.z * jerk.z),
                angular_velocity, angular_acceleration, angular_jerk,
                (unsigned)(blocked != 0), (unsigned)(retracted != 0),
                (unsigned)(recovering != 0), (unsigned)(held != 0),
                (unsigned)(alternate != 0),
                (unsigned)(emergency != 0), (unsigned)(degenerate != 0),
                (unsigned)(cut != 0),
                observe->blocker_stable_id, observe->blocker_kind,
                (unsigned)(churn != 0), (unsigned)(churn_same_surface != 0),
                side, (unsigned)(flipped != 0),
                (unsigned long long)(
                    phase == (int)MDKR_CAMERA_MOTION_PHASE_BLOCKED ?
                        tick - motion->block_onset_tick : 0U),
                motion->emergency_run);
    }
}

static void camera_obstruction_observe_slot(
    s32 viewport,
    s32 normal_slot,
    s32 physical_slot,
    int cutscene_bank,
    int gameplay_projection,
    MdkrCameraObstructionRuntimePolicy runtime_policy,
    float fixed_delta_seconds) {
    MdkrCameraObstructionObserveSlot *observe;
    MdkrCameraSweepInput stationary_input;
    MdkrCameraSweepInput corridor_input;
    int projection_ready;
    int trace_level;

    if (viewport < 0 || viewport >= 4 || normal_slot < 0 || normal_slot >= 4 ||
        physical_slot < 0 || physical_slot >= MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT) {
        sCameraObstructionRuntime.duplicate_solve_violations++;
        return;
    }
    observe = &sCameraObstructionRuntime.slots[physical_slot];
    trace_level = camera_obstruction_trace_level();
    if (observe->last_solve_tick == sCameraObstructionRuntime.tick_serial) {
        sCameraObstructionRuntime.duplicate_solve_violations++;
        return;
    }
    observe->last_solve_tick = sCameraObstructionRuntime.tick_serial;
    observe->selected = TRUE;
    observe->solve_count = 1;
    observe->viewport = (s16)viewport;
    observe->physical_slot = (s16)physical_slot;
    camera_obstruction_apply_intent(observe, physical_slot);
    if (sCameraObstructionRuntime.viewport_slot_valid[viewport] &&
        sCameraObstructionRuntime.last_physical_slot_by_viewport[viewport] != physical_slot) {
        observe->intent.discontinuity = TRUE;
    }
    sCameraObstructionRuntime.last_physical_slot_by_viewport[viewport] = (s16)physical_slot;
    sCameraObstructionRuntime.viewport_slot_valid[viewport] = TRUE;
    memset(&observe->projection, 0, sizeof(observe->projection));
    memset(&observe->render_projection, 0, sizeof(observe->render_projection));
    memset(&observe->guard, 0, sizeof(observe->guard));
    memset(&observe->exact_guard, 0, sizeof(observe->exact_guard));
    memset(&observe->stationary_hit, 0, sizeof(observe->stationary_hit));
    memset(&observe->corridor_hit, 0, sizeof(observe->corridor_hit));

    projection_ready = !camera_obstruction_test_projection_failure(
            sCameraObstructionRuntime.tick_serial) &&
        cam_latch_effective_projection_for_viewport_context(
            viewport, physical_slot, gameplay_projection,
            &observe->render_projection) &&
        cam_resolver_projection_for_viewport_context(
            viewport, physical_slot, gameplay_projection,
            &observe->projection) &&
        mdkr_camera_lens_guard_from_projection(
            observe->projection.near_plane,
            observe->projection.vertical_fov * MDKR_CAMERA_OBSTRUCTION_DEG_TO_RAD,
            observe->projection.aspect, 0.0f, &observe->guard);
    if (projection_ready && runtime_policy == MDKR_CAMERA_RUNTIME_MODERN) {
        observe->exact_guard_valid = camera_obstruction_build_exact_guard_for_camera(
            observe, &observe->authored, &observe->exact_guard,
            &observe->effective_eye) &&
            camera_obstruction_promote_sphere_guard_for_exact(
                &observe->guard, &observe->exact_guard);
        if (!observe->exact_guard_valid) {
            observe->query_source_degraded = TRUE;
            if (camera_obstruction_exact_shadow_enabled()) {
                observe->exact_shadow_degraded = TRUE;
                observe->exact_shadow_outcome =
                    MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_CONSERVATIVE_FALLBACK;
            }
        }
    }
    if (!projection_ready) {
        const MdkrCameraObstructionQuerySource sources[] = {
            { camera_obstruction_track_sweep_adapter, NULL },
            { camera_obstruction_dynamic_sweep_adapter, observe },
        };
        const MdkrCameraObstructionCombinedQuery combined = {
            sources, sizeof(sources) / sizeof(sources[0]),
            sizeof(sources) / sizeof(sources[0]),
        };
        const MdkrCameraObstructionRoundedLensQuerySource exact_sources[] = {
            { camera_obstruction_track_rounded_sweep_adapter, NULL },
            { camera_obstruction_dynamic_rounded_sweep_adapter, observe },
        };
        const MdkrCameraObstructionRoundedLensCombinedQuery exact_combined = {
            exact_sources, ARRAY_COUNT(exact_sources), ARRAY_COUNT(exact_sources),
        };
        MdkrCameraObstructionLensQuery fallback_lens_query = { 0 };
        MdkrCameraSweepHit fallback_hit;
        MdkrCameraVec3 fallback_eye;
        MdkrCameraSweepStatus fallback_status = MDKR_CAMERA_SWEEP_INVALID;

        observe->stationary_status = MDKR_CAMERA_SWEEP_INVALID;
        observe->corridor_status = MDKR_CAMERA_SWEEP_INVALID;
        observe->resolver_status = MDKR_CAMERA_OBSTRUCTION_RESOLVER_INVALID;
        fallback_eye = (MdkrCameraVec3) {
            observe->last_validated_camera.trans.x_position,
            observe->last_validated_camera.trans.y_position +
                (gNoCamShake ? observe->last_validated_camera.shakeMagnitude : 0.0f),
            observe->last_validated_camera.trans.z_position,
        };
        fallback_lens_query.sphere = &combined;
        fallback_lens_query.exact = &exact_combined;
        fallback_lens_query.guard = &observe->last_validated_exact_guard;
        fallback_lens_query.observe = observe;
        if (observe->last_validated_camera_valid &&
            (runtime_policy != MDKR_CAMERA_RUNTIME_MODERN ||
             observe->last_validated_exact_guard_valid)) {
            fallback_status = runtime_policy == MDKR_CAMERA_RUNTIME_MODERN ?
                camera_obstruction_lens_stationary_query(
                    &fallback_lens_query, observe->last_validated_guard,
                    fallback_eye, &fallback_hit) :
                camera_obstruction_stationary_query(
                    &combined, observe->last_validated_guard,
                    fallback_eye, &fallback_hit);
        }
        if (observe->last_validated_camera_valid &&
            observe->selected_previous_tick && !observe->intent.discontinuity &&
            observe->last_validated_apply_shake == (uint8_t)(gNoCamShake != 0) &&
            observe->last_validated_projection.display_generation ==
                mdkr_display_config_generation() &&
            observe->last_validated_viewport_layout == cam_get_viewport_layout() &&
            observe->last_validated_authored_fov == cam_get_fov() &&
            observe->last_validated_gameplay_projection == (uint8_t)gameplay_projection &&
            /* The world region is a render-lens input the record cannot carry,
             * so a held image may only be reissued into the same region it was
             * validated for. */
            observe->last_validated_world_region ==
                (uint8_t)viewport_world_region_uses_safe_aperture(viewport) &&
            fallback_status == MDKR_CAMERA_SWEEP_CLEAR &&
            !observe->query_source_degraded &&
            cam_restore_latched_effective_projection_for_viewport(
                viewport, physical_slot,
                &observe->last_validated_render_projection)) {
            sCameraObstructionRuntime.resolved_cameras[physical_slot] =
                observe->last_validated_camera;
            observe->projection = observe->last_validated_projection;
            observe->render_projection = observe->last_validated_render_projection;
            observe->guard = observe->last_validated_guard;
            observe->exact_guard = observe->last_validated_exact_guard;
            observe->exact_guard_valid = observe->last_validated_exact_guard_valid;
            observe->orientation_retargeted = observe->last_validated_retargeted;
            observe->effective_eye = fallback_eye;
            observe->resolved_valid = TRUE;
            observe->resolved_stationary_status = MDKR_CAMERA_SWEEP_CLEAR;
            observe->presentation_discontinuity = TRUE;
        } else {
            observe->resolved_valid = FALSE;
        }
    } else if (runtime_policy != MDKR_CAMERA_RUNTIME_MODERN || trace_level >= 2) {
        memset(&stationary_input, 0, sizeof(stationary_input));
        stationary_input.guard = observe->guard;
        stationary_input.start_eye = observe->desired_eye;
        stationary_input.desired_eye = observe->desired_eye;
        stationary_input.mask = MDKR_CAMERA_TRACK_OCCLUSION_HARD_MASK;
        observe->stationary_status = camera_obstruction_track_sweep(
            &stationary_input, &observe->stationary_hit);

        /* There is no universal authored pivot yet. The only honest corridor
         * available at CAM-00 is the prior observed eye to this desired eye. */
        if (observe->previous_desired_valid) {
            corridor_input = stationary_input;
            corridor_input.start_eye = observe->previous_desired_eye;
            observe->corridor_status = camera_obstruction_track_sweep(
                &corridor_input, &observe->corridor_hit);
        } else {
            observe->corridor_status = MDKR_CAMERA_SWEEP_CLEAR;
        }
    } else {
        /* These authored-eye probes exist only for diagnostic/legacy
         * comparison. Modern safety is decided by the combined resolver and
         * post-validation below, so shipping Modern does not pay two extra
         * static sweeps per viewport. */
        observe->stationary_status = MDKR_CAMERA_SWEEP_CLEAR;
        observe->corridor_status = MDKR_CAMERA_SWEEP_CLEAR;
    }
    if (projection_ready) {
        const float projection_guard_radius = observe->guard.radius;

        if (runtime_policy == MDKR_CAMERA_RUNTIME_CENTER_RAY) {
            /* Keep every resolution layer honest to the diagnostic control.
             * Restore the real projection guard before post-validation so the
             * witness can expose near-plane overlap caused by center-only
             * correction. */
            observe->guard.radius = 0.0f;
        }
        camera_obstruction_resolve_slot(
            observe, physical_slot, runtime_policy, fixed_delta_seconds);
        observe->guard.radius = projection_guard_radius;
        camera_obstruction_update_emergency_racer_opacity(observe, runtime_policy);
        camera_obstruction_validate_resolved(observe, runtime_policy);
        if (observe->resolved_valid &&
            observe->resolved_stationary_status == MDKR_CAMERA_SWEEP_CLEAR &&
            !observe->query_source_degraded) {
            MdkrCameraRoundedLensGuard validated_exact_guard;
            const int validated_exact_guard_valid =
                runtime_policy != MDKR_CAMERA_RUNTIME_MODERN ||
                camera_obstruction_build_exact_guard_for_camera(
                    observe,
                    &sCameraObstructionRuntime.resolved_cameras[physical_slot],
                    &validated_exact_guard, NULL);

            if (!validated_exact_guard_valid) {
                observe->query_source_degraded = TRUE;
                observe->last_validated_camera_valid = FALSE;
            } else {
                observe->last_validated_camera =
                    sCameraObstructionRuntime.resolved_cameras[physical_slot];
                observe->last_validated_projection = observe->projection;
                observe->last_validated_render_projection =
                    observe->render_projection;
                observe->last_validated_guard = observe->guard;
                if (runtime_policy == MDKR_CAMERA_RUNTIME_MODERN) {
                    observe->last_validated_exact_guard = validated_exact_guard;
                    observe->last_validated_exact_guard_valid = TRUE;
                } else {
                    memset(&observe->last_validated_exact_guard, 0,
                           sizeof(observe->last_validated_exact_guard));
                    observe->last_validated_exact_guard_valid = FALSE;
                }
                observe->last_validated_apply_shake = (uint8_t)(gNoCamShake != 0);
                observe->last_validated_retargeted = observe->orientation_retargeted;
                observe->last_validated_authored_fov = cam_get_fov();
                observe->last_validated_viewport_layout = cam_get_viewport_layout();
                observe->last_validated_world_region =
                    (uint8_t)viewport_world_region_uses_safe_aperture(viewport);
                observe->last_validated_gameplay_projection =
                    (uint8_t)gameplay_projection;
                observe->last_validated_camera_valid = TRUE;
            }
        }
    }
    if (runtime_policy == MDKR_CAMERA_RUNTIME_MODERN &&
        (!observe->resolved_valid ||
         observe->resolved_stationary_status != MDKR_CAMERA_SWEEP_CLEAR ||
         observe->query_source_degraded)) {
        /* The current snapshot will use authored fallback bytes. Retire the
         * exact predecessor and force both this tick and the first recovered
         * tick to cut; neither may interpolate from an unvalidated image. */
        observe->presentation_discontinuity = TRUE;
        observe->resolved_valid = FALSE;
        observe->last_validated_camera_valid = FALSE;
        observe->last_validated_exact_guard_valid = FALSE;
    }
    observe->previous_desired_eye = observe->desired_eye;
    observe->previous_desired_valid = TRUE;

    /* MOTION-01 reads the published result, so it samples last. */
    if (camera_obstruction_motion_enabled()) {
        camera_obstruction_motion_sample(
            observe, physical_slot, normal_slot, trace_level);
    }
    if (trace_level >= 2) {
        camera_obstruction_trace_detail(observe, normal_slot, cutscene_bank);
    }
}

/*
 * Presentation scopes are strictly balanced: depth is the authority that lets
 * render read resolved cameras, so a saturating push against a clamping pop
 * would eventually leave the scope open or close it early. Render nests these
 * only as deep as its own recursion, so a deeper push is a caller defect and is
 * refused and reported rather than absorbed.
 */
#define MDKR_CAMERA_OBSTRUCTION_PRESENTATION_DEPTH_MAX 8U

static void camera_obstruction_presentation_violation(const char *reason) {
    if (sCameraObstructionRuntime.presentation_depth_violations == 0U) {
        fprintf(stderr, "[CAMERA-PRESENTATION] unbalanced scope: %s\n", reason);
    }
    if (sCameraObstructionRuntime.presentation_depth_violations != UINT32_MAX) {
        sCameraObstructionRuntime.presentation_depth_violations++;
    }
}

void camera_obstruction_presentation_begin(void) {
    if (sCameraObstructionRuntime.presentation_depth >=
        MDKR_CAMERA_OBSTRUCTION_PRESENTATION_DEPTH_MAX) {
        camera_obstruction_presentation_violation("push past depth ceiling");
        if (sCameraObstructionRuntime.presentation_refused_depth != UINT32_MAX) {
            sCameraObstructionRuntime.presentation_refused_depth++;
        }
        return;
    }
    sCameraObstructionRuntime.presentation_depth++;
}

void camera_obstruction_presentation_end(void) {
    if (sCameraObstructionRuntime.presentation_refused_depth != 0U) {
        sCameraObstructionRuntime.presentation_refused_depth--;
        return;
    }
    if (sCameraObstructionRuntime.presentation_depth == 0U) {
        camera_obstruction_presentation_violation("pop without a matching push");
        return;
    }
    sCameraObstructionRuntime.presentation_depth--;
}

/*
 * Render substitutes this for &gCameras[slot] at call sites that never checked
 * an index, so it must return a camera for every index those sites can produce
 * and must never return NULL.
 *
 * viewport_reset() parks gActiveCameraID at 4, and the cutscene bank adds 4 to
 * it, so the arithmetic in cam_get_active_camera() and its peers reaches 8 --
 * one past the last slot. That combination selects no real camera; before this
 * clamp it selected a NULL dereference in six unguarded render paths. Clamping
 * to the last slot preserves the original engine's behavior, which indexed
 * gCameras with the same unchecked value.
 */
Camera *camera_obstruction_camera_for_slot(s32 physical_slot) {
    if (physical_slot < 0) {
        physical_slot = 0;
    } else if (physical_slot >= MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT) {
        physical_slot = MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT - 1;
    }
    if (sCameraObstructionRuntime.presentation_depth != 0U &&
        sCameraObstructionRuntime.slots[physical_slot].selected &&
        sCameraObstructionRuntime.slots[physical_slot].resolved_valid &&
        !sCameraObstructionRuntime.slots[physical_slot].query_source_degraded) {
        return &sCameraObstructionRuntime.resolved_cameras[physical_slot];
    }
    return &gCameras[physical_slot];
}

const Camera *camera_obstruction_snapshot_camera_for_slot(s32 physical_slot) {
    if (physical_slot < 0 || physical_slot >= MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT) {
        return NULL;
    }
    if (sCameraObstructionRuntime.slots[physical_slot].selected &&
        sCameraObstructionRuntime.slots[physical_slot].resolved_valid &&
        !sCameraObstructionRuntime.slots[physical_slot].query_source_degraded) {
        return &sCameraObstructionRuntime.resolved_cameras[physical_slot];
    }
    return &gCameras[physical_slot];
}

int camera_obstruction_snapshot_discontinuous(s32 physical_slot) {
    return physical_slot >= 0 && physical_slot < MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT &&
           sCameraObstructionRuntime.slots[physical_slot].presentation_discontinuity;
}

int camera_obstruction_projection_matches_render(
    s32 viewport, s32 physical_slot, uint64_t generation) {
    const MdkrCameraObstructionObserveSlot *observe;

    if (sCameraObstructionRuntime.presentation_depth == 0U) {
        return TRUE;
    }
    if (viewport < 0 || viewport >= 4 || physical_slot < 0 ||
        physical_slot >= MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT) {
        sCameraObstructionRuntime.projection_mismatch_violations++;
        return FALSE;
    }
    observe = &sCameraObstructionRuntime.slots[physical_slot];
    if (!observe->selected || observe->viewport != viewport ||
        observe->render_projection.generation != generation ||
        observe->render_projection.camera_id != physical_slot) {
        sCameraObstructionRuntime.projection_mismatch_violations++;
        return FALSE;
    }
    return TRUE;
}

int camera_obstruction_racer_opacity_for_viewport(s32 viewport) {
    s32 physical_slot;
    const MdkrCameraObstructionObserveSlot *observe;

    if (viewport < 0 || viewport >= 4 ||
        !sCameraObstructionRuntime.viewport_slot_valid[viewport]) {
        return 255;
    }
    physical_slot = sCameraObstructionRuntime.last_physical_slot_by_viewport[viewport];
    if (physical_slot < 0 ||
        physical_slot >= MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT) {
        return 255;
    }
    observe = &sCameraObstructionRuntime.slots[physical_slot];
    if (!observe->selected || observe->viewport != viewport) {
        return 255;
    }
    return observe->emergency_racer_opacity;
}

void camera_obstruction_runtime_reset(void) {
    s32 selected = 0;
    s32 validated = 0;
    s32 obstructed = 0;
    s32 slot;

    for (slot = 0; slot < MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT; slot++) {
        selected += sCameraObstructionRuntime.slots[slot].selected;
        validated += sCameraObstructionRuntime.slots[slot].last_validated_camera_valid;
        obstructed += sCameraObstructionRuntime.slots[slot].was_obstructed;
    }
    sCameraObstructionResetSerial++;
    if (sCameraObstructionResetSerial == 0U) {
        sCameraObstructionResetSerial = 1U;
    }
    if (getenv("MDKR_CAMERA_RESET_TRACE") != NULL) {
        fprintf(stderr,
                "[CAMERA-RESET] serial=%llu retired_tick=%llu selected=%d "
                "validated=%d obstructed=%d\n",
                (unsigned long long)sCameraObstructionResetSerial,
                (unsigned long long)sCameraObstructionRuntime.tick_serial,
                selected, validated, obstructed);
    }
    memset(&sCameraObstructionRuntime, 0, sizeof(sCameraObstructionRuntime));
    memset(sCameraIntents, 0, sizeof(sCameraIntents));
    sCameraIntentCaptureSerial = 0;
}

void camera_obstruction_tick(int update_rate_fields) {
    const uint64_t finalizer_started = camera_obstruction_perf_begin();
    const s32 viewport_count = camera_obstruction_viewport_count();
    const int cutscene_bank = check_if_showing_cutscene_camera() != 0;
    const int tt_viewport = camera_obstruction_tt_viewport_selected(viewport_count);
    const MdkrCameraObstructionRuntimePolicy runtime_policy = camera_obstruction_runtime_policy();
    const float field_rate = osTvType == OS_TV_TYPE_PAL ? 50.0f : 60.0f;
    const float fixed_delta_seconds = is_game_paused() || update_rate_fields <= 0 ? 0.0f :
        (float)update_rate_fields / field_rate;
    s32 viewport;
    s32 slot;
    s32 fresh_intents = 0;
    s32 stale_or_missing_intents = 0;
    s32 corrected = 0;
    s32 resolved_penetrated = 0;
    s32 resolved_invalid = 0;
    s32 source_degraded = 0;
    s32 dynamic_corrected = 0;
    s32 resolved_target_hidden = 0;
    s32 resolved_target_embedded = 0;
    s32 depenetrate_only = 0;
    s32 safety_only = 0;
    s32 elevated_emergency = 0;
    s32 transition_invoked = 0;
    s32 transition_clear = 0;
    s32 transition_cut = 0;
    s32 transition_tuple_cut = 0;
    s32 exact_runtime_invoked = 0;
    s32 exact_runtime_sphere_clear = 0;
    s32 exact_runtime_exact_clear = 0;
    s32 exact_runtime_exact_hit = 0;
    s32 exact_runtime_override = 0;
    s32 exact_runtime_degraded = 0;
    s32 exact_shadow_invoked = 0;
    s32 exact_shadow_sphere_clear = 0;
    s32 exact_shadow_exact_clear = 0;
    s32 exact_shadow_exact_hit = 0;
    s32 exact_shadow_degraded = 0;
    int trace_level;
    uint64_t publication_started;

    sCameraObstructionRuntime.tick_serial++;
    if (sCameraObstructionRuntime.tick_serial == 0U) {
        sCameraObstructionRuntime.tick_serial = 1U;
    }
    camera_obstruction_snapshot_authored_slots();
    publication_started = camera_obstruction_perf_begin();
    mdkr_camera_dynamic_occlusion_tick();
    camera_obstruction_perf_add(
        MDKR_CAMERA_PERF_DYNAMIC_PUBLICATION, publication_started);
    for (viewport = 0; viewport < viewport_count; viewport++) {
        const s32 normal_slot = camera_obstruction_normal_slot_for_viewport(viewport, viewport_count);
        const s32 physical_slot = normal_slot + (cutscene_bank ? 4 : 0);
        const uint64_t slot_started = camera_obstruction_perf_begin();

        camera_obstruction_observe_slot(
            viewport, normal_slot, physical_slot, cutscene_bank,
            !cutscene_bank, runtime_policy,
            fixed_delta_seconds);
        camera_obstruction_perf_add(MDKR_CAMERA_PERF_SLOT, slot_started);
    }
    if (tt_viewport) {
        const uint64_t slot_started = camera_obstruction_perf_begin();
        /* The 3P T.T. view masks cutscene state and uses normal camera slot 3. */
        camera_obstruction_observe_slot(
            viewport_count, PLAYER_FOUR, PLAYER_FOUR, FALSE, TRUE, runtime_policy,
            fixed_delta_seconds);
        camera_obstruction_perf_add(MDKR_CAMERA_PERF_SLOT, slot_started);
    }

    for (slot = 0; slot < MDKR_CAMERA_OBSTRUCTION_RUNTIME_SLOT_COUNT; slot++) {
        const MdkrCameraObstructionObserveSlot *observe = &sCameraObstructionRuntime.slots[slot];
        if (!observe->selected) {
            continue;
        }
        fresh_intents += observe->intent_fresh;
        stale_or_missing_intents += observe->intent_missing_or_stale;
        corrected += observe->correction_applied;
        dynamic_corrected += observe->correction_applied &&
            observe->blocker_stable_id >= UINT32_C(0x80000000);
        resolved_penetrated +=
            observe->resolved_stationary_status != MDKR_CAMERA_SWEEP_CLEAR;
        resolved_invalid += !observe->resolved_valid;
        source_degraded += observe->query_source_degraded;
        resolved_target_hidden += !observe->resolved_target_visible &&
            !observe->resolved_target_embedded;
        resolved_target_embedded += observe->resolved_target_embedded;
        /* Which profile each selected slot is under, so a reader can tell a
         * bounded depenetrate-only occlusion from a follow-camera defect, and
         * can see whether a route was measured under the shipped table. */
        depenetrate_only +=
            camera_obstruction_family_treatment(observe->intent.family) ==
                MDKR_CAMERA_OBSTRUCTION_TREATMENT_DEPENETRATE_ONLY;
        safety_only +=
            camera_obstruction_family_treatment(observe->intent.family) ==
                MDKR_CAMERA_OBSTRUCTION_TREATMENT_SAFETY_ONLY;
        elevated_emergency += observe->elevated_emergency;
        transition_invoked += observe->transition_invoked;
        transition_clear += observe->transition_clear;
        transition_cut += observe->transition_cut;
        transition_tuple_cut += observe->transition_tuple_cut;
        exact_runtime_invoked += observe->exact_runtime_invoked;
        exact_runtime_sphere_clear += observe->exact_runtime_sphere_clear;
        exact_runtime_exact_clear += observe->exact_runtime_clear;
        exact_runtime_exact_hit += observe->exact_runtime_hit;
        exact_runtime_override += observe->exact_runtime_override;
        exact_runtime_degraded += observe->exact_runtime_degraded;
        exact_shadow_invoked += observe->exact_shadow_invoked;
        exact_shadow_sphere_clear += observe->exact_shadow_outcome ==
            MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_SPHERE_CLEAR;
        exact_shadow_exact_clear += observe->exact_shadow_outcome ==
            MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_CLEAR;
        exact_shadow_exact_hit += observe->exact_shadow_outcome ==
            MDKR_CAMERA_OBSTRUCTION_TWO_PHASE_EXACT_HIT;
        exact_shadow_degraded += observe->exact_shadow_degraded;
    }
    if (camera_obstruction_perf_enabled() && viewport_count + tt_viewport <= 4) {
        sCameraPerfSelectedTicks[viewport_count + tt_viewport]++;
    }

    /* Stop before trace I/O: the census measures camera work, not diagnostics. */
    camera_obstruction_perf_add(MDKR_CAMERA_PERF_FINALIZER, finalizer_started);

    trace_level = camera_obstruction_trace_level();
    if (trace_level >= 1) {
        MdkrCameraDynamicOcclusionTelemetry dynamic_telemetry;
        mdkr_camera_dynamic_occlusion_get_telemetry(&dynamic_telemetry);
        fprintf(stderr,
                "camera_obstruction_observe summary tick=%llu selected=%d fresh_intents=%d stale_or_missing=%d "
                "tt=%d bank=%s gate=%s(logical_camera_unchanged) duplicates=%u "
                "projection_mismatches=%u "
                "resolved={corrected=%d penetrated=%d invalid=%d degraded=%d} "
                "target_hidden=%d target_embedded=%d depenetrate_only=%d safety_only=%d emergency=%d "
                "dynamic_corrected=%d "
                "transition={invoked=%d clear=%d cut=%d tuple_cut=%d} "
                "exact_runtime={invoked=%d sphere_clear=%d exact_clear=%d "
                "exact_hit=%d override=%d degraded=%d} "
                "exact_shadow={enabled=%d invoked=%d sphere_clear=%d exact_clear=%d "
                "exact_hit=%d degraded=%d} "
                "dynamic={published=%zu peak=%zu observed_doors=%zu observed_solids=%zu missing_cache=%zu "
                "missing_identity=%zu excluded_non_solid=%zu uncategorized=%zu "
                "invalid_transform=%zu capacity_failures=%zu transitioning_doors=%zu "
                "active_transitioning_doors=%zu temporal_moved=%zu temporal_proxy_hits=%zu "
                "sphere_hits=%zu exact_hits=%zu sphere_fallbacks=%zu exact_fallbacks=%zu invalid_sweeps=%zu "
                "sphere_invalid_sweeps=%zu exact_invalid_sweeps=%zu "
                "bytes=%zu}\n",
                (unsigned long long)sCameraObstructionRuntime.tick_serial,
                viewport_count + tt_viewport, fresh_intents, stale_or_missing_intents,
                tt_viewport, cutscene_bank ? "cutscene" : "normal",
                runtime_policy == MDKR_CAMERA_RUNTIME_MODERN ? "MODERN" :
                runtime_policy == MDKR_CAMERA_RUNTIME_CENTER_RAY ? "CENTER_RAY" :
                runtime_policy == MDKR_CAMERA_RUNTIME_LEGACY ? "LEGACY" : "OBSERVE",
                sCameraObstructionRuntime.duplicate_solve_violations,
                sCameraObstructionRuntime.projection_mismatch_violations,
                corrected, resolved_penetrated, resolved_invalid, source_degraded,
                resolved_target_hidden, resolved_target_embedded,
                depenetrate_only, safety_only,
                elevated_emergency,
                dynamic_corrected,
                transition_invoked, transition_clear,
                transition_cut, transition_tuple_cut,
                exact_runtime_invoked, exact_runtime_sphere_clear,
                exact_runtime_exact_clear, exact_runtime_exact_hit,
                exact_runtime_override, exact_runtime_degraded,
                camera_obstruction_exact_shadow_enabled(), exact_shadow_invoked,
                exact_shadow_sphere_clear, exact_shadow_exact_clear,
                exact_shadow_exact_hit, exact_shadow_degraded,
                dynamic_telemetry.published_instance_count,
                dynamic_telemetry.peak_instance_count,
                dynamic_telemetry.hard_door_instance_count,
                dynamic_telemetry.hard_solid_instance_count,
                dynamic_telemetry.missing_cache_count,
                dynamic_telemetry.missing_identity_count,
                dynamic_telemetry.excluded_non_solid_count,
                dynamic_telemetry.uncategorized_model_count,
                dynamic_telemetry.invalid_transform_count,
                dynamic_telemetry.capacity_failure_count,
                dynamic_telemetry.transitioning_door_instance_count,
                dynamic_telemetry.current_transitioning_door_count,
                dynamic_telemetry.temporal_moved_instance_count,
                dynamic_telemetry.temporal_proxy_hit_count,
                dynamic_telemetry.sphere_hit_count,
                dynamic_telemetry.exact_hit_count,
                dynamic_telemetry.sphere_conservative_fallback_count,
                dynamic_telemetry.exact_conservative_fallback_count,
                dynamic_telemetry.invalid_sweep_count,
                dynamic_telemetry.sphere_invalid_sweep_count,
                dynamic_telemetry.exact_invalid_sweep_count,
                dynamic_telemetry.allocation_bytes);
    }
}

void camera_obstruction_motion_summary(void) {
    size_t index;
    double per_thousand;

    if (!camera_obstruction_motion_enabled()) {
        return;
    }
    per_thousand = sCameraMotion.sampled_slot_ticks == 0U ? 0.0 :
        (double)sCameraMotion.discontinuities * 1000.0 /
            (double)sCameraMotion.sampled_slot_ticks;
    fprintf(stderr,
            "camera_motion summary slot_ticks=%llu ticks=%llu cut_ticks=%llu "
            "profiles={full=%llu safety_only=%llu depenetrate_only=%llu} "
            "events={retract=%llu recovery=%llu alternate_entry=%llu "
            "alternate_exit=%llu emergency_entry=%llu discontinuity=%llu "
            "degenerate=%llu} "
            "release_hold={held_ticks=%llu spans=%llu window=%u} "
            "chatter={oscillation_cycles=%llu oscillation_cycles_excused=%llu "
            "correction_reengagements=%llu} "
            "shoulder={flips=%llu continuous_surface=%llu new_surface=%llu} "
            "churn={blocker_changes=%llu same_surface=%llu new_surface=%llu} "
            "emergency={max_dwell=%llu} "
            "discontinuity_per_1000_ticks=%.4f\n",
            (unsigned long long)sCameraMotion.sampled_slot_ticks,
            (unsigned long long)sCameraMotion.sampled_ticks,
            (unsigned long long)sCameraMotion.cut_ticks,
            (unsigned long long)sCameraMotion.profile_slot_ticks
                [MDKR_CAMERA_OBSTRUCTION_TREATMENT_FULL],
            (unsigned long long)sCameraMotion.profile_slot_ticks
                [MDKR_CAMERA_OBSTRUCTION_TREATMENT_SAFETY_ONLY],
            (unsigned long long)sCameraMotion.profile_slot_ticks
                [MDKR_CAMERA_OBSTRUCTION_TREATMENT_DEPENETRATE_ONLY],
            (unsigned long long)sCameraMotion.retract_events,
            (unsigned long long)sCameraMotion.recovery_events,
            (unsigned long long)sCameraMotion.alternate_entries,
            (unsigned long long)sCameraMotion.alternate_exits,
            (unsigned long long)sCameraMotion.emergency_entries,
            (unsigned long long)sCameraMotion.discontinuities,
            (unsigned long long)sCameraMotion.degenerate_ticks,
            (unsigned long long)sCameraMotion.release_hold_ticks,
            (unsigned long long)sCameraMotion.release_hold_spans,
            (unsigned)MDKR_CAMERA_OBSTRUCTION_RELEASE_HOLD_TICKS,
            (unsigned long long)sCameraMotion.oscillation_cycles,
            (unsigned long long)sCameraMotion.oscillation_cycles_excused,
            (unsigned long long)sCameraMotion.correction_reengagements,
            (unsigned long long)sCameraMotion.shoulder_flips,
            (unsigned long long)sCameraMotion.shoulder_flips_continuous_surface,
            (unsigned long long)sCameraMotion.shoulder_flips_new_surface,
            (unsigned long long)sCameraMotion.blocker_changes,
            (unsigned long long)sCameraMotion.blocker_changes_same_surface,
            (unsigned long long)sCameraMotion.blocker_changes_new_surface,
            (unsigned long long)sCameraMotion.max_emergency_dwell,
            per_thousand);
    /*
     * One row per analog metric. These are BASELINE measurements: no numeric
     * threshold is asserted anywhere on them yet. Section 7.3's signed review
     * sets those from exactly these distributions. min/mean/max are exact; p95
     * is the upper edge of a log-spaced bin (~4.4% wide), clamped to max.
     */
    for (index = 0U; index < MDKR_CAMERA_MOTION_STAT_COUNT; index++) {
        const MdkrCameraMotionStat *stat = &sCameraMotion.stats[index];

        fprintf(stderr,
                "camera_motion stat name=%s unit=%s samples=%llu "
                "min=%.6f mean=%.6f p95=%.6f max=%.6f\n",
                kCameraMotionStatNames[index], kCameraMotionStatUnits[index],
                (unsigned long long)stat->count,
                stat->count == 0U ? 0.0 : stat->min,
                stat->count == 0U ? 0.0 : stat->total / (double)stat->count,
                camera_obstruction_motion_percentile(stat, 95U),
                stat->count == 0U ? 0.0 : stat->max);
    }
    fflush(stderr);
}

void camera_obstruction_perf_summary(void) {
    static const char *const names[MDKR_CAMERA_PERF_SECTION_COUNT] = {
        "finalizer", "slot", "static_query", "dynamic_query", "dynamic_publication",
        "exact_static_query", "exact_dynamic_query",
    };
    size_t section;
    MdkrCameraDynamicOcclusionTelemetry dynamic_telemetry;
    MdkrTrackOcclusionTelemetry track_telemetry;

    if (!camera_obstruction_perf_enabled()) {
        return;
    }
    mdkr_camera_dynamic_occlusion_get_telemetry(&dynamic_telemetry);
    mdkr_track_occlusion_get_telemetry(&track_telemetry);
    fprintf(stderr,
            "[CAMERAPERF-RUN] selected1=%llu selected2=%llu selected3=%llu "
            "selected4=%llu dynamic_bytes=%zu\n",
            (unsigned long long)sCameraPerfSelectedTicks[1],
            (unsigned long long)sCameraPerfSelectedTicks[2],
            (unsigned long long)sCameraPerfSelectedTicks[3],
            (unsigned long long)sCameraPerfSelectedTicks[4],
            dynamic_telemetry.allocation_bytes);
    fprintf(stderr,
            "[CAMERA-SPHERE-WORK] dynamic={sweeps=%zu instances=%zu nodes=%zu "
            "rejected_nodes=%zu retained_chunks=%zu chunk_triangles=%zu "
            "fallbacks=%zu invalid=%zu "
            "max_instances=%zu max_nodes=%zu max_retained_chunks=%zu "
            "max_chunk_triangles=%zu}\n",
            dynamic_telemetry.sphere_sweep_count,
            dynamic_telemetry.sphere_broadphase_instance_count,
            dynamic_telemetry.sphere_node_visited_count,
            dynamic_telemetry.sphere_node_rejected_count,
            dynamic_telemetry.sphere_chunk_retained_count,
            dynamic_telemetry.sphere_chunk_triangle_count,
            dynamic_telemetry.sphere_conservative_fallback_count,
            dynamic_telemetry.sphere_invalid_sweep_count,
            dynamic_telemetry.sphere_max_instances_per_sweep,
            dynamic_telemetry.sphere_max_nodes_visited_per_sweep,
            dynamic_telemetry.sphere_max_chunks_retained_per_sweep,
            dynamic_telemetry.sphere_max_chunk_triangles_per_sweep);
    fprintf(stderr,
            "[CAMERA-EXACT-WORK] track={sweeps=%llu segments=%llu candidates=%llu "
            "analytic=%llu analytic_miss=%llu bounded=%llu exhausted=%llu "
            "stationary=%llu advance=%llu "
            "refine=%llu fallbacks=%llu samples=%llu "
            "ambiguous=%llu revalidate=%llu invalid=%llu max_candidates=%llu "
            "max_stationary=%llu} dynamic={sweeps=%zu instances=%zu model_triangles=%zu "
            "nodes=%zu rejected_nodes=%zu retained_chunks=%zu chunk_triangles=%zu "
            "aabb_rejected=%zu narrowed=%zu stationary=%zu invalid=%zu "
            "max_instances=%zu max_model_triangles=%zu max_single_model_triangles=%zu "
            "max_nodes=%zu max_retained_chunks=%zu max_chunk_triangles=%zu "
            "max_narrowed=%zu max_stationary=%zu}\n",
            (unsigned long long)track_telemetry.exact_sweep_count,
            (unsigned long long)track_telemetry.exact_segment_candidate_count,
            (unsigned long long)track_telemetry.exact_triangle_candidate_count,
            (unsigned long long)track_telemetry.exact_analytic_sat_count,
            (unsigned long long)track_telemetry.exact_analytic_revalidation_miss_count,
            (unsigned long long)track_telemetry.exact_bounded_interval_test_count,
            (unsigned long long)track_telemetry.exact_bounded_interval_exhaustion_count,
            (unsigned long long)track_telemetry.exact_stationary_test_count,
            (unsigned long long)track_telemetry.exact_advance_iteration_count,
            (unsigned long long)track_telemetry.exact_refinement_test_count,
            (unsigned long long)track_telemetry.exact_interval_fallback_count,
            (unsigned long long)track_telemetry.exact_interval_sample_count,
            (unsigned long long)track_telemetry.exact_ambiguous_interval_count,
            (unsigned long long)track_telemetry.exact_publication_revalidation_count,
            (unsigned long long)track_telemetry.exact_invalid_sweep_count,
            (unsigned long long)track_telemetry.exact_max_triangle_candidates_per_sweep,
            (unsigned long long)track_telemetry.exact_max_stationary_tests_per_sweep,
            dynamic_telemetry.exact_sweep_count,
            dynamic_telemetry.exact_broadphase_instance_count,
            dynamic_telemetry.exact_model_triangle_count,
            dynamic_telemetry.exact_node_visited_count,
            dynamic_telemetry.exact_node_rejected_count,
            dynamic_telemetry.exact_chunk_retained_count,
            dynamic_telemetry.exact_chunk_triangle_count,
            dynamic_telemetry.exact_triangle_aabb_rejected_count,
            dynamic_telemetry.exact_triangle_narrowed_count,
            dynamic_telemetry.exact_stationary_test_count,
            dynamic_telemetry.exact_invalid_sweep_count,
            dynamic_telemetry.exact_max_instances_per_sweep,
            dynamic_telemetry.exact_max_model_triangles_per_sweep,
            dynamic_telemetry.exact_max_single_model_triangles,
            dynamic_telemetry.exact_max_nodes_visited_per_sweep,
            dynamic_telemetry.exact_max_chunks_retained_per_sweep,
            dynamic_telemetry.exact_max_chunk_triangles_per_sweep,
            dynamic_telemetry.exact_max_narrowed_triangles_per_sweep,
            dynamic_telemetry.exact_max_stationary_tests_per_sweep);
    for (section = 0U; section < MDKR_CAMERA_PERF_SECTION_COUNT; section++) {
        const MdkrCameraPerfHistogram *histogram = &sCameraPerf[section];
        fprintf(stderr,
                "[CAMERAPERF] section=%s hits=%llu total_ns=%llu min_ns=%llu "
                "mean_ns=%llu p50_ns=%llu p95_ns=%llu p99_ns=%llu max_ns=%llu "
                "overflow=%llu over_833333ns=%llu over_1666667ns=%llu "
                "bin_width_ns=%llu\n",
                names[section],
                (unsigned long long)histogram->hits,
                (unsigned long long)histogram->total_ns,
                (unsigned long long)histogram->min_ns,
                (unsigned long long)(histogram->hits != 0U ?
                    histogram->total_ns / histogram->hits : 0U),
                (unsigned long long)camera_obstruction_perf_percentile(histogram, 50U),
                (unsigned long long)camera_obstruction_perf_percentile(histogram, 95U),
                (unsigned long long)camera_obstruction_perf_percentile(histogram, 99U),
                (unsigned long long)histogram->max_ns,
                (unsigned long long)histogram->overflow,
                (unsigned long long)histogram->over_p99_budget,
                (unsigned long long)histogram->over_tail_budget,
                (unsigned long long)MDKR_CAMERA_PERF_BIN_WIDTH_NS);
    }
    fflush(stderr);
}

#else

void camera_obstruction_runtime_reset(void) {
}

void camera_obstruction_tick(int update_rate_fields) {
    (void)update_rate_fields;
}

void camera_obstruction_perf_summary(void) {
}

void camera_obstruction_motion_summary(void) {
}

#endif
