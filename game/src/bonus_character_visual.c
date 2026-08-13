#include "bonus_character_visual.h"

#ifdef NATIVE_PORT

#include "math_util.h"
#include "mdkr_trace.h"
#include "structs.h"

#include <math.h>

f32 bonus_visual_companion_scale(const Object *actor, const Object *owner,
                                 f32 scaleFactor) {
    f32 ownerRatio;

    if (actor == NULL || actor->header == NULL || owner == NULL ||
        owner->header == NULL || !isfinite(actor->header->scale) ||
        !isfinite(owner->header->scale) || !isfinite(owner->trans.scale) ||
        !isfinite(scaleFactor) || actor->header->scale <= 0.0f ||
        owner->header->scale <= 0.0f || owner->trans.scale <= 0.0f ||
        scaleFactor <= 0.0f) {
        return 0.0f;
    }

    /* Keep the companion's authored model scale. The donor contributes only
     * transient racer squash/stretch relative to its own authored scale; its
     * absolute kart/hover/plane scale must never replace the companion's. */
    ownerRatio = owner->trans.scale / owner->header->scale;
    return actor->header->scale * scaleFactor * ownerRatio;
}

void bonus_visual_apply_local_offset(Object *actor, f32 x, f32 y, f32 z) {
    Vec3f offset;

    if (actor == NULL || !isfinite(x) || !isfinite(y) || !isfinite(z)) {
        return;
    }
    offset.x = x;
    offset.y = y;
    offset.z = z;
    vec3f_rotate(&actor->trans.rotation, &offset);
    actor->trans.x_position += offset.x;
    actor->trans.y_position += offset.y;
    actor->trans.z_position += offset.z;
}

/* Object shadows carry an absolute header-authored size and do not inherit
 * trans.scale. Presentation actors therefore need the same proportional
 * correction as playable Taj. */
void bonus_visual_scale_shadow(Object *obj) {
    f32 scale;

    if (obj == NULL || obj->header == NULL || obj->shadow == NULL) {
        return;
    }
    if (!isfinite(obj->trans.scale) || !isfinite(obj->header->scale) ||
        !isfinite(obj->header->shadowScale) || obj->trans.scale <= 0.0f ||
        obj->header->scale <= 0.0f) {
        obj->shadow->scale = 0.0f;
        return;
    }
    scale = obj->header->shadowScale * (obj->trans.scale / obj->header->scale);
    obj->shadow->scale = isfinite(scale) && scale > 0.0f ? scale : 0.0f;
}

s32 bonus_visual_krunch_driver_batch(s32 vehicle, s32 lod, s32 batch) {
    if (vehicle == VEHICLE_CAR) {
        switch (lod) {
        case 0:
            return batch <= 18 || batch == 27;
        case 1:
            return batch <= 8 || (batch >= 10 && batch <= 20) || batch == 28;
        case 2:
            return batch <= 4 || (batch >= 6 && batch <= 15) || batch == 23;
        case 3:
            return (batch >= 1 && batch <= 10) || batch == 16;
        case 4:
            return batch <= 2 || (batch >= 4 && batch <= 10);
        default:
            return FALSE;
        }
    }
    if (vehicle == VEHICLE_HOVERCRAFT) {
        switch (lod) {
        case 0:
            return batch <= 9 || (batch >= 11 && batch <= 19) || batch == 29;
        case 1:
            return batch <= 7 || (batch >= 9 && batch <= 19) || batch == 29;
        case 2:
            return batch <= 4 || (batch >= 6 && batch <= 15) || batch == 28;
        case 3:
            return (batch >= 1 && batch <= 10) || batch == 23;
        case 4:
            return batch >= 1 && batch <= 10;
        default:
            return FALSE;
        }
    }
    return FALSE;
}

static u64 bonus_visual_vertex_hash(const Vertex *vertices, s32 count) {
    u64 hash = UINT64_C(1469598103934665603);
    s32 i;

    if (vertices == NULL || count <= 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        hash ^= (u16) vertices[i].x;
        hash *= UINT64_C(1099511628211);
        hash ^= (u16) vertices[i].y;
        hash *= UINT64_C(1099511628211);
        hash ^= (u16) vertices[i].z;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void bonus_visual_trace_animation(const char *identity, const Object *actor,
                                  u32 sequence) {
    ModelInstance *instance;
    ObjectModel *model;
    u64 front;
    u64 back;

    if (!mdkr_trace_enabled() || (sequence & 31u) != 0u || identity == NULL ||
        actor == NULL || actor->header == NULL || actor->modelInstances == NULL ||
        actor->modelIndex < 0 || actor->modelIndex >= actor->header->numberOfModelIds) {
        return;
    }
    instance = actor->modelInstances[actor->modelIndex];
    if (instance == NULL || instance->objModel == NULL) {
        return;
    }
    model = instance->objModel;
    if (model->numberOfVertices <= 0) {
        return;
    }
    /* vertices[0] and vertices[1] are the animated double buffer; vertices[2] is obj_animate's
     * absolute-position scratch and is deliberately not sampled. */
    front = bonus_visual_vertex_hash(instance->vertices[0], model->numberOfVertices);
    back = bonus_visual_vertex_hash(instance->vertices[1], model->numberOfVertices);
    MDKR_TRACE("bonus_animation: identity=%s anim=%d frame=%d task=%d verts=%d "
               "front=%016llx back=%016llx distinct=%d",
               identity, (s32) actor->animationID, (s32) actor->animFrame,
               (s32) instance->animationTaskNum, (s32) model->numberOfVertices,
               (unsigned long long) front, (unsigned long long) back,
               front != back);
}

#endif
