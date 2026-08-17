/*
 * presentation_snapshot.c — native-only presentation shadow state
 * (spec §7, Phase 3 Wave A). See presentation_snapshot.h for the design
 * rationale (identity scheme, publish ring, teleport threshold, discrete
 * selection rule).
 *
 * This translation unit is deliberately free of every game header: it knows
 * objects only as `const void *` addresses and plain POD samples. That is
 * what lets tests/test_presentation_snapshot.c compile it standalone and
 * drive lifecycle races that would be impossible to stage inside a running
 * race. The walk that turns gObjPtrList and gCameras into those samples
 * lives next door in presentation_snapshot_walk.c.
 */
#include "presentation_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- seam --------------------------------------------------------------- */

static int s_enabled = -1;

static void presentation_snapshot_report(void);

bool presentation_snapshot_enabled(void) {
    if (s_enabled < 0) {
        const char *value = getenv("MDKR_PRESENT_SNAPSHOT");
        s_enabled = value != NULL && value[0] != '\0' &&
                    strcmp(value, "0") != 0;
        /* Only the env seam prints the exit row: a unit test that enables
         * the module programmatically must not pollute CTest output. */
        if (s_enabled) {
            atexit(presentation_snapshot_report);
        }
    }
    return s_enabled != 0;
}

void presentation_snapshot_set_enabled(bool enabled) {
    s_enabled = enabled ? 1 : 0;
}

/* ---- test-only seams ----------------------------------------------------- */

/*
 * Two negative controls, and nothing else in this file may grow a third
 * without the same justification.
 *
 * A gate that only ever runs green proves that it ran, not that it can fail.
 * These are the committed code paths that turn two of
 * tests/check_motion_quality_battery.py's rows red on demand: one restores the
 * long-way-round angle smear the quarter-turn snap exists to prevent (artifact
 * class C3), the other restores blending across a spawn/teleport the
 * discontinuity flag exists to refuse (class C2's object-side sibling).
 *
 * Both are presentation-only by construction. They change what a REPLAYED pose
 * looks like and never touch a captured sample or a live object, so the
 * authoritative state/event/input streams cannot see either one -- which is
 * also why the battery can assert stream identity across its own red arms.
 *
 * Both sit behind the same versioned internal token present_sched.c gates its
 * adversarial replay seams with, and both default off: without the token the
 * arm's own env is inert. The token literal is repeated here rather than
 * shared through present_sched.h because this translation unit is deliberately
 * free of every other header -- that property is what lets
 * tests/test_presentation_snapshot.c link it standalone (cmake/tests.cmake) --
 * and one strcmp costs less than giving it up.
 */
#define PRESENTATION_SNAPSHOT_INTERNAL_TOKEN "mdkr64-presentation-replay-v1"

static int s_internal_test = -1;
static int s_test_long_arc = -1;
static int s_test_ignore_discontinuity = -1;

static bool presentation_snapshot_internal_test(void) {
    if (s_internal_test < 0) {
        const char *token = getenv("MDKR_INTERNAL_TEST_TOKEN");
        s_internal_test =
            token != NULL &&
            strcmp(token, PRESENTATION_SNAPSHOT_INTERNAL_TOKEN) == 0;
    }
    return s_internal_test != 0;
}

static bool presentation_snapshot_test_flag(const char *name, int *cache) {
    if (*cache < 0) {
        const char *value = getenv(name);
        /* Token first: an env set without it resolves to off and stays off,
         * so a stray MDKR_TEST_* in a shell profile cannot arm a seam. */
        *cache = presentation_snapshot_internal_test() && value != NULL &&
                 value[0] == '1';
    }
    return *cache != 0;
}

static bool presentation_snapshot_test_long_arc(void) {
    return presentation_snapshot_test_flag("MDKR_TEST_ROTATION_LONG_ARC",
                                           &s_test_long_arc);
}

static bool presentation_snapshot_test_ignore_discontinuity(void) {
    return presentation_snapshot_test_flag("MDKR_TEST_IGNORE_DISCONTINUITY",
                                           &s_test_ignore_discontinuity);
}

/* ---- identity registry --------------------------------------------------- */

/*
 * Open-addressed address -> (generation, last captured pose) map.
 *
 * It carries the last captured position and the publish serial that captured
 * it so discontinuity detection is O(1) per object: no scan of the previous
 * snapshot, and — critically — the comparison survives gObjPtrList being
 * permuted between ticks by the object sort.
 */
typedef struct SnapIdentity {
    const void *address; /* NULL == empty slot */
    uint64_t generation;
    float last_position[3];
    uint64_t last_capture; /* publish serial; 0 == never captured */
} SnapIdentity;

static SnapIdentity s_identity[PRESENTATION_SNAPSHOT_IDENTITY_SLOTS];
static size_t s_identity_live;
static uint64_t s_generation_serial;
/* A lifecycle hook that could not register an identity. Sticky until the next
 * commit spends it by failing the frame whole — see note_spawn. */
static bool s_identity_insert_failed;

static PresentationSnapshotStats s_stats;

static size_t hash_address(const void *address, size_t mask) {
    uint64_t hash = (uint64_t)(uintptr_t)address;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ull;
    hash ^= hash >> 33;
    return (size_t)(hash & (uint64_t)mask);
}

static SnapIdentity *identity_find(const void *address) {
    const size_t mask = PRESENTATION_SNAPSHOT_IDENTITY_SLOTS - 1u;
    size_t slot = hash_address(address, mask);
    for (size_t probe = 0; probe <= mask; probe++) {
        SnapIdentity *entry = &s_identity[slot];
        if (entry->address == NULL) {
            return NULL;
        }
        if (entry->address == address) {
            return entry;
        }
        slot = (slot + 1u) & mask;
    }
    return NULL;
}

/* Returns NULL only when the table is genuinely full (a capture-failing
 * condition, counted as an overflow). */
static SnapIdentity *identity_insert(const void *address) {
    const size_t mask = PRESENTATION_SNAPSHOT_IDENTITY_SLOTS - 1u;
    SnapIdentity *existing = identity_find(address);
    size_t slot;

    if (existing != NULL) {
        return existing;
    }
    /* Half load is the probing budget; the live population is bounded by the
     * object pool (OBJECT_SLOT_COUNT == 512) plus the handful of
     * OBJECT_SPAWN_NONE effect objects, so this is >4x headroom. */
    if (s_identity_live * 2u >= PRESENTATION_SNAPSHOT_IDENTITY_SLOTS) {
        return NULL;
    }
    slot = hash_address(address, mask);
    while (s_identity[slot].address != NULL) {
        slot = (slot + 1u) & mask;
    }
    memset(&s_identity[slot], 0, sizeof(s_identity[slot]));
    s_identity[slot].address = address;
    s_identity_live++;
    return &s_identity[slot];
}

/*
 * Backward-shift deletion (no tombstones): after clearing the hole, pull
 * forward any later entry whose ideal slot is not inside the (hole, scan]
 * window, so every probe chain stays unbroken. Objects churn constantly in
 * DKR — a tombstone scheme would need periodic compaction passes and those
 * would be unbounded work at an unpredictable tick.
 */
static void identity_erase(SnapIdentity *entry) {
    const size_t mask = PRESENTATION_SNAPSHOT_IDENTITY_SLOTS - 1u;
    size_t hole = (size_t)(entry - s_identity);
    size_t scan = hole;

    for (;;) {
        memset(&s_identity[hole], 0, sizeof(s_identity[hole]));
        for (;;) {
            size_t ideal;
            size_t offset_scan;
            size_t offset_ideal;

            scan = (scan + 1u) & mask;
            if (s_identity[scan].address == NULL) {
                s_identity_live--;
                return;
            }
            ideal = hash_address(s_identity[scan].address, mask);
            offset_scan = (scan - hole) & mask;
            offset_ideal = (ideal - hole) & mask;
            if (offset_ideal == 0u || offset_ideal > offset_scan) {
                break; /* movable into the hole */
            }
        }
        s_identity[hole] = s_identity[scan];
        hole = scan;
    }
}

static void identity_reset(void) {
    memset(s_identity, 0, sizeof(s_identity));
    s_identity_live = 0;
}

