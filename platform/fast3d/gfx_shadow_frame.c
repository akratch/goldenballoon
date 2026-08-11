#include "gfx_shadow_frame.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GFX_SHADOW_INITIAL_VERTICES 4096u
#define GFX_SHADOW_INITIAL_RANGES 256u
#define GFX_SHADOW_INITIAL_MATRICES 256u
#define GFX_SHADOW_MAX_VERTICES (1024u * 1024u)
#define GFX_SHADOW_MAX_RANGES (128u * 1024u)
#define GFX_SHADOW_MAX_MATRICES (16u * 1024u)
#define GFX_SHADOW_MAX_STATIC_KEYS (256u * 1024u)
#define GFX_SHADOW_MAX_PROJECTED_RANGES (64u * 1024u)

typedef struct GfxShadowMatrixEntry {
    const void *key;
    GfxShadowMatrixBinding binding;
    /*
     * Presentation-replay context (Phase 3 Wave B). The registry alone cannot
     * tell which camera an entry belongs to, and it must: gViewProjMatrixF is
     * not one matrix per frame. cam_build_view_basis() installs the gameplay
     * camera's VP, but mtx_perspective() overwrites it with a FIXED
     * z=-281 transform for the 2D/menu path and mtx_ortho() replaces it with
     * the ortho matrix outright. Substituting an interpolated gameplay camera
     * into entries registered under one of those would warp the HUD and the
     * menus. camera.c therefore tags each registration with the viewport it
     * was emitted for and whether the gameplay VP was the one in force.
     */
    int viewport;
    bool gameplay_vp;
} GfxShadowMatrixEntry;

typedef struct GfxShadowStaticKey {
    const void *source;
    /*
     * Where this key's three admitted vertices live in the stage cache. The
     * cache dedups by ADDRESS, so this is the only record of what the address
     * held when it was admitted — and therefore the only way to notice that it
     * no longer holds it (see stale_casters in GfxWorldFxStats).
     */
    size_t first_vertex;
    bool occupied;
} GfxShadowStaticKey;

typedef struct GfxShadowStaticCache {
    uint64_t stage_generation;
    GfxShadowVertex *vertices;
    size_t vertex_count;
    size_t vertex_capacity;
    GfxShadowRange *ranges;
    size_t range_count;
    size_t range_capacity;
    GfxShadowStaticKey *keys;
    size_t key_count;
    size_t key_capacity;
    /*
     * World-space AABB over every cached static triangle. Static ranges render
     * into every view's every cascade, including geometry the game CPU-culled
     * this frame, so the cascade planner must extend its caster depth range
     * over the whole cache — not only over the triangles observed this frame.
     * Without this, a stale static caster falls outside the planned light
     * z-range and is depth-clamped onto the light near plane on GL (a
     * full-strength phantom shadow with no visible source) while WebGPU clips
     * it away entirely.
     */
    float bounds_min[3];
    float bounds_max[3];
    /* False until the first admitted triangle seeds the AABB, so a shutdown
     * memset (or an admission before the first stage begin) cannot leak a
     * zero "bound" into the fold. */
    bool bounds_valid;
    /* Range count at the last successful commit. Ranges at or below this
     * watermark are visible to the published frame and must stay immutable;
     * ranges created after it may still merge, so static admissions do not
     * degenerate into one draw call per triangle. */
    size_t committed_range_count;
} GfxShadowStaticCache;

/*
 * Finite-but-implausible vertices are rejected before they can enter the
 * caster feed or the stage AABB: DKR worlds live within ~15k units of the
 * origin, and the stage AABB is fold-only until the next level load, so a
 * single wild vertex (an aliased cache entry, a corrupted transform) would
 * otherwise collapse shadow depth resolution for the rest of the stage with
 * no recovery and no telemetry.
 */
#define GFX_SHADOW_WORLD_LIMIT 250000.0f

static GfxShadowFrame s_frames[2];
static unsigned s_read_index;
static unsigned s_write_index = 1;
static uint64_t s_generation;

static GfxShadowMatrixEntry *s_matrix_entries;
static size_t s_matrix_count;
static size_t s_matrix_capacity;
static GfxShadowStaticCache s_static_cache;
static const void **s_projected_ranges;
static size_t s_projected_range_count;
static size_t s_projected_range_capacity;
static const void **s_excluded_casters;
static size_t s_excluded_caster_count;
static size_t s_excluded_caster_capacity;

static GfxWorldFxStats s_stats;
static int s_trace_enabled = -1;
static int s_matrix_control_drop = -1;
static bool s_capture_active;

/*
 * ---- presentation-replay freeze (Phase 3 Wave B, design §2) --------------
 *
 * THE LIFETIME PROBLEM. Registration happens only while the GAME builds its
 * display list (camera.c's mtx_perspective/mtx_cam_push), never during an HLE
 * walk, and gfx_end_frame() resets the registry as soon as that tick's one
 * real walk has consumed it. A bare second gfx_run() of the same display list
 * therefore finds an EMPTY registry and silently loses every world/VP
 * decomposition. The freeze below takes the copy at reset time — the last
 * instant the entries exist — so a replay can restore them.
 *
 * The projected-range and excluded-caster marks share that lifetime and that
 * reset, and dkr_sp_polygon reads the former during the walk. They are frozen
 * with the matrices or the replay would take a different branch than the real
 * walk took.
 */
static GfxShadowMatrixEntry *s_frozen_matrices;
static size_t s_frozen_matrix_count;
static size_t s_frozen_matrix_capacity;
static const void **s_frozen_projected_ranges;
static size_t s_frozen_projected_range_count;
static size_t s_frozen_projected_range_capacity;
static const void **s_frozen_excluded_casters;
static size_t s_frozen_excluded_caster_count;
static size_t s_frozen_excluded_caster_capacity;
/*
 * The LIVE registry the replay displaces.
 *
 * A presentation replay runs from the video-queue branch — after the game has
 * already built the NEXT tick's display list, which means it has already
 * registered that list's matrices and marked its projected ranges. Restoring
 * the frozen copy over them and then resetting would throw the next tick's
 * registrations away, and gfx_end_frame would freeze an empty registry from
 * then on. (Measured: 8619 gameplay entries per 1198 walks at the slice-1 call
 * site collapsed to 33 at the slice-2 call site.) So restore SAVES what it
 * displaces and release PUTS IT BACK.
 */
static GfxShadowMatrixEntry *s_displaced_matrices;
static size_t s_displaced_matrix_count;
static size_t s_displaced_matrix_capacity;
static const void **s_displaced_projected_ranges;
static size_t s_displaced_projected_range_count;
static size_t s_displaced_projected_range_capacity;
static const void **s_displaced_excluded_casters;
static size_t s_displaced_excluded_caster_count;
static size_t s_displaced_excluded_caster_capacity;
static bool s_displaced_held;
static bool s_frozen_valid;
static uint64_t s_freeze_count;
static uint64_t s_restore_count;
static uint64_t s_freeze_failures;
/* Restores/releases that hit an allocation failure and had to unwind. A
 * non-zero value means at least one present skipped its replay AND at least one
 * frame's registrations were dropped -- see replay_restore_unwind. */
static uint64_t s_restore_failures;
static GfxShadowCameraEndpointStats s_camera_endpoint_stats;
static int s_camera_endpoint_observer = -1;
static struct {
    bool valid;
    int camera_id;
    uint64_t authored_tick;
    float view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM];
} s_camera_next_endpoint[GFX_SHADOW_MAX_VIEWS];

static bool s_capture_suppressed;

/*
 * Which published caster frame the tick's REAL walk consumed.
 *
 * gfx_end_frame commits this tick's casters and swaps the read index, so by the
 * time a presentation replay runs, gfx_shadow_frame_previous() has already
 * moved on to a frame the real walk never saw. Replaying against it renders the
 * scene under a shadow map one tick ahead of the geometry carrying it, and the
 * redraw is no longer the same picture — found by the slice-1 zero-delta pixel
 * gate, which measured ~8k of 307k pixels off, over 7k of them by exactly 1.
 */
static unsigned s_replay_read_index;
static bool s_replay_hold_previous;

/* Registration context, published by camera.c before each register call. */
static int s_register_viewport;
static bool s_register_gameplay_vp;
static int s_register_site;
/*
 * Whether the NEXT registration's key points at a full Mtx image whose bytes
 * may be copied for the stale-tenant check.
 *
 * The registry keys by pointer and is happy to key by anything; the unit tests
 * register 4-byte int keys. Only camera.c registers real 64-byte Mtx
 * allocations, and only camera.c calls gfx_shadow_matrix_set_site(), so the
 * setter doubles as the opt-in. Consumed and cleared by every registration so
 * one tagged call can never license a byte copy off the next, untagged one.
 */
static bool s_register_key_is_mtx;
/* See GfxShadowMatrixBinding.camera_locked. Consumed and cleared by every
 * registration the same way s_register_site is. */
static bool s_register_camera_locked;
static GfxPresentationMatrixOwner s_register_presentation_owner;
static bool s_register_presentation_owner_valid;
static GfxPresentationOwnerStats s_presentation_owner_stats;

typedef struct GfxShadowCameraVpSemantic {
    uint64_t authored_tick;
    int viewport;
    int camera_id;
    float view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM];
} GfxShadowCameraVpSemantic;

