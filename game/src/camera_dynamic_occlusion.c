#include "camera_dynamic_occlusion.h"

#ifdef NATIVE_PORT

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "camera_object_occlusion.h"
#include "camera_dynamic_publication.h"
#include "camera_dynamic_temporal.h"
#include "camera_obstruction_query.h"
#include "math_util.h"
#include "objects.h"
#include "object_functions.h"
#include "structs.h"
#include "textures_sprites.h"

typedef struct MdkrCameraDynamicIdentity {
    const Object *object;
    uint64_t spawn_generation;
    /* High-bit namespace cannot collide with track source IDs, which are
     * counted from 1 and bounded by the s16 segment asset contract. */
    uint32_t stable_instance_id;
} MdkrCameraDynamicIdentity;

typedef struct MdkrCameraDynamicModelBounds {
    const ObjectModel *model;
    uint32_t model_generation;
    MdkrCameraDynamicAabb local_bounds;
} MdkrCameraDynamicModelBounds;

typedef struct MdkrCameraDynamicInstance {
    const ObjectModel *model;
    const MdkrCameraOcclusionWorld *world;
    ObjectTransform authored_transform;
    MdkrCameraObjectTransform transform;
    MdkrCameraDynamicAabb world_bounds;
    MdkrCameraDynamicAabb temporal_bounds;
    uint64_t object_spawn_generation;
    uint32_t model_generation;
    uint32_t stable_instance_id;
    uint32_t authoritative_list_index;
    uint8_t temporal_moved;
    uint8_t temporal_proxy;
} MdkrCameraDynamicInstance;

static MdkrCameraDynamicIdentity *sIdentities;
static MdkrCameraDynamicModelBounds *sModelBounds;
static MdkrCameraDynamicInstance *sInstances;
static MdkrCameraDynamicInstance *sPreviousInstances;
static size_t sCapacity;
static size_t sIdentityCount;
static size_t sModelBoundsCount;
static size_t sInstanceCount;
static size_t sPreviousInstanceCount;
static uint64_t sNextSpawnGeneration = 1U;
static uint32_t sNextStableInstanceId = UINT32_C(0x80000000);
static MdkrCameraDynamicPublicationState sPublicationState;
static MdkrCameraDynamicOcclusionTelemetry sTelemetry;

/*
 * Slot indices, not addresses: the census runs a lookup per candidate per tick
 * and particles churn identity slots on every spawn, so neither may be a scan
 * over the whole capacity. Each table is a chained index into the fixed slot
 * array it describes, so it holds no ownership and answers exactly what the
 * scan it replaces answered. Identity slot choice stays "lowest vacant" through
 * a free bitmap because the arrays are addressed by slot elsewhere.
 */
#define MDKR_CAMERA_DYNAMIC_INDEX_NONE (-1)
#define MDKR_CAMERA_DYNAMIC_INDEX_WORD_BITS 64U

static int32_t *sIdentityBuckets;
static int32_t *sIdentityNext;
static int32_t *sIdentityPrev;
static uint64_t *sIdentityVacant;
static size_t sIdentityVacantWords;
static int32_t *sModelBoundsBuckets;
static int32_t *sModelBoundsNext;
static int32_t *sModelBoundsPrev;
static int32_t *sPreviousInstanceBuckets;
static int32_t *sPreviousInstanceNext;
static size_t sIndexBucketCount;

static uint64_t mdkr_camera_dynamic_index_mix(uint64_t value) {
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 29;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 32;
    return value;
}

static size_t mdkr_camera_dynamic_index_bucket(uint64_t key) {
    return (size_t)(mdkr_camera_dynamic_index_mix(key) & (uint64_t)(sIndexBucketCount - 1U));
}

static size_t mdkr_camera_dynamic_pointer_bucket(const void *key) {
    return mdkr_camera_dynamic_index_bucket((uint64_t)(uintptr_t)key);
}

static void mdkr_camera_dynamic_index_clear(int32_t *buckets) {
    size_t index;
    for (index = 0U; index < sIndexBucketCount; index++) {
        buckets[index] = MDKR_CAMERA_DYNAMIC_INDEX_NONE;
    }
}

static void mdkr_camera_dynamic_index_link(
    int32_t *buckets, int32_t *next, int32_t *previous, size_t bucket, size_t slot) {
    const int32_t head = buckets[bucket];
    next[slot] = head;
    previous[slot] = MDKR_CAMERA_DYNAMIC_INDEX_NONE;
    if (head != MDKR_CAMERA_DYNAMIC_INDEX_NONE) {
        previous[head] = (int32_t)slot;
    }
    buckets[bucket] = (int32_t)slot;
}

static void mdkr_camera_dynamic_index_unlink(
    int32_t *buckets, int32_t *next, int32_t *previous, size_t bucket, size_t slot) {
    if (previous[slot] != MDKR_CAMERA_DYNAMIC_INDEX_NONE) {
        next[previous[slot]] = next[slot];
    } else if (buckets[bucket] == (int32_t)slot) {
        buckets[bucket] = next[slot];
    }
    if (next[slot] != MDKR_CAMERA_DYNAMIC_INDEX_NONE) {
        previous[next[slot]] = previous[slot];
    }
    next[slot] = MDKR_CAMERA_DYNAMIC_INDEX_NONE;
    previous[slot] = MDKR_CAMERA_DYNAMIC_INDEX_NONE;
}

static void mdkr_camera_dynamic_identity_mark_vacant(size_t slot, int vacant) {
    const size_t word = slot / MDKR_CAMERA_DYNAMIC_INDEX_WORD_BITS;
    const uint64_t bit = UINT64_C(1) << (slot % MDKR_CAMERA_DYNAMIC_INDEX_WORD_BITS);
    if (vacant) {
        sIdentityVacant[word] |= bit;
    } else {
        sIdentityVacant[word] &= ~bit;
    }
}

/* Equivalent to scanning sIdentities for the first NULL slot. */
static size_t mdkr_camera_dynamic_first_vacant_identity(void) {
    size_t word;
    for (word = 0U; word < sIdentityVacantWords; word++) {
        uint64_t bits = sIdentityVacant[word];
        size_t bit = 0U;
        if (bits == 0U) {
            continue;
        }
        while ((bits & UINT64_C(1)) == 0U) {
            bits >>= 1;
            bit++;
        }
        return word * MDKR_CAMERA_DYNAMIC_INDEX_WORD_BITS + bit;
    }
    return sCapacity;
}

static void mdkr_camera_dynamic_index_reset(void) {
    size_t index;

    if (sIndexBucketCount == 0U) {
        return;
    }
    mdkr_camera_dynamic_index_clear(sIdentityBuckets);
    mdkr_camera_dynamic_index_clear(sModelBoundsBuckets);
    mdkr_camera_dynamic_index_clear(sPreviousInstanceBuckets);
    for (index = 0U; index < sCapacity; index++) {
        sIdentityNext[index] = MDKR_CAMERA_DYNAMIC_INDEX_NONE;
        sIdentityPrev[index] = MDKR_CAMERA_DYNAMIC_INDEX_NONE;
        sModelBoundsNext[index] = MDKR_CAMERA_DYNAMIC_INDEX_NONE;
        sModelBoundsPrev[index] = MDKR_CAMERA_DYNAMIC_INDEX_NONE;
        sPreviousInstanceNext[index] = MDKR_CAMERA_DYNAMIC_INDEX_NONE;
    }
    for (index = 0U; index < sIdentityVacantWords; index++) {
        sIdentityVacant[index] = 0U;
    }
    for (index = 0U; index < sCapacity; index++) {
        mdkr_camera_dynamic_identity_mark_vacant(index, 1);
    }
}