void presentation_snapshot_note_spawn(const void *object) {
    SnapIdentity *entry;

    if (object == NULL || !presentation_snapshot_enabled()) {
        return;
    }
    entry = identity_insert(object);
    if (entry == NULL) {
        /*
         * Fail CLOSED, explicitly.
         *
         * The old comment here asserted "the next capture fails whole and
         * counts the overflow" and then returned silently, leaving nothing
         * behind that could make that happen. It was true only by an accident
         * of identity_insert's internals — the table is still full when the
         * capture runs, so capture_object's own insert also fails. Let enough
         * frees land between the spawn and the capture and the belief
         * evaporates, and the direction it fails in is the dangerous one: a
         * recycled address that still carries a DEAD object's generation,
         * last_position and last_capture publishes the new object under the
         * dead one's identity, resolve_object_pair's generation check passes,
         * and the interpolator blends a new object's first pose into an
         * unrelated corpse's last pose.
         *
         * So state the outcome instead of inferring it. The flag is sticky
         * across the rest of this tick and spent by the next commit, which
         * fails whole exactly the way an object-table overflow does. This is
         * the same direction presentation_snapshot_capture_object already
         * takes on the identical condition.
         */
        s_identity_insert_failed = true;
        s_stats.identity_insert_failures++;
        return;
    }
    /* A fresh generation is the whole point: even if this is the exact
     * address a just-freed object occupied, the pair can no longer match. */
    entry->generation = ++s_generation_serial;
    entry->last_capture = 0;
    entry->last_position[0] = 0.0f;
    entry->last_position[1] = 0.0f;
    entry->last_position[2] = 0.0f;
}

void presentation_snapshot_note_free(const void *object) {
    SnapIdentity *entry;

    if (object == NULL || !presentation_snapshot_enabled()) {
        return;
    }
    entry = identity_find(object);
    if (entry == NULL) {
        return;
    }
    identity_erase(entry);
}

bool presentation_snapshot_identity_generation(const void *object,
                                                uint64_t *generation) {
    const SnapIdentity *entry;

    if (object == NULL || generation == NULL ||
        !presentation_snapshot_enabled()) {
        return false;
    }
    entry = identity_find(object);
    if (entry == NULL || entry->generation == 0u) {
        return false;
    }
    *generation = entry->generation;
    return true;
}

bool presentation_snapshot_identity_ensure_generation(
    const void *object, uint64_t *generation) {
    SnapIdentity *entry;

    if (object == NULL || generation == NULL ||
        !presentation_snapshot_enabled()) {
        return false;
    }
    entry = identity_find(object);
    if (entry == NULL) {
        entry = identity_insert(object);
        if (entry == NULL) {
            return false;
        }
        entry->generation = ++s_generation_serial;
        entry->last_capture = 0u;
        entry->last_position[0] = 0.0f;
        entry->last_position[1] = 0.0f;
        entry->last_position[2] = 0.0f;
    }
    if (entry->generation == 0u) {
        return false;
    }
    *generation = entry->generation;
    return true;
}

/* ---- publish ring -------------------------------------------------------- */

/*
 * Three slots, not two. Consumers need the PAIR (previous, current) to stay
 * immutable for as long as they are drawing from it; a two-slot flip would
 * hand the next capture the very buffer that is publishing `previous`.
 */
static PresentationSnapshot s_frames[3];
static int s_current = -1;
static int s_previous = -1;
static int s_write = -1;
static bool s_write_failed;
static bool s_capturing;
/*
 * Two counters, deliberately.
 *
 * s_capture_serial counts capture ATTEMPTS — one per authoritative tick,
 * whether or not the capture publishes. Continuity is measured against it,
 * so a pair is only ever blended when it spans exactly one authoritative
 * tick. s_publish_serial counts successful publishes and is what a frame's
 * generation reports.
 *
 * Using the publish serial for both would quietly paper over a dropped
 * snapshot: after an overflow the surviving pair would be two ticks apart
 * but still look adjacent, and every object in it would render at half
 * speed for a frame and then snap. Wave B's interpolation assumes the pair
 * is one tick apart; this keeps that assumption true by construction.
 */
static uint64_t s_capture_serial;
static uint64_t s_publish_serial;
static uint64_t s_stage_generation;

typedef struct SnapCameraHistory {
    int32_t camera_id;
    float last_position[3];
    int16_t last_rotation_y;
    int16_t last_view_pitch;
    int16_t last_rotation_z;
    float last_fov;
    uint8_t last_world_region;
    uint64_t last_capture;
} SnapCameraHistory;

static SnapCameraHistory s_camera_history[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];

static int16_t camera_composed_pitch(int16_t rotation_x, int16_t pitch) {
    return (int16_t)(uint16_t)((uint16_t)rotation_x + (uint16_t)pitch);
}

/*
 * One bit per VIEWPORT the game has declared cut; consumed by that viewport's
 * capture.
 *
 * Viewport, not gCameras[] slot, and the distinction is the whole reason this
 * comment exists. The three note sites (racer.c's camera-mode change and
 * spectate handoff, tracks.c's 3P time-trial spectate switch) all name a
 * PLAYER INDEX, which is the viewport index — `camSetProjMtx` takes
 * `viewport = gActiveCameraID`. The gCameras[] slot it records alongside is a
 * different number: `gActiveCameraID + (gCutsceneCameraActive ? 4 : 0)`. Keying
 * this mask on the slot therefore made every note raised while a viewport ran
 * its cutscene camera land on bit p and be looked for at bit p+4 — a miss, on
 * exactly the ticks a cut is most likely. Both halves now speak viewport.
 *
 * A note is spent only by the capture of the viewport it names. An unconsumed
 * note is NOT discarded at commit: it is carried, so a note raised on a tick
 * whose camera capture never ran still applies to the next capture of that
 * viewport, which is the fail-closed direction. A note that survives the
 * publish of the very viewport it names is a keying bug by construction, and
 * `camera_cut_unconsumed` counts it so the next such mismatch is a nonzero
 * stat instead of a silent blend across a hard cut.
 */
static uint32_t s_camera_cut_pending;

void presentation_snapshot_note_camera_cut(int viewport_index) {
    if (viewport_index < 0 ||
        viewport_index >= PRESENTATION_SNAPSHOT_MAX_VIEWPORTS ||
        !presentation_snapshot_enabled()) {
        return;
    }
    s_camera_cut_pending |= 1u << (unsigned)viewport_index;
    s_stats.camera_cut_notes++;
}

/*
 * One bit per VIEWPORT under a STANDING exclusion (Task 9). Unlike
 * s_camera_cut_pending, this is not spent by the next capture: it stays set
 * across every capture of the viewport until the caller clears it, so a
 * finish/spectate camera cannot enter resolve_camera_pair as a blend
 * candidate for any tick it is active, dwell ticks included, whether or not
 * anything filed a one-shot note for that particular tick.
 */
static uint32_t s_camera_excluded;

void presentation_snapshot_set_camera_excluded(int viewport_index,
                                               bool excluded) {
    if (viewport_index < 0 ||
        viewport_index >= PRESENTATION_SNAPSHOT_MAX_VIEWPORTS ||
        !presentation_snapshot_enabled()) {
        return;
    }
    if (excluded) {
        s_camera_excluded |= 1u << (unsigned)viewport_index;
    } else {
        s_camera_excluded &= ~(1u << (unsigned)viewport_index);
    }
}

/*
 * Per-tick camera cut journal (test observability only).
 *
 * The exit row counts discontinuities in aggregate, which is enough to prove
 * the flag fires SOMEWHERE but not that it fires at the tick a particular cut
 * happened. The coverage gate needs the second thing: it classifies cuts
 * itself, from the raw poses below, and then demands that no blend spans one.
 * Emitting the recipe rather than a verdict is the point — a journal that only
 * printed this module's own discontinuity flag could not disagree with it.
 */
static int s_cut_trace = -1;

static bool camera_cut_trace_enabled(void) {
    /* Token-gated like every other seam in this file: a bare
     * MDKR_TEST_CAMERA_CUT_TRACE in a shell profile must not inject
     * [CAMERA-CUT] rows into the stdout stream the determinism gates and
     * presentation_snapshot_report parsers consume. */
    return presentation_snapshot_test_flag("MDKR_TEST_CAMERA_CUT_TRACE",
                                           &s_cut_trace);
}

typedef struct AuthoredCameraSet {
    uint64_t tick;
    uint8_t valid_mask;
    uint8_t conflict_mask;
    PresentationCameraEntry samples[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];
    bool active;
} AuthoredCameraSet;

static AuthoredCameraSet s_authored_cameras;

static PresentationSnapshotStats s_stats;

/* ---- renderer-owned transform registry ----------------------------------- */

/*
 * Small and fixed on purpose. A registry that could grow without bound would
 * be a way for the render path to push the capture over
 * PRESENTATION_SNAPSHOT_MAX_OBJECTS and fail every snapshot whole.
 *
 * Sized for the three renderer-owned families that exist: eight rain-splash
 * slots, sixteen lens-flare pieces, and one slot per visible wave tile. The
 * wave count is bounded by the renderer's own 26-slot wave-visibility table
 * (game/src/waves.c WAVE_VISIBLE_SLOTS), NOT by the number of wave tiles a
 * level has -- one tile, not one sub-quad: a double-density tile's four
 * sub-quads share a slot and differ only by the render-time offset the
 * owner's residual already carries. 8 + 16 + 26 == 50; 64 leaves headroom
 * without approaching the 512-entry object budget.
 */
#define PRESENTATION_SNAPSHOT_MAX_EXTERNALS 64u

