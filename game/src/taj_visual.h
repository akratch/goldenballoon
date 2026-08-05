#ifndef _TAJ_VISUAL_H_
#define _TAJ_VISUAL_H_

/*
 * Native-only presentation seam for the virtual Taj character.  This module
 * deliberately knows nothing about unlocks, menus, saves, or tuning.  The
 * eventual taj_mod identity service supplies one predicate for live racers.
 */

#include "types.h"

typedef struct Object          Object;
typedef struct Object_Racer    Object_Racer;
typedef struct TajSelectLayout TajSelectLayout;

typedef enum TajSelectVisualStatus {
    TAJ_SELECT_VISUAL_INACTIVE = 0,
    TAJ_SELECT_VISUAL_LOADING,
    TAJ_SELECT_VISUAL_READY,
    TAJ_SELECT_VISUAL_UNAVAILABLE
} TajSelectVisualStatus;

#ifdef NATIVE_PORT
/* Compatible with taj_mod_racer_is_taj(playerIndex) when Taj identity lands. */
typedef s32 (*TajVisualRacerPredicate)(s32 playerIndex);

/* NULL disables the presentation layer; taj_mod owns the predicate. */
void taj_visual_set_racer_predicate(TajVisualRacerPredicate predicate);

/* Character-select scene integration. The same layout drives actor placement
 * and menu navigation; Taj is a genuine composed actor, not a text tile. */
void taj_visual_select_begin(const TajSelectLayout *layout);
void taj_visual_select_set_state(u32 hoverMask, u32 confirmedMask);
void taj_visual_select_apply_authored_actor(Object *obj, s32 characterIndex);
void taj_visual_select_end(void);
TajSelectVisualStatus taj_visual_select_status(void);
s32  taj_visual_select_sign_visible(void);
s32  taj_visual_select_sign_object(const Object *obj);
s32  taj_visual_select_sign_player(const Object *obj);
s32  taj_visual_select_sign_batch(const Object *obj, s32 batchIndex);

/* Object lifecycle seams owned by objects.c. */
s32  taj_visual_claim_spawned_object(Object *obj);
/* TRUE between taj_visual's spawn_object() call and the claim inside
 * run_object_init_func. spawn_object consults this before applying a
 * behaviour-scoped animation-set narrowing to a SHARED cached ObjectModel:
 * a presentation companion must never decide how wide that cache entry is
 * for the other consumers of the same model id. */
s32  taj_visual_spawn_lease_active(void);
/* TRUE for every object this module composes. The authoritative state hash
 * uses it to skip presentation-only objects whole (platform/sim_hash.c). */
s32  taj_visual_is_presentation_object(const Object *obj);
void taj_visual_tick(s32 updateRate);
/* Called after obj_animate_tick publishes each animated vertex buffer. */
void taj_visual_animation_witness_tick(void);
void taj_visual_on_object_free(Object *obj);
void taj_visual_on_object_destroy(Object *obj);
void taj_visual_reset(void);
/* Monotonic scene counter for "log this once per scene" trace latches; see
 * the note on sTraceEpoch in taj_visual.c. */
u32  taj_visual_trace_epoch(void);

/* Draw seam: suppress only a successfully composed donor racer. A companion
 * loss atomically revokes this before deferred destruction, so callers never
 * render an orphaned Taj component over a hidden donor. */
s32  taj_visual_suppress_donor_draw(const Object *obj);
/* Shield/magnet matrices follow the visible carpet instead of the hidden
 * retail donor. Falls back to the supplied owner unless composition is live. */
Object *taj_visual_effect_anchor(Object *obj);
/* Split-screen intentionally limits actor shadows. Admit only the composed
 * carpet (not its rider) so playable Taj keeps one grounding decal. */
s32 taj_visual_multiplayer_shadow_object(const Object *obj);
/* The single-player actor pass otherwise admits every composed object. The
 * rider shares the carpet's footprint, so suppress only its duplicate decal. */
s32 taj_visual_suppress_companion_shadow(const Object *obj);
#endif

#endif
