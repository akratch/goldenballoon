/*
 * presentation_snapshot.h — native-only presentation shadow state
 * (spec §7, Phase 3 Wave A).
 *
 * The authoritative simulation runs in fixed original-region quanta; the
 * host presents at whatever rate it likes. To draw an intermediate frame the
 * renderer needs the two bracketing authoritative poses — and it must get
 * them WITHOUT reading (or writing) live game objects, because §4.5 forbids
 * presentation from touching authoritative state and §4.2 forbids rendering
 * from mutating it.
 *
 * This module is that shadow store. It contains NO original N64 struct: the
 * game's layouts are untouched, and every field here is a copy taken at the
 * authoritative tick boundary.
 *
 * THE IDENTITY PROBLEM (the reason this module is not just "two arrays of
 * transforms"). DKR allocates objects from a fixed 512-slot pool
 * (OBJECT_SLOT_COUNT) and recycles addresses aggressively — a banana freed
 * on tick N is very often the exact address a weapon spawns into on tick
 * N+1. Keyed on `Object *` alone, the interpolator would happily blend the
 * banana's last pose into the weapon's first pose and draw a projectile
 * flying in from wherever the fruit died. So entries are keyed by
 *
 *     identity = (Object * address, monotonically increasing spawn generation)
 *
 * where the generation is issued by presentation_snapshot_note_spawn() at
 * the real lifecycle sites in objects.c (spawn_object's success return and
 * add_particle_to_entity_list) and retired by
 * presentation_snapshot_note_free() at obj_destroy(), the single point where
 * an Object's memory actually goes back to the pool. A recycled address gets
 * a fresh generation, the generations of the pair disagree, and the
 * interpolator refuses to pair them.
 *
 * PUBLISHING. The gfx_shadow_frame.c house pattern: capture_begin() stages,
 * capture_commit() publishes atomically, and consumers only ever see
 * immutable published data. The one difference is that consumers here need a
 * PAIR (previous, current) rather than a single frame, so the ring is three
 * slots deep: while slot k is being filled, the published previous/current
 * pair keeps both of its slots and does not move. A capture that overflows
 * fails whole — never partially — and the previously published pair is
 * retained (the shadow-frame retention rule).
 *
 * COST WHEN OFF. Everything is gated on presentation_snapshot_enabled(),
 * one cached getenv("MDKR_PRESENT_SNAPSHOT") behind a static int. The
 * lifecycle hooks in objects.c and the capture call in stubs_dkr.c are a
 * predictable load-and-branch when the seam is off. Wave C replaces the env
 * read with the real config key; the seam stays.
 */
#ifndef MDKR_PRESENTATION_SNAPSHOT_H
#define MDKR_PRESENTATION_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors OBJECT_SLOT_COUNT (game/src/objects.c): gObjPtrList can never hold
 * more than the object pool can allocate, so a capture that would exceed
 * this is a real defect, not a sizing accident — hence "fail whole". */
#define PRESENTATION_SNAPSHOT_MAX_OBJECTS 512
#define PRESENTATION_SNAPSHOT_MAX_VIEWPORTS 4
#define PRESENTATION_SNAPSHOT_MAX_CAMERAS 8

/* Per-frame address index: power of two, >= 2x MAX_OBJECTS so linear probing
 * stays short. Built at commit, so lookups into the published pair are O(1)
 * instead of a 512-entry scan per drawn object. */
#define PRESENTATION_SNAPSHOT_INDEX_SLOTS 1024

/* Live identity registry. Must comfortably exceed the number of
 * simultaneously registered objects (<= OBJECT_SLOT_COUNT in gObjPtrList,
 * plus objects spawned OBJECT_SPAWN_NONE that never enter the list). */
#define PRESENTATION_SNAPSHOT_IDENTITY_SLOTS 4096