static const void *s_externals[PRESENTATION_SNAPSHOT_MAX_EXTERNALS];
/*
 * Per-tick topology key, parallel to s_externals (see
 * presentation_snapshot_set_external_topology_key). Zero means "this owner
 * publishes no key", which is what every registrant except the wave surfaces
 * is: an unkeyed pair always agrees, so an owner that never sets one is
 * treated exactly as it was before keys existed.
 */
static uint16_t s_external_topology_keys[PRESENTATION_SNAPSHOT_MAX_EXTERNALS];
static size_t s_external_count;

bool presentation_snapshot_register_external_transform(const void *transform) {
    size_t index;

    if (transform == NULL || !presentation_snapshot_enabled()) {
        return false;
    }
    for (index = 0u; index < s_external_count; index++) {
        if (s_externals[index] == transform) {
            /* Already live. Re-registering is a fresh lifetime -- the caller
             * reused the slot -- so reissue the generation but do not add a
             * second entry, which would publish the pose twice. */
            presentation_snapshot_note_spawn(transform);
            return true;
        }
    }
    if (s_external_count >= PRESENTATION_SNAPSHOT_MAX_EXTERNALS) {
        return false;
    }
    s_external_topology_keys[s_external_count] = 0u;
    s_externals[s_external_count++] = transform;
    if (s_external_count > s_stats.external_peak) {
        s_stats.external_peak = s_external_count;
    }
    presentation_snapshot_note_spawn(transform);
    return true;
}

void presentation_snapshot_unregister_external_transform(
    const void *transform) {
    size_t index;

    if (transform == NULL) {
        return;
    }
    for (index = 0u; index < s_external_count; index++) {
        if (s_externals[index] != transform) {
            continue;
        }
        s_external_count--;
        s_externals[index] = s_externals[s_external_count];
        s_external_topology_keys[index] =
            s_external_topology_keys[s_external_count];
        s_externals[s_external_count] = NULL;
        s_external_topology_keys[s_external_count] = 0u;
        presentation_snapshot_note_free(transform);
        return;
    }
}

bool presentation_snapshot_set_external_topology_key(const void *transform,
                                                     uint16_t key) {
    size_t index;

    if (transform == NULL) {
        return false;
    }
    for (index = 0u; index < s_external_count; index++) {
        if (s_externals[index] == transform) {
            s_external_topology_keys[index] = key;
            return true;
        }
    }
    return false;
}

bool presentation_snapshot_external_topology_key_at(size_t index,
                                                     uint16_t *out_key) {
    if (out_key == NULL || index >= s_external_count) {
        return false;
    }
    *out_key = s_external_topology_keys[index];
    return true;
}

size_t presentation_snapshot_external_transform_count(void) {
    if (s_external_count != 0u && s_capturing) {
        /* Counted here rather than in the walk: this is the call the walk makes
         * to decide whether it has anything to copy, so a capture that reaches
         * a non-empty registry is exactly what the counter means. */
        s_stats.external_captures++;
    }
    return s_external_count;
}

const void *presentation_snapshot_external_transform_at(size_t index) {
    return index < s_external_count ? s_externals[index] : NULL;
}


void presentation_snapshot_authored_cameras_begin(uint64_t authored_tick) {
    memset(&s_authored_cameras, 0, sizeof(s_authored_cameras));
    if (!presentation_snapshot_enabled()) {
        return;
    }
    s_authored_cameras.tick = authored_tick;
    s_authored_cameras.active = true;
}

bool presentation_snapshot_authored_camera_record(
    const PresentationCameraEntry *sample) {
    uint8_t viewport_mask;
    int viewport_index;

    if (!s_authored_cameras.active || sample == NULL) {
        return false;
    }
    viewport_index = sample->viewport_index;
    if (viewport_index < 0 ||
        viewport_index >= PRESENTATION_SNAPSHOT_MAX_VIEWPORTS ||
        sample->camera_id < 0 ||
        sample->camera_id >= PRESENTATION_SNAPSHOT_MAX_CAMERAS) {
        return false;
    }
    viewport_mask = (uint8_t)(1u << viewport_index);
    if ((s_authored_cameras.valid_mask & viewport_mask) != 0u &&
        memcmp(&s_authored_cameras.samples[viewport_index], sample,
               sizeof(*sample)) != 0) {
        /* One replayed viewport cannot safely substitute two camera recipes.
         * Preserve the first recipe and make the whole set fail closed. */
        s_authored_cameras.conflict_mask |= viewport_mask;
        return false;
    }
    s_authored_cameras.samples[viewport_index] = *sample;
    s_authored_cameras.valid_mask |= viewport_mask;
    return true;
}

size_t presentation_snapshot_authored_cameras_copy(
    uint64_t authored_tick, PresentationCameraEntry *out, size_t capacity) {
    size_t count = 0u;

    if (!s_authored_cameras.active || s_authored_cameras.tick != authored_tick ||
        s_authored_cameras.conflict_mask != 0u || out == NULL ||
        capacity == 0u) {
        return 0u;
    }
    while (count < PRESENTATION_SNAPSHOT_MAX_VIEWPORTS &&
           (s_authored_cameras.valid_mask & (uint8_t)(1u << count)) != 0u) {
        if (count >= capacity) {
            return 0u;
        }
        out[count] = s_authored_cameras.samples[count];
        count++;
    }
    /* Sparse ownership is not a camera list. Fail closed instead of shifting
     * a later viewport into an earlier snapshot slot. */
    if (s_authored_cameras.valid_mask !=
        (uint8_t)((1u << count) - 1u)) {
        return 0u;
    }
    return count;
}

static int pick_write_slot(void) {
    for (int slot = 0; slot < 3; slot++) {
        if (slot != s_current && slot != s_previous) {
            return slot;
        }
    }
    return 0; /* unreachable: three slots, at most two published */
}

static size_t frame_lookup(const PresentationSnapshot *frame,
                           const void *address) {
    const size_t mask = PRESENTATION_SNAPSHOT_INDEX_SLOTS - 1u;
    size_t slot;

    if (frame == NULL || !frame->valid || address == NULL) {
        return (size_t)-1;
    }
    slot = hash_address(address, mask);
    for (size_t probe = 0; probe <= mask; probe++) {
        uint16_t biased = frame->index[slot];
        if (biased == 0) {
            return (size_t)-1;
        }
        if (frame->objects[biased - 1u].address == address) {
            return (size_t)(biased - 1u);
        }
        slot = (slot + 1u) & mask;
    }
    return (size_t)-1;
}

static void frame_index_build(PresentationSnapshot *frame) {
    const size_t mask = PRESENTATION_SNAPSHOT_INDEX_SLOTS - 1u;

    memset(frame->index, 0, sizeof(frame->index));
    for (size_t entry = 0; entry < frame->object_count; entry++) {
        size_t slot = hash_address(frame->objects[entry].address, mask);
        while (frame->index[slot] != 0) {
            slot = (slot + 1u) & mask;
        }
        frame->index[slot] = (uint16_t)(entry + 1u);
    }
}

