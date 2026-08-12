#ifndef MDKR_ROLLBACK_AUTHORITY_VIEW_H
#define MDKR_ROLLBACK_AUTHORITY_VIEW_H

#include "types.h"

#ifdef NATIVE_PORT
#include <stddef.h>

/* Stable main-pool allocations whose contents participate in object/race
 * simulation. Pointer values are in-process references, never serialized. */
typedef struct MdkrObjectRollbackView {
    const void *object_list;
    const void *particle_list;
    const void *collision_objects;
    const void *animation_objects;
    const void *track_checkpoints;
    const void *camera_objects;
    const void *racers;
    const void *racers_by_position;
    const void *racers_by_port;
    const void *ai_nodes;
    const void *drawbridge_timers;
    const void *behavior_scratch;
    const void *loaded_object_headers;
    const void *object_header_references;
    const void *object_maps[2];
    const void *particle_buffers[5];
} MdkrObjectRollbackView;

typedef struct MdkrObjectRollbackAddressInfo {
    s32 object_index;
    s16 behavior_id;
    s16 header_type;
    size_t object_offset;
    size_t behavior_offset;
    size_t behavior_base_offset;
    size_t shading_base_offset;
    size_t shadow_base_offset;
    size_t interaction_base_offset;
    size_t collision_base_offset;
    s32 has_behavior_offset;
} MdkrObjectRollbackAddressInfo;

s32 mdkr_object_rollback_view(MdkrObjectRollbackView *view);
/* Diagnostic-only mapping used after a replay mismatch. It never participates
 * in simulation or snapshot identity. */
s32 mdkr_object_rollback_identify_address(
    const void *allocation_base, const void *address,
    MdkrObjectRollbackAddressInfo *info);
s32 mdkr_object_rollback_describe_object(
    const void *object_pointer, MdkrObjectRollbackAddressInfo *info);
#endif

#endif
