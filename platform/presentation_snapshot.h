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

/*
 * Camera-cut clauses for angle and FOV, in degrees of per-tick delta.
 *
 * The position clause above (PRESENTATION_SNAPSHOT_TELEPORT_UNITS) only
 * catches a cut that MOVES the camera. A pure re-aim -- the TT-cam spectate
 * switch handing the shot to a trackside camera a few units from the last
 * one, or a post-race spectator swap -- can leave position and even FOV
 * almost untouched while yaw snaps more than a quarter turn. Left
 * undetected at capture, that jump still reaches resolve_camera_pair's
 * per-axis interpolation, which blends position and FOV across the cut
 * while ONLY the yaw axis hits PRESENTATION_SNAPSHOT_ROTATION_SNAP and gets
 * forced to its endpoint -- a shear where one axis jumps ahead of the pair
 * it is supposedly paired with (evidence file's docs/evidence/
 * smoothing-artifact-repro-2026-08.md, section 2.1).
 *
 * DKR-R's measured values, adopted here as starting points and tuned only
 * against this tree's own fixtures: 67.5 deg/tick for yaw (three quarters
 * of PRESENTATION_SNAPSHOT_ROTATION_SNAP's 90 deg/tick, comfortably above
 * any camera-follow pan and comfortably below a snap turn) and 20.0 deg/tick
 * for FOV (an ordinary FOV kick -- boost pads, drafting -- stays well under
 * this; only a full recompose reads like this in one tick).
 *
 * Neither clause replaces the position clause or the per-axis rotation
 * snap: both stay as backstops for the cuts these two do not name.
 */
#define MDKR_CUT_VIEW_ANGLE_DEG 67.5f
/* Kept as an API spelling for existing telemetry/tests; yaw is one of the
 * three view axes governed by the common threshold. */
#define MDKR_CUT_YAW_DEG MDKR_CUT_VIEW_ANGLE_DEG
#define MDKR_CUT_FOV_DEG 20.0f

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
    /*
     * WHICH MESH THIS TICK DREW, for owners whose geometry is chosen per tick
     * from a family that shares an address and a vertex count. The wave
     * surfaces are the case: 25 grid LOD variants live in one allocation and
     * the visible variant changes as the camera closes on a tile, so two
     * adjacent ticks can name the same vertex array while meaning two
     * different grids. Nothing in the pose carries that, which is why it is
     * captured here beside the pose and not derived from it.
     *
     * Zero == this owner publishes no key. Every registrant except the wave
     * surfaces is unkeyed, and an unkeyed pair always agrees, so this field
     * is inert for them.
     */
    uint16_t topology_key;
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
    /*
     * The perceptual census tests/check_motion_quality_battery.py reads.
     *
     * Counters only: none of these changes a branch, so a build that carries
     * them resolves byte-identically to one that does not. They exist because
     * the two motion failures a player names first -- a rotation that smears
     * the long way round, and a respawn the reconstruction blends across --
     * are invisible to every pixel control the tree has. Both arms of such a
     * control put the object somewhere, and a wrong somewhere is as coherent
     * as a right one (see docs/evidence/smoothing-artifact-repro-2026-08.md
     * §2.4).
     */
    uint64_t rotation_arc_checks;     /* interpolated angle results audited */
    uint64_t rotation_arc_snaps;      /* of those, deltas past a quarter turn */
    uint64_t rotation_arc_violations; /* results outside the [prev,curr] arc */
    uint64_t discontinuity_holds;     /* resolves the cut/spawn flag refused */
    uint64_t discontinuity_blends;    /* resolves that BLENDED a flagged entry */
    /*
     * Camera-cut note accounting. `notes` counts game-side declarations,
     * `consumed` the captures that applied one. `unconsumed` counts notes
     * still pending after the publish of the very viewport they name — zero by
     * construction while the note sites and the capture agree on the viewport
     * ID space, and the only witness that would fire if they stopped agreeing.
     * A pose-based classifier cannot see that failure: a cut the pose does not
     * carry is exactly the cut these notes exist for.
     */
    uint64_t camera_cut_notes;
    uint64_t camera_cut_consumed;
    uint64_t camera_cut_unconsumed;
    /* Task 9: captures forced discontinuous by a standing camera exclusion
     * (presentation_snapshot_set_camera_excluded), independent of any note
     * and independent of what the pose itself says. Nonzero on a route that
     * reaches the finish/spectate camera is the proof the exclusion actually
     * armed, not just that it compiled -- the same reasoning
     * camera_cut_notes/consumed/unconsumed already document for the one-shot
     * note path above. */
    uint64_t camera_excluded_captures;
    /* Lifecycle spawn notes that could not register an identity. Each one
     * fails the next commit whole (see note_spawn); nonzero means the identity
     * table ran out of slots, not that anything was mis-blended. */
    uint64_t identity_insert_failures;
    /* Renderer-owned transforms (rain splashes, lens flare pieces): peak live
     * registry size, and captures that copied at least one of them. Zero on a
     * route with rain over water or a visible sun means the content is back to
     * being drawn with no presentation identity at all. */
    uint64_t external_peak;
    uint64_t external_captures;
} PresentationSnapshotStats;