static float distance_squared(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static void presentation_snapshot_capture_begin_impl(uint64_t authored_tick) {
    PresentationSnapshot *write;

    if (!presentation_snapshot_enabled()) {
        return;
    }
    s_capture_serial++;
    s_write = pick_write_slot();
    write = &s_frames[s_write];
    write->valid = false;
    write->generation = 0;
    write->stage_generation = s_stage_generation;
    write->authored_tick = authored_tick;
    write->object_count = 0;
    write->camera_count = 0;
    s_write_failed = false;
    s_capturing = true;
}

void presentation_snapshot_capture_begin(void) {
    /* Unit/synthetic writers get a deterministic adjacent timeline without
     * needing to know the production scheduler's tick index. */
    presentation_snapshot_capture_begin_impl(s_capture_serial);
}

void presentation_snapshot_capture_begin_authored(uint64_t authored_tick) {
    presentation_snapshot_capture_begin_impl(authored_tick);
}

bool presentation_snapshot_capture_object(
    const PresentationObjectEntry *sample) {
    PresentationSnapshot *write;
    PresentationObjectEntry *entry;
    SnapIdentity *identity;
    bool discontinuous;

    if (!s_capturing || sample == NULL || sample->address == NULL) {
        return false;
    }
    if (s_write_failed) {
        return false;
    }
    write = &s_frames[s_write];
    if (write->object_count >= PRESENTATION_SNAPSHOT_MAX_OBJECTS) {
        /* Atomic failure: no partial snapshot is ever published. */
        s_write_failed = true;
        return false;
    }

    identity = identity_find(sample->address);
    if (identity == NULL) {
        /*
         * Not registered: either the seam was switched on mid-run, or the
         * object reached gObjPtrList through a path the lifecycle hooks do
         * not cover. Register it now with a fresh generation — the safe
         * direction, since it makes the object look newly spawned and
         * suppresses interpolation for one tick rather than risking a blend
         * across an unknown identity.
         */
        identity = identity_insert(sample->address);
        if (identity == NULL) {
            s_write_failed = true;
            return false;
        }
        identity->generation = ++s_generation_serial;
        identity->last_capture = 0;
    }

    /*
     * Continuity requires the identity to have been captured by the
     * IMMEDIATELY preceding tick's capture. An object that missed a tick
     * (absent from the list, or present during a capture that failed whole)
     * has no adjacent pair and must not be blended across the gap.
     */
    discontinuous = identity->last_capture == 0 ||
                    identity->last_capture + 1u != s_capture_serial;
    if (!discontinuous) {
        const float moved =
            distance_squared(identity->last_position, sample->position);
        if (moved > PRESENTATION_SNAPSHOT_TELEPORT_UNITS *
                        PRESENTATION_SNAPSHOT_TELEPORT_UNITS) {
            discontinuous = true;
        }
    }

    entry = &write->objects[write->object_count++];
    *entry = *sample;
    entry->generation = identity->generation;
    entry->discontinuity = (uint8_t)(discontinuous || sample->discontinuity);
    return true;
}

bool presentation_snapshot_capture_camera(
    const PresentationCameraEntry *sample) {
    PresentationSnapshot *write;
    PresentationCameraEntry *entry;
    SnapCameraHistory *history;
    bool discontinuous;
    size_t viewport;

    if (!s_capturing || sample == NULL || s_write_failed) {
        return false;
    }
    write = &s_frames[s_write];
    if (write->camera_count >= PRESENTATION_SNAPSHOT_MAX_VIEWPORTS) {
        s_write_failed = true;
        return false;
    }
    viewport = write->camera_count;
    history = &s_camera_history[viewport];

    /* A viewport that changes which gCameras[] entry it draws (cutscene
     * camera on/off) or crosses between the safe aperture and presentation
     * region is a scene cut, not motion. Numeric viewport coordinates are not
     * a cut: Track Select and post-race deliberately animate those bounds. */
    discontinuous = history->last_capture == 0 ||
                    history->last_capture + 1u != s_capture_serial ||
                    history->camera_id != sample->camera_id ||
                    history->last_world_region != sample->world_region;
    /* Notes are filed per VIEWPORT (see s_camera_cut_pending): the game-side
     * sites name a player index, and this is the capture of that player's
     * viewport. Consume the bit here — the note has now been applied to the
     * capture it was raised for, and commit's unconsumed sweep must not see
     * it. */
    if ((s_camera_cut_pending & (1u << (unsigned)viewport)) != 0u) {
        discontinuous = true; /* the game snapped this camera (see the note) */
        s_camera_cut_pending &= ~(1u << (unsigned)viewport);
        s_stats.camera_cut_consumed++;
    }
    /* Task 9: a standing exclusion holds this viewport discontinuous on
     * EVERY capture while set, not only the tick a note was raised -- see
     * presentation_snapshot_set_camera_excluded. */
    if ((s_camera_excluded & (1u << (unsigned)viewport)) != 0u) {
        discontinuous = true;
        s_stats.camera_excluded_captures++;
    }
    if (!discontinuous) {
        const float moved =
            distance_squared(history->last_position, sample->position);
        if (moved > PRESENTATION_SNAPSHOT_TELEPORT_UNITS *
                        PRESENTATION_SNAPSHOT_TELEPORT_UNITS) {
            discontinuous = true;
        }
    }
    /*
     * Angle and FOV cut clauses (presentation-safety plan Task 4).
     *
     * Position can stay well under PRESENTATION_SNAPSHOT_TELEPORT_UNITS while
     * a re-aim is still a cut: the TT-cam spectate switch and the post-race
     * spectator handoff both swap to a camera object a few hundred units from
     * the last one but pointed somewhere else entirely. Left to the distance
     * clause alone, that reaches resolve_camera_pair as ordinary motion, which
     * blends position and FOV across the cut while only the yaw axis, once it
     * crosses PRESENTATION_SNAPSHOT_ROTATION_SNAP, gets forced to its
     * endpoint -- one axis snapping ahead of a pair the rest of the frame
     * still thinks is continuous. mdkr_yaw_delta_deg is the exact shortest-arc
     * helper resolve_camera_pair's own audit uses, reused rather than
     * re-derived so the two can never disagree about what a degree is.
     *
     * Filed the same way the distance clause is: setting `discontinuous`
     * here is what makes this the SAME atomic cut the position clause
     * produces -- one flag, consumed whole by resolve_camera_pair, so a
     * cut on any one of these clauses holds the entire camera pose for
     * this tick rather than letting position, rotation and FOV disagree
     * about whether the pair still exists.
     */
    if (!discontinuous) {
        const float yaw_delta_deg = mdkr_yaw_delta_deg(
            (uint16_t)history->last_rotation_y, (uint16_t)sample->rotation_y);
        const int16_t view_pitch = camera_composed_pitch(
            sample->rotation_x, sample->pitch);
        const float pitch_delta_deg = mdkr_yaw_delta_deg(
            (uint16_t)history->last_view_pitch, (uint16_t)view_pitch);
        const float roll_delta_deg = mdkr_yaw_delta_deg(
            (uint16_t)history->last_rotation_z,
            (uint16_t)sample->rotation_z);
        if (yaw_delta_deg > MDKR_CUT_VIEW_ANGLE_DEG ||
            yaw_delta_deg < -MDKR_CUT_VIEW_ANGLE_DEG ||
            pitch_delta_deg > MDKR_CUT_VIEW_ANGLE_DEG ||
            pitch_delta_deg < -MDKR_CUT_VIEW_ANGLE_DEG ||
            roll_delta_deg > MDKR_CUT_VIEW_ANGLE_DEG ||
            roll_delta_deg < -MDKR_CUT_VIEW_ANGLE_DEG) {
            discontinuous = true;
        }
    }
    if (!discontinuous) {
        const float fov_delta_deg = sample->fov - history->last_fov;
        if (fov_delta_deg > MDKR_CUT_FOV_DEG ||
            fov_delta_deg < -MDKR_CUT_FOV_DEG) {
            discontinuous = true;
        }
    }

    entry = &write->cameras[write->camera_count++];
    *entry = *sample;
    entry->viewport_index = (int32_t)viewport;
    entry->discontinuity = (uint8_t)(discontinuous || sample->discontinuity);
    return true;
}

void presentation_snapshot_capture_commit(void) {
    PresentationSnapshot *write;

    if (!s_capturing) {
        return;
    }
    s_capturing = false;
    write = &s_frames[s_write];

    /* A lifecycle hook that could not register an identity this tick makes the
     * whole frame fail, exactly as an in-capture insert failure does. Spent
     * here so it costs one snapshot, not every later one. */
    if (s_identity_insert_failed) {
        s_identity_insert_failed = false;
        s_write_failed = true;
    }

    /*
     * A note is spent by the capture of the viewport it names, in
     * presentation_snapshot_capture_camera — never here.
     *
     * Clearing the whole mask at commit is what turned the old ID-space
     * mismatch from a miss into a silent loss: a note the consumer failed to
     * find was destroyed on the same tick, so the cut it described could never
     * reach any later capture and the camera blended straight through it. A
     * note for a viewport this tick did not capture is therefore CARRIED.
     *
     * A note that survived the publish of its own viewport is impossible while
     * both sides key on viewport index. Count it (and spend it, so one keying
     * bug cannot latch a permanent cut) rather than assume it away.
     */
    if (!s_write_failed) {
        for (size_t viewport = 0; viewport < write->camera_count; viewport++) {
            const uint32_t bit = 1u << (unsigned)viewport;
            if ((s_camera_cut_pending & bit) != 0u) {
                s_camera_cut_pending &= ~bit;
                s_stats.camera_cut_unconsumed++;
            }
        }
    }

    if (s_write_failed) {
        /*
         * Retention (the gfx_shadow_frame rule): the failed frame is simply
         * never published, and the previously published pair stays exactly
         * as it was. Consumers keep drawing the last good pair; nothing sees
         * a half-built snapshot.
         */
        write->valid = false;
        write->object_count = 0;
        write->camera_count = 0;
        s_stats.overflows++;
        return;
    }

    s_publish_serial++;
    write->generation = s_publish_serial;
    write->valid = true;
    frame_index_build(write);

    /* Registry/history updates happen only for a snapshot that publishes,
     * so a failed capture cannot desynchronise continuity bookkeeping. */
    for (size_t index = 0; index < write->object_count; index++) {
        const PresentationObjectEntry *entry = &write->objects[index];
        SnapIdentity *identity = identity_find(entry->address);
        if (identity != NULL) {
            identity->last_position[0] = entry->position[0];
            identity->last_position[1] = entry->position[1];
            identity->last_position[2] = entry->position[2];
            identity->last_capture = s_capture_serial;
        }
        if (entry->discontinuity) {
            s_stats.discontinuities++;
        }
    }
    for (size_t index = 0; index < write->camera_count; index++) {
        const PresentationCameraEntry *entry = &write->cameras[index];
        SnapCameraHistory *history = &s_camera_history[index];
        if (camera_cut_trace_enabled()) {
            /* s_current still names the frame that is about to become
             * previous, so this is exactly the pair resolve_camera will be
             * offered — same four conditions, in the same order. */
            const PresentationSnapshot *before =
                s_current >= 0 ? &s_frames[s_current] : NULL;
            const bool blend =
                !entry->discontinuity && before != NULL && before->valid &&
                before->stage_generation == write->stage_generation &&
                index < before->camera_count &&
                before->cameras[index].camera_id == entry->camera_id;
            printf("[CAMERA-CUT] tick=%llu vp=%zu cams=%zu cam=%d blend=%d "
                   "region=%u x=%.3f y=%.3f z=%.3f rx=%d ry=%d rz=%d "
                   "pitch=%d fov=%.4f vfov=%.4f near=%.3f far=%.3f\n",
                   (unsigned long long)write->authored_tick, index,
                   write->camera_count, (int)entry->camera_id, blend ? 1 : 0,
                   (unsigned)entry->world_region, (double)entry->position[0],
                   (double)entry->position[1], (double)entry->position[2],
                   (int)entry->rotation_x, (int)entry->rotation_y,
                   (int)entry->rotation_z, (int)entry->pitch,
                   (double)entry->fov, (double)entry->vertical_fov,
                   (double)entry->near_plane, (double)entry->far_plane);
        }
        history->camera_id = entry->camera_id;
        history->last_position[0] = entry->position[0];
        history->last_position[1] = entry->position[1];
        history->last_position[2] = entry->position[2];
        history->last_rotation_y = entry->rotation_y;
        history->last_view_pitch = camera_composed_pitch(
            entry->rotation_x, entry->pitch);
        history->last_rotation_z = entry->rotation_z;
        history->last_fov = entry->fov;
        history->last_world_region = entry->world_region;
        history->last_capture = s_capture_serial;
        if (entry->discontinuity) {
            s_stats.discontinuities++;
        }
        if (entry->camera_id >= 0 &&
            entry->camera_id < PRESENTATION_SNAPSHOT_MAX_CAMERAS) {
            s_stats.camera_id_mask |= 1ull << (unsigned)entry->camera_id;
            s_stats.camera_captures[entry->camera_id]++;
        }
    }

    s_previous = s_current;
    s_current = s_write;
    s_write = -1;
    s_stats.captures++;
    if ((uint64_t)write->object_count > s_stats.objects_peak) {
        s_stats.objects_peak = (uint64_t)write->object_count;
    }
}

void presentation_snapshot_stage_reset(void) {
    if (!presentation_snapshot_enabled()) {
        return;
    }
    /*
     * Spec §5: level transitions reset snapshot history so interpolation
     * never crosses two unrelated scenes. Both published frames are dropped
     * and every identity is retired — the object pool is about to be torn
     * down and rebuilt, so every surviving address is a coincidence.
     *
     * s_generation_serial deliberately keeps counting: generations are never
     * reissued, across levels or otherwise.
     */
    for (int slot = 0; slot < 3; slot++) {
        s_frames[slot].valid = false;
        s_frames[slot].object_count = 0;
        s_frames[slot].camera_count = 0;
        s_frames[slot].generation = 0;
        s_frames[slot].authored_tick = 0;
    }
    memset(s_camera_history, 0, sizeof(s_camera_history));
    memset(&s_authored_cameras, 0, sizeof(s_authored_cameras));
    identity_reset();
    /* The registry names storage the renderer owns, but a stage boundary
     * retires every identity, and a member that survives the transition must
     * re-register so it is issued a generation from the new epoch rather than
     * carrying a retired one. */
    memset(s_externals, 0, sizeof(s_externals));
    memset(s_external_topology_keys, 0, sizeof(s_external_topology_keys));
    s_external_count = 0u;
    s_current = -1;
    s_previous = -1;
    s_write = -1;
    s_capturing = false;
    s_write_failed = false;
    /* Carried camera-cut notes belong to the stage that raised them. The first
     * capture of every viewport in the new stage is discontinuous on its own
     * account (cleared history), so a carried note has nothing left to protect
     * and would only be consumed as a spurious cut. Dropping it here is not
     * the unconsumed-note failure the counter watches for — that one is a note
     * lost while the viewport it names WAS published. */
    s_camera_cut_pending = 0u;
    /* A standing exclusion belongs to the mode that set it (racer.c
     * re-asserts it every tick the mode is active), and the stage boundary
     * that just retired every identity retired the racer along with it. */
    s_camera_excluded = 0u;
    /* The table is empty again, so the refusal that set this has been undone
     * before any capture could observe it. */
    s_identity_insert_failed = false;
    s_stage_generation++;
    s_stats.resets++;
}

const PresentationSnapshot *presentation_snapshot_current(void) {
    if (s_current < 0) {
        return NULL;
    }
    return &s_frames[s_current];
}

const PresentationSnapshot *presentation_snapshot_previous(void) {
    if (s_previous < 0) {
        return NULL;
    }
    return &s_frames[s_previous];
}

bool presentation_snapshot_replay_target_tick(
    uint64_t authored_task_tick, uint64_t *target_tick) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    const PresentationSnapshot *previous = presentation_snapshot_previous();

    if (target_tick == NULL || current == NULL || previous == NULL ||
        !current->valid || !previous->valid ||
        current->stage_generation != previous->stage_generation ||
        authored_task_tick == UINT64_MAX ||
        previous->authored_tick != authored_task_tick ||
        current->authored_tick != authored_task_tick + 1u) {
        return false;
    }
    *target_tick = current->authored_tick;
    return true;
}