static int mdkr_camera_dynamic_finite_vec3(MdkrCameraVec3 value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static int mdkr_camera_dynamic_aabb_from_world(
    const MdkrCameraOcclusionWorld *world,
    MdkrCameraDynamicAabb *out_bounds) {
    size_t index;
    MdkrCameraDynamicAabb bounds;

    if (world == NULL || out_bounds == NULL || world->vertices == NULL ||
        world->vertex_count == 0U) {
        return 0;
    }
    bounds.minimum = world->vertices[0];
    bounds.maximum = world->vertices[0];
    if (!mdkr_camera_dynamic_finite_vec3(bounds.minimum)) {
        return 0;
    }
    for (index = 1U; index < world->vertex_count; index++) {
        MdkrCameraVec3 point = world->vertices[index];
        if (!mdkr_camera_dynamic_finite_vec3(point)) {
            return 0;
        }
        if (point.x < bounds.minimum.x) bounds.minimum.x = point.x;
        if (point.y < bounds.minimum.y) bounds.minimum.y = point.y;
        if (point.z < bounds.minimum.z) bounds.minimum.z = point.z;
        if (point.x > bounds.maximum.x) bounds.maximum.x = point.x;
        if (point.y > bounds.maximum.y) bounds.maximum.y = point.y;
        if (point.z > bounds.maximum.z) bounds.maximum.z = point.z;
    }
    *out_bounds = bounds;
    return 1;
}

static int mdkr_camera_dynamic_transform_equal(
    const MdkrCameraObjectTransform *left,
    const MdkrCameraObjectTransform *right) {
    return left->translation.x == right->translation.x &&
        left->translation.y == right->translation.y &&
        left->translation.z == right->translation.z &&
        left->local_x_axis.x == right->local_x_axis.x &&
        left->local_x_axis.y == right->local_x_axis.y &&
        left->local_x_axis.z == right->local_x_axis.z &&
        left->local_y_axis.x == right->local_y_axis.x &&
        left->local_y_axis.y == right->local_y_axis.y &&
        left->local_y_axis.z == right->local_y_axis.z &&
        left->local_z_axis.x == right->local_z_axis.x &&
        left->local_z_axis.y == right->local_z_axis.y &&
        left->local_z_axis.z == right->local_z_axis.z;
}

/* Rebuilt from the retained copy each tick. Linking downward keeps every chain
 * in ascending slot order, so a lookup still returns the first match. */
static void mdkr_camera_dynamic_previous_index_rebuild(void) {
    size_t index;

    if (sIndexBucketCount == 0U) {
        return;
    }
    mdkr_camera_dynamic_index_clear(sPreviousInstanceBuckets);
    for (index = sPreviousInstanceCount; index-- > 0U;) {
        const size_t bucket = mdkr_camera_dynamic_index_bucket(
            sPreviousInstances[index].object_spawn_generation);
        sPreviousInstanceNext[index] = sPreviousInstanceBuckets[bucket];
        sPreviousInstanceBuckets[bucket] = (int32_t)index;
    }
}

static const MdkrCameraDynamicInstance *mdkr_camera_dynamic_find_previous_instance(
    uint64_t object_spawn_generation) {
    int32_t slot;

    if (!mdkr_camera_dynamic_publication_previous_valid(&sPublicationState) ||
        object_spawn_generation == 0U || sIndexBucketCount == 0U) {
        return NULL;
    }
    for (slot = sPreviousInstanceBuckets[
             mdkr_camera_dynamic_index_bucket(object_spawn_generation)];
         slot != MDKR_CAMERA_DYNAMIC_INDEX_NONE;
         slot = sPreviousInstanceNext[slot]) {
        if ((size_t)slot < sPreviousInstanceCount &&
            sPreviousInstances[slot].object_spawn_generation ==
                object_spawn_generation) {
            return &sPreviousInstances[slot];
        }
    }
    return NULL;
}

static MdkrCameraSweepStatus mdkr_camera_dynamic_sphere_aabb_sweep(
    const MdkrCameraDynamicAabb *bounds,
    const MdkrCameraSweepInput *input,
    uint32_t stable_id,
    MdkrCameraSweepHit *out_hit) {
    const double start[3] = {
        input->start_eye.x, input->start_eye.y, input->start_eye.z,
    };
    const double end[3] = {
        input->desired_eye.x, input->desired_eye.y, input->desired_eye.z,
    };
    const double minimum[3] = {
        (double)bounds->minimum.x - input->guard.radius,
        (double)bounds->minimum.y - input->guard.radius,
        (double)bounds->minimum.z - input->guard.radius,
    };
    const double maximum[3] = {
        (double)bounds->maximum.x + input->guard.radius,
        (double)bounds->maximum.y + input->guard.radius,
        (double)bounds->maximum.z + input->guard.radius,
    };
    double first = 0.0;
    double last = 1.0;
    double overlap_depth = INFINITY;
    int first_axis = 0;
    double first_normal = 1.0;
    int overlap_axis = 0;
    double overlap_normal = 1.0;
    int started_inside = 1;
    int axis;

    if (bounds == NULL || input == NULL || out_hit == NULL ||
        input->guard.kind != MDKR_CAMERA_LENS_GUARD_SPHERE ||
        !isfinite(input->guard.radius) || input->guard.radius < 0.0f ||
        !mdkr_camera_dynamic_finite_vec3(input->start_eye) ||
        !mdkr_camera_dynamic_finite_vec3(input->desired_eye) ||
        stable_id == 0U) {
        if (out_hit != NULL) memset(out_hit, 0, sizeof(*out_hit));
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    memset(out_hit, 0, sizeof(*out_hit));
    for (axis = 0; axis < 3; axis++) {
        const double delta = end[axis] - start[axis];
        double enter;
        double leave;
        double enter_normal;
        double low_depth;
        double high_depth;

        if (start[axis] < minimum[axis] || start[axis] > maximum[axis]) {
            started_inside = 0;
        }
        low_depth = start[axis] - minimum[axis];
        high_depth = maximum[axis] - start[axis];
        if (low_depth < overlap_depth) {
            overlap_depth = low_depth;
            overlap_axis = axis;
            overlap_normal = -1.0;
        }
        if (high_depth < overlap_depth) {
            overlap_depth = high_depth;
            overlap_axis = axis;
            overlap_normal = 1.0;
        }
        if (delta == 0.0) {
            if (start[axis] < minimum[axis] || start[axis] > maximum[axis]) {
                return MDKR_CAMERA_SWEEP_CLEAR;
            }
            continue;
        }
        enter = (minimum[axis] - start[axis]) / delta;
        leave = (maximum[axis] - start[axis]) / delta;
        enter_normal = -1.0;
        if (enter > leave) {
            const double temporary = enter;
            enter = leave;
            leave = temporary;
            enter_normal = 1.0;
        }
        if (enter > first) {
            first = enter;
            first_axis = axis;
            first_normal = enter_normal;
        }
        if (leave < last) last = leave;
        if (first > last) return MDKR_CAMERA_SWEEP_CLEAR;
    }
    if (!started_inside && (first < 0.0 || first > 1.0)) {
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    out_hit->fraction = started_inside ? 0.0f : (float)first;
    out_hit->point.x = (float)(start[0] + (end[0] - start[0]) * out_hit->fraction);
    out_hit->point.y = (float)(start[1] + (end[1] - start[1]) * out_hit->fraction);
    out_hit->point.z = (float)(start[2] + (end[2] - start[2]) * out_hit->fraction);
    if (started_inside) {
        if (!isfinite(overlap_depth) || overlap_depth < 0.0 || overlap_depth > FLT_MAX) {
            memset(out_hit, 0, sizeof(*out_hit));
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        out_hit->penetration_depth = (float)overlap_depth;
        out_hit->started_overlapping = 1U;
        if (overlap_axis == 0) out_hit->normal.x = (float)overlap_normal;
        if (overlap_axis == 1) out_hit->normal.y = (float)overlap_normal;
        if (overlap_axis == 2) out_hit->normal.z = (float)overlap_normal;
    } else {
        if (first_axis == 0) out_hit->normal.x = (float)first_normal;
        if (first_axis == 1) out_hit->normal.y = (float)first_normal;
        if (first_axis == 2) out_hit->normal.z = (float)first_normal;
    }
    out_hit->clearance = 0.0f;
    out_hit->feature = MDKR_CAMERA_SWEEP_FEATURE_FACE;
    out_hit->kind = 1U;
    out_hit->stable_id = stable_id;
    return MDKR_CAMERA_SWEEP_HIT;
}

static MdkrCameraDynamicIdentity *mdkr_camera_dynamic_find_identity(const Object *object) {
    int32_t slot;

    /* A vacant slot's object is NULL, so a NULL key would match the first hole
     * rather than miss; identity only exists for a live object. */
    if (object == NULL || sIndexBucketCount == 0U) {
        return NULL;
    }
    for (slot = sIdentityBuckets[mdkr_camera_dynamic_pointer_bucket(object)];
         slot != MDKR_CAMERA_DYNAMIC_INDEX_NONE; slot = sIdentityNext[slot]) {
        if (sIdentities[slot].object == object) {
            return &sIdentities[slot];
        }
    }
    return NULL;
}

static MdkrCameraDynamicModelBounds *mdkr_camera_dynamic_find_model_bounds(
    const ObjectModel *model,
    uint32_t model_generation) {
    int32_t slot;

    if (model == NULL || sIndexBucketCount == 0U) {
        return NULL;
    }
    for (slot = sModelBoundsBuckets[mdkr_camera_dynamic_pointer_bucket(model)];
         slot != MDKR_CAMERA_DYNAMIC_INDEX_NONE; slot = sModelBoundsNext[slot]) {
        if (sModelBounds[slot].model == model &&
            sModelBounds[slot].model_generation == model_generation) {
            return &sModelBounds[slot];
        }
    }
    return NULL;
}

static MdkrCameraDynamicModelBounds *mdkr_camera_dynamic_get_model_bounds(
    const ObjectModel *model,
    uint32_t model_generation,
    const MdkrCameraOcclusionWorld *world) {
    MdkrCameraDynamicModelBounds *entry;
    MdkrCameraDynamicModelBounds *vacant = NULL;
    size_t index;

    entry = mdkr_camera_dynamic_find_model_bounds(model, model_generation);

    if (entry != NULL) {
        return entry;
    }
    for (index = 0U; index < sCapacity; index++) {
        uint32_t live_generation;
        if (sModelBounds[index].model == NULL) {
            vacant = &sModelBounds[index];
            break;
        }
        /* Object-model caches are immutable and their lookup is read-only.
         * Once an old model cache retires, its fixed side slot is reusable. */
        if (mdkr_camera_object_occlusion_world_for_model(sModelBounds[index].model,
                                                         &live_generation) == NULL ||
            live_generation != sModelBounds[index].model_generation) {
            vacant = &sModelBounds[index];
            break;
        }
    }
    if (vacant == NULL || !mdkr_camera_dynamic_aabb_from_world(world, &vacant->local_bounds)) {
        return NULL;
    }
    entry = vacant;
    index = (size_t)(entry - sModelBounds);
    if (entry->model != NULL) {
        mdkr_camera_dynamic_index_unlink(
            sModelBoundsBuckets, sModelBoundsNext, sModelBoundsPrev,
            mdkr_camera_dynamic_pointer_bucket(entry->model), index);
    }
    entry->model = model;
    entry->model_generation = model_generation;
    mdkr_camera_dynamic_index_link(
        sModelBoundsBuckets, sModelBoundsNext, sModelBoundsPrev,
        mdkr_camera_dynamic_pointer_bucket(model), index);
    if (sModelBoundsCount < sCapacity) sModelBoundsCount++;
    sTelemetry.model_bounds_count = sModelBoundsCount;
    return entry;
}

static int mdkr_camera_dynamic_candidate_kind(const Object *object, int *out_is_door) {
    const int door = object->behaviorId == BHV_DOOR || object->behaviorId == BHV_TT_DOOR;
    const int solid = object->interactObj != NULL &&
        (object->interactObj->flags & INTERACT_FLAGS_SOLID) != 0;
    *out_is_door = door;
    return door || solid;
}

static int mdkr_camera_dynamic_input_valid(const MdkrCameraSweepInput *input) {
    return input != NULL && input->guard.kind == MDKR_CAMERA_LENS_GUARD_SPHERE &&
        isfinite(input->guard.radius) && input->guard.radius >= 0.0f &&
        mdkr_camera_dynamic_finite_vec3(input->start_eye) &&
        mdkr_camera_dynamic_finite_vec3(input->desired_eye);
}

static int mdkr_camera_dynamic_rounded_lens_input_valid(
    const MdkrCameraRoundedLensSweepInput *input,
    double *out_outward_radius) {
    const MdkrCameraOcclusionWorld empty_world = { 0 };
    MdkrCameraSweepHit ignored_hit;

    /* Delegate the complete oriented-guard contract to the exact kernel. A
     * dynamic source must reject a corrupt guard even when no object AABB is
     * retained for the current camera path. */
    if (input == NULL || out_outward_radius == NULL ||
        mdkr_camera_rounded_lens_sweep(&empty_world, input, &ignored_hit) !=
            MDKR_CAMERA_SWEEP_CLEAR) {
        return 0;
    }
    /* The shared helper recomputes and rounds outward in double precision.
     * Do not use the serialized broadphase_radius here: its accepted guard
     * tolerance must not become a false AABB cull. */
    return mdkr_camera_rounded_lens_guard_conservative_radius(
        &input->guard, out_outward_radius);
}

int mdkr_camera_dynamic_occlusion_prepare(size_t object_capacity) {
    MdkrCameraDynamicIdentity *identities;
    MdkrCameraDynamicModelBounds *model_bounds;
    MdkrCameraDynamicInstance *instances;
    MdkrCameraDynamicInstance *previous_instances;
    int32_t *link_arrays[5] = { NULL, NULL, NULL, NULL, NULL };
    int32_t *buckets[3] = { NULL, NULL, NULL };
    uint64_t *vacant = NULL;
    size_t bucket_count = 1U;
    size_t vacant_words;
    size_t index;
    size_t index_bytes;

    const size_t bytes_per_slot = sizeof(*identities) + sizeof(*model_bounds) +
        sizeof(*instances) + sizeof(*previous_instances);

    if (object_capacity == 0U || sCapacity != 0U) {
        return sCapacity >= object_capacity && sCapacity != 0U;
    }
    if (object_capacity > SIZE_MAX / bytes_per_slot) {
        return 0;
    }
    while (bucket_count < object_capacity) {
        if (bucket_count > SIZE_MAX / 2U) {
            return 0;
        }
        bucket_count *= 2U;
    }
    vacant_words = (object_capacity + MDKR_CAMERA_DYNAMIC_INDEX_WORD_BITS - 1U) /
        MDKR_CAMERA_DYNAMIC_INDEX_WORD_BITS;
    identities = calloc(object_capacity, sizeof(*identities));
    model_bounds = calloc(object_capacity, sizeof(*model_bounds));
    instances = calloc(object_capacity, sizeof(*instances));
    previous_instances = calloc(object_capacity, sizeof(*previous_instances));
    for (index = 0U; index < 5U; index++) {
        link_arrays[index] = calloc(object_capacity, sizeof(**link_arrays));
    }
    for (index = 0U; index < 3U; index++) {
        buckets[index] = calloc(bucket_count, sizeof(**buckets));
    }
    vacant = calloc(vacant_words, sizeof(*vacant));
    if (identities == NULL || model_bounds == NULL || instances == NULL ||
        previous_instances == NULL || vacant == NULL ||
        link_arrays[0] == NULL || link_arrays[1] == NULL ||
        link_arrays[2] == NULL || link_arrays[3] == NULL ||
        link_arrays[4] == NULL ||
        buckets[0] == NULL || buckets[1] == NULL || buckets[2] == NULL) {
        free(identities);
        free(model_bounds);
        free(instances);
        free(previous_instances);
        free(vacant);
        for (index = 0U; index < 5U; index++) free(link_arrays[index]);
        for (index = 0U; index < 3U; index++) free(buckets[index]);
        return 0;
    }
    sIdentities = identities;
    sModelBounds = model_bounds;
    sInstances = instances;
    sPreviousInstances = previous_instances;
    sIdentityNext = link_arrays[0];
    sIdentityPrev = link_arrays[1];
    sModelBoundsNext = link_arrays[2];
    sModelBoundsPrev = link_arrays[3];
    sPreviousInstanceNext = link_arrays[4];
    sIdentityBuckets = buckets[0];
    sModelBoundsBuckets = buckets[1];
    sPreviousInstanceBuckets = buckets[2];
    sIdentityVacant = vacant;
    sIdentityVacantWords = vacant_words;
    sIndexBucketCount = bucket_count;
    sCapacity = object_capacity;
    mdkr_camera_dynamic_index_reset();
    index_bytes = 5U * object_capacity * sizeof(**link_arrays) +
        3U * bucket_count * sizeof(**buckets) + vacant_words * sizeof(*vacant);
    sTelemetry.capacity = object_capacity;
    sTelemetry.allocation_bytes = object_capacity * bytes_per_slot + index_bytes;
    return 1;
}

void mdkr_camera_dynamic_occlusion_reset(void) {
    sIdentityCount = 0U;
    sModelBoundsCount = 0U;
    sInstanceCount = 0U;
    sPreviousInstanceCount = 0U;
    mdkr_camera_dynamic_publication_reset(&sPublicationState);
    if (sIdentities != NULL) memset(sIdentities, 0, sCapacity * sizeof(*sIdentities));
    if (sModelBounds != NULL) memset(sModelBounds, 0, sCapacity * sizeof(*sModelBounds));
    if (sInstances != NULL) memset(sInstances, 0, sCapacity * sizeof(*sInstances));
    if (sPreviousInstances != NULL) {
        memset(sPreviousInstances, 0, sCapacity * sizeof(*sPreviousInstances));
    }
    mdkr_camera_dynamic_index_reset();
    sTelemetry.published_instance_count = 0U;
    sTelemetry.temporal_moved_instance_count = 0U;
    sTelemetry.identity_count = 0U;
    sTelemetry.model_bounds_count = 0U;
}

void mdkr_camera_dynamic_occlusion_shutdown(void) {
    free(sIdentities);
    free(sModelBounds);
    free(sInstances);
    free(sPreviousInstances);
    free(sIdentityBuckets);
    free(sIdentityNext);
    free(sIdentityPrev);
    free(sIdentityVacant);
    free(sModelBoundsBuckets);
    free(sModelBoundsNext);
    free(sModelBoundsPrev);
    free(sPreviousInstanceBuckets);
    free(sPreviousInstanceNext);
    sIdentities = NULL;
    sModelBounds = NULL;
    sInstances = NULL;
    sPreviousInstances = NULL;
    sIdentityBuckets = NULL;
    sIdentityNext = NULL;
    sIdentityPrev = NULL;
    sIdentityVacant = NULL;
    sModelBoundsBuckets = NULL;
    sModelBoundsNext = NULL;
    sModelBoundsPrev = NULL;
    sPreviousInstanceBuckets = NULL;
    sPreviousInstanceNext = NULL;
    sIdentityVacantWords = 0U;
    sIndexBucketCount = 0U;
    sCapacity = 0U;
    mdkr_camera_dynamic_occlusion_reset();
    sTelemetry.capacity = 0U;
    sTelemetry.allocation_bytes = 0U;
}

void mdkr_camera_dynamic_occlusion_note_spawn(const Object *object) {
    MdkrCameraDynamicIdentity *identity;
    size_t index;
    if (object == NULL || sCapacity == 0U) {
        return;
    }
    identity = mdkr_camera_dynamic_find_identity(object);
    if (identity != NULL) {
        return;
    }
    index = mdkr_camera_dynamic_first_vacant_identity();
    if (index >= sCapacity || sNextSpawnGeneration == 0U ||
        sNextSpawnGeneration == UINT64_MAX || sNextStableInstanceId == 0U) {
        sTelemetry.capacity_failure_count++;
        return;
    }
    identity = &sIdentities[index];
    identity->object = object;
    identity->spawn_generation = sNextSpawnGeneration++;
    identity->stable_instance_id = sNextStableInstanceId;
    sNextStableInstanceId = sNextStableInstanceId == UINT32_MAX
        ? 0U : sNextStableInstanceId + 1U;
    mdkr_camera_dynamic_identity_mark_vacant(index, 0);
    mdkr_camera_dynamic_index_link(
        sIdentityBuckets, sIdentityNext, sIdentityPrev,
        mdkr_camera_dynamic_pointer_bucket(object), index);
    sIdentityCount++;
    sTelemetry.identity_count = sIdentityCount;
}

void mdkr_camera_dynamic_occlusion_note_free(const Object *object) {
    MdkrCameraDynamicIdentity *identity;
    size_t index;

    if (object == NULL || sCapacity == 0U) {
        return;
    }
    identity = mdkr_camera_dynamic_find_identity(object);
    if (identity != NULL) {
        index = (size_t)(identity - sIdentities);
        mdkr_camera_dynamic_index_unlink(
            sIdentityBuckets, sIdentityNext, sIdentityPrev,
            mdkr_camera_dynamic_pointer_bucket(identity->object), index);
        identity->object = NULL;
        identity->spawn_generation = 0U;
        identity->stable_instance_id = 0U;
        mdkr_camera_dynamic_identity_mark_vacant(index, 1);
        if (sIdentityCount > 0U) sIdentityCount--;
        sTelemetry.identity_count = sIdentityCount;
    }
}

void mdkr_camera_dynamic_occlusion_tick(void) {
    Object **objects;
    s32 first;
    s32 count;
    s32 index;
    int publication_degraded = 0;
    const int recovery_discontinuity =
        mdkr_camera_dynamic_publication_begin(&sPublicationState);

    sTelemetry.tick_count++;
    sTelemetry.current_transitioning_door_count = 0U;
    sTelemetry.temporal_moved_instance_count = 0U;
    sPreviousInstanceCount = sInstanceCount;
    if (sPreviousInstances != NULL && sInstances != NULL && sInstanceCount != 0U) {
        memcpy(sPreviousInstances, sInstances,
               sInstanceCount * sizeof(*sPreviousInstances));
    }
    mdkr_camera_dynamic_previous_index_rebuild();
    sInstanceCount = 0U;
    objects = objGetObjList(&first, &count);
    if (objects == NULL || first < 0 || count < first || (size_t)count > sCapacity) {
        sTelemetry.capacity_failure_count++;
        return;
    }
    for (index = first; index < count; index++) {
        Object *object = objects[index];
        ModelInstance *instance;
        const MdkrCameraOcclusionWorld *world;
        MdkrCameraDynamicIdentity *identity;
        MdkrCameraDynamicModelBounds *model_bounds;
        const MdkrCameraDynamicInstance *previous_instance;
        MdkrCameraDynamicInstance *published_instance;
        MdkrCameraObjectTransform transform;
        MtxF visual_transform;
        uint32_t model_generation;
        int is_door;

        if (object == NULL || object->header == NULL) {
            continue;
        }
        if (object->trans.flags & OBJ_FLAGS_PARTICLE) {
            sTelemetry.excluded_particle_count++;
            continue;
        }
        if (object->behaviorId == BHV_RACER) {
            sTelemetry.excluded_racer_count++;
            continue;
        }
        if (object->header->modelType != OBJECT_MODEL_TYPE_3D_MODEL ||
            object->modelInstances == NULL || object->modelIndex < 0 ||
            object->modelIndex >= object->header->numberOfModelIds) {
            sTelemetry.excluded_non_model_count++;
            continue;
        }
        if (!mdkr_camera_dynamic_candidate_kind(object, &is_door)) {
            sTelemetry.excluded_non_solid_count++;
            continue;
        }
        instance = object->modelInstances[object->modelIndex];
        if (instance == NULL || instance->objModel == NULL) {
            sTelemetry.missing_cache_count++;
            publication_degraded = 1;
            continue;
        }
        world = mdkr_camera_object_occlusion_world_for_model(instance->objModel, &model_generation);
        if (world == NULL || world->triangle_count == 0U || model_generation == 0U) {
            sTelemetry.missing_cache_count++;
            publication_degraded = 1;
            continue;
        }
        identity = mdkr_camera_dynamic_find_identity(object);
        if (identity == NULL || identity->spawn_generation == 0U) {
            sTelemetry.missing_identity_count++;
            publication_degraded = 1;
            continue;
        }
        /* Use the renderer's exact fixed-angle/table transform, including its
         * Z->X->Y order. A libm reconstruction is visually close but can put a
         * large door's camera shell on the opposite side of a marginal lens. */
        mtxf_from_transform(&visual_transform, &object->trans);
        transform.translation = (MdkrCameraVec3) {
            visual_transform[3][0], visual_transform[3][1], visual_transform[3][2],
        };
        transform.local_x_axis = (MdkrCameraVec3) {
            visual_transform[0][0], visual_transform[0][1], visual_transform[0][2],
        };
        transform.local_y_axis = (MdkrCameraVec3) {
            visual_transform[1][0], visual_transform[1][1], visual_transform[1][2],
        };
        transform.local_z_axis = (MdkrCameraVec3) {
            visual_transform[2][0], visual_transform[2][1], visual_transform[2][2],
        };
        {
            float validated_scale;
            if (!mdkr_camera_object_transform_validate(&transform, &validated_scale)) {
                sTelemetry.invalid_transform_count++;
                publication_degraded = 1;
                continue;
            }
        }
        model_bounds = mdkr_camera_dynamic_get_model_bounds(instance->objModel, model_generation, world);
        if (model_bounds == NULL || sInstanceCount >= sCapacity ||
            !mdkr_camera_dynamic_world_aabb(&model_bounds->local_bounds, &transform,
                                            &sInstances[sInstanceCount].world_bounds)) {
            sTelemetry.capacity_failure_count++;
            return;
        }
        published_instance = &sInstances[sInstanceCount];
        published_instance->world = world;
        published_instance->model = instance->objModel;
        published_instance->authored_transform = object->trans;
        published_instance->transform = transform;
        published_instance->object_spawn_generation = identity->spawn_generation;
        published_instance->model_generation = model_generation;
        published_instance->stable_instance_id = identity->stable_instance_id;
        published_instance->authoritative_list_index = (uint32_t)index;
        published_instance->temporal_bounds = published_instance->world_bounds;
        published_instance->temporal_moved = FALSE;
        published_instance->temporal_proxy = FALSE;
        previous_instance = mdkr_camera_dynamic_find_previous_instance(
            identity->spawn_generation);
        if (previous_instance != NULL &&
            previous_instance->model_generation == model_generation &&
            previous_instance->model == instance->objModel &&
            !mdkr_camera_dynamic_transform_equal(
                &previous_instance->transform, &transform)) {
            if (!mdkr_camera_dynamic_temporal_bounds(
                    &model_bounds->local_bounds,
                    &previous_instance->authored_transform,
                    &published_instance->authored_transform,
                    &published_instance->temporal_bounds)) {
                sTelemetry.invalid_transform_count++;
                publication_degraded = 1;
                continue;
            }
            published_instance->temporal_moved = TRUE;
            published_instance->temporal_proxy = (uint8_t)is_door;
            sTelemetry.temporal_moved_instance_count++;
        }
        /* A failed census left no authoritative previous transform envelope.
         * Cut presentation for every hard object on the first recovered tick;
         * interpolating across the missing publication would otherwise create
         * geometry that neither valid immutable snapshot represented. */
        if (recovery_discontinuity && !published_instance->temporal_moved) {
            published_instance->temporal_moved = TRUE;
            published_instance->temporal_proxy = FALSE;
            sTelemetry.temporal_moved_instance_count++;
        } else if (recovery_discontinuity) {
            published_instance->temporal_proxy = FALSE;
        }
        sInstanceCount++;
        if (is_door) {
            sTelemetry.hard_door_instance_count++;
            if (object->door != NULL && object->door->openDir != DOOR_CLOSED) {
                sTelemetry.transitioning_door_instance_count++;
                sTelemetry.current_transitioning_door_count++;
            }
        } else {
            sTelemetry.hard_solid_instance_count++;
        }
    }
    /* Omitting one hard candidate is not a valid empty result. Publish the
     * usable instances for telemetry, but make every query report INVALID so
     * the runtime's static-only fallback is explicitly release-gated. */
    mdkr_camera_dynamic_publication_finish(
        &sPublicationState, !publication_degraded);
    sTelemetry.published_instance_count = sInstanceCount;
    if (sInstanceCount > sTelemetry.peak_instance_count) sTelemetry.peak_instance_count = sInstanceCount;
}

static int mdkr_camera_dynamic_hit_precedes(
    const MdkrCameraDynamicOcclusionHit *candidate,
    const MdkrCameraDynamicOcclusionHit *best) {
    /* Match the exact kernels and source composer: fraction differences within
     * the public tolerance are a deterministic identity tie, not incidental
     * floating-point order. Preserve dynamic provenance ordering inside it. */
    if ((double)candidate->hit.fraction < (double)best->hit.fraction -
            MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON) {
        return 1;
    }
    if (fabs((double)candidate->hit.fraction - (double)best->hit.fraction) >
        MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON) {
        return 0;
    }
    if (candidate->object_spawn_generation != best->object_spawn_generation) {
        return candidate->object_spawn_generation < best->object_spawn_generation;
    }
    if (candidate->model_generation != best->model_generation) {
        return candidate->model_generation < best->model_generation;
    }
    if (candidate->source_triangle_stable_id != best->source_triangle_stable_id) {
        return candidate->source_triangle_stable_id < best->source_triangle_stable_id;
    }
    if (candidate->hit.stable_id != best->hit.stable_id) {
        return candidate->hit.stable_id < best->hit.stable_id;
    }
    return candidate->authoritative_list_index < best->authoritative_list_index;
}

static void mdkr_camera_dynamic_record_sphere_sweep(
    size_t instance_candidates,
    size_t nodes_visited,
    size_t chunks_retained,
    size_t chunk_triangles,
    int invalid) {
    if (instance_candidates > sTelemetry.sphere_max_instances_per_sweep) {
        sTelemetry.sphere_max_instances_per_sweep = instance_candidates;
    }
    if (nodes_visited > sTelemetry.sphere_max_nodes_visited_per_sweep) {
        sTelemetry.sphere_max_nodes_visited_per_sweep = nodes_visited;
    }
    if (chunks_retained > sTelemetry.sphere_max_chunks_retained_per_sweep) {
        sTelemetry.sphere_max_chunks_retained_per_sweep = chunks_retained;
    }
    if (chunk_triangles > sTelemetry.sphere_max_chunk_triangles_per_sweep) {
        sTelemetry.sphere_max_chunk_triangles_per_sweep = chunk_triangles;
    }
    if (invalid) sTelemetry.sphere_invalid_sweep_count++;
}

MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_sweep_detailed(
    const MdkrCameraSweepInput *input,
    MdkrCameraDynamicOcclusionHit *out_hit) {
    MdkrCameraDynamicOcclusionHit best;
    MdkrCameraSweepInput local_input;
    size_t index;
    size_t instance_candidates = 0U;
    size_t nodes_visited = 0U;
    size_t chunks_retained = 0U;
    size_t chunk_triangles = 0U;
    int found = 0;

    sTelemetry.sweep_count++;
    sTelemetry.sphere_sweep_count++;
    if (out_hit == NULL) {
        sTelemetry.invalid_sweep_count++;
        mdkr_camera_dynamic_record_sphere_sweep(0U, 0U, 0U, 0U, 1);
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    memset(out_hit, 0, sizeof(*out_hit));
    if (!mdkr_camera_dynamic_input_valid(input) ||
        !mdkr_camera_dynamic_publication_current_valid(&sPublicationState)) {
        sTelemetry.invalid_sweep_count++;
        mdkr_camera_dynamic_record_sphere_sweep(0U, 0U, 0U, 0U, 1);
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    local_input = *input;
    /* The immutable model cache tags triangles by model generation. Dynamic
     * self-ignore is instance generation, so skip the instance here and keep
     * other instances of the same model eligible. */
    local_input.ignored_object_generation = 0U;
    for (index = 0U; index < sInstanceCount; index++) {
        const MdkrCameraDynamicInstance *instance = &sInstances[index];
        uint32_t live_model_generation;
        MdkrCameraDynamicOcclusionHit candidate;
        MdkrCameraObjectOcclusionExactLimits query_limits;
        MdkrCameraObjectOcclusionExactWork query_work;
        MdkrCameraSweepStatus status;

        if (input->ignored_object_generation != 0U &&
            instance->object_spawn_generation == input->ignored_object_generation) {
            continue;
        }
        if (mdkr_camera_object_occlusion_world_for_model(
                instance->model, &live_model_generation) != instance->world ||
            live_model_generation != instance->model_generation) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_sphere_sweep(
                instance_candidates, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (instance->temporal_proxy &&
            !(input->mask &
              MDKR_CAMERA_DYNAMIC_OCCLUSION_QUERY_CURRENT_POSE)) {
            status = mdkr_camera_dynamic_sphere_aabb_sweep(
                &instance->temporal_bounds, input,
                instance->stable_instance_id, &candidate.hit);
            if (status == MDKR_CAMERA_SWEEP_INVALID ||
                (status == MDKR_CAMERA_SWEEP_HIT &&
                 instance_candidates >=
                     MDKR_CAMERA_DYNAMIC_OCCLUSION_MAX_QUERY_INSTANCES)) {
                sTelemetry.invalid_sweep_count++;
                mdkr_camera_dynamic_record_sphere_sweep(
                    instance_candidates, nodes_visited, chunks_retained,
                    chunk_triangles, 1);
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            if (status == MDKR_CAMERA_SWEEP_HIT) {
                instance_candidates++;
                sTelemetry.broadphase_candidate_count++;
                sTelemetry.sphere_broadphase_instance_count++;
                sTelemetry.temporal_proxy_hit_count++;
                candidate.object_spawn_generation =
                    instance->object_spawn_generation;
                candidate.model_generation = instance->model_generation;
                candidate.source_triangle_stable_id = 0U;
                candidate.authoritative_list_index =
                    instance->authoritative_list_index;
                if (!found || mdkr_camera_dynamic_hit_precedes(&candidate, &best)) {
                    best = candidate;
                    found = 1;
                }
            }
            continue;
        }
        if (!mdkr_camera_dynamic_swept_aabb_intersects(
                &instance->world_bounds, input->start_eye, input->desired_eye,
                input->guard.radius)) {
            continue;
        }
        if (instance_candidates >= MDKR_CAMERA_DYNAMIC_OCCLUSION_MAX_QUERY_INSTANCES ||
            nodes_visited >= MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES ||
            chunks_retained >= MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS ||
            chunk_triangles >= MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_sphere_sweep(
                instance_candidates, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        instance_candidates++;
        sTelemetry.sphere_broadphase_instance_count++;
        query_limits = (MdkrCameraObjectOcclusionExactLimits) {
            MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES - nodes_visited,
            MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS - chunks_retained,
            MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES - chunk_triangles,
            1U,
        };
        sTelemetry.broadphase_candidate_count++;
        status = mdkr_camera_object_occlusion_sweep_model_profiled(
            instance->model, instance->model_generation, &instance->transform,
            &local_input, &candidate.hit, &query_limits, &query_work);
        if (query_work.nodes_visited > SIZE_MAX - nodes_visited ||
            query_work.chunks_retained > SIZE_MAX - chunks_retained ||
            query_work.triangles_retained > SIZE_MAX - chunk_triangles) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_sphere_sweep(
                instance_candidates, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (query_work.nodes_visited > SIZE_MAX - sTelemetry.sphere_node_visited_count ||
            query_work.nodes_rejected > SIZE_MAX - sTelemetry.sphere_node_rejected_count ||
            query_work.chunks_retained > SIZE_MAX - sTelemetry.sphere_chunk_retained_count ||
            query_work.triangles_retained > SIZE_MAX -
                sTelemetry.sphere_chunk_triangle_count) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_sphere_sweep(
                instance_candidates, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        nodes_visited += query_work.nodes_visited;
        chunks_retained += query_work.chunks_retained;
        chunk_triangles += query_work.triangles_retained;
        sTelemetry.sphere_node_visited_count += query_work.nodes_visited;
        sTelemetry.sphere_node_rejected_count += query_work.nodes_rejected;
        sTelemetry.sphere_chunk_retained_count += query_work.chunks_retained;
        sTelemetry.sphere_chunk_triangle_count += query_work.triangles_retained;
        sTelemetry.narrowphase_candidate_count++;
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            if (query_work.exhausted) {
                /* Work fences are healthy bounded operation, not source
                 * corruption. Preserve fail-closed safety with the already
                 * published world AABB; the later exact lens phase can still
                 * reject this conservative broadphase hit. */
                status = mdkr_camera_dynamic_sphere_aabb_sweep(
                    &instance->world_bounds, input,
                    instance->stable_instance_id, &candidate.hit);
                if (status != MDKR_CAMERA_SWEEP_INVALID) {
                    sTelemetry.sphere_conservative_fallback_count++;
                    if (status == MDKR_CAMERA_SWEEP_CLEAR) {
                        continue;
                    }
                    candidate.object_spawn_generation =
                        instance->object_spawn_generation;
                    candidate.model_generation = instance->model_generation;
                    candidate.source_triangle_stable_id = 0U;
                    candidate.authoritative_list_index =
                        instance->authoritative_list_index;
                    if (!found ||
                        mdkr_camera_dynamic_hit_precedes(&candidate, &best)) {
                        best = candidate;
                        found = 1;
                    }
                    continue;
                }
            }
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_sphere_sweep(
                instance_candidates, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (status != MDKR_CAMERA_SWEEP_HIT) {
            continue;
        }
        candidate.object_spawn_generation = instance->object_spawn_generation;
        candidate.model_generation = instance->model_generation;
        candidate.source_triangle_stable_id = candidate.hit.stable_id;
        candidate.hit.stable_id = instance->stable_instance_id;
        candidate.authoritative_list_index = instance->authoritative_list_index;
        if (!found || mdkr_camera_dynamic_hit_precedes(&candidate, &best)) {
            best = candidate;
            found = 1;
        }
    }
    if (!found) {
        mdkr_camera_dynamic_record_sphere_sweep(
            instance_candidates, nodes_visited, chunks_retained,
            chunk_triangles, 0);
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    *out_hit = best;
    sTelemetry.sphere_hit_count++;
    mdkr_camera_dynamic_record_sphere_sweep(
        instance_candidates, nodes_visited, chunks_retained,
        chunk_triangles, 0);
    return MDKR_CAMERA_SWEEP_HIT;
}

MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_sweep(
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraDynamicOcclusionHit detailed_hit;
    MdkrCameraSweepStatus status =
        mdkr_camera_dynamic_occlusion_sweep_detailed(input, &detailed_hit);
    if (out_hit != NULL) {
        memset(out_hit, 0, sizeof(*out_hit));
    }
    if (status == MDKR_CAMERA_SWEEP_HIT && out_hit != NULL) {
        *out_hit = detailed_hit.hit;
    }
    return status;
}

static void mdkr_camera_dynamic_record_exact_sweep(
    size_t instance_candidates,
    size_t model_triangles,
    size_t narrowed_triangles,
    size_t stationary_tests,
    size_t nodes_visited,
    size_t chunks_retained,
    size_t chunk_triangles,
    int invalid) {
    if (instance_candidates > sTelemetry.exact_max_instances_per_sweep) {
        sTelemetry.exact_max_instances_per_sweep = instance_candidates;
    }
    if (model_triangles > sTelemetry.exact_max_model_triangles_per_sweep) {
        sTelemetry.exact_max_model_triangles_per_sweep = model_triangles;
    }
    if (narrowed_triangles > sTelemetry.exact_max_narrowed_triangles_per_sweep) {
        sTelemetry.exact_max_narrowed_triangles_per_sweep = narrowed_triangles;
    }
    if (stationary_tests > sTelemetry.exact_max_stationary_tests_per_sweep) {
        sTelemetry.exact_max_stationary_tests_per_sweep = stationary_tests;
    }
    if (nodes_visited > sTelemetry.exact_max_nodes_visited_per_sweep) {
        sTelemetry.exact_max_nodes_visited_per_sweep = nodes_visited;
    }
    if (chunks_retained > sTelemetry.exact_max_chunks_retained_per_sweep) {
        sTelemetry.exact_max_chunks_retained_per_sweep = chunks_retained;
    }
    if (chunk_triangles > sTelemetry.exact_max_chunk_triangles_per_sweep) {
        sTelemetry.exact_max_chunk_triangles_per_sweep = chunk_triangles;
    }
    if (invalid) {
        sTelemetry.exact_invalid_sweep_count++;
    }
}

/* The rounded lens is contained in the sphere of the guard's recomputed
 * outward radius swept along the same corridor, so this proxy answers every
 * conservative question the exact kernel would have answered with a superset.
 * The float radius is rounded outward so the promotion never culls. */
static MdkrCameraSweepInput mdkr_camera_dynamic_rounded_lens_proxy_input(
    const MdkrCameraRoundedLensSweepInput *input,
    double outward_radius) {
    float proxy_radius = (float)outward_radius;

    if ((double)proxy_radius < outward_radius) {
        proxy_radius = nextafterf(proxy_radius, INFINITY);
    }
    return (MdkrCameraSweepInput) {
        .guard = { MDKR_CAMERA_LENS_GUARD_SPHERE, proxy_radius },
        .start_eye = input->start_eye,
        .desired_eye = input->desired_eye,
        .mask = input->mask,
        .ignored_object_generation = input->ignored_object_generation,
    };
}

/*
 * Did the exact model kernel stop on one of the work fences this sweep handed
 * it, rather than on a corrupt immutable index?
 *
 * Both kernels answer that question directly through
 * MdkrCameraObjectOcclusionExactWork::exhausted, which every fence exit now
 * sets. The derived comparison below is retained as a subordinate fallback:
 * the published limits are compared against the work reported back, covering
 * a producer that predates the flag. It cannot recognise the node-stack
 * fence (stack depth is frame-local and never reported), which is exactly
 * why the authoritative flag is honoured first.
 *
 * Reaching a fence is healthy bounded operation. Corruption is not, and must
 * keep failing closed, so the comparison is deliberately made against the
 * caller's own fences and nothing else.
 */
static int mdkr_camera_dynamic_exact_work_fenced(
    const MdkrCameraObjectOcclusionExactLimits *limits,
    const MdkrCameraObjectOcclusionExactWork *work) {
    if (limits == NULL || work == NULL) {
        return 0;
    }
    if (work->exhausted) {
        return 1;
    }
    return work->nodes_visited >= limits->nodes ||
        work->chunks_retained >= limits->chunks ||
        work->triangles_retained +
            MDKR_CAMERA_OBJECT_OCCLUSION_CHUNK_TRIANGLES > limits->triangles ||
        work->kernel.stationary_tests >= limits->stationary_tests;
}

MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_rounded_lens_sweep_detailed(
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraDynamicOcclusionHit *out_hit) {
    MdkrCameraDynamicOcclusionHit best;
    MdkrCameraRoundedLensSweepInput local_input;
    double outward_radius;
    size_t index;
    size_t instance_candidates = 0U;
    size_t model_triangles = 0U;
    size_t narrowed_triangles = 0U;
    size_t stationary_tests = 0U;
    size_t nodes_visited = 0U;
    size_t chunks_retained = 0U;
    size_t chunk_triangles = 0U;
    int found = 0;

    sTelemetry.sweep_count++;
    sTelemetry.exact_sweep_count++;
    if (out_hit == NULL) {
        sTelemetry.invalid_sweep_count++;
        mdkr_camera_dynamic_record_exact_sweep(0U, 0U, 0U, 0U, 0U, 0U, 0U, 1);
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    memset(out_hit, 0, sizeof(*out_hit));
    if (!mdkr_camera_dynamic_rounded_lens_input_valid(input, &outward_radius) ||
        !mdkr_camera_dynamic_publication_current_valid(&sPublicationState)) {
        sTelemetry.invalid_sweep_count++;
        mdkr_camera_dynamic_record_exact_sweep(0U, 0U, 0U, 0U, 0U, 0U, 0U, 1);
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    local_input = *input;
    /* Model triangles use model generations. Object self-ignore must remain
     * instance-specific, so remove it for the local model kernel after this
     * source skips the published matching instance. */
    local_input.ignored_object_generation = 0U;
    for (index = 0U; index < sInstanceCount; index++) {
        const MdkrCameraDynamicInstance *instance = &sInstances[index];
        uint32_t live_model_generation;
        MdkrCameraDynamicOcclusionHit candidate;
        MdkrCameraObjectOcclusionExactLimits exact_limits;
        MdkrCameraObjectOcclusionExactWork exact_work;
        MdkrCameraSweepStatus status;
        int conservative = 0;

        if (input->ignored_object_generation != 0U &&
            instance->object_spawn_generation == input->ignored_object_generation) {
            continue;
        }
        if (mdkr_camera_object_occlusion_world_for_model(
                instance->model, &live_model_generation) != instance->world ||
            live_model_generation != instance->model_generation) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_exact_sweep(
                instance_candidates, model_triangles, narrowed_triangles,
                stationary_tests, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (instance->temporal_proxy &&
            !(input->mask &
              MDKR_CAMERA_DYNAMIC_OCCLUSION_QUERY_CURRENT_POSE)) {
            const MdkrCameraSweepInput proxy_input =
                mdkr_camera_dynamic_rounded_lens_proxy_input(
                    input, outward_radius);

            status = mdkr_camera_dynamic_sphere_aabb_sweep(
                &instance->temporal_bounds, &proxy_input,
                instance->stable_instance_id, &candidate.hit);
            if (status == MDKR_CAMERA_SWEEP_INVALID ||
                (status == MDKR_CAMERA_SWEEP_HIT &&
                 instance_candidates >=
                     MDKR_CAMERA_DYNAMIC_OCCLUSION_MAX_QUERY_INSTANCES)) {
                sTelemetry.invalid_sweep_count++;
                mdkr_camera_dynamic_record_exact_sweep(
                    instance_candidates, model_triangles, narrowed_triangles,
                    stationary_tests, nodes_visited, chunks_retained,
                    chunk_triangles, 1);
                return MDKR_CAMERA_SWEEP_INVALID;
            }
            if (status == MDKR_CAMERA_SWEEP_HIT) {
                instance_candidates++;
                sTelemetry.broadphase_candidate_count++;
                sTelemetry.exact_broadphase_instance_count++;
                sTelemetry.temporal_proxy_hit_count++;
                candidate.object_spawn_generation =
                    instance->object_spawn_generation;
                candidate.model_generation = instance->model_generation;
                candidate.source_triangle_stable_id = 0U;
                candidate.authoritative_list_index =
                    instance->authoritative_list_index;
                if (!found || mdkr_camera_dynamic_hit_precedes(&candidate, &best)) {
                    best = candidate;
                    found = 1;
                }
            }
            continue;
        }
        /* The rounded lens' enclosing sphere retains every possible exact
         * blocker. Do not take a sphere sweep winner: every retained object is
         * narrowed by the object-local oriented-lens kernel below. */
        if (!mdkr_camera_dynamic_swept_aabb_intersects(
                &instance->world_bounds, input->start_eye, input->desired_eye,
                outward_radius)) {
            continue;
        }
        if (instance_candidates >= MDKR_CAMERA_DYNAMIC_OCCLUSION_MAX_QUERY_INSTANCES) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_exact_sweep(
                instance_candidates, model_triangles, narrowed_triangles,
                stationary_tests, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        sTelemetry.broadphase_candidate_count++;
        sTelemetry.exact_broadphase_instance_count++;
        instance_candidates++;
        if (instance->world->triangle_count > SIZE_MAX - model_triangles ||
            instance->world->triangle_count >
                SIZE_MAX - sTelemetry.exact_model_triangle_count) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_exact_sweep(
                instance_candidates, model_triangles, narrowed_triangles,
                stationary_tests, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        model_triangles += instance->world->triangle_count;
        sTelemetry.exact_model_triangle_count += instance->world->triangle_count;
        if (instance->world->triangle_count >
            sTelemetry.exact_max_single_model_triangles) {
            sTelemetry.exact_max_single_model_triangles =
                instance->world->triangle_count;
        }
        if (nodes_visited >= MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES ||
            chunks_retained >= MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS ||
            chunk_triangles >= MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES ||
            stationary_tests >= MDKR_CAMERA_OBJECT_OCCLUSION_MAX_STATIONARY_TESTS) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_exact_sweep(
                instance_candidates, model_triangles, narrowed_triangles,
                stationary_tests, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        exact_limits = (MdkrCameraObjectOcclusionExactLimits) {
            MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_NODES - nodes_visited,
            MDKR_CAMERA_OBJECT_OCCLUSION_MAX_RETAINED_CHUNKS - chunks_retained,
            MDKR_CAMERA_OBJECT_OCCLUSION_MAX_QUERY_TRIANGLES - chunk_triangles,
            MDKR_CAMERA_OBJECT_OCCLUSION_MAX_STATIONARY_TESTS - stationary_tests,
        };
        status = mdkr_camera_object_occlusion_rounded_lens_sweep_model_profiled(
            instance->model, instance->model_generation, &instance->transform,
            &local_input, &candidate.hit,
            &exact_limits,
            &exact_work);
        if (exact_work.kernel.triangles_aabb_rejected > SIZE_MAX -
                sTelemetry.exact_triangle_aabb_rejected_count ||
            exact_work.kernel.triangles_narrowed > SIZE_MAX -
                sTelemetry.exact_triangle_narrowed_count ||
            exact_work.kernel.stationary_tests > SIZE_MAX -
                sTelemetry.exact_stationary_test_count ||
            exact_work.nodes_visited > SIZE_MAX - sTelemetry.exact_node_visited_count ||
            exact_work.nodes_rejected > SIZE_MAX - sTelemetry.exact_node_rejected_count ||
            exact_work.chunks_retained > SIZE_MAX - sTelemetry.exact_chunk_retained_count ||
            exact_work.triangles_retained > SIZE_MAX - sTelemetry.exact_chunk_triangle_count ||
            exact_work.kernel.triangles_narrowed > SIZE_MAX - narrowed_triangles ||
            exact_work.kernel.stationary_tests > SIZE_MAX - stationary_tests ||
            exact_work.nodes_visited > SIZE_MAX - nodes_visited ||
            exact_work.chunks_retained > SIZE_MAX - chunks_retained ||
            exact_work.triangles_retained > SIZE_MAX - chunk_triangles) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_exact_sweep(
                instance_candidates, model_triangles, narrowed_triangles,
                stationary_tests, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        sTelemetry.exact_triangle_aabb_rejected_count +=
            (size_t)exact_work.kernel.triangles_aabb_rejected;
        sTelemetry.exact_triangle_narrowed_count +=
            (size_t)exact_work.kernel.triangles_narrowed;
        sTelemetry.exact_stationary_test_count +=
            (size_t)exact_work.kernel.stationary_tests;
        sTelemetry.exact_node_visited_count += exact_work.nodes_visited;
        sTelemetry.exact_node_rejected_count += exact_work.nodes_rejected;
        sTelemetry.exact_chunk_retained_count += exact_work.chunks_retained;
        sTelemetry.exact_chunk_triangle_count += exact_work.triangles_retained;
        narrowed_triangles += (size_t)exact_work.kernel.triangles_narrowed;
        stationary_tests += (size_t)exact_work.kernel.stationary_tests;
        nodes_visited += exact_work.nodes_visited;
        chunks_retained += exact_work.chunks_retained;
        chunk_triangles += exact_work.triangles_retained;
        sTelemetry.narrowphase_candidate_count++;
        if (status == MDKR_CAMERA_SWEEP_INVALID &&
            mdkr_camera_dynamic_exact_work_fenced(&exact_limits, &exact_work)) {
            /* Same recovery the sphere path takes when its kernel exhausts:
             * work fences are healthy bounded operation, not source
             * corruption. Preserve fail-closed safety with the already
             * published world AABB under the lens' enclosing sphere. INVALID
             * here would instead reach the resolver as no information at all,
             * which is the unsafe direction: the two-phase caller would have
             * to fall back to the sphere corridor and publish a degraded,
             * penetrated pose. */
            const MdkrCameraSweepInput fence_input =
                mdkr_camera_dynamic_rounded_lens_proxy_input(
                    input, outward_radius);

            status = mdkr_camera_dynamic_sphere_aabb_sweep(
                &instance->world_bounds, &fence_input,
                instance->stable_instance_id, &candidate.hit);
            if (status != MDKR_CAMERA_SWEEP_INVALID) {
                sTelemetry.exact_conservative_fallback_count++;
                conservative = 1;
            }
        }
        if (status == MDKR_CAMERA_SWEEP_INVALID) {
            sTelemetry.invalid_sweep_count++;
            mdkr_camera_dynamic_record_exact_sweep(
                instance_candidates, model_triangles, narrowed_triangles,
                stationary_tests, nodes_visited, chunks_retained,
                chunk_triangles, 1);
            return MDKR_CAMERA_SWEEP_INVALID;
        }
        if (status != MDKR_CAMERA_SWEEP_HIT) {
            continue;
        }
        candidate.object_spawn_generation = instance->object_spawn_generation;
        candidate.model_generation = instance->model_generation;
        if (conservative) {
            /* No exact triangle backs an enclosing-AABB recovery, and the
             * proxy sweep already stamped the dynamic instance ID. */
            candidate.source_triangle_stable_id = 0U;
        } else {
            candidate.source_triangle_stable_id = candidate.hit.stable_id;
            candidate.hit.stable_id = instance->stable_instance_id;
        }
        candidate.authoritative_list_index = instance->authoritative_list_index;
        if (!found || mdkr_camera_dynamic_hit_precedes(&candidate, &best)) {
            best = candidate;
            found = 1;
        }
    }
    if (!found) {
        mdkr_camera_dynamic_record_exact_sweep(
            instance_candidates, model_triangles, narrowed_triangles,
            stationary_tests, nodes_visited, chunks_retained,
            chunk_triangles, 0);
        return MDKR_CAMERA_SWEEP_CLEAR;
    }
    *out_hit = best;
    sTelemetry.exact_hit_count++;
    mdkr_camera_dynamic_record_exact_sweep(
        instance_candidates, model_triangles, narrowed_triangles,
        stationary_tests, nodes_visited, chunks_retained,
        chunk_triangles, 0);
    return MDKR_CAMERA_SWEEP_HIT;
}

MdkrCameraSweepStatus mdkr_camera_dynamic_occlusion_rounded_lens_sweep(
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraDynamicOcclusionHit detailed_hit;
    MdkrCameraSweepStatus status =
        mdkr_camera_dynamic_occlusion_rounded_lens_sweep_detailed(input, &detailed_hit);

    if (out_hit != NULL) {
        memset(out_hit, 0, sizeof(*out_hit));
    }
    if (status == MDKR_CAMERA_SWEEP_HIT && out_hit != NULL) {
        *out_hit = detailed_hit.hit;
    }
    return status;
}

void mdkr_camera_dynamic_occlusion_get_telemetry(
    MdkrCameraDynamicOcclusionTelemetry *out_telemetry) {
    if (out_telemetry != NULL) {
        *out_telemetry = sTelemetry;
    }
}

int mdkr_camera_dynamic_occlusion_instance_footprint(
    uint32_t stable_instance_id,
    MdkrCameraDynamicOcclusionFootprint *out_footprint) {
    size_t index;

    if (out_footprint == NULL) {
        return 0;
    }
    memset(out_footprint, 0, sizeof(*out_footprint));
    if (stable_instance_id == 0U ||
        !mdkr_camera_dynamic_publication_current_valid(&sPublicationState)) {
        return 0;
    }
    for (index = 0U; index < sInstanceCount; index++) {
        const MdkrCameraDynamicInstance *instance = &sInstances[index];
        double extent;
        double half_x;
        double half_y;
        double half_z;

        if (instance->stable_instance_id != stable_instance_id) {
            continue;
        }
        /*
         * The published world AABB of the instance this tick. This is the only
         * per-blocker extent the resolver result can honestly reach: a hit
         * carries a contact point and a triangle/instance ID, not a size. A
         * moving instance publishes a temporal proxy whose bounds cover the
         * whole interval, so read the pose bounds and refuse the proxy rather
         * than call a swept interval a small prop.
         */
        if (instance->temporal_proxy) {
            return 0;
        }
        half_x = ((double)instance->world_bounds.maximum.x -
                  (double)instance->world_bounds.minimum.x) * 0.5;
        half_y = ((double)instance->world_bounds.maximum.y -
                  (double)instance->world_bounds.minimum.y) * 0.5;
        half_z = ((double)instance->world_bounds.maximum.z -
                  (double)instance->world_bounds.minimum.z) * 0.5;
        if (!isfinite(half_x) || !isfinite(half_y) || !isfinite(half_z) ||
            half_x < 0.0 || half_y < 0.0 || half_z < 0.0) {
            return 0;
        }
        extent = half_x > half_y ? half_x : half_y;
        if (half_z > extent) {
            extent = half_z;
        }
        if (!isfinite(extent) || extent > (double)FLT_MAX) {
            return 0;
        }
        if (instance->object_spawn_generation == 0U ||
            instance->object_spawn_generation > UINT32_MAX) {
            return 0;
        }
        out_footprint->object_generation =
            (uint32_t)instance->object_spawn_generation;
        out_footprint->max_half_extent = (float)extent;
        out_footprint->center.x = (float)(
            ((double)instance->world_bounds.maximum.x +
             (double)instance->world_bounds.minimum.x) * 0.5);
        out_footprint->center.y = (float)(
            ((double)instance->world_bounds.maximum.y +
             (double)instance->world_bounds.minimum.y) * 0.5);
        out_footprint->center.z = (float)(
            ((double)instance->world_bounds.maximum.z +
             (double)instance->world_bounds.minimum.z) * 0.5);
        return 1;
    }
    return 0;
}

int mdkr_camera_dynamic_occlusion_object_discontinuous(const Object *object) {
    const MdkrCameraDynamicIdentity *identity;
    size_t index;

    if (object == NULL) {
        return 0;
    }
    if (mdkr_camera_dynamic_publication_requires_global_cut(
            &sPublicationState)) {
        int is_door;
        /* Failed publication invalidates every hard object's interpolation
         * source even though there is deliberately no queryable snapshot. */
        return mdkr_camera_dynamic_candidate_kind(object, &is_door);
    }
    identity = mdkr_camera_dynamic_find_identity(object);
    if (identity == NULL || identity->spawn_generation == 0U) {
        return 0;
    }
    for (index = 0U; index < sInstanceCount; index++) {
        const MdkrCameraDynamicInstance *instance = &sInstances[index];
        if (instance->object_spawn_generation == identity->spawn_generation) {
            return instance->temporal_moved && !instance->temporal_proxy;
        }
    }
    return 0;
}

int mdkr_camera_dynamic_occlusion_discontinuous_sweep_candidate(
    const MdkrCameraSweepInput *input) {
    size_t index;

    if (!mdkr_camera_dynamic_input_valid(input) ||
        !mdkr_camera_dynamic_publication_current_valid(&sPublicationState)) {
        return 1;
    }
    for (index = 0U; index < sInstanceCount; index++) {
        const MdkrCameraDynamicInstance *instance = &sInstances[index];
        if (instance->temporal_moved && !instance->temporal_proxy &&
            mdkr_camera_dynamic_swept_aabb_intersects(
                &instance->world_bounds, input->start_eye,
                input->desired_eye, input->guard.radius)) {
            return 1;
        }
    }
    return 0;
}

#endif /* NATIVE_PORT */
