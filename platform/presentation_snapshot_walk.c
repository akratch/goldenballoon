/*
 * presentation_snapshot_walk.c — the game-side capture wiring for
 * presentation_snapshot.c (spec §7, Phase 3 Wave A).
 *
 * Kept separate from the module proper for one reason: this file needs the
 * original N64 struct headers, and presentation_snapshot.c must stay free of
 * them so the unit test can compile and drive it standalone. Nothing here
 * decides anything — it reads live authoritative state once per tick and
 * hands plain POD samples to the writer API.
 *
 * READ-ONLY. Every access below is a load. No field of any Object, Camera,
 * ModelInstance or Particle is written, no game function with side effects is
 * called, and no RNG is consumed. That is what lets the capture run inside
 * the authoritative tick boundary without perturbing the state hash
 * (spec §4.2, §4.5).
 */
#include <ultra64.h>
#include <string.h>

#include "structs.h"
#include "camera.h"
#include "camera_dynamic_occlusion.h"
#include "game.h"
#include "game_ui.h"
#include "objects.h"
#include "particles.h"
#include "racer.h"
#include "textures_sprites.h" /* OBJ_FLAGS_PARTICLE */
#include "thread3_main.h"
#include "tracks.h"

#include "presentation_snapshot.h"

#if defined(MDKR_ENABLE_ONLINE_BETA)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "rollback/rollback_game_authority.h"
#endif

extern Camera gCameras[PRESENTATION_SNAPSHOT_MAX_CAMERAS];
extern f32 gCurCamFOV;
extern s32 gNoCamShake;
extern ScreenViewport gScreenViewports[];

/*
 * Particles share gObjPtrList with objects (add_particle_to_entity_list), but
 * a Particle only overlaps Object for the first 0x18 bytes — its
 * ObjectTransform. Past that the layouts diverge completely: Object::opacity
 * at 0x39 is Particle::movementType, and Object::header at 0x40 is
 * Particle::descFlags. Reading a particle through Object is therefore
 * garbage, which is exactly why render_object_parts routes them to
 * render_particle instead. This walk makes the same split.
 */
static void capture_particle(const Object *object) {
    const Particle *particle = (const Particle *)object;
    PresentationObjectEntry sample;

    memset(&sample, 0, sizeof(sample));
    sample.address = object;
    sample.position[0] = particle->trans.x_position;
    sample.position[1] = particle->trans.y_position;
    sample.position[2] = particle->trans.z_position;
    sample.scale = particle->trans.scale;
    sample.rotation_y = particle->trans.rotation.y_rotation;
    sample.rotation_x = particle->trans.rotation.x_rotation;
    sample.rotation_z = particle->trans.rotation.z_rotation;
    /* Particle kind is the stable topology selector used by retained point/
     * line meshes. It is presentation metadata only, not an Object model ID. */
    sample.model_index = particle->kind;
    /* A particle's "animation" is its texture frame; there is no
     * ModelInstance to read an animationID/animationFrame from. */
    sample.animation_id = particle->textureFrame;
    sample.animation_frame = 0;
    sample.opacity = presentation_particle_opacity_u8(particle->opacity);
    sample.is_particle = 1;
    presentation_snapshot_capture_object(&sample);
}

static void capture_object(const Object *object) {
    PresentationObjectEntry sample;
    const ModelInstance *modelInstance = NULL;

    memset(&sample, 0, sizeof(sample));
    sample.address = object;
    sample.position[0] = object->trans.x_position;
    sample.position[1] = object->trans.y_position;
    sample.position[2] = object->trans.z_position;
    sample.scale = object->trans.scale;
    sample.rotation_y = object->trans.rotation.y_rotation;
    sample.rotation_x = object->trans.rotation.x_rotation;
    sample.rotation_z = object->trans.rotation.z_rotation;
    sample.model_index = object->modelIndex;
    sample.opacity = object->opacity;
    sample.animation_id = -1;
    sample.animation_frame = 0;
    if (object->header != NULL &&
        object->header->modelType == OBJECT_MODEL_TYPE_SPRITE_BILLBOARD) {
        sample.sprite_phase = (uint8_t)object->animFrame;
    }
    sample.discontinuity = (uint8_t)
        mdkr_camera_dynamic_occlusion_object_discontinuous(object);

    /* The ACTIVE ModelInstance, selected exactly the way obj_animate_tick and
     * render_3d_model select it: modelInstances[modelIndex], and only for
     * OBJECT_MODEL_TYPE_3D_MODEL (the union is textures/sprites otherwise). */
    if (object->header != NULL &&
        object->header->modelType == OBJECT_MODEL_TYPE_3D_MODEL &&
        object->modelInstances != NULL && object->modelIndex >= 0 &&
        object->modelIndex < object->header->numberOfModelIds) {
        modelInstance = object->modelInstances[object->modelIndex];
    }
    if (modelInstance != NULL) {
        sample.animation_id = modelInstance->animationID;
        sample.animation_frame = modelInstance->animationFrame;
    }
    presentation_snapshot_capture_object(&sample);
}