bool presentation_snapshot_authored_endpoint_tick(uint64_t *authored_tick) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    const PresentationSnapshot *previous = presentation_snapshot_previous();

    if (authored_tick == NULL || current == NULL || previous == NULL ||
        !current->valid || !previous->valid ||
        current->stage_generation != previous->stage_generation) {
        return false;
    }
    *authored_tick = previous->authored_tick;
    return true;
}

void presentation_snapshot_get_stats(PresentationSnapshotStats *out) {
    if (out != NULL) {
        *out = s_stats;
    }
}

/* ---- authored UV-scroll rates ------------------------------------------- *
 * See the header for why the authored rate is published instead of a measured
 * tick-to-tick difference. The store is a flat array walked linearly: a level
 * drives a few dozen batches at most, the walk touches it once per G_TRIN,
 * and a flat array of spans is both the cheapest and the only structure that
 * can answer "which batch contains this address" when a display list splits a
 * batch into several polygon commands. */
typedef struct UvScrollAuthoredSpan {
    const unsigned char *first;
    const unsigned char *end;
    PresentationUvScrollAuthored rate;
    bool ambiguous;
} UvScrollAuthoredSpan;

static UvScrollAuthoredSpan
    s_uv_authored[PRESENTATION_UV_SCROLL_AUTHORED_MAX_SPANS];
static size_t s_uv_authored_count;
static bool s_uv_authored_wanted;
/* Diagnostic census for the authored-rate chain (reported in [SNAPSHOT]):
 * a run whose falls hold at 30 Hz needs to say WHERE the chain broke —
 * never armed, never registered, refused as overlap, or looked up and
 * missed. Counters only; no decision reads them. */
static uint64_t s_uv_authored_arms;
static uint64_t s_uv_authored_register_calls;
static uint64_t s_uv_authored_register_overlaps;
static uint64_t s_uv_authored_register_capacity;
static uint64_t s_uv_authored_lookup_calls;
static uint64_t s_uv_authored_lookup_hits;
static uint64_t s_uv_authored_lookup_ambiguous;

bool presentation_uv_scroll_authored_wanted(void) {
    return s_uv_authored_wanted;
}

void presentation_uv_scroll_authored_set_wanted(bool wanted) {
    if (wanted) {
        s_uv_authored_arms++;
    }
    s_uv_authored_wanted = wanted;
}

void presentation_uv_scroll_authored_reset(void) {
    s_uv_authored_count = 0u;
}

void presentation_uv_scroll_authored_register(
    const void *first, const void *end,
    const PresentationUvScrollAuthored *rate) {
    const unsigned char *begin = (const unsigned char *)first;
    const unsigned char *last = (const unsigned char *)end;
    size_t index;

    s_uv_authored_register_calls++;
    if (begin == NULL || last == NULL || last <= begin || rate == NULL) {
        return;
    }
    for (index = 0u; index < s_uv_authored_count; index++) {
        UvScrollAuthoredSpan *span = &s_uv_authored[index];
        if (span->first >= last || span->end <= begin) {
            continue;
        }
        /* Overlap while the table is live. Either two texscroll objects drive
         * one batch — in which case neither rate describes the surface — or a
         * tick's walk never consumed the previous registration. Both are
         * resolved by refusing this span and letting measurement decide. */
        span->ambiguous = true;
        s_uv_authored_register_overlaps++;
        return;
    }
    if (s_uv_authored_count >= PRESENTATION_UV_SCROLL_AUTHORED_MAX_SPANS) {
        s_uv_authored_register_capacity++;
        return;
    }
    s_uv_authored[s_uv_authored_count].first = begin;
    s_uv_authored[s_uv_authored_count].end = last;
    s_uv_authored[s_uv_authored_count].rate = *rate;
    s_uv_authored[s_uv_authored_count].ambiguous = false;
    s_uv_authored_count++;
}

