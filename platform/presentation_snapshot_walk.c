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
static void capture_external_transform(const ObjectTransform *transform) {
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
    presentation_snapshot_capture_object(&sample);
}

static void capture_external_transforms(void) {
    size_t count = presentation_snapshot_external_transform_count();
    size_t index;

    for (index = 0u; index < count; index++) {
        const ObjectTransform *transform =
            (const ObjectTransform *)
                presentation_snapshot_external_transform_at(index);
        if (transform != NULL) {
            capture_external_transform(transform);
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
     * view-projection. A loaded cutscene level is different: render_scene owns
     * its bank-4 camera for the full pass, even though the cinematic shell runs
     * it under GAMEMODE_MENU. Its exact viewport IDs are safe to snapshot. */
    if (gameMode != GAMEMODE_INGAME && gameMode != GAMEMODE_MENU) {
        return;
    }
    if (gameMode == GAMEMODE_MENU) {
        levelHeader = level_header();
        if (levelHeader == NULL ||
            (levelHeader->race_type != RACETYPE_CUTSCENE_1 &&
             levelHeader->race_type != RACETYPE_CUTSCENE_2)) {
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
}
