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
#include "objects.h"
#include "particles.h"
#include "textures_sprites.h" /* OBJ_FLAGS_PARTICLE */

#include "presentation_snapshot.h"

extern Camera gCameras[8];
extern s32 gNumCameras;
extern s8 gCutsceneCameraActive;
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

static void capture_cameras(void) {
    s32 viewports = gNumCameras;
    s32 viewport;

    if (viewports < 1) {
        viewports = 1;
    }
    if (viewports > PRESENTATION_SNAPSHOT_MAX_VIEWPORTS) {
        viewports = PRESENTATION_SNAPSHOT_MAX_VIEWPORTS;
    }

    for (viewport = 0; viewport < viewports; viewport++) {
        /* cam_get_active_camera()'s selection: the cutscene bank is the same
         * index offset by 4. */
        const s32 cameraId = gCutsceneCameraActive ? viewport + 4 : viewport;
        const Camera *camera = &gCameras[cameraId];
        const ScreenViewport *screen = &gScreenViewports[viewport];
        PresentationCameraEntry sample;

        memset(&sample, 0, sizeof(sample));
        sample.camera_id = cameraId;
        sample.viewport_index = viewport;
        /*
         * The TRUE inputs to cam_build_view_basis(), not the matrices it
         * derives: position, the three fixed rotations, the separately
         * stored pitch it folds into x_rotation, and the shake offset it
         * applies to y when gNoCamShake is set. Interpolating these and
         * rebuilding the basis afterwards is spec §7's "camera: interpolate
         * before deriving the view matrix".
         */
        sample.position[0] = camera->trans.x_position;
        sample.position[1] = camera->trans.y_position;
        sample.position[2] = camera->trans.z_position;
        sample.rotation_x = camera->trans.rotation.x_rotation;
        sample.rotation_y = camera->trans.rotation.y_rotation;
        sample.rotation_z = camera->trans.rotation.z_rotation;
        sample.pitch = camera->pitch;
        sample.shake_magnitude = camera->shakeMagnitude;
        sample.apply_shake = gNoCamShake;
        /*
         * Projection inputs. gCurCamFOV is the authored level FOV;
         * cam_rebuild_native_projection() turns it into the fovy/aspect pair
         * actually handed to guPerspectiveF, together with the fixed
         * CAMERA_NEAR/CAMERA_FAR.
         *
         * Wave A limitation, recorded rather than hidden: those effective
         * values live in single globals that viewport_main rewrites per
         * viewport, so at the tick boundary they hold the LAST viewport's
         * projection. For 1P (and for every split-screen layout that shares
         * one projection) that is exact. Per-viewport projection divergence
         * is a Wave B item, and the per-viewport viewport rect below is
         * already captured correctly.
         */
        sample.fov = gCurCamFOV;
        sample.vertical_fov = cam_get_effective_vertical_fov();
        sample.aspect = cam_get_effective_aspect();
        sample.near_plane = CAMERA_NEAR;
        sample.far_plane = CAMERA_FAR;
        sample.viewport[0] = (f32)screen->posX;
        sample.viewport[1] = (f32)screen->posY;
        sample.viewport[2] = (f32)screen->width;
        sample.viewport[3] = (f32)screen->height;
        presentation_snapshot_capture_camera(&sample);
    }
}

void presentation_snapshot_capture(void) {
    Object **objects;
    s32 first = 0;
    s32 count = 0;
    s32 index;

    if (!presentation_snapshot_enabled()) {
        return;
    }

    presentation_snapshot_capture_begin();

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

    capture_cameras();
    presentation_snapshot_capture_commit();
}