/*
 * One renderer-owned transform (see the registry note in
 * presentation_snapshot.h). There is no Object behind it, so there is no
 * modelIndex, no ModelInstance and no opacity to read: the entry carries the
 * pose and nothing else, which is exactly what a billboard anchor needs. It is
 * NOT flagged is_particle -- that flag selects the particle-topology
 * compatibility rule, and these are not particles.
 */
static void capture_external_transform(const ObjectTransform *transform,
                                       uint16_t topology_key) {
    PresentationObjectEntry sample;

    memset(&sample, 0, sizeof(sample));
    sample.address = transform;
    sample.position[0] = transform->x_position;
    sample.position[1] = transform->y_position;
    sample.position[2] = transform->z_position;
    sample.scale = transform->scale;
    sample.rotation_y = transform->rotation.y_rotation;
    sample.rotation_x = transform->rotation.x_rotation;
    sample.rotation_z = transform->rotation.z_rotation;
    sample.model_index = -1;
    sample.animation_id = -1;
    sample.animation_frame = 0;
    sample.opacity = 255;
    /* The one thing an external entry carries beyond its pose: which mesh the
     * owner drew this tick, for the families that have more than one. Unset
     * (zero) for every registrant that does not select geometry per tick. */
    sample.topology_key = topology_key;
    presentation_snapshot_capture_object(&sample);
}

static void capture_external_transforms(void) {
    size_t count = presentation_snapshot_external_transform_count();
    size_t index;

    for (index = 0u; index < count; index++) {
        const ObjectTransform *transform =
            (const ObjectTransform *)
                presentation_snapshot_external_transform_at(index);
        uint16_t topology_key = 0u;
        if (transform != NULL) {
            (void)presentation_snapshot_external_topology_key_at(
                index, &topology_key);
            capture_external_transform(transform, topology_key);
        }
    }
}

static void capture_cameras(uint64_t authored_tick) {
    PresentationCameraEntry cameras[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];
    s32 cameraCount;
    s32 gameMode;
    const LevelHeader *levelHeader;
    s32 viewport;

    gameMode = get_game_mode();
    /* Ordinary menus temporarily borrow camera-shaped matrices and may restore
     * gCameras[] before this boundary, so they deliberately keep their authored
     * view-projection. A loaded LEVEL SCENE under the menu shell is different:
     * render_scene owns its cameras for the full pass, even though the shell
     * runs it under GAMEMODE_MENU. That covers cutscene levels (bank-4
     * cinematic cameras), the title attract's live race demos, which stay
     * RACETYPE_DEFAULT (menu.c loads them with player count 0, not the
     * cutscene sentinel) — restricting this to the cutscene race types left
     * every attract demo with ZERO captured cameras, so its flyby camera and
     * terrain stepped at the authored tick rate while the racers blended
     * (measured as the "characters jitter on the title animation" report,
     * 2026-08-17) — and the cleared-track preview flybys (loaded with the
     * ZERO_PLAYERS sentinel, RACETYPE_CUTSCENE_1). The borrow hazard the gate
     * exists for cannot slip through the wider test, but NOT for the reason an
     * earlier version of this comment gave ("a borrowing menu never runs
     * camSetProjMtx for a level scene" — false: menu_camera_centre runs it
     * every tick of a track preview, IN ADDITION to the scene, which conflicted
     * the set and unsmoothed the whole flyby, issue #44b). The real guarantee:
     * a borrowing latch declares itself via
     * presentation_snapshot_authored_camera_borrow_begin and never files, so
     * presentation_snapshot_authored_cameras_copy() below returns entries only
     * when a real scene latched a complete, conflict-free authored record this
     * tick — and a genuinely ambiguous double-lens tick still fails closed via
     * the conflict rule, which is unchanged. */
    if (gameMode != GAMEMODE_INGAME && gameMode != GAMEMODE_MENU) {
        return;
    }
    if (gameMode == GAMEMODE_MENU) {
        levelHeader = level_header();
        if (levelHeader == NULL) {
            return;
        }
    }

    cameraCount = (s32)presentation_snapshot_authored_cameras_copy(
        authored_tick, cameras, ARRAY_COUNT(cameras));

    for (viewport = 0; viewport < cameraCount; viewport++) {
        /* This is the complete recipe captured beside the exact matrix it
         * authored. Never reconstruct it from mutable camera globals here:
         * cinematic logic can update bank 4 after viewport rendering. */
        presentation_snapshot_capture_camera(&cameras[viewport]);
    }
}

