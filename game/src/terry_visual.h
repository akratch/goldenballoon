#ifndef _TERRY_VISUAL_H_
#define _TERRY_VISUAL_H_

#include "taj_mod.h"
#include "taj_visual.h"
#include "types.h"

typedef struct Object          Object;
typedef struct ObjectModel     ObjectModel;
typedef struct TajSelectLayout TajSelectLayout;

#ifdef NATIVE_PORT
typedef ModRacerIdentity (*TerryVisualIdentityPredicate)(s32 playerIndex);

void                  terry_visual_set_identity_predicate(TerryVisualIdentityPredicate predicate);
void                  terry_visual_select_begin(const TajSelectLayout *layout);
void                  terry_visual_select_set_state(u32 hoverMask, u32 confirmedMask);
void                  terry_visual_select_apply_authored_actor(Object *obj, s32 characterIndex);
void                  terry_visual_select_end(void);
TajSelectVisualStatus terry_visual_select_status(void);
s32                   terry_visual_select_sign_object(const Object *obj);
s32                   terry_visual_select_sign_player(const Object *obj);
s32                   terry_visual_select_sign_batch(const Object *obj, s32 batchIndex);

s32                   terry_visual_claim_spawned_object(Object *obj);
s32                   terry_visual_is_presentation_object(const Object *obj);
s32                   terry_visual_uses_boss_head_matrix(const Object *obj);
s32                   terry_visual_multiplayer_shadow_object(const Object *obj);
void                  terry_visual_tick(s32 updateRate);
void                  terry_visual_on_object_free(Object *obj);
void                  terry_visual_on_object_destroy(Object *obj);
void                  terry_visual_reset(void);

s32                   terry_visual_suppress_donor_draw(const Object *obj);
s32                   terry_visual_batch_visible(const ObjectModel *model, const Object *obj, s32 batchIndex);
s32                   terry_visual_cap_donor_lod(const Object *obj, s32 modelIndex);
s32                   terry_visual_suppress_shadow(const Object *obj);
#endif

#endif
