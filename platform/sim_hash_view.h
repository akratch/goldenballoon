/*
 * sim_hash_view.h -- read-only view of the object list the v3 state hash walks.
 *
 * WHY THIS LIVES BESIDE THE HASH AND NOT BESIDE THE VIEWER. The object viewer's
 * whole value is that it agrees with the authority. If it enumerated live
 * objects through its own extern of gObjPtrList, or through the presentation
 * snapshot, it could show a population the [SIMHASH] stream does not hash --
 * and a viewer that can disagree with the game is worse than no viewer at all
 * (the same rule the collision overlay is held to). So the accessor is
 * implemented in platform/sim_hash.c, walks the SAME objGetObjList() array with
 * the SAME presentation-companion filter as sim_hash_compute_authoritative_v3,
 * and reports per entry whether the hash counted it.
 *
 * It is a pure read. Nothing here writes an Object, allocates, or caches; two
 * calls in one frame observe the same bytes the hash would.
 */
#ifndef MDKR64_SIM_HASH_VIEW_H
#define MDKR64_SIM_HASH_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One entry of the walked list, copied out. Deliberately carries no pointer
 * into game memory: a consumer holding an Object * across a frame would be
 * holding a pool slot that may already have been recycled. */
typedef struct MdkrSimObjectView {
    int32_t index;          /* position in the walked array; the hash mixes it */
    int32_t behaviour_id;   /* Object::behaviorId, or -1 for a particle */
    float   position[3];    /* trans.x/y/z_position */
    int32_t flags;          /* trans.flags, the same word the hash mixes */
    int32_t active_emitters;/* numActiveEmitters (shared Object/Particle prefix) */
    uint8_t is_particle;    /* flags & OBJ_FLAGS_PARTICLE */
    uint8_t emitters_on;    /* particleEmittersEnabled; 0 for a particle */
    uint8_t live;           /* 0 == empty slot (the hash mixes presence too) */
    uint8_t hashed;         /* 0 == filtered out as a presentation companion */
} MdkrSimObjectView;

/* Length of the walked array, i.e. the highest index mdkr_sim_object_view()
 * will accept. This is the RAW slot count, not the authoritative population the
 * `objs=` field of a [SIMHASH] row reports -- empty slots and filtered
 * companions occupy indices, and the viewer shows them for the same reason the
 * hash mixes their presence byte. Returns 0 before the object pool exists. */
int mdkr_sim_object_count(void);

/* Copy entry `index` out. False for an out-of-range index or a NULL out. A live
 * == 0 result is a successful read of an empty slot, not a failure. */
bool mdkr_sim_object_view(int index, MdkrSimObjectView *out);

/* The authoritative population -- exactly the number a [SIMHASH] row publishes
 * as `objs=`, computed by the hash's own counter. */
int mdkr_sim_object_authoritative_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_SIM_HASH_VIEW_H */