#if defined(MDKR_ENABLE_ONLINE_BETA)
/*
 * Camera out-of-bounds census (log-only, env-armed, beta-only).
 *
 * MDKR_CAMERA_OOB_CENSUS=1 accumulates counters and prints a [CAM-OOB]
 * summary row every 300 census ticks (and one final row at exit); =2 adds a
 * [CAM-OOB-EVT] row per event. Nothing here writes game state: the probes are
 * get_level_segment_index_from_position (a pure bounding-box scan, the game's
 * own "which segment holds the camera" answer -- -1 IS the game's out-of-
 * bounds verdict, the one that makes initialise_player_viewport_vars fall
 * back to gSceneStartSegment = -1) and presentation_snapshot_resolve_camera
 * (read-only over the published pair).
 *
 * Every counter is split near/far of a rollback correction: "near" means
 * within MDKR_CAMERA_OOB_CENSUS_WINDOW (default 8) authored ticks after the
 * game-authority restore serial advanced. Corrections are where rollback
 * mispredictions concentrate, so this split is the discriminator between
 * "online camera leaks are correction-correlated" and "same as offline".
 */
typedef struct CamOobCensus {
    int level;                 /* 0 off, 1 counters, 2 +event rows */
    uint64_t window;           /* "near" horizon in authored ticks */
    uint64_t last_restore_serial;
    uint64_t last_restore_tick;
    int restore_seen;
    uint64_t ticks, ticks_near, corrections, cams;
    uint64_t authored_oob_near, authored_oob_far;
    uint64_t interp_checked_near, interp_checked_far;
    uint64_t interp_oob_near, interp_oob_far;
    uint64_t hold_near, hold_far, blend_near, blend_far;
    uint64_t step10_near, step30_near, step100_near;
    uint64_t step10_far, step30_far, step100_far;
    double maxstep_near, maxstep_far;
    uint64_t yaw45_near, yaw45_far;
    uint64_t segchange_near, segchange_far;
    /* Ticks where a viewport that HAS been publishing a camera published
     * none. Pre-fix, every online correction tick lands here (the stage
     * reset wiped the authored-camera latch the in-flight pass had already
     * armed), which is why the counters above go blind exactly where the
     * defect lives. The live gCameras probe below keeps measuring through
     * those windows. */
    uint64_t miss_near, miss_far;
    uint64_t liveoob_near, liveoob_far;
    /* Continuity lifecycle. Sticky/last_* state survives CORRECTION stage
     * resets on purpose (that is the whole point of the live probe), but it
     * must NOT survive a census gap (we left INGAME: menus between races,
     * a level load) or a stage-generation change that no restore caused
     * (level boundary, present rebase) -- a retired viewport would otherwise
     * count phantom misses for the rest of a long owner session. */
    uint64_t last_stage_generation;
    uint64_t last_census_tick;
    uint8_t stage_gen_seen;
    uint8_t census_tick_seen;
    /* Cross-correction pose continuity, independent of the snapshot store's
     * stage generation (which a restore deliberately resets). */
    float last_pos[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS][3];
    int16_t last_yaw[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];
    s32 last_seg[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];
    uint8_t last_valid[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];
    int32_t sticky_camera_id[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];
    uint8_t sticky_valid[PRESENTATION_SNAPSHOT_MAX_VIEWPORTS];
} CamOobCensus;

static CamOobCensus sCamOob = { .level = -1 };

