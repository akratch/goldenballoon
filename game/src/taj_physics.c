#include "taj_physics.h"

#ifdef NATIVE_PORT

#include "PR/os_cont.h"
#include "structs.h"

#include <stdio.h>
#include <stdlib.h>

#define TAJ_PHYSICS_MAX_RACERS 8
/* Same-route stock controls on Ancient Lake establish the sustained p95
 * calibration points used by tests/check_taj_speed_profile.py. Keep these
 * vehicle-specific: applying one nominal cap to three different retail drag
 * models did not produce the advertised 1.35x result in actual play. */
#define TAJ_PHYSICS_NOMINAL_CAR_SPEED 13.5f
#define TAJ_PHYSICS_NOMINAL_HOVER_SPEED 13.5f
#define TAJ_PHYSICS_NOMINAL_PLANE_SPEED 12.4f
#define TAJ_PHYSICS_SUSTAINED_SPEED taj_physics_sustained_speed(TAJ_PHYSICS_NOMINAL_CAR_SPEED)
#define TAJ_PHYSICS_DASH_SPEED (TAJ_PHYSICS_SUSTAINED_SPEED * 1.20f)
#define TAJ_PHYSICS_DASH_ACCELERATION 0.32f
#define TAJ_PHYSICS_CRUISE_ACCELERATION 0.32f

typedef struct TajPhysicsRacerState {
    const Object_Racer *racer;
    f32 entrySpeed;
    s32 wasDrifting;
    s32 selected;
    TajPhysicsDashState dash;
} TajPhysicsRacerState;

static TajPhysicsRacerPredicate sTajPredicate;
static TajPhysicsRacerState sTajStates[TAJ_PHYSICS_MAX_RACERS];
static s32 sRunNoncanonical;

static s32 taj_tunable_vehicle(s32 vehicle) {
    return vehicle == VEHICLE_CAR || vehicle == VEHICLE_HOVERCRAFT || vehicle == VEHICLE_PLANE;
}

static s32 taj_racer_vehicle(s32 vehicle) {
    return taj_tunable_vehicle(vehicle) || vehicle == VEHICLE_LOOPDELOOP || vehicle == VEHICLE_FLYING_CAR;
}

static f32 taj_nominal_speed(s32 vehicle) {
    if (vehicle == VEHICLE_HOVERCRAFT) {
        return TAJ_PHYSICS_NOMINAL_HOVER_SPEED;
    }
    return vehicle == VEHICLE_PLANE ? TAJ_PHYSICS_NOMINAL_PLANE_SPEED
                                    : TAJ_PHYSICS_NOMINAL_CAR_SPEED;
}

static f32 taj_abs(f32 value) {
    return value < 0.0f ? -value : value;
}

static f32 taj_clamp(f32 value, f32 low, f32 high) {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

static s32 taj_trace_enabled(void) {
    static s32 enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MDKR_TAJ_PHYSICS_TRACE");
        enabled = value != NULL && value[0] == '1';
    }
    return enabled;
}

#ifndef TAJ_PHYSICS_TESTING
static s32 taj_test_dash_requested(void) {
    static s32 triggerFrame = -2;
    const char *value;
    char *end = NULL;
    long parsed;
    /* AUTHORED TICKS, not host presents: this is queried from the tick-scoped
     * Taj physics update, and a present-counted trigger fires 2-4 ticks early
     * (or not at all) once the presentation subloop is running. */
    extern int g_simTickCounter;

    if (triggerFrame == -2) {
        value = getenv("MDKR_TAJ_TEST_DASH_AT");
        triggerFrame = -1;
        if (value != NULL && value[0] != '\0') {
            parsed = strtol(value, &end, 10);
            if (end != value && *end == '\0' && parsed >= 0 &&
                parsed <= 0x7FFFFFFF) {
                triggerFrame = (s32)parsed;
            }
        }
    }
    return triggerFrame >= 0 && g_simTickCounter == triggerFrame;
}
#endif