bool presentation_uv_scroll_authored_lookup(
    const void *address, PresentationUvScrollAuthored *out) {
    const unsigned char *probe = (const unsigned char *)address;
    size_t index;

    s_uv_authored_lookup_calls++;
    if (probe == NULL || out == NULL) {
        return false;
    }
    for (index = 0u; index < s_uv_authored_count; index++) {
        const UvScrollAuthoredSpan *span = &s_uv_authored[index];
        if (probe < span->first || probe >= span->end) {
            continue;
        }
        if (span->ambiguous) {
            s_uv_authored_lookup_ambiguous++;
            return false;
        }
        *out = span->rate;
        s_uv_authored_lookup_hits++;
        return true;
    }
    return false;
}

void presentation_snapshot_shutdown(void) {
    s_uv_authored_count = 0u;
    s_uv_authored_wanted = false;
    memset(s_frames, 0, sizeof(s_frames));
    memset(s_camera_history, 0, sizeof(s_camera_history));
    memset(&s_authored_cameras, 0, sizeof(s_authored_cameras));
    identity_reset();
    memset(&s_stats, 0, sizeof(s_stats));
    s_current = -1;
    s_previous = -1;
    s_write = -1;
    s_capturing = false;
    s_write_failed = false;
    s_capture_serial = 0;
    s_publish_serial = 0;
    s_stage_generation = 0;
    s_generation_serial = 0;
    s_camera_cut_pending = 0u;
    s_camera_excluded = 0u;
    s_identity_insert_failed = false;
}

static void presentation_snapshot_report(void) {
    printf("[SNAPSHOT] captures=%llu objects_peak=%llu discontinuities=%llu "
           "overflows=%llu resets=%llu camera_mask=%llu "
           "cam0=%llu cam1=%llu cam2=%llu cam3=%llu "
           "cam4=%llu cam5=%llu cam6=%llu cam7=%llu "
           "caminterp0=%llu caminterp1=%llu caminterp2=%llu "
           "caminterp3=%llu caminterp4=%llu caminterp5=%llu "
           "caminterp6=%llu caminterp7=%llu "
           "rotarccheck=%llu rotarcsnap=%llu rotarcviolation=%llu "
           "disconthold=%llu discontblend=%llu "
           "camcutnote=%llu camcutconsumed=%llu camcutunconsumed=%llu "
           "camexcluded=%llu "
           "identityinsertfail=%llu "
           "externalpeak=%llu externalcaptures=%llu "
           "uvauth_arms=%llu uvauth_reg=%llu uvauth_overlap=%llu "
           "uvauth_cap=%llu uvauth_lookup=%llu uvauth_hit=%llu "
           "uvauth_ambig=%llu\n",
           (unsigned long long)s_stats.captures,
           (unsigned long long)s_stats.objects_peak,
           (unsigned long long)s_stats.discontinuities,
           (unsigned long long)s_stats.overflows,
           (unsigned long long)s_stats.resets,
           (unsigned long long)s_stats.camera_id_mask,
           (unsigned long long)s_stats.camera_captures[0],
           (unsigned long long)s_stats.camera_captures[1],
           (unsigned long long)s_stats.camera_captures[2],
           (unsigned long long)s_stats.camera_captures[3],
           (unsigned long long)s_stats.camera_captures[4],
           (unsigned long long)s_stats.camera_captures[5],
           (unsigned long long)s_stats.camera_captures[6],
           (unsigned long long)s_stats.camera_captures[7],
           (unsigned long long)s_stats.camera_interpolations[0],
           (unsigned long long)s_stats.camera_interpolations[1],
           (unsigned long long)s_stats.camera_interpolations[2],
           (unsigned long long)s_stats.camera_interpolations[3],
           (unsigned long long)s_stats.camera_interpolations[4],
           (unsigned long long)s_stats.camera_interpolations[5],
           (unsigned long long)s_stats.camera_interpolations[6],
           (unsigned long long)s_stats.camera_interpolations[7],
           (unsigned long long)s_stats.rotation_arc_checks,
           (unsigned long long)s_stats.rotation_arc_snaps,
           (unsigned long long)s_stats.rotation_arc_violations,
           (unsigned long long)s_stats.discontinuity_holds,
           (unsigned long long)s_stats.discontinuity_blends,
           (unsigned long long)s_stats.camera_cut_notes,
           (unsigned long long)s_stats.camera_cut_consumed,
           (unsigned long long)s_stats.camera_cut_unconsumed,
           (unsigned long long)s_stats.camera_excluded_captures,
           (unsigned long long)s_stats.identity_insert_failures,
           (unsigned long long)s_stats.external_peak,
           (unsigned long long)s_stats.external_captures,
           (unsigned long long)s_uv_authored_arms,
           (unsigned long long)s_uv_authored_register_calls,
           (unsigned long long)s_uv_authored_register_overlaps,
           (unsigned long long)s_uv_authored_register_capacity,
           (unsigned long long)s_uv_authored_lookup_calls,
           (unsigned long long)s_uv_authored_lookup_hits,
           (unsigned long long)s_uv_authored_lookup_ambiguous);
    fflush(stdout);
}

/* ---- pure interpolation helpers ------------------------------------------ */

double presentation_alpha(uint64_t numerator, uint64_t denominator) {
    if (denominator == 0u || numerator == 0u) {
        return 0.0;
    }
    if (numerator >= denominator) {
        return 1.0;
    }
    return (double)numerator / (double)denominator;
}

float presentation_lerp1(float a, float b, uint64_t numerator,
                         uint64_t denominator) {
    double alpha;

    if (denominator == 0u || numerator == 0u) {
        return a; /* exact endpoint: no arithmetic touches the bits */
    }
    if (numerator >= denominator) {
        return b;
    }
    alpha = (double)numerator / (double)denominator;
    return (float)((double)a + ((double)b - (double)a) * alpha);
}

void presentation_lerp3(const float a[3], const float b[3],
                        uint64_t numerator, uint64_t denominator,
                        float out[3]) {
    if (a == NULL || b == NULL || out == NULL) {
        return;
    }
    for (int axis = 0; axis < 3; axis++) {
        out[axis] = presentation_lerp1(a[axis], b[axis], numerator,
                                       denominator);
    }
}

uint8_t presentation_lerp_u8(uint8_t a, uint8_t b, uint64_t numerator,
                             uint64_t denominator) {
    const float value =
        presentation_lerp1((float)a, (float)b, numerator, denominator);
    return (uint8_t)(value + 0.5f);
}

uint8_t presentation_particle_opacity_u8(int16_t opacity) {
    return (uint8_t)(((uint16_t)opacity) >> 8);
}

uint8_t presentation_scale_opacity_u8(uint8_t authored, uint8_t current,
                                      uint8_t target) {
    uint32_t scaled;

    if (current == 0u || current == target || authored == 0u) {
        return authored;
    }
    scaled = ((uint32_t)authored * (uint32_t)target + current / 2u) /
             current;
    return (uint8_t)(scaled > UINT8_MAX ? UINT8_MAX : scaled);
}

int16_t presentation_lerp_angle(int16_t a, int16_t b, uint64_t numerator,
                                uint64_t denominator) {
    int32_t delta;
    double alpha;
    int32_t stepped;

    if (denominator == 0u || numerator == 0u) {
        return a;
    }
    if (numerator >= denominator) {
        return b;
    }
    /* The narrowing to int16_t IS the shortest-arc selection. */
    delta = (int16_t)((uint16_t)((uint16_t)b - (uint16_t)a));
    if (presentation_snapshot_test_long_arc()) {
        /*
         * Negative control only (see the seam block at the top of this file).
         * Re-express the same rotation as the LONG way round and smear across
         * it: the object turns backwards through the rest of the circle to
         * arrive exactly where the short arc would have taken it. That is
         * artifact class C3, and the snap below is one of the two things that
         * prevent it -- so the seam deliberately reaches EVERY delta rather
         * than only the past-quarter-turn ones. A control that fired only on
         * rotations this route may not contain would be a control whose red
         * depends on content, which is the property it exists to not have.
         */
        if (delta != 0) {
            delta = delta > 0 ? delta - 0x10000 : delta + 0x10000;
        }
    } else if (delta > PRESENTATION_SNAPSHOT_ROTATION_SNAP ||
               delta < -PRESENTATION_SNAPSHOT_ROTATION_SNAP) {
        return b; /* beyond a quarter turn: snap, never smear */
    }
    alpha = (double)numerator / (double)denominator;
    stepped = (int32_t)((double)delta * alpha); /* truncates toward zero */
    /* Unsigned addition so the wrap around 0x7FFF/0x8000 is defined. */
    return (int16_t)(uint16_t)((uint16_t)a + (uint16_t)(int16_t)stepped);
}