static bool camera_endpoint_observer_enabled(void) {
    if (s_camera_endpoint_observer < 0) {
        const char *value = getenv("MDKR_TEST_CAMERA_VP_ENDPOINTS");
        s_camera_endpoint_observer =
            value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    return s_camera_endpoint_observer != 0;
}

static uint64_t camera_endpoint_hash(
    uint64_t hash, const void *bytes, size_t size) {
    const uint8_t *cursor = (const uint8_t *)bytes;
    for (size_t index = 0; index < size; index++) {
        hash ^= cursor[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void camera_endpoint_hash_pair(
    uint64_t *expected_hash, uint64_t *actual_hash,
    const GfxShadowCameraVpSemantic *expected,
    const GfxShadowCameraVpSemantic *actual) {
    if (*expected_hash == 0u) {
        *expected_hash = UINT64_C(1469598103934665603);
        *actual_hash = UINT64_C(1469598103934665603);
    }
    *expected_hash = camera_endpoint_hash(
        *expected_hash, expected, sizeof(*expected));
    *actual_hash = camera_endpoint_hash(
        *actual_hash, actual, sizeof(*actual));
}

static void camera_endpoint_observe(
    int viewport, const float captured[4][4],
    const GfxShadowReplayViewProjection *override) {
    GfxShadowCameraVpSemantic expected;
    GfxShadowCameraVpSemantic actual;

    if (!camera_endpoint_observer_enabled() || override == NULL ||
        !override->valid || override->denominator == 0u || viewport < 0 ||
        viewport >= GFX_SHADOW_MAX_VIEWS) {
        return;
    }
    memset(&expected, 0, sizeof(expected));
    expected.viewport = viewport;
    expected.camera_id = override->camera_id;
    expected.authored_tick = override->authored_tick;
    memcpy(expected.view_projection, captured,
           sizeof(expected.view_projection));

    actual = expected;
    memcpy(actual.view_projection, override->view_projection,
           sizeof(actual.view_projection));

    if (override->numerator == 0u) {
        GfxShadowCameraVpSemantic mutated;
        uint32_t bits;

        s_camera_endpoint_stats.alpha_zero_checks++;
        camera_endpoint_hash_pair(
            &s_camera_endpoint_stats.alpha_zero_expected_hash,
            &s_camera_endpoint_stats.alpha_zero_actual_hash,
            &expected, &actual);
        if (memcmp(&expected, &actual, sizeof(expected)) != 0) {
            s_camera_endpoint_stats.alpha_zero_mismatches++;
            fprintf(stderr,
                    "[CAMERA-VP-MISMATCH] viewport=%d camera=%d tick=%llu",
                    viewport, override->camera_id,
                    (unsigned long long)override->authored_tick);
            for (int row = 0; row < 4; row++) {
                for (int column = 0; column < 4; column++) {
                    uint32_t expected_bits;
                    uint32_t actual_bits;
                    memcpy(&expected_bits, &captured[row][column],
                           sizeof(expected_bits));
                    memcpy(&actual_bits,
                           &override->view_projection[row][column],
                           sizeof(actual_bits));
                    if (expected_bits != actual_bits) {
                        fprintf(stderr, " m%d%d=%08x/%08x", row, column,
                                expected_bits, actual_bits);
                    }
                }
            }
            fputc('\n', stderr);
        }

        if (s_camera_next_endpoint[viewport].valid &&
            s_camera_next_endpoint[viewport].camera_id ==
                override->camera_id &&
            s_camera_next_endpoint[viewport].authored_tick ==
                override->authored_tick) {
            GfxShadowCameraVpSemantic next_expected;
            memset(&next_expected, 0, sizeof(next_expected));
            next_expected.viewport = viewport;
            next_expected.camera_id =
                s_camera_next_endpoint[viewport].camera_id;
            next_expected.authored_tick =
                s_camera_next_endpoint[viewport].authored_tick;
            memcpy(next_expected.view_projection,
                   s_camera_next_endpoint[viewport].view_projection,
                   sizeof(next_expected.view_projection));
            s_camera_endpoint_stats.next_endpoint_checks++;
            camera_endpoint_hash_pair(
                &s_camera_endpoint_stats.next_expected_hash,
                &s_camera_endpoint_stats.next_actual_hash,
                &next_expected, &expected);
            if (memcmp(&next_expected, &expected, sizeof(expected)) != 0) {
                s_camera_endpoint_stats.next_endpoint_mismatches++;
            }
        }

        /* Built-in negative control: one flipped matrix bit must be rejected by
         * the exact same semantic comparator used above. This mutates only a
         * stack copy and can never reach the renderer. */
        mutated = expected;
        memcpy(&bits, &mutated.view_projection[0][0], sizeof(bits));
        bits ^= 1u;
        memcpy(&mutated.view_projection[0][0], &bits, sizeof(bits));
        if (memcmp(&expected, &mutated, sizeof(expected)) != 0) {
            s_camera_endpoint_stats.mutation_control_rejections++;
        }

        s_camera_next_endpoint[viewport].valid = override->next_valid;
        if (override->next_valid) {
            s_camera_next_endpoint[viewport].camera_id = override->camera_id;
            s_camera_next_endpoint[viewport].authored_tick =
                override->next_authored_tick;
            memcpy(s_camera_next_endpoint[viewport].view_projection,
                   override->next_view_projection,
                   sizeof(s_camera_next_endpoint[viewport].view_projection));
        }
    } else if (override->numerator < override->denominator) {
        s_camera_endpoint_stats.midpoint_checks++;
        if (override->next_valid &&
            memcmp(override->view_projection, captured,
                   sizeof(override->view_projection)) != 0 &&
            memcmp(override->view_projection, override->next_view_projection,
                   sizeof(override->view_projection)) != 0) {
            s_camera_endpoint_stats.midpoint_moved++;
        }
        if (s_camera_endpoint_stats.midpoint_hash == 0u) {
            s_camera_endpoint_stats.midpoint_hash =
                UINT64_C(1469598103934665603);
        }
        s_camera_endpoint_stats.midpoint_hash = camera_endpoint_hash(
            s_camera_endpoint_stats.midpoint_hash, &actual, sizeof(actual));
    }
}

static bool checked_add_size(size_t left, size_t right, size_t *out) {
    if (out == NULL || left > SIZE_MAX - right) {
        return false;
    }
    *out = left + right;
    return true;
}

static bool grow_array(
    void **storage,
    size_t *capacity,
    size_t need,
    size_t initial,
    size_t maximum,
    size_t element_size) {
    size_t next;
    void *grown;

    if (storage == NULL || capacity == NULL || element_size == 0 ||
        need > maximum) {
        return false;
    }
    if (*capacity >= need) {
        return true;
    }
    next = *capacity != 0 ? *capacity : initial;
    while (next < need) {
        if (next > maximum / 2) {
            next = maximum;
        } else {
            next *= 2;
        }
        if (next < need && next == maximum) {
            return false;
        }
    }
    if (next > SIZE_MAX / element_size) {
        return false;
    }
    grown = realloc(*storage, next * element_size);
    if (grown == NULL) {
        return false;
    }
    *storage = grown;
    *capacity = next;
    return true;
}

static size_t static_key_hash(const void *source) {
    uintptr_t value = (uintptr_t)source;
    value ^= value >> 17;
    value *= (uintptr_t)0xed5ad4bbu;
    value ^= value >> 11;
    return (size_t)value;
}

static bool static_keys_reserve(size_t need) {
    size_t next;
    GfxShadowStaticKey *replacement;

    if (need > GFX_SHADOW_MAX_STATIC_KEYS / 2) {
        return false;
    }
    if (s_static_cache.key_capacity != 0 &&
        need * 2 <= s_static_cache.key_capacity) {
        return true;
    }
    next = s_static_cache.key_capacity != 0
        ? s_static_cache.key_capacity * 2
        : GFX_SHADOW_INITIAL_RANGES;
    while (next < need * 2) {
        if (next > GFX_SHADOW_MAX_STATIC_KEYS / 2) {
            next = GFX_SHADOW_MAX_STATIC_KEYS;
            break;
        }
        next *= 2;
    }
    if (next < need * 2 ||
        next > SIZE_MAX / sizeof(*replacement)) {
        return false;
    }
    replacement = calloc(next, sizeof(*replacement));
    if (replacement == NULL) {
        return false;
    }
    for (size_t index = 0; index < s_static_cache.key_capacity; index++) {
        GfxShadowStaticKey entry = s_static_cache.keys[index];
        if (entry.occupied) {
            size_t slot = static_key_hash(entry.source) & (next - 1);
            while (replacement[slot].occupied) {
                slot = (slot + 1) & (next - 1);
            }
            replacement[slot] = entry;
        }
    }
    free(s_static_cache.keys);
    s_static_cache.keys = replacement;
    s_static_cache.key_capacity = next;
    return true;
}

/* Slot index of `source`, or SIZE_MAX when it is not cached. */
static size_t static_key_find(const void *source) {
    size_t slot;

    if (source == NULL || s_static_cache.key_capacity == 0) {
        return SIZE_MAX;
    }
    slot = static_key_hash(source) & (s_static_cache.key_capacity - 1);
    while (s_static_cache.keys[slot].occupied) {
        if (s_static_cache.keys[slot].source == source) {
            return slot;
        }
        slot = (slot + 1) & (s_static_cache.key_capacity - 1);
    }
    return SIZE_MAX;
}

static void static_key_insert(const void *source, size_t first_vertex) {
    size_t slot = static_key_hash(source) &
        (s_static_cache.key_capacity - 1);
    while (s_static_cache.keys[slot].occupied) {
        slot = (slot + 1) & (s_static_cache.key_capacity - 1);
    }
    s_static_cache.keys[slot].source = source;
    s_static_cache.keys[slot].first_vertex = first_vertex;
    s_static_cache.keys[slot].occupied = true;
    s_static_cache.key_count++;
}

/*
 * Tenancy: does the live triangle still match what this key was admitted with?
 *
 * A static caster is cached for the whole stage and drawn into every cascade
 * whether or not the game submits it again. If the arena address it is keyed on
 * comes to hold different geometry — freed and reallocated, or a fixed buffer
 * the game rewrites in place — the cache silently keeps casting the ORIGINAL
 * geometry: a shadow with no object under it, in a place the player can see no
 * reason for. Wave "shadowdeep" closed two instances of this by excluding the
 * known offenders at DL-build time; this is the general detector, and it can
 * only run on a HIT, where the address is provably live because the game is
 * drawing it this very frame.
 *
 * The comparison is on world-space positions, which are what the shadow map
 * consumes. Identical geometry re-submitted through the same static
 * (world-origin) matrix reproduces bit-identical floats, so the tolerance only
 * has to exclude nothing at all; it is set at one world unit purely so that a
 * future world matrix with a different-but-equivalent factorisation cannot
 * report a phantom. DKR worlds span ~15k units and the defect shape displaces
 * geometry by hundreds to thousands.
 */
#define GFX_SHADOW_TENANCY_EPSILON 1.0f

static bool static_key_tenancy_ok(size_t slot, const float positions[9]) {
    const GfxShadowStaticKey *key;
    float worst = 0.0f;

    if (slot == SIZE_MAX || positions == NULL) {
        return true;
    }
    key = &s_static_cache.keys[slot];
    if (s_static_cache.vertices == NULL ||
        s_static_cache.vertex_count < 3 ||
        key->first_vertex > s_static_cache.vertex_count - 3) {
        /* Cannot be checked (a truncated cache). Do not invent a violation
         * from missing evidence. */
        return true;
    }
    for (size_t vertex = 0; vertex < 3; vertex++) {
        const GfxShadowVertex *cached =
            &s_static_cache.vertices[key->first_vertex + vertex];
        for (size_t axis = 0; axis < 3; axis++) {
            float delta = positions[vertex * 3 + axis] - cached->position[axis];
            if (delta < 0.0f) {
                delta = -delta;
            }
            if (delta > worst) {
                worst = delta;
            }
        }
    }
    if (worst <= GFX_SHADOW_TENANCY_EPSILON) {
        return true;
    }
    s_stats.stale_casters++;
    if (worst > s_stats.stale_worst_delta) {
        s_stats.stale_worst_delta = worst;
    }
    return false;
}

/*
 * Re-seat a stale entry onto the geometry the address holds NOW.
 *
 * Leaving it alone would keep the phantom for the rest of the stage — the whole
 * point of noticing. Rewriting the cached vertices in place is sound for ranges
 * above the commit watermark and for ranges below it alike: the published frame
 * borrows the vertex ARRAY, and a caster that moved is a caster that moved. The
 * stage AABB is fold-only, so it can only stay a superset.
 */
static void static_key_reseat(size_t slot, const float positions[9]) {
    const GfxShadowStaticKey *key;

    if (slot == SIZE_MAX || positions == NULL) {
        return;
    }
    key = &s_static_cache.keys[slot];
    if (s_static_cache.vertices == NULL ||
        s_static_cache.vertex_count < 3 ||
        key->first_vertex > s_static_cache.vertex_count - 3) {
        return;
    }
    for (size_t vertex = 0; vertex < 3; vertex++) {
        GfxShadowVertex *cached =
            &s_static_cache.vertices[key->first_vertex + vertex];
        for (size_t axis = 0; axis < 3; axis++) {
            cached->position[axis] = positions[vertex * 3 + axis];
        }
    }
}

/* Fold one triangle's positions into the stage AABB. Fold-only: the bound can
 * grow but never shrink, so a frame already published against it stays a
 * subset. */
static void static_bounds_fold(const float positions[9]) {
    for (size_t vertex = 0; vertex < 3; vertex++) {
        for (size_t axis = 0; axis < 3; axis++) {
            float value = positions[vertex * 3 + axis];
            if (!s_static_cache.bounds_valid ||
                value < s_static_cache.bounds_min[axis]) {
                s_static_cache.bounds_min[axis] = value;
            }
            if (!s_static_cache.bounds_valid ||
                value > s_static_cache.bounds_max[axis]) {
                s_static_cache.bounds_max[axis] = value;
            }
        }
        s_static_cache.bounds_valid = true;
    }
}

static void sync_static_frame_references(void) {
    for (size_t index = 0; index < 2; index++) {
        if (s_frames[index].stage_generation ==
            s_static_cache.stage_generation) {
            /*
             * realloc may move the cache storage while the previous frame
             * still borrows it, so its pointers must follow. Its published
             * counts are immutable, however: a failed construction retains
             * the previous frame and must not expose newly admitted geometry.
             */
            s_frames[index].static_vertices = s_static_cache.vertices;
            s_frames[index].static_ranges = s_static_cache.ranges;
        }
    }
}

static bool finite_matrix(
    const float matrix[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM]) {
    if (matrix == NULL) {
        return false;
    }
    for (size_t row = 0; row < GFX_SHADOW_MATRIX_DIM; row++) {
        for (size_t column = 0; column < GFX_SHADOW_MATRIX_DIM; column++) {
            float value = matrix[row][column];
            if (!(value <= FLT_MAX && value >= -FLT_MAX)) {
                return false;
            }
        }
    }
    return true;
}

static bool finite_values(const float *values, size_t count) {
    if (values == NULL) {
        return false;
    }
    for (size_t index = 0; index < count; index++) {
        float value = values[index];
        if (!(value <= FLT_MAX && value >= -FLT_MAX)) {
            return false;
        }
    }
    return true;
}

static void frame_reset(GfxShadowFrame *frame) {
    if (frame == NULL) {
        return;
    }
    frame->generation = 0;
    frame->stage_generation = s_static_cache.stage_generation;
    frame->valid = false;
    frame->failed = false;
    frame->static_vertices = NULL;
    frame->static_vertex_count = 0;
    frame->static_ranges = NULL;
    frame->static_range_count = 0;
    frame->vertex_count = 0;
    frame->range_count = 0;
    frame->view_count = 0;
    memset(frame->views, 0, sizeof(frame->views));
}

static void frame_fail(GfxShadowFrame *frame) {
    if (frame == NULL || frame->failed) {
        return;
    }
    frame->failed = true;
    s_stats.allocation_failures++;
}

bool gfx_shadow_matrix_register(
    const void *key,
    const float world[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM],
    const float view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM],
    GfxShadowMobility mobility) {
    /* Consume the per-call registration tags up front: every return below,
     * including the failure paths, must leave them cleared or the next
     * untagged registration would inherit them. */
    const int site = s_register_site;
    const bool key_is_mtx = s_register_key_is_mtx;
    const bool camera_locked = s_register_camera_locked;
    const bool owner_valid = s_register_presentation_owner_valid;
    const GfxPresentationMatrixOwner owner = s_register_presentation_owner;
    s_register_site = GFX_SHADOW_SITE_UNKNOWN;
    s_register_key_is_mtx = false;
    s_register_camera_locked = false;
    s_register_presentation_owner_valid = false;
    memset(&s_register_presentation_owner, 0,
           sizeof(s_register_presentation_owner));
    if (s_matrix_control_drop < 0) {
        const char *control = getenv("MDKR_WORLD_FX_MATRIX_CONTROL");
        s_matrix_control_drop =
            control != NULL && strcmp(control, "drop") == 0;
    }
    if (s_matrix_control_drop != 0) {
        return false;
    }
    if (key == NULL || !finite_matrix(world) ||
        (mobility != GFX_SHADOW_MOBILITY_STATIC &&
         mobility != GFX_SHADOW_MOBILITY_DYNAMIC) ||
        !finite_matrix(view_projection)) {
        return false;
    }
    for (size_t index = 0; index < s_matrix_count; index++) {
        if (s_matrix_entries[index].key == key) {
            memcpy(s_matrix_entries[index].binding.world, world,
                   sizeof(s_matrix_entries[index].binding.world));
            memcpy(s_matrix_entries[index].binding.view_projection,
                   view_projection,
                   sizeof(s_matrix_entries[index].binding.view_projection));
            memcpy(s_matrix_entries[index].binding.captured_view_projection,
                   view_projection,
                   sizeof(s_matrix_entries[index]
                              .binding.captured_view_projection));
            s_matrix_entries[index].binding.mobility = mobility;
            s_matrix_entries[index].viewport = s_register_viewport;
            s_matrix_entries[index].gameplay_vp = s_register_gameplay_vp;
            s_matrix_entries[index].binding.viewport = s_register_viewport;
            s_matrix_entries[index].binding.gameplay_vp = s_register_gameplay_vp;
            s_matrix_entries[index].binding.site = site;
            s_matrix_entries[index].binding.camera_locked = camera_locked;
            s_matrix_entries[index].binding.key_bytes_valid = key_is_mtx;
            s_matrix_entries[index].binding.walked_key_bytes_valid = false;
            if (owner_valid) {
                s_matrix_entries[index].binding.presentation_owner = owner;
                s_matrix_entries[index].binding.presentation_owner.valid = true;
            } else {
                memset(&s_matrix_entries[index].binding.presentation_owner, 0,
                       sizeof(s_matrix_entries[index]
                                  .binding.presentation_owner));
            }
            if (key_is_mtx) {
                memcpy(s_matrix_entries[index].binding.key_bytes, key,
                       sizeof(s_matrix_entries[index].binding.key_bytes));
            }
            s_stats.matrix_registrations++;
            s_presentation_owner_stats.registrations++;
            if (!owner_valid) {
                s_presentation_owner_stats.unowned++;
            } else if (owner.matrix_class == GFX_PRESENTATION_MATRIX_ROOT) {
                s_presentation_owner_stats.roots++;
            } else if (owner.matrix_class == GFX_PRESENTATION_MATRIX_CHILD) {
                s_presentation_owner_stats.children++;
            } else if (owner.matrix_class == GFX_PRESENTATION_MATRIX_EFFECT) {
                s_presentation_owner_stats.effects++;
            } else {
                s_presentation_owner_stats.unowned++;
            }
            return true;
        }
    }
    if (!grow_array(
            (void **)&s_matrix_entries,
            &s_matrix_capacity,
            s_matrix_count + 1,
            GFX_SHADOW_INITIAL_MATRICES,
            GFX_SHADOW_MAX_MATRICES,
            sizeof(*s_matrix_entries))) {
        /*
         * A partial matrix registry produces a partial caster set. Poison the
         * active frame so projected decals remain the atomic fallback instead
         * of publishing an incomplete map as ready.
         */
        if (s_capture_active) {
            frame_fail(&s_frames[s_write_index]);
        } else {
            s_stats.allocation_failures++;
        }
        return false;
    }
    s_matrix_entries[s_matrix_count].key = key;
    memset(&s_matrix_entries[s_matrix_count].binding, 0,
           sizeof(s_matrix_entries[s_matrix_count].binding));
    memcpy(s_matrix_entries[s_matrix_count].binding.world, world,
           sizeof(s_matrix_entries[s_matrix_count].binding.world));
    memcpy(s_matrix_entries[s_matrix_count].binding.view_projection,
           view_projection,
           sizeof(s_matrix_entries[s_matrix_count].binding.view_projection));
    memcpy(s_matrix_entries[s_matrix_count].binding.captured_view_projection,
           view_projection,
           sizeof(s_matrix_entries[s_matrix_count]
                      .binding.captured_view_projection));
    s_matrix_entries[s_matrix_count].binding.mobility = mobility;
    s_matrix_entries[s_matrix_count].viewport = s_register_viewport;
    s_matrix_entries[s_matrix_count].gameplay_vp = s_register_gameplay_vp;
    s_matrix_entries[s_matrix_count].binding.viewport = s_register_viewport;
    s_matrix_entries[s_matrix_count].binding.gameplay_vp =
        s_register_gameplay_vp;
    s_matrix_entries[s_matrix_count].binding.site = site;
    s_matrix_entries[s_matrix_count].binding.camera_locked = camera_locked;
    s_matrix_entries[s_matrix_count].binding.key_bytes_valid = key_is_mtx;
    if (owner_valid) {
        s_matrix_entries[s_matrix_count].binding.presentation_owner = owner;
        s_matrix_entries[s_matrix_count].binding.presentation_owner.valid = true;
    } else {
        memset(&s_matrix_entries[s_matrix_count].binding.presentation_owner, 0,
               sizeof(s_matrix_entries[s_matrix_count]
                          .binding.presentation_owner));
    }
    if (key_is_mtx) {
        memcpy(s_matrix_entries[s_matrix_count].binding.key_bytes, key,
               sizeof(s_matrix_entries[s_matrix_count].binding.key_bytes));
    }
    s_matrix_count++;
    s_stats.matrix_registrations++;
    s_presentation_owner_stats.registrations++;
    if (!owner_valid) {
        s_presentation_owner_stats.unowned++;
    } else if (owner.matrix_class == GFX_PRESENTATION_MATRIX_ROOT) {
        s_presentation_owner_stats.roots++;
    } else if (owner.matrix_class == GFX_PRESENTATION_MATRIX_CHILD) {
        s_presentation_owner_stats.children++;
    } else if (owner.matrix_class == GFX_PRESENTATION_MATRIX_EFFECT) {
        s_presentation_owner_stats.effects++;
    } else {
        s_presentation_owner_stats.unowned++;
    }
    s_stats.matrix_entries = s_matrix_count;
    if (s_matrix_count > s_stats.matrix_peak) {
        s_stats.matrix_peak = s_matrix_count;
    }
    return true;
}

bool gfx_shadow_matrix_lookup(
    const void *key,
    GfxShadowMatrixBinding *out) {
    if (key == NULL || out == NULL) {
        s_stats.matrix_lookup_misses++;
        return false;
    }
    for (size_t index = s_matrix_count; index > 0; index--) {
        const GfxShadowMatrixEntry *entry = &s_matrix_entries[index - 1];
        if (entry->key == key) {
            *out = entry->binding;
            s_stats.matrix_lookup_hits++;
            return true;
        }
    }
    s_stats.matrix_lookup_misses++;
    return false;
}

bool gfx_shadow_matrix_note_walked_key(
    const void *key, const void *bytes, size_t size) {
    if (key == NULL || bytes == NULL ||
        size != sizeof(((GfxShadowMatrixBinding *)0)->walked_key_bytes)) {
        return false;
    }
    for (size_t index = s_matrix_count; index > 0; index--) {
        GfxShadowMatrixEntry *entry = &s_matrix_entries[index - 1];
        if (entry->key != key) {
            continue;
        }
        memcpy(entry->binding.walked_key_bytes, bytes,
               sizeof(entry->binding.walked_key_bytes));
        entry->binding.walked_key_bytes_valid = true;
        return true;
    }
    return false;
}

void gfx_shadow_matrix_registry_reset(void) {
    s_matrix_count = 0;
    s_stats.matrix_entries = 0;
}

void gfx_shadow_matrix_set_context(int viewport, bool gameplay_vp) {
    s_register_viewport = viewport;
    s_register_gameplay_vp = gameplay_vp;
}

void gfx_shadow_matrix_set_site(int site) {
    s_register_site = site;
    s_register_key_is_mtx = true;
}

void gfx_shadow_matrix_set_camera_locked(bool camera_locked) {
    s_register_camera_locked = camera_locked;
}

void gfx_shadow_matrix_set_presentation_owner(
    const GfxPresentationMatrixOwner *owner) {
    if (owner == NULL || !owner->valid || owner->address == NULL ||
        owner->generation == 0u) {
        memset(&s_register_presentation_owner, 0,
               sizeof(s_register_presentation_owner));
        s_register_presentation_owner_valid = false;
        return;
    }
    s_register_presentation_owner = *owner;
    s_register_presentation_owner.valid = true;
    s_register_presentation_owner_valid = true;
}

void gfx_shadow_presentation_owner_get_stats(GfxPresentationOwnerStats *out) {
    if (out != NULL) {
        *out = s_presentation_owner_stats;
    }
}

/* ---- presentation-replay freeze / restore -------------------------------- */

static bool freeze_pointer_list(
    const void **source, size_t count,
    const void ***frozen, size_t *frozen_count, size_t *frozen_capacity,
    size_t maximum) {
    if (count != 0) {
        if (!grow_array((void **)frozen, frozen_capacity, count,
                        GFX_SHADOW_INITIAL_RANGES, maximum,
                        sizeof(**frozen))) {
            *frozen_count = 0;
            return false;
        }
        memcpy(*frozen, source, count * sizeof(**frozen));
    }
    *frozen_count = count;
    return true;
}

void gfx_shadow_replay_freeze(void) {
    bool ok = true;

    if (s_matrix_count != 0) {
        if (grow_array((void **)&s_frozen_matrices, &s_frozen_matrix_capacity,
                       s_matrix_count, GFX_SHADOW_INITIAL_MATRICES,
                       GFX_SHADOW_MAX_MATRICES, sizeof(*s_frozen_matrices))) {
            memcpy(s_frozen_matrices, s_matrix_entries,
                   s_matrix_count * sizeof(*s_frozen_matrices));
            s_frozen_matrix_count = s_matrix_count;
        } else {
            s_frozen_matrix_count = 0;
            ok = false;
        }
    } else {
        s_frozen_matrix_count = 0;
    }
    ok = freeze_pointer_list(
             s_projected_ranges, s_projected_range_count,
             &s_frozen_projected_ranges, &s_frozen_projected_range_count,
             &s_frozen_projected_range_capacity,
             GFX_SHADOW_MAX_PROJECTED_RANGES) && ok;
    ok = freeze_pointer_list(
             s_excluded_casters, s_excluded_caster_count,
             &s_frozen_excluded_casters, &s_frozen_excluded_caster_count,
             &s_frozen_excluded_caster_capacity,
             GFX_SHADOW_MAX_PROJECTED_RANGES) && ok;
    /*
     * A partial freeze would replay a partial scene, which is worse than not
     * replaying at all: fail whole, exactly as the capture path does. The
     * caller sees gfx_shadow_replay_frozen() == false and skips the replay.
     */
    if (!ok) {
        s_frozen_matrix_count = 0;
        s_frozen_projected_range_count = 0;
        s_frozen_excluded_caster_count = 0;
        s_frozen_valid = false;
        s_freeze_failures++;
        return;
    }
    s_frozen_valid = true;
    s_freeze_count++;
}

bool gfx_shadow_replay_frozen(void) {
    return s_frozen_valid;
}

size_t gfx_shadow_replay_frozen_matrix_count(void) {
    return s_frozen_matrix_count;
}

/*
 * Unwind a restore that failed AFTER it took custody of the live registry.
 *
 * s_displaced_held is the module's "the live registry is not in
 * s_matrix_entries right now, it is in s_displaced_*" flag. Leaving it set on
 * an allocation failure wedges the module permanently: gfx_shadow_replay_restore
 * refuses every future call on `s_displaced_held`, so one transient OOM would
 * kill interpolation for the rest of the process AND leave the frozen copy
 * masquerading as the live registration set until the next
 * gfx_shadow_matrix_registry_reset.
 *
 * There are two failure regions and they need different unwinds:
 *
 *   BEFORE the frozen copy is memcpy'd over the live one (the very first
 *   grow_array after the flag is set), nothing has been destroyed -- the live
 *   set is still exactly where the game left it and s_matrix_count still
 *   describes it -- so the flag is merely cleared. Calling release() there
 *   would be wrong: it would overwrite the intact live set with the identical
 *   displaced copy for no reason, and on a mismatched count it would truncate.
 *
 *   AFTER any memcpy-over point, the live set exists ONLY in s_displaced_*, so
 *   it must be put BACK, which is precisely gfx_shadow_replay_release's job.
 *
 * Either way the replay is refused for this present (the caller sees false and
 * skips the walk), which is the same "fail whole rather than replay a partial
 * scene" rule the freeze path already follows.
 */
static bool replay_restore_unwind(bool displaced_live_set) {
    if (displaced_live_set) {
        /* Puts s_displaced_* back into the live arrays and clears the flag.
         * If even that fails it clears the flag anyway (see there). */
        (void)gfx_shadow_replay_release();
    } else {
        s_displaced_held = false;
    }
    s_restore_failures++;
    return false;
}

bool gfx_shadow_replay_restore(
    const GfxShadowReplayViewProjection *overrides, size_t override_count) {
    bool camera_observed[GFX_SHADOW_MAX_VIEWS] = { false };

    if (!s_frozen_valid || s_displaced_held) {
        return false;
    }
    /* Save whatever the game has registered since the freeze (see
     * s_displaced_matrices) before the frozen copy lands on top of it. */
    if (s_matrix_count != 0) {
        if (!grow_array((void **)&s_displaced_matrices,
                        &s_displaced_matrix_capacity, s_matrix_count,
                        GFX_SHADOW_INITIAL_MATRICES, GFX_SHADOW_MAX_MATRICES,
                        sizeof(*s_displaced_matrices))) {
            return false;
        }
        memcpy(s_displaced_matrices, s_matrix_entries,
               s_matrix_count * sizeof(*s_displaced_matrices));
    }
    s_displaced_matrix_count = s_matrix_count;
    if (!freeze_pointer_list(
            s_projected_ranges, s_projected_range_count,
            &s_displaced_projected_ranges, &s_displaced_projected_range_count,
            &s_displaced_projected_range_capacity,
            GFX_SHADOW_MAX_PROJECTED_RANGES) ||
        !freeze_pointer_list(
            s_excluded_casters, s_excluded_caster_count,
            &s_displaced_excluded_casters, &s_displaced_excluded_caster_count,
            &s_displaced_excluded_caster_capacity,
            GFX_SHADOW_MAX_PROJECTED_RANGES)) {
        return false;
    }
    s_displaced_held = true;

    if (s_frozen_matrix_count != 0) {
        if (!grow_array((void **)&s_matrix_entries, &s_matrix_capacity,
                        s_frozen_matrix_count, GFX_SHADOW_INITIAL_MATRICES,
                        GFX_SHADOW_MAX_MATRICES, sizeof(*s_matrix_entries))) {
            /* Nothing overwritten yet: the live registry is intact. */
            return replay_restore_unwind(false);
        }
        memcpy(s_matrix_entries, s_frozen_matrices,
               s_frozen_matrix_count * sizeof(*s_matrix_entries));
    }
    s_matrix_count = s_frozen_matrix_count;
    s_stats.matrix_entries = s_matrix_count;

    if (s_frozen_projected_range_count != 0) {
        if (!grow_array((void **)&s_projected_ranges,
                        &s_projected_range_capacity,
                        s_frozen_projected_range_count,
                        GFX_SHADOW_INITIAL_RANGES,
                        GFX_SHADOW_MAX_PROJECTED_RANGES,
                        sizeof(*s_projected_ranges))) {
            /* The matrix set is already the frozen copy; the live one exists
             * only in s_displaced_matrices. Put it back. */
            return replay_restore_unwind(true);
        }
        memcpy(s_projected_ranges, s_frozen_projected_ranges,
               s_frozen_projected_range_count * sizeof(*s_projected_ranges));
    }
    s_projected_range_count = s_frozen_projected_range_count;

    if (s_frozen_excluded_caster_count != 0) {
        if (!grow_array((void **)&s_excluded_casters,
                        &s_excluded_caster_capacity,
                        s_frozen_excluded_caster_count,
                        GFX_SHADOW_INITIAL_RANGES,
                        GFX_SHADOW_MAX_PROJECTED_RANGES,
                        sizeof(*s_excluded_casters))) {
            /* Matrices and projected ranges are already the frozen copies. */
            return replay_restore_unwind(true);
        }
        memcpy(s_excluded_casters, s_frozen_excluded_casters,
               s_frozen_excluded_caster_count * sizeof(*s_excluded_casters));
    }
    s_excluded_caster_count = s_frozen_excluded_caster_count;

    /*
     * Only the gameplay camera's own entries take an interpolated VP (see the
     * GfxShadowMatrixEntry comment). Everything else — HUD, menus, the
     * mtx_perspective 2D path — keeps the VP it was authored with, which is
     * why an interpolated present cannot make the HUD swim.
     */
    for (size_t index = 0; index < s_matrix_count; index++) {
        GfxShadowMatrixEntry *entry = &s_matrix_entries[index];
        entry->binding.vp_overridden = false;
        if (overrides == NULL || !entry->gameplay_vp ||
            !entry->binding.walked_key_bytes_valid || entry->viewport < 0 ||
            (size_t)entry->viewport >= override_count ||
            !overrides[entry->viewport].valid) {
            continue;
        }
        if (entry->viewport < GFX_SHADOW_MAX_VIEWS &&
            !camera_observed[entry->viewport]) {
            camera_endpoint_observe(
                entry->viewport, entry->binding.captured_view_projection,
                &overrides[entry->viewport]);
            camera_observed[entry->viewport] = true;
        }
        /* Alpha zero is the retained display list's exact authored endpoint.
         * Keep its captured VP bytes instead of needlessly rebuilding every
         * world matrix through an equivalent float product. The semantic
         * observer above still proves the supplied camera endpoint is exact;
         * only non-zero interpolation alphas replace the VP used by replay. */
        if (overrides[entry->viewport].numerator == 0u) {
            continue;
        }
        memcpy(entry->binding.view_projection,
               overrides[entry->viewport].view_projection,
               sizeof(entry->binding.view_projection));
        entry->binding.vp_overridden = true;
        /* Only a camera-locked entry (the skydome) ever reads this; carried
         * for every overridden entry regardless because it costs nothing and
         * keeps this loop the single place vp_overridden's payload is set. */
        memcpy(entry->binding.camera_position,
               overrides[entry->viewport].camera_position,
               sizeof(entry->binding.camera_position));
    }
    s_restore_count++;
    return true;
}

void gfx_shadow_camera_endpoint_validate(
    const GfxShadowReplayViewProjection *overrides, size_t override_count) {
    bool observed[GFX_SHADOW_MAX_VIEWS] = { false };

    if (overrides == NULL) {
        return;
    }
    for (size_t index = 0; index < s_frozen_matrix_count; index++) {
        const GfxShadowMatrixEntry *entry = &s_frozen_matrices[index];
        if (!entry->gameplay_vp ||
            !entry->binding.walked_key_bytes_valid || entry->viewport < 0 ||
            entry->viewport >= GFX_SHADOW_MAX_VIEWS ||
            (size_t)entry->viewport >= override_count ||
            observed[entry->viewport] ||
            !overrides[entry->viewport].valid) {
            continue;
        }
        camera_endpoint_observe(
            entry->viewport, entry->binding.captured_view_projection,
            &overrides[entry->viewport]);
        observed[entry->viewport] = true;
    }
}

/*
 * A release that could not put the displaced set back.
 *
 * Whatever it managed to copy, the registry is now a partial mix of frozen and
 * live entries, and there is no allocation available to finish the job. Two
 * things must still be true when this returns:
 *
 *   1. s_displaced_held MUST be cleared. It is the flag
 *      gfx_shadow_replay_restore refuses on, so leaving it set converts one
 *      transient allocation failure into a permanently dead replay path -- and
 *      leaves the module believing the live registry lives in s_displaced_*
 *      long after the next gfx_shadow_matrix_registry_reset has moved on.
 *   2. The registry must not be left claiming entries it did not restore.
 *      Emptying it is the safe direction: registration is rebuilt from scratch
 *      by the game every frame and reset at every gfx_end_frame, so an empty
 *      set costs at most this frame's shadow/replay fidelity, while a
 *      half-frozen set is a silently wrong scene.
 */
static bool replay_release_abort(void) {
    s_matrix_count = 0;
    s_stats.matrix_entries = 0;
    s_projected_range_count = 0;
    s_excluded_caster_count = 0;
    s_displaced_held = false;
    s_restore_failures++;
    return false;
}

bool gfx_shadow_replay_release(void) {
    if (!s_displaced_held) {
        return false;
    }
    if (s_displaced_matrix_count != 0) {
        if (!grow_array((void **)&s_matrix_entries, &s_matrix_capacity,
                        s_displaced_matrix_count, GFX_SHADOW_INITIAL_MATRICES,
                        GFX_SHADOW_MAX_MATRICES, sizeof(*s_matrix_entries))) {
            return replay_release_abort();
        }
        memcpy(s_matrix_entries, s_displaced_matrices,
               s_displaced_matrix_count * sizeof(*s_matrix_entries));
    }
    s_matrix_count = s_displaced_matrix_count;
    s_stats.matrix_entries = s_matrix_count;

    if (s_displaced_projected_range_count != 0) {
        if (!grow_array((void **)&s_projected_ranges,
                        &s_projected_range_capacity,
                        s_displaced_projected_range_count,
                        GFX_SHADOW_INITIAL_RANGES,
                        GFX_SHADOW_MAX_PROJECTED_RANGES,
                        sizeof(*s_projected_ranges))) {
            return replay_release_abort();
        }
        memcpy(s_projected_ranges, s_displaced_projected_ranges,
               s_displaced_projected_range_count *
                   sizeof(*s_projected_ranges));
    }
    s_projected_range_count = s_displaced_projected_range_count;

    if (s_displaced_excluded_caster_count != 0) {
        if (!grow_array((void **)&s_excluded_casters,
                        &s_excluded_caster_capacity,
                        s_displaced_excluded_caster_count,
                        GFX_SHADOW_INITIAL_RANGES,
                        GFX_SHADOW_MAX_PROJECTED_RANGES,
                        sizeof(*s_excluded_casters))) {
            return replay_release_abort();
        }
        memcpy(s_excluded_casters, s_displaced_excluded_casters,
               s_displaced_excluded_caster_count *
                   sizeof(*s_excluded_casters));
    }
    s_excluded_caster_count = s_displaced_excluded_caster_count;
    s_displaced_held = false;
    return true;
}

void gfx_shadow_replay_get_stats(
    uint64_t *freezes, uint64_t *restores, uint64_t *failures,
    uint64_t *restore_failures) {
    if (freezes != NULL) {
        *freezes = s_freeze_count;
    }
    if (restores != NULL) {
        *restores = s_restore_count;
    }
    if (failures != NULL) {
        *failures = s_freeze_failures;
    }
    if (restore_failures != NULL) {
        *restore_failures = s_restore_failures;
    }
}

void gfx_shadow_camera_endpoint_get_stats(
    GfxShadowCameraEndpointStats *out) {
    if (out != NULL) {
        *out = s_camera_endpoint_stats;
    }
}

void gfx_shadow_capture_suppress(bool suppressed) {
    s_capture_suppressed = suppressed;
    /* A replay both refuses to capture casters and must keep consuming the
     * caster frame the real walk consumed (see s_replay_read_index). */
    s_replay_hold_previous = suppressed;
}

void gfx_shadow_stage_begin(uint64_t stage_generation) {
    s_capture_active = false;
    s_static_cache.stage_generation = stage_generation;
    memset(s_camera_next_endpoint, 0, sizeof(s_camera_next_endpoint));
    s_static_cache.vertex_count = 0;
    s_static_cache.range_count = 0;
    s_static_cache.key_count = 0;
    for (size_t axis = 0; axis < 3; axis++) {
        s_static_cache.bounds_min[axis] = FLT_MAX;
        s_static_cache.bounds_max[axis] = -FLT_MAX;
    }
    s_static_cache.bounds_valid = false;
    s_static_cache.committed_range_count = 0;
    if (s_static_cache.keys != NULL) {
        memset(
            s_static_cache.keys, 0,
            s_static_cache.key_capacity * sizeof(*s_static_cache.keys));
    }
    for (size_t index = 0; index < 2; index++) {
        s_frames[index].valid = false;
        s_frames[index].static_vertices = NULL;
        s_frames[index].static_vertex_count = 0;
        s_frames[index].static_ranges = NULL;
        s_frames[index].static_range_count = 0;
    }
}

bool gfx_world_fx_trace_enabled(void) {
    if (s_trace_enabled < 0) {
        const char *value = getenv("MDKR_WORLD_FX_TRACE");
        s_trace_enabled =
            value != NULL &&
            value[0] != '\0' &&
            strcmp(value, "0") != 0 &&
            strcmp(value, "off") != 0;
    }
    return s_trace_enabled != 0;
}

/*
 * MDKR_TEST_SHADOW_BOGUS_CASTER=<world units> — the broken direction for
 * tests/check_shadow_plausibility.py.
 *
 * A plausibility gate that can only ever pass proves nothing, and the failure
 * it has to be able to see is a specific one: the stage cache holding caster
 * geometry that is not where the object is. That is what every historical
 * instance of this defect looked like from the shadow map's side, whether the
 * cause was a recycled arena address or a buffer the game rewrote in place —
 * a shadow with no object under it, cast from somewhere the player can see is
 * empty.
 *
 * This seam reproduces it directly and minimally: the FIRST admission of each
 * static caster is displaced along +Y by the requested distance, so the cache
 * (and therefore the depth map) disagrees with the live geometry from then on.
 * Only the cached copy moves — the drawn image, the physics and the game state
 * are untouched — so the positive-control arm differs from production in
 * exactly the property the check measures. Off, empty, "0" and unparseable
 * text all leave production behaviour alone.
 */
static float bogus_caster_offset(void) {
    static int resolved = 0;
    static float offset = 0.0f;
    const char *value;
    char *end;
    double parsed;

    if (resolved) {
        return offset;
    }
    resolved = 1;
    value = getenv("MDKR_TEST_SHADOW_BOGUS_CASTER");
    if (value == NULL || value[0] == '\0') {
        return offset;
    }
    parsed = strtod(value, &end);
    if (end == value || *end != '\0' ||
        !(parsed > -GFX_SHADOW_WORLD_LIMIT) ||
        !(parsed < GFX_SHADOW_WORLD_LIMIT)) {
        return offset;
    }
    offset = (float)parsed;
    return offset;
}

void gfx_shadow_capture_begin(void) {
    GfxShadowFrame *write = &s_frames[s_write_index];
    frame_reset(write);
    write->generation = ++s_generation;
    s_capture_active = true;
    s_stats.frames_begun++;
}

static bool same_viewport(const float left[4], const float right[4]) {
    return memcmp(left, right, 4 * sizeof(float)) == 0;
}

int gfx_shadow_capture_view(
    const float viewport[4],
    const float view_projection[GFX_SHADOW_MATRIX_DIM][GFX_SHADOW_MATRIX_DIM]) {
    GfxShadowFrame *write = &s_frames[s_write_index];
    GfxShadowView *view;
    size_t index;

    /* A presentation replay walks a display list whose casters were already
     * captured and committed by the real walk. Capturing them again would
     * double this frame's caster set and republish it mid-present. */
    if (s_capture_suppressed) {
        return -1;
    }
    if (write->failed || !finite_values(viewport, 4) ||
        !finite_matrix(view_projection)) {
        return -1;
    }
    for (index = 0; index < write->view_count; index++) {
        view = &write->views[index];
        if (same_viewport(view->viewport, viewport)) {
            memcpy(view->view_projection, view_projection,
                   sizeof(view->view_projection));
            return (int)index;
        }
    }
    if (write->view_count >= GFX_SHADOW_MAX_VIEWS) {
        frame_fail(write);
        return -1;
    }
    index = write->view_count++;
    view = &write->views[index];
    memset(view, 0, sizeof(*view));
    view->valid = true;
    memcpy(view->viewport, viewport, sizeof(view->viewport));
    memcpy(view->view_projection, view_projection,
           sizeof(view->view_projection));
    for (size_t axis = 0; axis < 3; axis++) {
        view->bounds_min[axis] = FLT_MAX;
        view->bounds_max[axis] = -FLT_MAX;
    }
    return (int)index;
}

static bool same_material(
    const GfxShadowRange *range,
    uint8_t view_index,
    const GfxShadowMaterial *material,
    size_t expected_first) {
    return range != NULL && material != NULL &&
           range->first_vertex + range->vertex_count == expected_first &&
           range->view_index == view_index &&
           range->texture_id == material->texture_id &&
           range->alpha_mode == (uint8_t)material->alpha_mode &&
           range->two_sided == (uint8_t)(material->two_sided ? 1 : 0);
}

static bool append_triangle(
    GfxShadowVertex **vertices,
    size_t *vertex_count,
    size_t *vertex_capacity,
    GfxShadowRange **ranges,
    size_t *range_count,
    size_t *range_capacity,
    uint8_t range_view,
    bool force_new_range,
    const float positions[9],
    const float uv[6],
    const GfxShadowMaterial *material) {
    GfxShadowRange *range = NULL;
    size_t required_vertices;
    size_t required_ranges;
    bool new_range;

    if (!checked_add_size(*vertex_count, 3, &required_vertices)) {
        return false;
    }
    if (*range_count > 0) {
        range = &(*ranges)[*range_count - 1];
    }
    new_range = force_new_range ||
        !same_material(range, range_view, material, *vertex_count);
    if (!checked_add_size(
            *range_count, new_range ? 1 : 0, &required_ranges) ||
        !grow_array(
            (void **)vertices,
            vertex_capacity,
            required_vertices,
            GFX_SHADOW_INITIAL_VERTICES,
            GFX_SHADOW_MAX_VERTICES,
            sizeof(**vertices)) ||
        !grow_array(
            (void **)ranges,
            range_capacity,
            required_ranges,
            GFX_SHADOW_INITIAL_RANGES,
            GFX_SHADOW_MAX_RANGES,
            sizeof(**ranges))) {
        return false;
    }

    if (new_range) {
        range = &(*ranges)[(*range_count)++];
        memset(range, 0, sizeof(*range));
        range->first_vertex = *vertex_count;
        range->texture_id = material->texture_id;
        range->view_index = range_view;
        range->alpha_mode = (uint8_t)material->alpha_mode;
        range->two_sided = (uint8_t)(material->two_sided ? 1 : 0);
    }
    for (size_t vertex = 0; vertex < 3; vertex++) {
        GfxShadowVertex *destination = &(*vertices)[*vertex_count + vertex];
        for (size_t axis = 0; axis < 3; axis++) {
            destination->position[axis] =
                positions[vertex * 3 + axis];
        }
        destination->uv[0] = uv[vertex * 2 + 0];
        destination->uv[1] = uv[vertex * 2 + 1];
    }
    *vertex_count = required_vertices;
    range->vertex_count += 3;
    return true;
}

static void observe_triangle_bounds(
    GfxShadowView *view,
    const float positions[9]) {
    for (size_t vertex = 0; vertex < 3; vertex++) {
        for (size_t axis = 0; axis < 3; axis++) {
            float value = positions[vertex * 3 + axis];
            if (value < view->bounds_min[axis]) {
                view->bounds_min[axis] = value;
            }
            if (value > view->bounds_max[axis]) {
                view->bounds_max[axis] = value;
            }
        }
    }
}

bool gfx_shadow_capture_triangle(
    int view_index,
    const void *source_key,
    GfxShadowMobility mobility,
    const float positions[9],
    const float uv[6],
    const GfxShadowMaterial *material) {
    GfxShadowFrame *write = &s_frames[s_write_index];
    GfxShadowView *view;
    bool cache_static;
    bool cached = false;

    if (s_capture_suppressed) {   /* see gfx_shadow_capture_view */
        return false;
    }
    if (write->failed || material == NULL ||
        view_index < 0 || view_index >= (int)write->view_count ||
        !write->views[view_index].valid ||
        !finite_values(positions, 9) || !finite_values(uv, 6) ||
        (material->alpha_mode != GFX_SHADOW_ALPHA_OPAQUE &&
         material->alpha_mode != GFX_SHADOW_ALPHA_MASKED) ||
        (mobility != GFX_SHADOW_MOBILITY_STATIC &&
         mobility != GFX_SHADOW_MOBILITY_DYNAMIC)) {
        return false;
    }

    for (size_t index = 0; index < 9; index++) {
        if (positions[index] > GFX_SHADOW_WORLD_LIMIT ||
            positions[index] < -GFX_SHADOW_WORLD_LIMIT) {
            s_stats.implausible_triangles++;
            return false;
        }
    }

    view = &write->views[view_index];
    /*
     * Masked ranges are captured for classification but skipped by both
     * backends' depth passes, so they must not extend the caster bounds the
     * planner turns into the light z-range (every extra unit of z-span
     * dilutes depth precision and scales the world-unit bias).
     */
    if (material->alpha_mode == GFX_SHADOW_ALPHA_OPAQUE) {
        observe_triangle_bounds(view, positions);
    }
    view->triangle_count++;

    /*
     * Only opaque geometry with an exact stable source identity enters the
     * stage cache. Alpha-tested ranges retain live texture ids and may animate;
     * model-transformed geometry is dynamic even when it reuses a mesh.
     */
    cache_static =
        mobility == GFX_SHADOW_MOBILITY_STATIC &&
        material->alpha_mode == GFX_SHADOW_ALPHA_OPAQUE &&
        source_key != NULL;
    if (cache_static) {
        size_t slot = static_key_find(source_key);
        cached = slot != SIZE_MAX;
        if (cached) {
            s_stats.static_cache_hits++;
            /*
             * The address is live this frame — the only moment its tenancy can
             * be checked at all. A mismatch means the cache has been casting
             * geometry that is no longer there.
             */
            if (!static_key_tenancy_ok(slot, positions)) {
                static_key_reseat(slot, positions);
                /* The stage AABB is a superset of every position the cache
                 * holds; a reseat introduces one it has never seen. */
                static_bounds_fold(positions);
            }
        } else {
            size_t required_keys;
            size_t first_vertex = s_static_cache.vertex_count;
            float admitted[9];
            float bogus = bogus_caster_offset();
            bool appended;
            memcpy(admitted, positions, sizeof(admitted));
            if (bogus != 0.0f) {
                for (size_t vertex = 0; vertex < 3; vertex++) {
                    admitted[vertex * 3 + 1] += bogus;
                }
            }
            if (!checked_add_size(
                    s_static_cache.key_count, 1, &required_keys) ||
                !static_keys_reserve(required_keys)) {
                frame_fail(write);
                return false;
            }
            /*
             * Ranges at or below the committed watermark are borrowed by the
             * published frame and must not change (the f04668e regression);
             * ranges created after it are invisible to that frame, so
             * same-material admissions may still merge instead of costing
             * one draw call per triangle.
             */
            appended = append_triangle(
                    &s_static_cache.vertices,
                    &s_static_cache.vertex_count,
                    &s_static_cache.vertex_capacity,
                    &s_static_cache.ranges,
                    &s_static_cache.range_count,
                    &s_static_cache.range_capacity,
                    UINT8_MAX,
                    s_static_cache.range_count <=
                        s_static_cache.committed_range_count,
                    admitted,
                    uv,
                    material);
            /*
             * realloc may move either stage array even if the second reserve
             * then fails. Keep the published frame's borrowed pointers valid
             * before reporting failure and retaining that prior frame.
             */
            sync_static_frame_references();
            if (!appended) {
                frame_fail(write);
                return false;
            }
            static_key_insert(source_key, first_vertex);
            sync_static_frame_references();
            s_stats.static_cache_misses++;
            static_bounds_fold(admitted);
        }
    } else if (!append_triangle(
                   &write->vertices,
                   &write->vertex_count,
                   &write->vertex_capacity,
                   &write->ranges,
                   &write->range_count,
                   &write->range_capacity,
                   (uint8_t)view_index,
                   false,
                   positions,
                   uv,
                   material)) {
        frame_fail(write);
        return false;
    }

    s_stats.triangles_captured++;
    if (material->alpha_mode == GFX_SHADOW_ALPHA_MASKED) {
        s_stats.masked_triangles++;
    } else {
        s_stats.opaque_triangles++;
    }
    if (s_static_cache.vertex_count + write->vertex_count >
        s_stats.peak_vertices) {
        s_stats.peak_vertices =
            s_static_cache.vertex_count + write->vertex_count;
    }
    if (s_static_cache.range_count + write->range_count >
        s_stats.peak_ranges) {
        s_stats.peak_ranges =
            s_static_cache.range_count + write->range_count;
    }
    return true;
}

void gfx_shadow_capture_commit(void) {
    GfxShadowFrame *write = &s_frames[s_write_index];
    const GfxShadowFrame *published;
    unsigned old_read;

    s_capture_active = false;
    write->stage_generation = s_static_cache.stage_generation;
    write->static_vertices = s_static_cache.vertices;
    write->static_vertex_count = s_static_cache.vertex_count;
    write->static_ranges = s_static_cache.ranges;
    write->static_range_count = s_static_cache.range_count;
    /*
     * Every cached static triangle is drawn into every view's cascades, so
     * each view's caster bounds must cover the whole stage cache, not just the
     * triangles the game happened to submit this frame (see the cache comment).
     */
    if (!write->failed && s_static_cache.bounds_valid &&
        s_static_cache.vertex_count >= 3) {
        for (size_t index = 0; index < write->view_count; index++) {
            GfxShadowView *view = &write->views[index];
            if (!view->valid) {
                continue;
            }
            for (size_t axis = 0; axis < 3; axis++) {
                if (s_static_cache.bounds_min[axis] <
                    view->bounds_min[axis]) {
                    view->bounds_min[axis] =
                        s_static_cache.bounds_min[axis];
                }
                if (s_static_cache.bounds_max[axis] >
                    view->bounds_max[axis]) {
                    view->bounds_max[axis] =
                        s_static_cache.bounds_max[axis];
                }
            }
        }
    }
    write->valid =
        !write->failed &&
        write->view_count > 0 &&
        ((write->static_vertex_count >= 3 &&
          write->static_range_count > 0) ||
         (write->vertex_count >= 3 &&
          write->range_count > 0));
    if (write->failed) {
        s_stats.frames_failed++;
        /*
         * The commit did NOT swap, so the frame this walk consumed is still
         * s_read_index. s_replay_read_index is only assigned on the successful
         * path below, and a replay of THIS walk's display list reads through it
         * (gfx_shadow_frame_previous under s_replay_hold_previous) -- leaving it
         * pointing at whatever the last successful commit set would light the
         * replay with a caster frame that is not the one the real walk used.
         * That is the same class of defect arms C/D of check_render_purity
         * root-caused (the read index swapping ahead of the geometry), just
         * reached through the capture-failure path instead.
         */
        s_replay_read_index = s_read_index;
        published = &s_frames[s_read_index];
        s_stats.current_views = published->view_count;
        s_stats.current_static_triangles =
            published->static_vertex_count / 3;
        s_stats.current_dynamic_triangles =
            published->vertex_count / 3;
        s_stats.current_triangles =
            s_stats.current_static_triangles +
            s_stats.current_dynamic_triangles;
        s_stats.current_ranges =
            published->static_range_count + published->range_count;
        return;
    }
    s_stats.frames_committed++;
    s_stats.current_views = write->view_count;
    s_stats.current_static_triangles =
        write->static_vertex_count / 3;
    s_stats.current_dynamic_triangles =
        write->vertex_count / 3;
    s_stats.current_triangles =
        s_stats.current_static_triangles +
        s_stats.current_dynamic_triangles;
    s_stats.current_ranges =
        write->static_range_count + write->range_count;

    old_read = s_read_index;
    /* The frame the walk that just ended consumed — what a replay of that same
     * display list must keep seeing. */
    s_replay_read_index = old_read;
    s_read_index = s_write_index;
    s_write_index = old_read;
    /* Only a successful commit advances the merge watermark: the newly
     * published frame borrows every current static range, so from here on
     * they are immutable. */
    s_static_cache.committed_range_count = s_static_cache.range_count;
}

const GfxShadowFrame *gfx_shadow_frame_previous(void) {
    return &s_frames[
        s_replay_hold_previous ? s_replay_read_index : s_read_index];
}

int gfx_shadow_previous_view_index(const float viewport[4]) {
    const GfxShadowFrame *frame = &s_frames[
        s_replay_hold_previous ? s_replay_read_index : s_read_index];

    if (!frame->valid || viewport == NULL) {
        return -1;
    }
    for (size_t index = 0; index < frame->view_count; index++) {
        if (frame->views[index].valid &&
            same_viewport(frame->views[index].viewport, viewport)) {
            return (int)index;
        }
    }
    return -1;
}

bool gfx_shadow_projected_range_mark(const void *source) {
    if (source == NULL) {
        return false;
    }
    for (size_t index = 0; index < s_projected_range_count; index++) {
        if (s_projected_ranges[index] == source) {
            return true;
        }
    }
    if (!grow_array(
            (void **)&s_projected_ranges,
            &s_projected_range_capacity,
            s_projected_range_count + 1,
            GFX_SHADOW_INITIAL_RANGES,
            GFX_SHADOW_MAX_PROJECTED_RANGES,
            sizeof(*s_projected_ranges))) {
        return false;
    }
    s_projected_ranges[s_projected_range_count++] = source;
    return true;
}

bool gfx_shadow_projected_range_contains(const void *source) {
    for (size_t index = 0; index < s_projected_range_count; index++) {
        if (s_projected_ranges[index] == source) {
            return true;
        }
    }
    return false;
}

void gfx_shadow_projected_ranges_reset(void) {
    s_projected_range_count = 0;
    s_excluded_caster_count = 0;
}

bool gfx_shadow_caster_exclude_mark(const void *source) {
    if (source == NULL) {
        return false;
    }
    for (size_t index = 0; index < s_excluded_caster_count; index++) {
        if (s_excluded_casters[index] == source) {
            return true;
        }
    }
    if (!grow_array(
            (void **)&s_excluded_casters,
            &s_excluded_caster_capacity,
            s_excluded_caster_count + 1,
            GFX_SHADOW_INITIAL_RANGES,
            GFX_SHADOW_MAX_PROJECTED_RANGES,
            sizeof(*s_excluded_casters))) {
        return false;
    }
    s_excluded_casters[s_excluded_caster_count++] = source;
    return true;
}

bool gfx_shadow_caster_excluded(const void *source) {
    for (size_t index = 0; index < s_excluded_caster_count; index++) {
        if (s_excluded_casters[index] == source) {
            s_stats.excluded_triangles++;
            return true;
        }
    }
    return false;
}

void gfx_world_fx_get_stats(GfxWorldFxStats *out) {
    if (out != NULL) {
        *out = s_stats;
        out->matrix_entries = s_matrix_count;
    }
}

void gfx_shadow_frame_shutdown(void) {
    for (size_t index = 0; index < 2; index++) {
        free(s_frames[index].vertices);
        free(s_frames[index].ranges);
        memset(&s_frames[index], 0, sizeof(s_frames[index]));
    }
    free(s_matrix_entries);
    s_matrix_entries = NULL;
    s_matrix_count = 0;
    s_matrix_capacity = 0;
    free(s_static_cache.vertices);
    free(s_static_cache.ranges);
    free(s_static_cache.keys);
    memset(&s_static_cache, 0, sizeof(s_static_cache));
    /* The memset zeroes the AABB; restore the empty sentinels so a capture
     * before the next stage begin cannot fold "zero" in as a bound. */
    for (size_t axis = 0; axis < 3; axis++) {
        s_static_cache.bounds_min[axis] = FLT_MAX;
        s_static_cache.bounds_max[axis] = -FLT_MAX;
    }
    free(s_projected_ranges);
    s_projected_ranges = NULL;
    s_projected_range_count = 0;
    s_projected_range_capacity = 0;
    free(s_excluded_casters);
    s_excluded_casters = NULL;
    s_excluded_caster_count = 0;
    s_excluded_caster_capacity = 0;
    /*
     * The replay freeze and the registry it displaces are stage-lifetime state
     * like everything above; the tests use shutdown as the reset, so anything
     * left here is carried into the next case as a live frozen registry.
     */
    free(s_frozen_matrices);
    s_frozen_matrices = NULL;
    s_frozen_matrix_count = 0;
    s_frozen_matrix_capacity = 0;
    free(s_frozen_projected_ranges);
    s_frozen_projected_ranges = NULL;
    s_frozen_projected_range_count = 0;
    s_frozen_projected_range_capacity = 0;
    free(s_frozen_excluded_casters);
    s_frozen_excluded_casters = NULL;
    s_frozen_excluded_caster_count = 0;
    s_frozen_excluded_caster_capacity = 0;
    free(s_displaced_matrices);
    s_displaced_matrices = NULL;
    s_displaced_matrix_count = 0;
    s_displaced_matrix_capacity = 0;
    free(s_displaced_projected_ranges);
    s_displaced_projected_ranges = NULL;
    s_displaced_projected_range_count = 0;
    s_displaced_projected_range_capacity = 0;
    free(s_displaced_excluded_casters);
    s_displaced_excluded_casters = NULL;
    s_displaced_excluded_caster_count = 0;
    s_displaced_excluded_caster_capacity = 0;
    s_displaced_held = false;
    s_frozen_valid = false;
    s_freeze_count = 0;
    s_restore_count = 0;
    s_freeze_failures = 0;
    s_restore_failures = 0;
    s_capture_suppressed = false;
    s_replay_read_index = 0;
    s_replay_hold_previous = false;
    s_read_index = 0;
    s_write_index = 1;
    s_generation = 0;
    s_capture_active = false;
    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_presentation_owner_stats, 0,
           sizeof(s_presentation_owner_stats));
    memset(&s_camera_endpoint_stats, 0, sizeof(s_camera_endpoint_stats));
    memset(s_camera_next_endpoint, 0, sizeof(s_camera_next_endpoint));
    memset(&s_register_presentation_owner, 0,
           sizeof(s_register_presentation_owner));
    s_register_presentation_owner_valid = false;
    /* Registration context is published per call; a reset must not leave the
     * previous stage's viewport available to inherit. */
    s_register_viewport = 0;
    s_register_gameplay_vp = false;
    s_register_site = GFX_SHADOW_SITE_UNKNOWN;
    s_register_key_is_mtx = false;
    s_trace_enabled = -1;
    s_matrix_control_drop = -1;
    s_camera_endpoint_observer = -1;
}