static TajPhysicsRacerState *taj_state(const Object_Racer *racer) {
    s32 index;
    TajPhysicsRacerState *state;

    if (racer == NULL) {
        return NULL;
    }
    index = racer->racerIndex;
    /* Racer indexes are allocator-owned and must never be folded into another
     * racer's sidecar.  Aliasing an invalid index to slot zero made a corrupt
     * or tearing-down racer share P1's dash and finished-state identity. */
    if (index < 0 || index >= TAJ_PHYSICS_MAX_RACERS) {
        if (taj_trace_enabled()) {
            fprintf(stderr, "[TAJPHYS] invalid-racer-index=%d player=%d\n",
                    index, racer->playerIndex);
        }
        return NULL;
    }
    state = &sTajStates[index];
    if (state->racer != racer) {
        state->racer = racer;
        state->entrySpeed = 0.0f;
        state->wasDrifting = FALSE;
        state->selected = FALSE;
        state->dash.dashTicks = 0;
        state->dash.cooldownTicks = 0;
    }
    return state;
}

f32 taj_physics_accelerated_speed(f32 entrySpeed, f32 ordinarySpeed, s32 accelerating) {
    f32 ordinaryGain;

    if (!accelerating || ordinarySpeed >= 0.0f) {
        return ordinarySpeed;
    }
    ordinaryGain = taj_abs(ordinarySpeed) - taj_abs(entrySpeed);
    if (ordinaryGain <= 0.0f) {
        return ordinarySpeed;
    }
    return -(taj_abs(entrySpeed) + ordinaryGain * TAJ_PHYSICS_ACCELERATION_MULTIPLIER);
}

f32 taj_physics_retained_turn_speed(f32 entrySpeed, f32 ordinarySpeed, s32 hardTurn) {
    f32 minimum;

    if (!hardTurn || entrySpeed >= -1.0f || ordinarySpeed >= 0.0f) {
        return ordinarySpeed;
    }
    minimum = taj_abs(entrySpeed) * TAJ_PHYSICS_TURN_RETENTION;
    if (taj_abs(ordinarySpeed) < minimum) {
        return -minimum;
    }
    return ordinarySpeed;
}

/* docs/architecture/taj-playable-mod.md:159 -- Taj "retains zippers".  The
 * sustained cap is a CRUISING cap: it describes how fast Taj holds a line under
 * his own power. It must not eat a zip pad, a boost balloon or a slipstream,
 * all of which are stock effects stock physics has already written into
 * racer->velocity this tick. Unconditionally clamping at the 18.225 car cap
 * clipped the stock 22.357 boosted peak, so Taj was the one character for whom
 * zippers did nothing -- and no gate could see it, because the sustained
 * profile test filters boosting frames out by construction and its Ancient Lake
 * route never takes a pad at all.
 *
 * The exemption is a FLOOR, not a bypass. While a boost is live the cap rises
 * to exactly what stock produced, so the clamp cannot pull Taj below a boosted
 * stock racer -- and cannot let him exceed one either, which is what keeps his
 * sustained multipliers from stacking on top of the boost.
 *
 * `stockSpeed` is the racer's authored forward velocity, negative-is-forward as
 * everywhere else in this module; `cap` and the result are positive magnitudes.
 */
f32 taj_physics_speed_cap(f32 cap, f32 stockSpeed, s32 boostActive) {
    if (!boostActive) {
        return cap;
    }
    if (stockSpeed >= -cap) {
        return cap;
    }
    return taj_abs(stockSpeed);
}

f32 taj_physics_sustained_speed(f32 stockSpeed) {
    return stockSpeed * TAJ_PHYSICS_SUSTAINED_SPEED_MULTIPLIER;
}

f32 taj_physics_horizontal_speed_component(f32 current, f32 forwardAxis, f32 speedDelta) {
    return taj_clamp(current + forwardAxis * speedDelta, -50.0f, 50.0f);
}

void taj_physics_advance_dash(TajPhysicsDashState *state, s32 driftReleased, s32 updateRate) {
    if (state->dashTicks > 0) {
        state->dashTicks -= updateRate;
        if (state->dashTicks <= 0) {
            state->dashTicks = 0;
            state->cooldownTicks = TAJ_PHYSICS_DASH_COOLDOWN_TICKS;
        }
    } else if (state->cooldownTicks > 0) {
        state->cooldownTicks -= updateRate;
        if (state->cooldownTicks < 0) {
            state->cooldownTicks = 0;
        }
    } else if (driftReleased) {
        state->dashTicks = TAJ_PHYSICS_DASH_TICKS;
    }
}