/*
 * Teleport threshold, in DKR world units of per-tick displacement.
 *
 * Calibration: an authoritative tick is 2 VI fields (1/30 s NTSC). The
 * fastest thing in the game is a boosted plane; racer speeds observed in the
 * state-hash traces stay well under ~60 units/tick, and the largest tracks
 * span order 10^4 units end to end. Warps that MUST NOT interpolate — level
 * start placement, checkpoint respawn, cutscene camera cuts, Taj
 * transformations — move objects thousands of units in a single tick.
 * 2000 is therefore >30x the fastest legitimate motion and far below any
 * real warp: generous on purpose, because a missed teleport draws a smear
 * across the level while a false positive costs one un-interpolated frame.
 */
#define PRESENTATION_SNAPSHOT_TELEPORT_UNITS 2000.0f

/*
 * Rotation snap threshold, in DKR fixed angle units of per-tick shortest-arc
 * delta (0x10000 == one turn).
 *
 * Calibration: blending is only honest when the simulation could plausibly
 * have passed through the intermediate orientations. A quarter turn per
 * authored tick (90 deg / 33 ms == 2700 deg/s) is beyond any steered racer
 * rotation; deltas past it are spin-outs, snap turns and animation pops,
 * where a blend draws orientations the tick never held ("paper" smearing).
 * Ghostship ships the same 0x4000 boundary and it is the single heuristic
 * most responsible for its clean fast-motion feel. The ambiguous half turn
 * (delta == -0x8000, both arcs equal) also snaps: either arc is a guess.
 */
#define PRESENTATION_SNAPSHOT_ROTATION_SNAP 0x4000

/* One captured object pose. Native-only; no original struct is embedded. */
typedef struct PresentationObjectEntry {
    const void *address;      /* Object * — identity half one */
    uint64_t generation;      /* identity half two: spawn serial */
    float position[3];
    float scale;
    int16_t rotation_y;       /* DKR fixed angles: 0x10000 == one turn */
    int16_t rotation_x;
    int16_t rotation_z;
    int16_t animation_id;     /* active ModelInstance; -1 when not a 3D model */
    int16_t animation_frame;  /* active ModelInstance; 0 when not a 3D model */
    int8_t model_index;       /* obj->modelIndex (LOD select); -1 for particles */
    uint8_t opacity;
    uint8_t is_particle;      /* read through Particle, not Object (see .c) */
    uint8_t discontinuity;    /* 1 == spawn/teleport: draw current, never blend */
} PresentationObjectEntry;

/*
 * One captured viewport camera.
 *
 * These are the ACTUAL inputs to camera.c's matrix path, not derived
 * matrices: cam_build_view_basis() builds gViewMatrixF from
 * (trans.position, trans.rotation, pitch, shakeMagnitude when gNoCamShake),
 * and cam_rebuild_native_projection() builds gPerspectiveMatrixF from
 * (gCurCamFOV -> projection.vertical_fov, projection.aspect, CAMERA_NEAR,
 * CAMERA_FAR). Interpolating these and deriving the view matrix afterwards
 * is what spec §7 requires ("camera: interpolate before deriving the view
 * matrix").
 */
typedef struct PresentationCameraEntry {
    int32_t camera_id;        /* index into gCameras[] actually used */
    int32_t viewport_index;   /* 0..3 */
    float position[3];
    int16_t rotation_x;
    int16_t rotation_y;
    int16_t rotation_z;
    int16_t pitch;            /* added to rotation_x by cam_build_view_basis */
    float shake_magnitude;
    int32_t apply_shake;      /* gNoCamShake: nonzero == shake IS applied */
    float fov;                /* gCurCamFOV, the authored level FOV */
    float vertical_fov;       /* guPerspectiveF's fovy */
    float aspect;             /* guPerspectiveF's aspect */
    float near_plane;
    float far_plane;
    float viewport[4];        /* posX, posY, width, height */
    /* The exact matrix authored from this recipe. Interpolated presents still
     * rebuild from the blended inputs above; alpha 0/1 copy this canonical
     * endpoint so no reconstruction rounding or hidden legacy state can alter
     * an original frame. */
    float authored_view_projection[4][4];
    /* Discrete projection/draw-region owner. Zero is the ordinary presentation
     * region; nonzero is the authored safe aperture. Viewport coordinates may
     * animate continuously while this remains unchanged. */
    uint8_t world_region;
    uint8_t discontinuity;
    uint8_t reserved[2];
} PresentationCameraEntry;

