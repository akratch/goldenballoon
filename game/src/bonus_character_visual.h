#ifndef _BONUS_CHARACTER_VISUAL_H_
#define _BONUS_CHARACTER_VISUAL_H_

#include "types.h"

struct Object;

#ifdef NATIVE_PORT

f32  bonus_visual_companion_scale(const struct Object *actor,
                                  const struct Object *owner,
                                  f32                  scaleFactor);
void bonus_visual_apply_local_offset(struct Object *actor, f32 x, f32 y, f32 z);
void bonus_visual_scale_shadow(struct Object *obj);
s32  bonus_visual_krunch_driver_batch(s32 vehicle, s32 lod, s32 batch);
/* Publishes the two halves of a presentation actor's animated double buffer, throttled by
 * `sequence` and gated on MDKR_TRACE. `distinct` is the oracle for the double-animate defect: a
 * module that deforms its own actor AND lets obj_animate_tick() deform it again in the same tick
 * leaves both halves holding one frame, so interpolation has nothing to blend. */
void bonus_visual_trace_animation(const char *identity, const struct Object *actor,
                                  u32 sequence);

#endif

#endif