/* Interpolated result handed to the renderer. Never written into a live
 * object (spec §4.5, §14). */
/*
 * Why a replayed world matrix held instead of being substituted. Diagnostic
 * only -- nothing branches on this; it exists so the single "holds" total can
 * be split into the cases that are correct (a prop that did not move; a spawn
 * or teleport that must not blend) and the one that is not (a continuous
 * object that moved and still held, which is drawn with its tick-T world under
 * a tick-T+alpha camera and therefore slides against the terrain).
 */
typedef enum MdkrReplayHoldClass {
    MDKR_REPLAY_HOLD_NO_OWNER = 0,
    MDKR_REPLAY_HOLD_NO_PAIR,               /* not in the pair: recycled/uncaptured */
    MDKR_REPLAY_HOLD_DISCONTINUOUS_STILL,   /* correct: cut, and it did not move */
    MDKR_REPLAY_HOLD_DISCONTINUOUS_MOVING,  /* correct by design: spawn/teleport */
    MDKR_REPLAY_HOLD_PAIRED_STILL,          /* correct: paired, but it did not move */
    MDKR_REPLAY_HOLD_PAIRED_MOVING          /* THE DEFECT SIGNAL */
} MdkrReplayHoldClass;

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
    /* Canonical authored view pitch: rotation_x + pitch, wrapped once before
     * interpolation. Rendering consumes this field so two independently
     * chosen shortest arcs can never compose into the long arc. */
    int16_t view_pitch;
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
 * Note that this VIEWPORT's camera pose is a CUT, not motion.
 *
 * Capture already recognises the cuts it can see from the pose alone: a
 * different gCameras[] slot, a change of draw region, a capture gap, a jump
 * past PRESENTATION_SNAPSHOT_TELEPORT_UNITS, a yaw delta past MDKR_CUT_YAW_DEG,
 * or an FOV delta past MDKR_CUT_FOV_DEG. A game-side cut that clears none of
 * those bars is invisible to it — a spectate handoff between two trackside
 * cameras that sit close together, look the same direction, and share an FOV
 * is the case that matters, because it clears none of the automatic bars and
 * both belong to the same camera slot. Blend that and the camera flies
 * between two shots inside one authoritative tick.
 *
 * So the sites that SNAP a camera say so here, exactly the way spawn/free are
 * noted at the object lifecycle sites: the note is a fact the game knows and
 * the pose does not carry.
 *
 * The argument is the VIEWPORT index (== the player index every note site
 * already has), not the gCameras[] slot. The recorded slot is
 * `viewport + (gCutsceneCameraActive ? 4 : 0)`, so a note filed in slot space
 * from a player index is looked for four bits away on every tick a cutscene
 * camera owns the viewport. The note applies to the next capture of
 * `viewport_index` and is consumed by it; it is carried, not discarded, until
 * that capture happens. No-op unless the seam is enabled.
 */
void presentation_snapshot_note_camera_cut(int viewport_index);

/*
 * Task 9: declare (or retract) a STANDING exclusion for this VIEWPORT --
 * every capture of it is a cut, not just the tick the game happened to note
 * one.
 *
 * note_camera_cut above is a one-shot fact spent by the very next capture:
 * exactly right for a mode-change tick, but it depends on every site that
 * can re-aim the shot remembering to raise it. The finish/post-race spectate
 * cameras (racer.c's CAMERA_FINISH_RACE / CAMERA_FINISH_CHALLENGE) hop
 * between trackside objects that individually clear every automatic bar
 * (position, yaw, FOV, region) and are individually noted at the handoff --
 * but they also idle at each point for a run of ticks, and nothing says a
 * future site that adjusts the shot mid-dwell (a look-at ease, a boom
 * settle) will remember to note itself. A standing exclusion does not
 * depend on that: while set, this viewport's captures hold discontinuous
 * regardless of what the pose says or whether anything noted a cut this
 * tick, so it can never enter resolve_camera_pair as a blend candidate.
 *
 * The caller re-asserts this every tick the excluded mode is active (see
 * update_player_camera) rather than latching once, so leaving the mode
 * un-excludes the viewport with no separate call needed and no reliance on
 * a stage reset. Also cleared by presentation_snapshot_stage_reset and
 * _shutdown. No-op unless the seam is enabled.
 */
