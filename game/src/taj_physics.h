#ifndef TAJ_PHYSICS_H
#define TAJ_PHYSICS_H

/*
 * Native-only virtual-character mechanics.  Taj selection deliberately lives
 * elsewhere: taj_mod registers its authoritative predicate once its racer
 * binding exists.  Keeping that boundary here avoids a second character-ID
 * convention and keeps this module harmless until Taj is selected.
 */

#include "types.h"

typedef struct Object Object;
typedef struct Object_Racer Object_Racer;

/* Matches taj_mod_racer_is_taj(int player_index); Taj owns identity. */
typedef s32 (*TajPhysicsRacerPredicate)(s32 playerIndex);

#define TAJ_PHYSICS_ACCELERATION_MULTIPLIER 1.50f
#define TAJ_PHYSICS_SUSTAINED_SPEED_MULTIPLIER 1.35f
#define TAJ_PHYSICS_TURN_RETENTION 0.85f
#define TAJ_PHYSICS_DASH_TICKS 45
#define TAJ_PHYSICS_DASH_COOLDOWN_TICKS 120

typedef struct TajPhysicsDashState {
    s32 dashTicks;
    s32 cooldownTicks;
} TajPhysicsDashState;

/* Small pure seams used by the ROM-free test target. */
f32 taj_physics_accelerated_speed(f32 entrySpeed, f32 ordinarySpeed, s32 accelerating);
f32 taj_physics_retained_turn_speed(f32 entrySpeed, f32 ordinarySpeed, s32 hardTurn);
f32 taj_physics_sustained_speed(f32 stockSpeed);
f32 taj_physics_horizontal_speed_component(f32 current, f32 forwardAxis, f32 speedDelta);
void taj_physics_advance_dash(TajPhysicsDashState *state, s32 driftReleased, s32 updateRate);

void taj_physics_set_racer_predicate(TajPhysicsRacerPredicate predicate);
void taj_physics_reset(void);
s32 taj_physics_is_taj(const Object_Racer *racer);
s32 taj_physics_absorb_attack(Object_Racer *racer, s32 attackType);
s32 taj_physics_canonical_records_allowed(const Object_Racer *racer);
s32 taj_physics_run_is_noncanonical(void);
void taj_physics_trace_record_suppressed(const Object_Racer *racer);

/* Pre-update also protects authored loop/flying-car trigger states; post-update
 * only tunes ordinary car, hovercraft, and plane authority. */
void taj_physics_pre_vehicle_update(Object *obj, Object_Racer *racer);
void taj_physics_post_vehicle_update(Object *obj, Object_Racer *racer, s32 updateRate, f32 updateRateF, s32 heldInput);

/* Presentation may query this without ever taking ownership of boostTimer.
 * Invalid or tearing-down racer indexes fail closed as no active dash. */
s32 taj_physics_dash_ticks_remaining(const Object_Racer *racer);

#endif