float mdkr_yaw_delta_deg(uint16_t a, uint16_t b) {
    /* (a - b), not (b - a): this is the plan's specified contract (Task 4
     * brief step 2), and Task 6 reuses this exact helper, so the sign
     * convention has to match what both call sites were written against
     * rather than presentation_lerp_angle's own (b - a) delta above. The
     * wrap itself is the same trick: narrowing the unsigned difference to
     * int16_t IS the shortest-arc selection, before this converts the raw
     * N64 angle unit to degrees. */
    const int16_t delta = (int16_t)(uint16_t)(a - b);
    return (float)delta * (360.0f / 65536.0f);
}

/*
 * Grade one interpolated angle against the arc it was allowed to travel.
 *
 * Two clauses, because a reconstruction can be wrong about a rotation in two
 * unrelated ways and only one of them is about magnitude:
 *
 *   - past a quarter turn there is no arc worth smearing across, because the
 *     shortest path is no longer the one the object took. The only legal
 *     reconstruction is the authored endpoint itself, and anything else is the
 *     long-way-round smear of artifact class C3;
 *   - inside a quarter turn the result must lie ON the shortest arc: same
 *     direction as `b - a` narrowed to int16_t, and never past its far end. An
 *     interpolated present lies BETWEEN its endpoints -- the same statement
 *     check_effect_shell_envelope.py makes about position, in the units
 *     rotation is measured in.
 *
 * Recomputed here from (a, b, out) rather than reported by
 * presentation_lerp_angle itself, deliberately. A helper that both performs
 * the arithmetic and grades it can only ever agree with itself, and the
 * mutation this exists to catch lives inside that arithmetic. It is the same
 * independence check_camera_snapshot_coverage.py buys by classifying cuts out
 * of raw poses instead of asking the interpolator what it thought.
 *
 * Counters only. No branch above depends on them, so the audit cannot move a
 * pixel or a hash.
 */
static void rotation_arc_audit(int16_t a, int16_t b, int16_t out,
                               uint64_t numerator, uint64_t denominator) {
    const int32_t delta = (int16_t)((uint16_t)((uint16_t)b - (uint16_t)a));
    const int32_t step = (int16_t)((uint16_t)((uint16_t)out - (uint16_t)a));

    s_stats.rotation_arc_checks++;
    /*
     * The two exact-endpoint contracts come first, because the interior rules
     * below are false at the ends. A replay at alpha 0 must reproduce the
     * authored endpoint bit for bit -- `a`, NOT the snap's `b` -- and the
     * first draft of this audit graded those presents by the snap clause and
     * reported one violation per past-quarter-turn alpha-0 resolve on the
     * three-lap route. The counter was right and the grader was wrong, which
     * is the more useful of the two ways to learn that at this stage.
     */
    if (denominator == 0u || numerator == 0u) {
        if (out != a) {
            s_stats.rotation_arc_violations++;
        }
        return;
    }
    if (numerator >= denominator) {
        if (out != b) {
            s_stats.rotation_arc_violations++;
        }
        return;
    }
    if (delta > PRESENTATION_SNAPSHOT_ROTATION_SNAP ||
        delta < -PRESENTATION_SNAPSHOT_ROTATION_SNAP) {
        s_stats.rotation_arc_snaps++;
        if (out != b) {
            s_stats.rotation_arc_violations++;
        }
        return;
    }
    if (step == 0) {
        return; /* a delta too small to move the fixed point at this alpha */
    }
    if ((step < 0) != (delta < 0)) {
        s_stats.rotation_arc_violations++; /* wrong way round the circle */
        return;
    }
    if (step < 0 ? step < delta : step > delta) {
        s_stats.rotation_arc_violations++; /* overshot the authored endpoint */
    }
}

bool presentation_discrete_use_current(uint64_t numerator,
                                       uint64_t denominator) {
    if (denominator == 0u) {
        return false;
    }
    return numerator >= denominator;
}

/* ---- resolution ---------------------------------------------------------- */

static void object_pose_from_entry(const PresentationObjectEntry *entry,
                                   PresentationObjectPose *out) {
    out->position[0] = entry->position[0];
    out->position[1] = entry->position[1];
    out->position[2] = entry->position[2];
    out->scale = entry->scale;
    out->rotation_y = entry->rotation_y;
    out->rotation_x = entry->rotation_x;
    out->rotation_z = entry->rotation_z;
    out->animation_id = entry->animation_id;
    out->animation_frame = entry->animation_frame;
    out->model_index = entry->model_index;
    out->opacity = entry->opacity;
    out->is_particle = entry->is_particle;
    out->interpolated = 0u;
}

static bool resolve_object_pair(const PresentationSnapshot *current,
                                size_t index, uint64_t numerator,
                                uint64_t denominator,
                                PresentationObjectPose *out) {
    const PresentationObjectEntry *entry = &current->objects[index];
    const PresentationSnapshot *previous = presentation_snapshot_previous();
    const PresentationObjectEntry *before;
    size_t before_index;
    bool use_current;

    object_pose_from_entry(entry, out);
    if (entry->discontinuity) {
        if (!presentation_snapshot_test_ignore_discontinuity()) {
            s_stats.discontinuity_holds++;
            return true; /* spawn / teleport: current pose, never blended */
        }
        /* Negative control only (see the seam block at the top of this file):
         * fall through and blend a respawn, which draws the racer smeared
         * along the line between where it died and where it came back. The
         * blend is counted at the bottom of this function so the production
         * zero is a zero for a reason rather than for want of a witness. */
    }
    if (previous == NULL || !previous->valid ||
        previous->stage_generation != current->stage_generation) {
        return true;
    }
    before_index = frame_lookup(previous, entry->address);
    if (before_index == (size_t)-1) {
        return true;
    }
    before = &previous->objects[before_index];
    if (before->generation != entry->generation) {
        /* Same address, different life: a freed slot reused by a new spawn.
         * This is the case the generation exists to catch. */
        return true;
    }

    presentation_lerp3(before->position, entry->position, numerator,
                       denominator, out->position);
    out->scale =
        presentation_lerp1(before->scale, entry->scale, numerator, denominator);
    out->rotation_y = presentation_lerp_angle(before->rotation_y,
                                              entry->rotation_y, numerator,
                                              denominator);
    out->rotation_x = presentation_lerp_angle(before->rotation_x,
                                              entry->rotation_x, numerator,
                                              denominator);
    out->rotation_z = presentation_lerp_angle(before->rotation_z,
                                              entry->rotation_z, numerator,
                                              denominator);
    rotation_arc_audit(before->rotation_y, entry->rotation_y, out->rotation_y,
                       numerator, denominator);
    rotation_arc_audit(before->rotation_x, entry->rotation_x, out->rotation_x,
                       numerator, denominator);
    rotation_arc_audit(before->rotation_z, entry->rotation_z, out->rotation_z,
                       numerator, denominator);
    out->opacity = presentation_lerp_u8(
        before->opacity, entry->opacity, numerator, denominator);

    /* Discrete: previous until the tick completes (see the header). */
    use_current = presentation_discrete_use_current(numerator, denominator);
    out->animation_id =
        use_current ? entry->animation_id : before->animation_id;
    out->animation_frame =
        use_current ? entry->animation_frame : before->animation_frame;
    out->model_index = use_current ? entry->model_index : before->model_index;
    out->is_particle = use_current ? entry->is_particle : before->is_particle;
    if (entry->discontinuity) {
        s_stats.discontinuity_blends++;
    }
    out->interpolated = 1u;
    return true;
}

bool presentation_snapshot_resolve_object(const void *address,
                                          uint64_t numerator,
                                          uint64_t denominator,
                                          PresentationObjectPose *out) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    size_t index;

    if (out == NULL || current == NULL || !current->valid) {
        return false;
    }
    index = frame_lookup(current, address);
    if (index == (size_t)-1) {
        return false;
    }
    return resolve_object_pair(current, index, numerator, denominator, out);
}

bool presentation_snapshot_resolve_object_generation(
    const void *address, uint64_t generation, uint64_t numerator,
    uint64_t denominator, PresentationObjectPose *out) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    size_t index;

    if (out == NULL || current == NULL || !current->valid || generation == 0u) {
        return false;
    }
    index = frame_lookup(current, address);
    if (index == (size_t)-1 ||
        current->objects[index].generation != generation) {
        return false;
    }
    return resolve_object_pair(current, index, numerator, denominator, out);
}

