#include "rollback_authority_view.h"

#ifdef NATIVE_PORT
#include <string.h>

#include "objects.h"
#include "particles.h"
#include "structs.h"

extern Object **gObjPtrList;
extern Object **gParticlePtrList;
extern Object **gCollisionObjects;
extern Object **D_8011AE74;
extern CheckpointNode *gTrackCheckpoints;
extern Object *(*gCameraObjList)[20];
extern Object *(*gRacers)[10];
extern Object **gRacersByPosition;
extern Object **gRacersByPort;
extern Object *(*gAINodes)[128];
extern s8 (*gDrawbridgeTimers)[8];
extern unk800179D0 *D_8011AFF4;
extern ObjectHeader *(*gLoadedObjectHeaders)[ASSET_OBJECTS_COUNT];
extern u8 (*gObjectHeaderReferences)[ASSET_OBJECTS_COUNT];
extern s32 *gObjectMap[2];
extern s32 gObjectCount;
extern Particle *gTriangleParticleBuffer;
extern Particle *gRectangleParticleBuffer;
extern Particle *gSpriteParticleBuffer;
extern Particle *gLineParticleBuffer;
extern PointParticle *gPointParticleBuffer;

s32 mdkr_object_rollback_view(MdkrObjectRollbackView *view) {
    if (view == NULL) {
        return FALSE;
    }
    memset(view, 0, sizeof(*view));
    view->object_list = gObjPtrList;
    view->particle_list = gParticlePtrList;
    view->collision_objects = gCollisionObjects;
    view->animation_objects = D_8011AE74;
    view->track_checkpoints = gTrackCheckpoints;
    view->camera_objects = gCameraObjList;
    view->racers = gRacers;
    view->racers_by_position = gRacersByPosition;
    view->racers_by_port = gRacersByPort;
    view->ai_nodes = gAINodes;
    view->drawbridge_timers = gDrawbridgeTimers;
    view->behavior_scratch = D_8011AFF4;
    view->loaded_object_headers = gLoadedObjectHeaders;
    view->object_header_references = gObjectHeaderReferences;
    view->object_maps[0] = gObjectMap[0];
    view->object_maps[1] = gObjectMap[1];
    view->particle_buffers[0] = gTriangleParticleBuffer;
    view->particle_buffers[1] = gRectangleParticleBuffer;
    view->particle_buffers[2] = gSpriteParticleBuffer;
    view->particle_buffers[3] = gLineParticleBuffer;
    view->particle_buffers[4] = gPointParticleBuffer;
    return view->object_list != NULL && view->particle_list != NULL &&
           view->collision_objects != NULL &&
           view->animation_objects != NULL &&
           view->track_checkpoints != NULL && view->camera_objects != NULL &&
           view->racers != NULL && view->racers_by_position != NULL &&
           view->racers_by_port != NULL && view->ai_nodes != NULL &&
           view->drawbridge_timers != NULL &&
           view->behavior_scratch != NULL &&
           view->loaded_object_headers != NULL &&
           view->object_header_references != NULL &&
           view->particle_buffers[0] != NULL &&
           view->particle_buffers[1] != NULL &&
           view->particle_buffers[2] != NULL &&
           view->particle_buffers[3] != NULL &&
           view->particle_buffers[4] != NULL;
}

s32 mdkr_object_rollback_identify_address(
    const void *allocation_base, const void *address,
    MdkrObjectRollbackAddressInfo *info) {
    const uint8_t *behavior;
    if (allocation_base == NULL || address == NULL || info == NULL ||
        gObjPtrList == NULL) {
        return FALSE;
    }
    if (!mdkr_object_rollback_describe_object(allocation_base, info)) {
        return FALSE;
    }
    info->object_offset =
        (size_t)((const uint8_t *)address - (const uint8_t *)allocation_base);
    behavior = (const uint8_t *)((const Object *)allocation_base)->anyBehaviorData;
    if (behavior != NULL && behavior >= (const uint8_t *)allocation_base &&
        behavior <= (const uint8_t *)address) {
        info->behavior_offset =
            (size_t)((const uint8_t *)address - behavior);
        info->has_behavior_offset = TRUE;
    }
    return TRUE;
}

s32 mdkr_object_rollback_describe_object(
    const void *object_pointer, MdkrObjectRollbackAddressInfo *info) {
    const Object *object = (const Object *)object_pointer;
    s32 index;
    if (object == NULL || info == NULL || gObjPtrList == NULL) {
        return FALSE;
    }
    memset(info, 0, sizeof(*info));
    info->object_index = -1;
    for (index = 0; index < gObjectCount; index++) {
        if (gObjPtrList[index] == object) {
            info->object_index = index;
            break;
        }
    }
    info->behavior_id = object->behaviorId;
    info->header_type = object->headerType;
    info->behavior_base_offset = object->anyBehaviorData != NULL
        ? (size_t)((const uint8_t *)object->anyBehaviorData -
                   (const uint8_t *)object) : SIZE_MAX;
    info->shading_base_offset = object->shading != NULL
        ? (size_t)((const uint8_t *)object->shading -
                   (const uint8_t *)object) : SIZE_MAX;
    info->shadow_base_offset = object->shadow != NULL
        ? (size_t)((const uint8_t *)object->shadow -
                   (const uint8_t *)object) : SIZE_MAX;
    info->interaction_base_offset = object->interactObj != NULL
        ? (size_t)((const uint8_t *)object->interactObj -
                   (const uint8_t *)object) : SIZE_MAX;
    info->collision_base_offset = object->collisionData != NULL
        ? (size_t)((const uint8_t *)object->collisionData -
                   (const uint8_t *)object) : SIZE_MAX;
    return TRUE;
}
#endif
