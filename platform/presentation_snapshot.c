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
        return; /* the next capture fails whole and counts the overflow */
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
    uint8_t last_world_region;
    uint64_t last_capture;
} SnapCameraHistory;

static SnapCameraHistory s_camera_history[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];

/* One bit per gCameras[] slot the game has declared cut this tick; consumed
 * by that slot's capture and cleared when the capture completes. */
static uint32_t s_camera_cut_pending;

void presentation_snapshot_note_camera_cut(int camera_id) {
    if (camera_id < 0 || camera_id >= PRESENTATION_SNAPSHOT_MAX_CAMERAS ||
        !presentation_snapshot_enabled()) {
        return;
    }
    s_camera_cut_pending |= 1u << (unsigned)camera_id;
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
    if (sample->camera_id >= 0 &&
        sample->camera_id < PRESENTATION_SNAPSHOT_MAX_CAMERAS &&
        (s_camera_cut_pending & (1u << (unsigned)sample->camera_id)) != 0u) {
        discontinuous = true; /* the game snapped this camera (see the note) */
    }
    if (!discontinuous) {
        const float moved =
            distance_squared(history->last_position, sample->position);
        if (moved > PRESENTATION_SNAPSHOT_TELEPORT_UNITS *
                        PRESENTATION_SNAPSHOT_TELEPORT_UNITS) {
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
    /* Notes belong to the tick that raised them and are spent here, on the
     * capture that just read them. A capture that fails whole spends them too:
     * the next published pair is not capture-adjacent, so it is discontinuous
     * on its own account and needs no carried-over note. */
    s_camera_cut_pending = 0u;

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
        history->camera_id = entry->camera_id;
        history->last_position[0] = entry->position[0];
        history->last_position[1] = entry->position[1];
        history->last_position[2] = entry->position[2];
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
    s_current = -1;
    s_previous = -1;
    s_write = -1;
    s_capturing = false;
    s_write_failed = false;
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

void presentation_snapshot_get_stats(PresentationSnapshotStats *out) {
    if (out != NULL) {
        *out = s_stats;
    }
}

void presentation_snapshot_shutdown(void) {
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
}

static void presentation_snapshot_report(void) {
    printf("[SNAPSHOT] captures=%llu objects_peak=%llu discontinuities=%llu "
           "overflows=%llu resets=%llu camera_mask=%llu "
           "cam0=%llu cam1=%llu cam2=%llu cam3=%llu "
           "cam4=%llu cam5=%llu cam6=%llu cam7=%llu "
           "caminterp0=%llu caminterp1=%llu caminterp2=%llu "
           "caminterp3=%llu caminterp4=%llu caminterp5=%llu "
           "caminterp6=%llu caminterp7=%llu\n",
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
           (unsigned long long)s_stats.camera_interpolations[7]);
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
    if (delta > PRESENTATION_SNAPSHOT_ROTATION_SNAP ||
        delta < -PRESENTATION_SNAPSHOT_ROTATION_SNAP) {
        return b; /* beyond a quarter turn: snap, never smear */
    }
    alpha = (double)numerator / (double)denominator;
    stepped = (int32_t)((double)delta * alpha); /* truncates toward zero */
    /* Unsigned addition so the wrap around 0x7FFF/0x8000 is defined. */
    return (int16_t)(uint16_t)((uint16_t)a + (uint16_t)(int16_t)stepped);
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
        return true; /* spawn / teleport: current pose, never blended */
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
    out->shake_magnitude = entry->shake_magnitude;
    out->apply_shake = entry->apply_shake;
    out->fov = entry->fov;
    out->vertical_fov = entry->vertical_fov;
    out->aspect = entry->aspect;
    out->near_plane = entry->near_plane;
    out->far_plane = entry->far_plane;
    memcpy(out->viewport, entry->viewport, sizeof(out->viewport));
    out->interpolated = 0u;

    if (entry->discontinuity || previous == NULL || !previous->valid ||
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
    out->interpolated = 1u;
    if (entry->camera_id >= 0 &&
        entry->camera_id < PRESENTATION_SNAPSHOT_MAX_CAMERAS) {
        s_stats.camera_interpolations[entry->camera_id]++;
    }
    return true;
}