static void cam_oob_census_report(void) {
    CamOobCensus *c = &sCamOob;
    if (c->level <= 0 || c->ticks == 0u) {
        return;
    }
    fprintf(stderr,
            "[CAM-OOB] ticks=%llu near=%llu corr=%llu cams=%llu "
            "authoob_near=%llu authoob_far=%llu "
            "interpchk_near=%llu interpchk_far=%llu "
            "interpoob_near=%llu interpoob_far=%llu "
            "hold_near=%llu hold_far=%llu blend_near=%llu blend_far=%llu "
            "step10_near=%llu step30_near=%llu step100_near=%llu "
            "step10_far=%llu step30_far=%llu step100_far=%llu "
            "maxstep_near=%.1f maxstep_far=%.1f "
            "yaw45_near=%llu yaw45_far=%llu "
            "segchg_near=%llu segchg_far=%llu "
            "miss_near=%llu miss_far=%llu "
            "liveoob_near=%llu liveoob_far=%llu\n",
            (unsigned long long)c->ticks, (unsigned long long)c->ticks_near,
            (unsigned long long)c->corrections, (unsigned long long)c->cams,
            (unsigned long long)c->authored_oob_near,
            (unsigned long long)c->authored_oob_far,
            (unsigned long long)c->interp_checked_near,
            (unsigned long long)c->interp_checked_far,
            (unsigned long long)c->interp_oob_near,
            (unsigned long long)c->interp_oob_far,
            (unsigned long long)c->hold_near, (unsigned long long)c->hold_far,
            (unsigned long long)c->blend_near,
            (unsigned long long)c->blend_far,
            (unsigned long long)c->step10_near,
            (unsigned long long)c->step30_near,
            (unsigned long long)c->step100_near,
            (unsigned long long)c->step10_far,
            (unsigned long long)c->step30_far,
            (unsigned long long)c->step100_far,
            c->maxstep_near, c->maxstep_far,
            (unsigned long long)c->yaw45_near,
            (unsigned long long)c->yaw45_far,
            (unsigned long long)c->segchange_near,
            (unsigned long long)c->segchange_far,
            (unsigned long long)c->miss_near, (unsigned long long)c->miss_far,
            (unsigned long long)c->liveoob_near,
            (unsigned long long)c->liveoob_far);
}

/* Shared cross-tick pose-step tracker for captured and live-probed poses, so
 * the continuity measurement never pauses while captures are blind. */
static void cam_oob_track_pose(CamOobCensus *c, size_t vp, uint64_t tick,
                               float x, float y, float z, int16_t yaw_raw,
                               s32 seg, int near_correction) {
    if (c->last_valid[vp]) {
        const double dx = (double)x - c->last_pos[vp][0];
        const double dy = (double)y - c->last_pos[vp][1];
        const double dz = (double)z - c->last_pos[vp][2];
        const double step = sqrt(dx * dx + dy * dy + dz * dz);
        const float yaw = mdkr_yaw_delta_deg(
            (uint16_t)c->last_yaw[vp], (uint16_t)yaw_raw);
        if (near_correction) {
            if (step > 10.0) c->step10_near++;
            if (step > 30.0) c->step30_near++;
            if (step > 100.0) c->step100_near++;
            if (step > c->maxstep_near) c->maxstep_near = step;
            if (yaw > 45.0f || yaw < -45.0f) c->yaw45_near++;
            if (seg != c->last_seg[vp]) c->segchange_near++;
        } else {
            if (step > 10.0) c->step10_far++;
            if (step > 30.0) c->step30_far++;
            if (step > 100.0) c->step100_far++;
            if (step > c->maxstep_far) c->maxstep_far = step;
            if (yaw > 45.0f || yaw < -45.0f) c->yaw45_far++;
            if (seg != c->last_seg[vp]) c->segchange_far++;
        }
        if ((c->level >= 2) && step > 30.0) {
            fprintf(stderr,
                    "[CAM-OOB-EVT] tick=%llu vp=%zu kind=step "
                    "step=%.1f yaw=%.1f seg=%d near=%d\n",
                    (unsigned long long)tick, vp, step,
                    (double)yaw, seg, near_correction);
        }
    }
    c->last_pos[vp][0] = x;
    c->last_pos[vp][1] = y;
    c->last_pos[vp][2] = z;
    c->last_yaw[vp] = yaw_raw;
    c->last_seg[vp] = seg;
    c->last_valid[vp] = 1;
}