typedef struct PresentationSnapshot {
    uint64_t generation;      /* publish serial; 0 == never published */
    uint64_t stage_generation;/* bumped by presentation_snapshot_stage_reset */
    uint64_t authored_tick;   /* fixed tick whose post-update state was copied */
    bool valid;
    size_t object_count;
    size_t camera_count;
    PresentationObjectEntry objects[PRESENTATION_SNAPSHOT_MAX_OBJECTS];
    PresentationCameraEntry cameras[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];
    /* address -> objects[] slot, +1 biased so 0 means empty. Built at commit. */
    uint16_t index[PRESENTATION_SNAPSHOT_INDEX_SLOTS];
} PresentationSnapshot;

typedef struct PresentationSnapshotStats {
    uint64_t captures;        /* commits that published */
    uint64_t objects_peak;    /* high-water object count of a published frame */
    uint64_t discontinuities; /* entries published with discontinuity == 1 */
    uint64_t overflows;       /* captures failed whole (object/camera/identity) */
    uint64_t resets;          /* stage boundaries that cleared history */
    uint64_t camera_id_mask;  /* exact gCameras[] IDs seen in published frames */
    uint64_t camera_captures[PRESENTATION_SNAPSHOT_MAX_CAMERAS];
    uint64_t camera_interpolations[PRESENTATION_SNAPSHOT_MAX_CAMERAS];
} PresentationSnapshotStats;

/* Interpolated result handed to the renderer. Never written into a live
 * object (spec §4.5, §14). */
typedef struct PresentationObjectPose {
    float position[3];
    float scale;
    int16_t rotation_y;
    int16_t rotation_x;
    int16_t rotation_z;
    int16_t animation_id;
    int16_t animation_frame;
    int8_t model_index;
    uint8_t opacity;
    uint8_t is_particle;
    uint8_t interpolated;     /* 0 == current pose used verbatim */
} PresentationObjectPose;

typedef struct PresentationCameraPose {
    int32_t camera_id;
    float position[3];
    int16_t rotation_x;
    int16_t rotation_y;
    int16_t rotation_z;
    int16_t pitch;
    float shake_magnitude;
    int32_t apply_shake;
    float fov;
    float vertical_fov;
    float aspect;
    float near_plane;
    float far_plane;
    float viewport[4];
    uint8_t interpolated;
} PresentationCameraPose;

/* ---- seam ------------------------------------------------------------- */

bool presentation_snapshot_enabled(void);
/* Wave C wires the real config key through here; tests use it directly. */
void presentation_snapshot_set_enabled(bool enabled);

/* ---- lifecycle hooks (objects.c, NATIVE_PORT-gated) -------------------- */

void presentation_snapshot_note_spawn(const void *object);
void presentation_snapshot_note_free(const void *object);

/*
 * Return the generation currently assigned by the lifecycle registry without
 * exposing or reading the live Object. Matrix registration uses this to bind
 * a frozen display-list transform to the exact object lifetime that produced
 * it; an address alone is not an identity because the 512-slot pool recycles
 * addresses aggressively.
 *
 * This strict lookup is deliberately read-only. If the seam was enabled after
 * a normal captured object spawned and its first capture has not happened yet,
 * it returns false and that matrix remains unowned for one tick rather than
 * manufacturing identity state from the render path.
 */
bool presentation_snapshot_identity_generation(const void *object,
                                               uint64_t *generation);