void taj_physics_set_racer_predicate(TajPhysicsRacerPredicate predicate) {
    sTajPredicate = predicate;
    if (taj_trace_enabled()) {
        fprintf(stderr, "[TAJPHYS] predicate=%s\n", predicate != NULL ? "registered" : "cleared");
    }
}

void taj_physics_reset(void) {
    s32 i;

    for (i = 0; i < TAJ_PHYSICS_MAX_RACERS; i++) {
        sTajStates[i].racer = NULL;
        sTajStates[i].entrySpeed = 0.0f;
        sTajStates[i].wasDrifting = FALSE;
        sTajStates[i].selected = FALSE;
        sTajStates[i].dash.dashTicks = 0;
        sTajStates[i].dash.cooldownTicks = 0;
    }
    sRunNoncanonical = FALSE;
}

s32 taj_physics_is_taj(const Object_Racer *racer) {
    s32 index;

    if (racer == NULL || sTajPredicate == NULL) {
        return FALSE;
    }
    if (sTajPredicate(racer->playerIndex) != 0) {
        return TRUE;
    }
    /* A finished human is handed to the CPU before the finish/record path.
     * Preserve its previously authoritative player binding through that handoff. */
    index = racer->racerIndex;
    return racer->raceFinished && index >= 0 && index < TAJ_PHYSICS_MAX_RACERS && sTajStates[index].racer == racer &&
           sTajStates[index].selected;
}

s32 taj_physics_canonical_records_allowed(const Object_Racer *racer) {
    /* The run latch is deliberately stronger than a live racer lookup.  A
     * finished racer can become CPU-controlled, and an invalid racer index
     * must never turn a modded run recordable merely because it has no sidecar. */
    return !sRunNoncanonical && !taj_physics_is_taj(racer);
}

s32 taj_physics_run_is_noncanonical(void) { return sRunNoncanonical; }

void taj_physics_trace_record_suppressed(const Object_Racer *racer) {
    if (taj_trace_enabled()) {
        fprintf(stderr, "[TAJPHYS] canonical-records-suppressed racer=%d\n", racer != NULL ? racer->racerIndex : -1);
    }
}

static void taj_clear_ordinary_attack_state(Object_Racer *racer) {
    racer->spinout_timer = 0;
    racer->squish_timer = 0;
    racer->bubbleTrapTimer = 0;
    racer->attackType = 0;
    racer->unk188 = 0;
}

s32 taj_physics_absorb_attack(Object_Racer *racer, s32 attackType) {
    if (!taj_physics_is_taj(racer)) {
        return FALSE;
    }
    taj_clear_ordinary_attack_state(racer);
    if (taj_trace_enabled()) {
        fprintf(stderr, "[TAJPHYS] attack-absorbed racer=%d type=%d\n",
                racer != NULL ? racer->racerIndex : -1, attackType);
    }
    return TRUE;
}

void taj_physics_pre_vehicle_update(Object *obj, Object_Racer *racer) {
    TajPhysicsRacerState *state;

    (void) obj;
    if (!taj_physics_is_taj(racer)) {
        return;
    }
    sRunNoncanonical = TRUE;
    if (!taj_racer_vehicle(racer->vehicleID)) {
        return;
    }
    state = taj_state(racer);
    if (state == NULL) {
        /* Keep immunity fail-safe without creating or borrowing a sidecar. */
        taj_clear_ordinary_attack_state(racer);
        return;
    }
    if (!state->selected && taj_trace_enabled()) {
        fprintf(stderr, "[TAJPHYS] active racer=%d player=%d vehicle=%d\n",
                racer->racerIndex, racer->playerIndex, racer->vehicleID);
    }
    state->selected = TRUE;
    state->entrySpeed = racer->velocity;
    state->wasDrifting = racer->drift_direction != 0;
    taj_clear_ordinary_attack_state(racer);
}

