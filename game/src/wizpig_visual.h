#ifndef _WIZPIG_VISUAL_H_
#define _WIZPIG_VISUAL_H_

#include "taj_mod.h"
#include "taj_visual.h"
#include "types.h"

typedef struct Object          Object;
typedef struct ObjectModel     ObjectModel;
typedef struct TajSelectLayout TajSelectLayout;

#ifdef NATIVE_PORT
typedef ModRacerIdentity (*WizpigVisualIdentityPredicate)(s32 playerIndex);

void wizpig_visual_set_identity_predicate(
    WizpigVisualIdentityPredicate predicate);
void                  wizpig_visual_select_begin(const TajSelectLayout *layout);
void                  wizpig_visual_select_set_state(u32 hoverMask, u32 confirmedMask);
void                  wizpig_visual_select_apply_authored_actor(Object *obj,
                                                                s32     characterIndex);
void                  wizpig_visual_select_end(void);
TajSelectVisualStatus wizpig_visual_select_status(void);
s32                   wizpig_visual_select_sign_object(const Object *obj);
s32                   wizpig_visual_select_sign_player(const Object *obj);
s32                   wizpig_visual_select_sign_batch(const Object *obj, s32 batchIndex);

s32                   wizpig_visual_claim_spawned_object(Object *obj);
s32                   wizpig_visual_is_presentation_object(const Object *obj);
s32                   wizpig_visual_uses_boss_head_matrix(const Object *obj);
s32                   wizpig_visual_multiplayer_shadow_object(const Object *obj);
void                  wizpig_visual_tick(s32 updateRate);
void                  wizpig_visual_on_object_free(Object *obj);
void                  wizpig_visual_on_object_destroy(Object *obj);
void                  wizpig_visual_reset(void);

/* These render queries are pure. They never mutate a shared ObjectModel. */
s32                   wizpig_visual_suppress_donor_draw(const Object *obj);
s32                   wizpig_visual_batch_visible(const ObjectModel *model, const Object *obj, s32 batchIndex);
s32                   wizpig_visual_cap_donor_lod(const Object *obj, s32 modelIndex);
s32                   wizpig_visual_suppress_shadow(const Object *obj);
#endif

#endif