void presentation_snapshot_set_camera_excluded(int viewport_index,
                                               bool excluded);

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

/*
 * ---- renderer-owned transforms the object walk cannot discover -----------
 *
 * The capture walk enumerates gObjPtrList, so anything drawn from a static or
 * stack-local ObjectTransform is invisible to it: no snapshot entry, no
 * presentation owner, and an interpolated present replays its tick-T bytes
 * verbatim while the camera around it moves. The rain splashes are the case
 * this exists for -- eight module-static RainPosData slots, billboarded every
 * tick, never spawned as Objects.
 *
 * Registering a transform makes the walk copy it exactly as it copies an
 * Object's, so everything downstream -- billboard anchors, teleport and
 * missed-tick discontinuity, generation-keyed resolution -- applies to it
 * unchanged. `transform` is an `ObjectTransform *`; it is typed void here only
 * to keep this header free of the game's structs (see the file header).
 *
 * REGISTER AT THE LIFECYCLE SITE, NOT AT THE DRAW. Registration issues a fresh
 * generation, which is what stops a recycled slot from blending out of the
 * previous tenant's last pose. A slot that retires must be unregistered, or
 * its stale pose stays in the published frame and its next occupant looks
 * continuous with a splash that is already gone.
 */
bool presentation_snapshot_register_external_transform(const void *transform);
void presentation_snapshot_unregister_external_transform(
    const void *transform);
/*
 * Publish which mesh THIS authored tick drew for an already-registered
 * renderer-owned transform (PresentationObjectEntry::topology_key). Called
 * from the same place the pose is written, so the key and the pose it belongs
 * to always land in the same captured entry; a key set against an
 * unregistered address is refused rather than remembered.
 *
 * Setting it does not decide anything. The comparison of two ticks' keys, and
 * the snap that a disagreement earns, both live at the replay choke point
 * (dkr_replay_resolve_alpha -> MDKR_VERDICT_TOPOLOGY_MISMATCH).
 */
bool presentation_snapshot_set_external_topology_key(const void *transform,
                                                     uint16_t key);
/* Registry read-back for the walk, matching _external_transform_at's shape. */
bool presentation_snapshot_external_topology_key_at(size_t index,
                                                     uint16_t *out_key);
/*
 * Do the two published ticks agree about this identity's mesh?
 *
 * FACT, NOT VERDICT: false is returned both when the keys disagree and when
 * the pair does not contain the identity at all, because in both cases there
 * is no evidence the two endpoints describe one surface. The caller turns
 * that into a reason; this only reports it.
 */
bool presentation_snapshot_topology_keys_agree(const void *address,
                                                uint64_t generation);
/* Live registered count, and the registry's contents by index. The walk in
 * presentation_snapshot_walk.c enumerates through these because it is the only
 * translation unit allowed to know what an ObjectTransform is. */
size_t presentation_snapshot_external_transform_count(void);
const void *presentation_snapshot_external_transform_at(size_t index);

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
/* The authored tick a resolve at numerator 0 returns the pose of -- i.e. the
 * tick every capture-time residual in the replay is measured against. Succeeds
 * only when a published pair exists, because without one no resolve
 * interpolates and there is no residual to check. */