static void taj_apply_horizontal_forward_speed(Object *obj, Object_Racer *racer, f32 speed) {
    f32 delta = speed - racer->velocity;

    racer->velocity = speed;
    /* Never alter y_velocity: plane pitch, hover jumps, OOB recovery, and
     * vertical collision all remain under the authored vehicle update. */
    obj->x_velocity = taj_physics_horizontal_speed_component(obj->x_velocity, racer->ox1, delta);
    obj->z_velocity = taj_physics_horizontal_speed_component(obj->z_velocity, racer->oz1, delta);
    racer->velocity = taj_clamp(racer->velocity, -50.0f, 50.0f);
    racer->lateral_velocity = taj_clamp(racer->lateral_velocity, -50.0f, 50.0f);
}

void taj_physics_post_vehicle_update(Object *obj, Object_Racer *racer, s32 updateRate, f32 updateRateF, s32 heldInput) {
    TajPhysicsRacerState *state;
    f32 speed;
    f32 maxSpeed;
    s32 accelerating;
    s32 hardTurn;
    s32 dashWasActive;

    if (!taj_physics_is_taj(racer) || !taj_tunable_vehicle(racer->vehicleID)) {
        return;
    }
    state = taj_state(racer);
    if (state == NULL) {
        taj_clear_ordinary_attack_state(racer);
        return;
    }
    taj_clear_ordinary_attack_state(racer);
    accelerating = (heldInput & A_BUTTON) != 0 && !racer->raceFinished && racer->approachTarget == NULL;
    hardTurn = racer->vehicleID == VEHICLE_CAR && taj_abs((f32) racer->steerAngle) >= 40.0f && racer->groundedWheels >= 3;
    dashWasActive = state->dash.dashTicks > 0;
    taj_physics_advance_dash(&state->dash, racer->vehicleID == VEHICLE_CAR && state->wasDrifting &&
                                               racer->drift_direction == 0 && accelerating, updateRate);
#ifndef TAJ_PHYSICS_TESTING
    /* Deterministic presentation witness used by the real-ROM matrix. It is
     * inert unless explicitly armed and cannot apply to ordinary racers. */
    if (racer->vehicleID == VEHICLE_CAR && taj_test_dash_requested()) {
        state->dash.dashTicks = TAJ_PHYSICS_DASH_TICKS;
    }
#endif

    speed = taj_physics_accelerated_speed(state->entrySpeed, racer->velocity, accelerating);
    speed = taj_physics_retained_turn_speed(state->entrySpeed, speed, hardTurn);
    /* Sustain only when stock physics is no longer accelerating.  This keeps
     * the measurable positive-gain response at exactly 1.50x. */
    maxSpeed = taj_physics_sustained_speed(taj_nominal_speed(racer->vehicleID));
    if (accelerating && speed < 0.0f && taj_abs(speed) < maxSpeed &&
        taj_abs(racer->velocity) <= taj_abs(state->entrySpeed)) {
        speed -= TAJ_PHYSICS_CRUISE_ACCELERATION * updateRateF;
    }
    if (racer->vehicleID == VEHICLE_CAR && state->dash.dashTicks > 0 && speed < 0.0f) {
        speed -= TAJ_PHYSICS_DASH_ACCELERATION * updateRateF;
    }
    if (racer->vehicleID == VEHICLE_CAR && state->dash.dashTicks > 0) {
        maxSpeed = TAJ_PHYSICS_DASH_SPEED;
    }
    maxSpeed = taj_physics_speed_cap(maxSpeed, racer->velocity,
                                     racer->boostTimer != 0);
    if (speed < -maxSpeed) {
        speed = -maxSpeed;
    }
    taj_apply_horizontal_forward_speed(obj, racer, speed);

    if (taj_trace_enabled() && !dashWasActive && state->dash.dashTicks > 0) {
        fprintf(stderr, "[TAJPHYS] dash-start racer=%d ticks=%d\n", racer->racerIndex, TAJ_PHYSICS_DASH_TICKS);
    }
    if (taj_trace_enabled() && dashWasActive && state->dash.dashTicks == 0) {
        fprintf(stderr, "[TAJPHYS] dash-end racer=%d cooldown=%d\n", racer->racerIndex, state->dash.cooldownTicks);
    }
}

s32 taj_physics_dash_ticks_remaining(const Object_Racer *racer) {
    TajPhysicsRacerState *state;

    if (!taj_physics_is_taj(racer)) {
        return 0;
    }
    state = taj_state(racer);
    return state != NULL ? state->dash.dashTicks : 0;
}

#endif