static void cam_oob_census_tick(uint64_t authored_tick) {
    CamOobCensus *c = &sCamOob;
    const PresentationSnapshot *current;
    const PresentationSnapshot *previous;
    uint64_t restore_serial;
    int near_correction;
    size_t vp;

    if (c->level < 0) {
        const char *value = getenv("MDKR_CAMERA_OOB_CENSUS");
        c->level = value != NULL && value[0] != '\0' && value[0] != '0'
            ? (value[0] == '2' ? 2 : 1) : 0;
        if (c->level > 0) {
            const char *window = getenv("MDKR_CAMERA_OOB_CENSUS_WINDOW");
            c->window = 8u;
            if (window != NULL && window[0] != '\0') {
                long parsed = strtol(window, NULL, 10);
                if (parsed > 0 && parsed < 1000) {
                    c->window = (uint64_t)parsed;
                }
            }
            atexit(cam_oob_census_report);
        }
    }
    if (c->level == 0 || get_game_mode() != GAMEMODE_INGAME) {
        return;
    }

    restore_serial = mdkr_rollback_game_authority_restore_serial();
    {
        const int restored_this_tick = restore_serial != c->last_restore_serial;
        if (restored_this_tick) {
            c->corrections += restore_serial - c->last_restore_serial;
            c->last_restore_serial = restore_serial;
            c->last_restore_tick = authored_tick;
            c->restore_seen = 1;
        }
        /* Continuity lifecycle (see the struct note): drop sticky/last_*
         * state across a census gap, or across a stage-generation change
         * that no restore caused. A CORRECTION's own stage bump keeps them
         * -- the live probe exists precisely to see through that reset. */
        current = presentation_snapshot_current();
        if (c->census_tick_seen && authored_tick != c->last_census_tick + 1u) {
            memset(c->sticky_valid, 0, sizeof(c->sticky_valid));
            memset(c->last_valid, 0, sizeof(c->last_valid));
        }
        if (current != NULL && current->valid) {
            if (c->stage_gen_seen &&
                current->stage_generation != c->last_stage_generation &&
                !restored_this_tick) {
                memset(c->sticky_valid, 0, sizeof(c->sticky_valid));
                memset(c->last_valid, 0, sizeof(c->last_valid));
            }
            c->last_stage_generation = current->stage_generation;
            c->stage_gen_seen = 1;
        }
        c->last_census_tick = authored_tick;
        c->census_tick_seen = 1;
    }
    near_correction = c->restore_seen &&
                      authored_tick >= c->last_restore_tick &&
                      authored_tick - c->last_restore_tick < c->window;

    c->ticks++;
    if (near_correction) {
        c->ticks_near++;
    }

    previous = presentation_snapshot_previous();
    {
        const size_t captured =
            (current != NULL && current->valid &&
             current->authored_tick == authored_tick)
                ? current->camera_count : 0u;
        /* Viewports that have been publishing a camera but did not this tick:
         * measure the AUTHORED camera live so the census cannot go blind on
         * exactly the (correction) ticks it exists to characterize. */
        for (vp = captured; vp < PRESENTATION_SNAPSHOT_MAX_VIEWPORTS; vp++) {
            const Camera *live;
            s32 seg;
            if (!c->sticky_valid[vp]) {
                continue;
            }
            if (near_correction) c->miss_near++;
            else c->miss_far++;
            live = &gCameras[(size_t)c->sticky_camera_id[vp] %
                             PRESENTATION_SNAPSHOT_MAX_CAMERAS];
            seg = get_level_segment_index_from_position(
                live->trans.x_position, live->trans.y_position,
                live->trans.z_position);
            if (seg == -1) {
                if (near_correction) c->liveoob_near++;
                else c->liveoob_far++;
                if (c->level >= 2) {
                    fprintf(stderr,
                            "[CAM-OOB-EVT] tick=%llu vp=%zu kind=live seg=-1 "
                            "pos=(%.1f,%.1f,%.1f) near=%d\n",
                            (unsigned long long)authored_tick, vp,
                            (double)live->trans.x_position,
                            (double)live->trans.y_position,
                            (double)live->trans.z_position, near_correction);
                }
            }
            cam_oob_track_pose(c, vp, authored_tick,
                               live->trans.x_position,
                               live->trans.y_position,
                               live->trans.z_position,
                               live->trans.rotation.y_rotation,
                               seg, near_correction);
        }
        if (captured == 0u) {
            if (c->ticks % 300u == 0u) {
                cam_oob_census_report();
            }
            return;
        }
    }

    for (vp = 0; vp < current->camera_count &&
                 vp < PRESENTATION_SNAPSHOT_MAX_VIEWPORTS; vp++) {
        const PresentationCameraEntry *entry = &current->cameras[vp];
        const s32 seg = get_level_segment_index_from_position(
            entry->position[0], entry->position[1], entry->position[2]);
        PresentationCameraPose pose;
        int blendable = 0;

        c->cams++;
        if (seg == -1) {
            if (near_correction) c->authored_oob_near++;
            else c->authored_oob_far++;
            if (c->level >= 2) {
                fprintf(stderr,
                        "[CAM-OOB-EVT] tick=%llu vp=%zu kind=authored seg=-1 "
                        "pos=(%.1f,%.1f,%.1f) near=%d\n",
                        (unsigned long long)authored_tick, vp,
                        (double)entry->position[0], (double)entry->position[1],
                        (double)entry->position[2], near_correction);
            }
        }

        if (presentation_snapshot_resolve_camera(
                (int)vp, 1u, 2u, &pose) && pose.interpolated) {
            blendable = 1;
        }
        if (blendable) {
            if (near_correction) c->blend_near++;
            else c->blend_far++;
        } else {
            if (near_correction) c->hold_near++;
            else c->hold_far++;
        }

        /* Sub-tick interpolation probes: does the production blend path pass
         * through out-of-bounds space while BOTH endpoints are in bounds?
         * (The falls-flash class: a defect that exists only on interpolated
         * presentation slots and never on an authored endpoint.) */
        if (blendable && seg != -1 && previous != NULL && previous->valid &&
            vp < previous->camera_count) {
            const PresentationCameraEntry *prev_entry =
                &previous->cameras[vp];
            const s32 prev_seg = get_level_segment_index_from_position(
                prev_entry->position[0], prev_entry->position[1],
                prev_entry->position[2]);
            if (prev_seg != -1) {
                uint64_t k;
                if (near_correction) c->interp_checked_near++;
                else c->interp_checked_far++;
                for (k = 1u; k < 8u; k++) {
                    PresentationCameraPose sample;
                    if (!presentation_snapshot_resolve_camera(
                            (int)vp, k, 8u, &sample) ||
                        !sample.interpolated) {
                        continue;
                    }
                    if (get_level_segment_index_from_position(
                            sample.position[0], sample.position[1],
                            sample.position[2]) == -1) {
                        if (near_correction) c->interp_oob_near++;
                        else c->interp_oob_far++;
                        if (c->level >= 2) {
                            fprintf(stderr,
                                    "[CAM-OOB-EVT] tick=%llu vp=%zu "
                                    "kind=interp alpha=%llu/8 seg=-1 "
                                    "pos=(%.1f,%.1f,%.1f) near=%d\n",
                                    (unsigned long long)authored_tick, vp,
                                    (unsigned long long)k,
                                    (double)sample.position[0],
                                    (double)sample.position[1],
                                    (double)sample.position[2],
                                    near_correction);
                        }
                    }
                }
            }
        }

        /* Cross-tick pose continuity, tracked outside the snapshot store so a
         * stage reset (which every correction restore performs) cannot hide
         * the step it just caused. */
        cam_oob_track_pose(c, vp, authored_tick,
                           entry->position[0], entry->position[1],
                           entry->position[2], entry->rotation_y,
                           seg, near_correction);
        c->sticky_camera_id[vp] = entry->camera_id;
        c->sticky_valid[vp] = 1;
    }

    if (c->ticks % 300u == 0u) {
        cam_oob_census_report();
    }
}
#else
#define cam_oob_census_tick(authored_tick) ((void)(authored_tick))
#endif /* MDKR_ENABLE_ONLINE_BETA */

void presentation_snapshot_capture(uint64_t authored_tick) {
    Object **objects;
    s32 first = 0;
    s32 count = 0;
    s32 index;

    if (!presentation_snapshot_enabled()) {
        return;
    }

    presentation_snapshot_capture_begin_authored(authored_tick);

    objects = objGetObjList(&first, &count);
    if (objects != NULL) {
        /*
         * From 0, not gObjectListStart: that cursor is render's own
         * draw-order optimisation (get_first_active_object narrows it), and
         * the snapshot must describe every live object so an object that
         * drops out of the drawn range does not read as a destroy.
         */
        for (index = 0; index < count; index++) {
            const Object *object = objects[index];
            if (object == NULL) {
                continue;
            }
            if (object->trans.flags & OBJ_FLAGS_PARTICLE) {
                capture_particle(object);
            } else {
                capture_object(object);
            }
        }
    }

    capture_external_transforms();
    capture_cameras(authored_tick);
    presentation_snapshot_capture_commit();
    cam_oob_census_tick(authored_tick);
}