bool presentation_snapshot_deformation_compatible(const void *address,
                                                   uint64_t generation) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    const PresentationSnapshot *previous = presentation_snapshot_previous();
    const PresentationObjectEntry *now;
    const PresentationObjectEntry *before;
    size_t now_index;
    size_t before_index;

    if (address == NULL || generation == 0u || current == NULL ||
        previous == NULL || !current->valid || !previous->valid ||
        current->stage_generation != previous->stage_generation) {
        return false;
    }
    now_index = frame_lookup(current, address);
    before_index = frame_lookup(previous, address);
    if (now_index == (size_t)-1 || before_index == (size_t)-1) {
        return false;
    }
    now = &current->objects[now_index];
    before = &previous->objects[before_index];
    return !now->discontinuity && !before->discontinuity &&
           !now->is_particle && !before->is_particle &&
           now->generation == generation &&
           before->generation == generation &&
           now->model_index == before->model_index &&
           now->animation_id == before->animation_id;
}

bool presentation_snapshot_particle_deformation_compatible(
    const void *address, uint64_t generation) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    const PresentationSnapshot *previous = presentation_snapshot_previous();
    const PresentationObjectEntry *now;
    const PresentationObjectEntry *before;
    size_t now_index;
    size_t before_index;

    if (address == NULL || generation == 0u || current == NULL ||
        previous == NULL || !current->valid || !previous->valid ||
        current->stage_generation != previous->stage_generation) {
        return false;
    }
    now_index = frame_lookup(current, address);
    before_index = frame_lookup(previous, address);
    if (now_index == (size_t)-1 || before_index == (size_t)-1) {
        return false;
    }
    now = &current->objects[now_index];
    before = &previous->objects[before_index];
    return !now->discontinuity && !before->discontinuity &&
           now->is_particle && before->is_particle &&
           now->generation == generation &&
           before->generation == generation &&
           now->model_index == before->model_index;
}

bool presentation_snapshot_topology_keys_agree(const void *address,
                                                uint64_t generation) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    const PresentationSnapshot *previous = presentation_snapshot_previous();
    size_t now_index;
    size_t before_index;

    if (address == NULL || generation == 0u || current == NULL ||
        previous == NULL || !current->valid || !previous->valid ||
        current->stage_generation != previous->stage_generation) {
        return false;
    }
    now_index = frame_lookup(current, address);
    before_index = frame_lookup(previous, address);
    if (now_index == (size_t)-1 || before_index == (size_t)-1) {
        return false;
    }
    if (current->objects[now_index].generation != generation ||
        previous->objects[before_index].generation != generation) {
        return false;
    }
    return current->objects[now_index].topology_key ==
           previous->objects[before_index].topology_key;
}

bool presentation_snapshot_resolve_object_at(size_t index,
                                             uint64_t numerator,
                                             uint64_t denominator,
                                             PresentationObjectPose *out) {
    const PresentationSnapshot *current = presentation_snapshot_current();

    if (out == NULL || current == NULL || !current->valid ||
        index >= current->object_count) {
        return false;
    }
    return resolve_object_pair(current, index, numerator, denominator, out);
}

bool presentation_snapshot_resolve_camera(int viewport_index,
                                          uint64_t numerator,
                                          uint64_t denominator,
                                          PresentationCameraPose *out) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    const PresentationSnapshot *previous = presentation_snapshot_previous();
    const PresentationCameraEntry *entry;
    const PresentationCameraEntry *before;

    if (out == NULL || current == NULL || !current->valid ||
        viewport_index < 0 ||
        (size_t)viewport_index >= current->camera_count) {
        return false;
    }
    entry = &current->cameras[viewport_index];

    out->camera_id = entry->camera_id;
    out->position[0] = entry->position[0];
    out->position[1] = entry->position[1];
    out->position[2] = entry->position[2];
    out->rotation_x = entry->rotation_x;
    out->rotation_y = entry->rotation_y;
    out->rotation_z = entry->rotation_z;
    out->pitch = entry->pitch;
    out->view_pitch = camera_composed_pitch(entry->rotation_x, entry->pitch);
    out->shake_magnitude = entry->shake_magnitude;
    out->apply_shake = entry->apply_shake;
    out->fov = entry->fov;
    out->vertical_fov = entry->vertical_fov;
    out->aspect = entry->aspect;
    out->near_plane = entry->near_plane;
    out->far_plane = entry->far_plane;
    memcpy(out->viewport, entry->viewport, sizeof(out->viewport));
    out->interpolated = 0u;

    if (entry->discontinuity) {
        if (!presentation_snapshot_test_ignore_discontinuity()) {
            s_stats.discontinuity_holds++;
            return true; /* a cut is not motion: never blend across one */
        }
        /* Negative control only -- the camera-side twin of the object case
         * above, and the mechanism artifact class C2 describes. */
    }
    if (previous == NULL || !previous->valid ||
        previous->stage_generation != current->stage_generation ||
        (size_t)viewport_index >= previous->camera_count) {
        return true;
    }
    before = &previous->cameras[viewport_index];
    if (before->camera_id != entry->camera_id) {
        return true;
    }

    presentation_lerp3(before->position, entry->position, numerator,
                       denominator, out->position);
    out->rotation_x = presentation_lerp_angle(before->rotation_x,
                                              entry->rotation_x, numerator,
                                              denominator);
    out->rotation_y = presentation_lerp_angle(before->rotation_y,
                                              entry->rotation_y, numerator,
                                              denominator);
    out->rotation_z = presentation_lerp_angle(before->rotation_z,
                                              entry->rotation_z, numerator,
                                              denominator);
    out->pitch = presentation_lerp_angle(before->pitch, entry->pitch,
                                         numerator, denominator);
    out->view_pitch = presentation_lerp_angle(
        camera_composed_pitch(before->rotation_x, before->pitch),
        camera_composed_pitch(entry->rotation_x, entry->pitch),
        numerator, denominator);
    rotation_arc_audit(before->rotation_x, entry->rotation_x, out->rotation_x,
                       numerator, denominator);
    rotation_arc_audit(before->rotation_y, entry->rotation_y, out->rotation_y,
                       numerator, denominator);
    rotation_arc_audit(before->rotation_z, entry->rotation_z, out->rotation_z,
                       numerator, denominator);
    rotation_arc_audit(before->pitch, entry->pitch, out->pitch,
                       numerator, denominator);
    rotation_arc_audit(
        camera_composed_pitch(before->rotation_x, before->pitch),
        camera_composed_pitch(entry->rotation_x, entry->pitch),
        out->view_pitch, numerator, denominator);
    out->shake_magnitude = presentation_lerp1(before->shake_magnitude,
                                              entry->shake_magnitude,
                                              numerator, denominator);
    out->fov =
        presentation_lerp1(before->fov, entry->fov, numerator, denominator);
    out->vertical_fov = presentation_lerp1(before->vertical_fov,
                                           entry->vertical_fov, numerator,
                                           denominator);
    out->aspect = presentation_lerp1(before->aspect, entry->aspect, numerator,
                                     denominator);
    out->near_plane = presentation_lerp1(before->near_plane, entry->near_plane,
                                         numerator, denominator);
    out->far_plane = presentation_lerp1(before->far_plane, entry->far_plane,
                                        numerator, denominator);
    for (int axis = 0; axis < 4; axis++) {
        out->viewport[axis] = presentation_lerp1(
            before->viewport[axis], entry->viewport[axis], numerator,
            denominator);
    }
    /* apply_shake is discrete: previous until the tick completes. */
    out->apply_shake =
        presentation_discrete_use_current(numerator, denominator)
            ? entry->apply_shake
            : before->apply_shake;
    if (entry->discontinuity) {
        s_stats.discontinuity_blends++;
    }
    out->interpolated = 1u;
    if (entry->camera_id >= 0 &&
        entry->camera_id < PRESENTATION_SNAPSHOT_MAX_CAMERAS) {
        s_stats.camera_interpolations[entry->camera_id]++;
    }
    return true;
}

bool presentation_snapshot_camera_pan_rate_deg(int viewport_index,
                                               float *out_yaw_delta_deg) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    const PresentationSnapshot *previous = presentation_snapshot_previous();
    const PresentationCameraEntry *entry;
    const PresentationCameraEntry *before;

    if (out_yaw_delta_deg == NULL || current == NULL || !current->valid ||
        viewport_index < 0 ||
        (size_t)viewport_index >= current->camera_count) {
        return false;
    }
    entry = &current->cameras[viewport_index];
    /* Same refusal resolve_camera makes: a cut is not motion, so it has no
     * pan rate -- not "a very high one". */
    if (entry->discontinuity) {
        return false;
    }
    if (previous == NULL || !previous->valid ||
        previous->stage_generation != current->stage_generation ||
        (size_t)viewport_index >= previous->camera_count) {
        return false;
    }
    before = &previous->cameras[viewport_index];
    if (before->camera_id != entry->camera_id) {
        return false;
    }
    *out_yaw_delta_deg = mdkr_yaw_delta_deg((uint16_t)before->rotation_y,
                                            (uint16_t)entry->rotation_y);
    return true;
}