bool presentation_snapshot_authored_endpoint_tick(uint64_t *authored_tick);
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
 * Wraparound-correct yaw delta, in degrees, from N64 angle `a` to `b`.
 *
 * Same wrap `presentation_lerp_angle` uses for its shortest-arc selection
 * (the `(int16_t)` narrowing of the unsigned difference), scaled to degrees
 * and left UNCLAMPED to any snap threshold: this is a rate measurement, not
 * a pose reconstruction, so the caller decides what a large delta means.
 * Range is (-180, 180]. Exported here (Task 4's pan-rate demotion) because
 * Task 6 needs the identical wrap for its own per-tick yaw measurement, and
 * two independently written copies of a wraparound cast are exactly the kind
 * of arithmetic this tree does not trust itself to duplicate correctly.
 */
float mdkr_yaw_delta_deg(uint16_t a, uint16_t b);

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

/*
 * Per-tick camera yaw rate for `viewport_index`, in degrees, from the exact
 * same authored pose pair `presentation_snapshot_resolve_camera` blends
 * (this tick's entry and the immediately preceding published one) -- the
 * cut-detection pairing, not a resolved/interpolated pose. Fails closed
 * (returns false, `*out_yaw_delta_deg` left unwritten) under every condition
 * `resolve_camera` itself would refuse to blend across: no published pair,
 * mismatched stage generation, a viewport absent from either snapshot, a
 * camera-bank change, or this tick's own capture flagged as a cut. A caller
 * that ignores the return value and reads a stale/garbage float would be
 * exactly the C2 mechanism-artifact class the discontinuity flag exists to
 * prevent, so there is no fallback value -- only success or "do not use
 * this".
 */
bool presentation_snapshot_camera_pan_rate_deg(int viewport_index,
                                               float *out_yaw_delta_deg);

/* ---- authored UV-scroll rates ------------------------------------------- *
 *
 * WHY THIS EXISTS. obj_loop_texscroll advances a level texture through a
 * TWO-BIT accumulator: it adds the authored rate (quarter-units per tick) to
 * `unk8`, emits `unk8 >> 2` whole units into the triangle UVs and keeps
 * `unk8 & 3` as the residue. An authored rate that is not a multiple of four
 * therefore emits ZERO units on some ticks and N on others, even though the
 * surface is scrolling at a perfectly constant speed.
 *
 * A presentation stage that recovers the speed by DIFFERENCING two authored
 * ticks cannot see through that quantisation. It reads 0 then N, and either
 * publishes nothing (a zero difference is not a scroll) or publishes two
 * disagreeing displacements — so the confirm-or-hold rule refuses, on every
 * present, forever. The surface keeps its 30 Hz texture cadence beside a
 * world drawn at the host rate, which is the shimmer the owner reported on
 * waterfalls, rivers and lava.
 *
 * The game does not have to be measured: it KNOWS the rate. This registry is
 * how it says so. Per authored tick, obj_loop_texscroll registers the address
 * span of every triangle batch it drives together with
 *
 *   rate  — the tick's authored advance in QUARTER units (rate * updateRate)
 *   phase — the accumulator residue BELONGING TO THE BYTES THE SPAN NOW HOLDS,
 *           i.e. `unk8` as it stood BEFORE this tick's advance
 *
 * from which presentation reconstructs the true sub-tick position exactly:
 *
 *   offset(alpha) = (phase + rate * alpha) / 4      [whole UV units]
 *
 * At alpha 1 that equals the emitted whole units plus the NEXT tick's phase,
 * so consecutive ticks join without a step: the quantisation cancels instead
 * of beating against the present rate. One observation is enough, so no
 * second tick has to confirm anything and the wrap rule that
 * gfx_presentation_packet_lookup_uv_scroll enforces for MEASURED scrollers is
 * left exactly as strict as it was.
 *
 * SCOPE. Only the texscroll driver registers. Every other UV mover (waves,
 * rain, the skydome, the texture animator's flipbooks) is untouched and still
 * goes through measurement and confirmation — a flipbook can never be
 * mistaken for a scroll here because it never appears in this table.
 *
 * LIFETIME. The table is filled during a game tick and consumed by the
 * following future-task walk, which clears it. A span registered twice while
 * the table is live is ambiguous (two scrollers driving one batch, or a tick
 * whose walk never ran) and is refused, falling back to measurement.
 */
#define PRESENTATION_UV_SCROLL_AUTHORED_MAX_SPANS 512

typedef struct PresentationUvScrollAuthored {
    int32_t rate_u;    /* quarter UV units advanced this authored tick */
    int32_t rate_v;
    int32_t phase_u;   /* quarter-unit residue already owed by these bytes */
    int32_t phase_v;
} PresentationUvScrollAuthored;

/* True only while a future-task walk is armed to consume the table. Game code
 * checks this before doing any per-batch work, so the seam costs one
 * load-and-branch per texscroll object when smoothing is off. */
bool presentation_uv_scroll_authored_wanted(void);
void presentation_uv_scroll_authored_set_wanted(bool wanted);

/* Drop every registration. Called by the walk that just consumed them. */
void presentation_uv_scroll_authored_reset(void);

/* `first`/`end` bound one triangle batch, end-exclusive. */
void presentation_uv_scroll_authored_register(
    const void *first, const void *end,
    const PresentationUvScrollAuthored *rate);

/* Resolve the batch containing `address`. False when no texscroll drives it,
 * or when the span was registered more than once. */
bool presentation_uv_scroll_authored_lookup(
    const void *address, PresentationUvScrollAuthored *out);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_PRESENTATION_SNAPSHOT_H */