/* Register a presentation-only lifetime that predates replay activation (for
 * example the shared shield/magnet render objects, which are not in
 * gObjPtrList) without resetting an already-issued generation. This is only
 * for renderer-owned identities that the authoritative object walk cannot
 * discover; normal game objects use the strict lookup above. */
bool presentation_snapshot_identity_ensure_generation(
    const void *object, uint64_t *generation);

/* Level transitions reset snapshot history so interpolation never crosses
 * two unrelated scenes (spec §5). Hooked at game.c's stage boundary, beside
 * gfx_dkr_resource_generation_begin. */
void presentation_snapshot_stage_reset(void);

/* ---- capture ----------------------------------------------------------- */

/*
 * Walks gObjPtrList and the active cameras and publishes one snapshot.
 * Called at the authoritative tick boundary (stubs_dkr.c, beside
 * mdkr_sim_hash_frame). READ-ONLY over authoritative state.
 */
void presentation_snapshot_capture(uint64_t authored_tick);

/* Writer API — the walk in presentation_snapshot_walk.c drives these, and
 * the unit test drives them directly with synthetic samples. */
void presentation_snapshot_capture_begin(void);
/* Production capture variant. The explicit token is shared with the display
 * list authoring boundary and is later used to reject phase-shifted retained
 * vertex/effect history. */
void presentation_snapshot_capture_begin_authored(uint64_t authored_tick);
bool presentation_snapshot_capture_object(const PresentationObjectEntry *sample);
bool presentation_snapshot_capture_camera(const PresentationCameraEntry *sample);
void presentation_snapshot_capture_commit(void);

/* Exact camera inputs latched while the game authors one display list.
 * begin() invalidates the previous list; record() is called at the point where
 * camera.c derives the task's view-projection matrix; copy() succeeds only for
 * the same immutable authored tick. This keeps capture independent of camera
 * or lifecycle state that may change after the matrix has been authored but
 * before the tick-boundary snapshot walk. */
void presentation_snapshot_authored_cameras_begin(uint64_t authored_tick);
bool presentation_snapshot_authored_camera_record(
    const PresentationCameraEntry *sample);
size_t presentation_snapshot_authored_cameras_copy(
    uint64_t authored_tick, PresentationCameraEntry *out, size_t capacity);

/* ---- published pair ---------------------------------------------------- */

const PresentationSnapshot *presentation_snapshot_current(void);
const PresentationSnapshot *presentation_snapshot_previous(void);
/* Return the current snapshot tick only when the published pair is exactly
 * authored_task_tick -> authored_task_tick+1. Retained packet interpolation
 * must target this tick; otherwise replay holds the task's current bytes. */
bool presentation_snapshot_replay_target_tick(
    uint64_t authored_task_tick, uint64_t *target_tick);
void presentation_snapshot_get_stats(PresentationSnapshotStats *out);
void presentation_snapshot_shutdown(void);

/* ---- pure interpolation helpers ---------------------------------------- */

/*
 * Alpha always arrives as sim_sched_alpha's EXACT rational (numerator,
 * denominator) and is converted to double once, here. Nothing in this module
 * accumulates a float alpha across frames — the accumulator lives in
 * SimSched as integer units and every frame re-derives its own alpha from
 * scratch, so there is no drift to accumulate.
 */
double presentation_alpha(uint64_t numerator, uint64_t denominator);

/* Endpoint exactness (spec §12.4 "interpolation endpoints are exact"):
 * alpha == 0 returns `a`'s bits unchanged, alpha >= 1 returns `b`'s bits
 * unchanged. No arithmetic runs at the endpoints, so no rounding can move
 * them. */
float presentation_lerp1(float a, float b, uint64_t numerator,
                         uint64_t denominator);
void presentation_lerp3(const float a[3], const float b[3],
                        uint64_t numerator, uint64_t denominator,
                        float out[3]);
uint8_t presentation_lerp_u8(uint8_t a, uint8_t b, uint64_t numerator,
                             uint64_t denominator);

