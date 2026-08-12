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

#endif

#endif