/* Particle::opacity is an s16 containing an unsigned 8.8 fixed-point bit
 * pattern. Extracting through uint16_t matches render_particle's high-byte
 * interpretation without relying on implementation-defined right shift of a
 * negative signed value. */
uint8_t presentation_particle_opacity_u8(int16_t opacity);

/* Preserve draw-local fades/modifiers while replacing the authoritative
 * object's current opacity with its interpolated presentation opacity. */
uint8_t presentation_scale_opacity_u8(uint8_t authored, uint8_t current,
                                      uint8_t target);

/*
 * Shortest-arc interpolation of a DKR fixed angle.
 *
 *     delta  = (int16_t)(b - a)   // wraps: always the short way round
 *     result = a + (int16_t)(delta * alpha)
 *
 * The (int16_t) cast of the difference IS the shortest-arc selection: for any
 * a, b the wrapped difference lands in [-32768, 32767], i.e. at most half a
 * turn. An exact half turn (delta == -32768) is ambiguous and resolves
 * negative, deterministically.
 */
int16_t presentation_lerp_angle(int16_t a, int16_t b, uint64_t numerator,
                                uint64_t denominator);

/*
 * Discrete state selection rule (spec §7 "a documented current/previous
 * selection rule, never a numeric blend").
 *
 * THE RULE: discrete values show the PREVIOUS tick's state until alpha
 * reaches 1, at which point they switch to current. Equivalently, a discrete
 * value displays the state of the last COMPLETED authoritative tick.
 *
 * Why not "nearest" (alpha >= 0.5)? Because a discrete value paired with an
 * interpolated one must not disagree about which tick it is describing. An
 * animationFrame that flips at alpha 0.5 while the position it belongs to is
 * still 50% of the way from the previous pose puts the model's limbs one
 * frame ahead of its root for half of every tick. Previous-until-complete
 * keeps every discrete field consistent with the pose the interpolator is
 * still travelling away from, and makes the switch coincide exactly with the
 * pose becoming the new endpoint.
 */
bool presentation_discrete_use_current(uint64_t numerator,
                                       uint64_t denominator);

/* ---- resolution -------------------------------------------------------- */

/*
 * Resolve the pose to draw for `address` at the given alpha.
 *
 * Returns false when the object is not in the published current frame (it is
 * gone; §7 "destroy: do not retain gameplay presence").
 *
 * out->interpolated is 0 — and the CURRENT entry is copied verbatim — when
 * there is no usable previous entry: no published previous frame, the
 * address is absent from it, the generations disagree (slot reuse), or the
 * current entry carries a discontinuity flag.
 */
bool presentation_snapshot_resolve_object(const void *address,
                                          uint64_t numerator,
                                          uint64_t denominator,
                                          PresentationObjectPose *out);

/* Resolve only when the published current entry is the exact registered
 * lifetime. This is the renderer-facing form; pointer-only resolution remains
 * available for callers that enumerate the snapshot itself. */
bool presentation_snapshot_resolve_object_generation(
    const void *address, uint64_t generation, uint64_t numerator,
    uint64_t denominator, PresentationObjectPose *out);

/* True only when previous/current entries are one continuous, non-particle
 * lifetime with the same model and animation topology. Retained animated
 * vertex batches use this before blending deformation. */
bool presentation_snapshot_deformation_compatible(const void *address,
                                                   uint64_t generation);
bool presentation_snapshot_particle_deformation_compatible(
    const void *address, uint64_t generation);

/* Same, indexed into the published current frame's object array. */
bool presentation_snapshot_resolve_object_at(size_t index,
                                             uint64_t numerator,
                                             uint64_t denominator,
                                             PresentationObjectPose *out);

/* All viewports use the same simulation pair and alpha (spec §7). */
bool presentation_snapshot_resolve_camera(int viewport_index,
                                          uint64_t numerator,
                                          uint64_t denominator,
                                          PresentationCameraPose *out);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_PRESENTATION_SNAPSHOT_H */
