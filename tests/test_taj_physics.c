#include "taj_physics.h"
#include "racer.h"
#include "PR/os_cont.h"
#include "structs.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "taj-physics test failed: %s:%d: %s\n",           \
                    __FILE__, __LINE__, #condition);                            \
            failures++;                                                        \
        }                                                                       \
    } while (0)

static s32 player_one_is_taj(s32 playerIndex) { return playerIndex == 0; }

static void init_racer(Object *object, Object_Racer *racer, s32 vehicle) {
    memset(object, 0, sizeof(*object));
    memset(racer, 0, sizeof(*racer));
    racer->playerIndex = PLAYER_ONE;
    racer->racerIndex = 0;
    racer->vehicleID = vehicle;
    racer->velocity = -4.0f;
    racer->lateral_velocity = 75.0f;
    racer->groundedWheels = 3;
    racer->ox1 = 1.0f;
    racer->oz1 = 0.0f;
}

static f32 expected_sustained_cap(s32 vehicle) {
    if (vehicle == VEHICLE_PLANE) {
        return 16.74f;
    }
    return 18.225f;
}

static void test_real_vehicle_path(s32 vehicle) {
    Object object;
    Object_Racer racer;

    taj_physics_reset();
    init_racer(&object, &racer, vehicle);
    racer.spinout_timer = 12;
    racer.squish_timer = 13;
    racer.bubbleTrapTimer = 14;
    racer.attackType = ATTACK_BUBBLE;
    racer.unk188 = 2;
    racer.bananas = 7;
    taj_physics_pre_vehicle_update(&object, &racer);
    CHECK(racer.spinout_timer == 0 && racer.squish_timer == 0 &&
          racer.bubbleTrapTimer == 0 && racer.attackType == ATTACK_NONE &&
          racer.unk188 == 0);
    CHECK(racer.bananas == 7);

    /* Model the result of one ordinary vehicle update, then require Taj's
     * post-update seam to apply its 1.50x response in every tunable dispatch. */
    racer.velocity = -5.0f;
    taj_physics_post_vehicle_update(&object, &racer, 1, 1.0f, A_BUTTON);
    CHECK(racer.velocity == -5.5f);
    CHECK(object.x_velocity == -0.5f);
    CHECK(racer.lateral_velocity == 50.0f);

    /* The protection must also hold if an ordinary hit arrives between the
     * pre- and post-update seams.  Banana count remains intact; the producer
     * guard is separately hardened in the full racer/object path. */
    racer.spinout_timer = 21;
    racer.squish_timer = 22;
    racer.bubbleTrapTimer = 23;
    racer.attackType = ATTACK_EXPLOSION;
    racer.unk188 = 2;
    taj_physics_post_vehicle_update(&object, &racer, 1, 1.0f, 0);
    CHECK(racer.spinout_timer == 0 && racer.squish_timer == 0 &&
          racer.bubbleTrapTimer == 0 && racer.attackType == ATTACK_NONE &&
          racer.unk188 == 0 && racer.bananas == 7);

    /* A stock result beyond the profile cap may never escape the global
     * horizontal-velocity safety boundary. */
    racer.velocity = -20.0f;
    taj_physics_post_vehicle_update(&object, &racer, 1, 1.0f, A_BUTTON);
    CHECK(racer.velocity == -expected_sustained_cap(vehicle));
}

static void test_car_turn_and_dash(void) {
    Object object;
    Object_Racer racer;

    taj_physics_reset();
    init_racer(&object, &racer, VEHICLE_CAR);
    racer.velocity = -10.0f;
    taj_physics_pre_vehicle_update(&object, &racer);
    racer.velocity = -7.0f;
    racer.steerAngle = 40;
    taj_physics_post_vehicle_update(&object, &racer, 1, 1.0f, 0);
    CHECK(racer.velocity == -8.5f);

    taj_physics_reset();
    init_racer(&object, &racer, VEHICLE_CAR);
    racer.velocity = -10.0f;
    racer.drift_direction = 1;
    taj_physics_pre_vehicle_update(&object, &racer);
    racer.drift_direction = 0;
    taj_physics_post_vehicle_update(&object, &racer, 1, 1.0f, A_BUTTON);
    CHECK(taj_physics_dash_ticks_remaining(&racer) == TAJ_PHYSICS_DASH_TICKS);
    taj_physics_pre_vehicle_update(&object, &racer);
    taj_physics_post_vehicle_update(&object, &racer, TAJ_PHYSICS_DASH_TICKS,
                                    (f32)TAJ_PHYSICS_DASH_TICKS, A_BUTTON);
    CHECK(taj_physics_dash_ticks_remaining(&racer) == 0);
}

static void test_identity_handoff_and_negative_control(void) {
    Object object;
    Object_Racer racer;

    taj_physics_reset();
    init_racer(&object, &racer, VEHICLE_CAR);
    taj_physics_pre_vehicle_update(&object, &racer);
    CHECK(taj_physics_run_is_noncanonical());
    CHECK(!taj_physics_canonical_records_allowed(&racer));
    racer.playerIndex = PLAYER_COMPUTER;
    racer.raceFinished = TRUE;
    CHECK(taj_physics_is_taj(&racer));
    CHECK(!taj_physics_canonical_records_allowed(&racer));
    racer.raceFinished = FALSE;
    CHECK(!taj_physics_is_taj(&racer));
    CHECK(!taj_physics_canonical_records_allowed(&racer));

    taj_physics_reset();
    init_racer(&object, &racer, VEHICLE_CAR);
    racer.playerIndex = PLAYER_TWO;
    racer.spinout_timer = 17;
    taj_physics_pre_vehicle_update(&object, &racer);
    taj_physics_post_vehicle_update(&object, &racer, 1, 1.0f, A_BUTTON);
    CHECK(!taj_physics_run_is_noncanonical());
    CHECK(taj_physics_canonical_records_allowed(&racer));
    CHECK(racer.spinout_timer == 17 && racer.velocity == -4.0f);
}

static void test_invalid_racer_index_does_not_alias_p1(void) {
    Object validObject;
    Object invalidObject;
    Object_Racer valid;
    Object_Racer invalid;

    taj_physics_reset();
    init_racer(&validObject, &valid, VEHICLE_CAR);
    valid.drift_direction = 1;
    taj_physics_pre_vehicle_update(&validObject, &valid);

    init_racer(&invalidObject, &invalid, VEHICLE_CAR);
    /* Legal racer slots are 0..7; this is one past the sidecar array. */
    invalid.racerIndex = 8;
    invalid.drift_direction = 1;
    invalid.spinout_timer = 19;
    taj_physics_pre_vehicle_update(&invalidObject, &invalid);
    invalid.drift_direction = 0;
    taj_physics_post_vehicle_update(&invalidObject, &invalid, 1, 1.0f,
                                    A_BUTTON);
    CHECK(invalid.spinout_timer == 0);
    CHECK(invalid.velocity == -4.0f);
    CHECK(taj_physics_dash_ticks_remaining(&invalid) == 0);
    CHECK(taj_physics_run_is_noncanonical());
    CHECK(!taj_physics_canonical_records_allowed(&invalid));

    /* If invalid index 8 were still folded into state[0], its release above
     * would consume P1's pending dash and this would report 44 instead of 45. */
    valid.drift_direction = 0;
    taj_physics_post_vehicle_update(&validObject, &valid, 1, 1.0f, A_BUTTON);
    CHECK(taj_physics_dash_ticks_remaining(&valid) == TAJ_PHYSICS_DASH_TICKS);
}

/* M15 #4: Taj's sustained cruise cap used to be applied unconditionally, so
 * every stock boost was clipped back to it and Taj was the one character for
 * whom zip pads did nothing. The shipped speed-profile gate cannot witness
 * this -- it filters boosting frames out by construction, and its Ancient Lake
 * route takes no pad at all (measured: 2578 [BOOST] rows, every one timer=0) --
 * so the exemption is pinned here instead.
 *
 * Sign convention matches the module: forward velocity is NEGATIVE, caps are
 * positive magnitudes. */
static void test_boost_survives_the_sustained_cap(void) {
    const f32 car_cap = 13.5f * TAJ_PHYSICS_SUSTAINED_SPEED_MULTIPLIER; /* 18.225 */
    const f32 stock_boost_peak = -22.357f; /* check_boost_magnitude.py:114 */

    /* No boost: the cap is untouched, whatever stock is doing. */
    CHECK(taj_physics_speed_cap(car_cap, -10.0f, FALSE) == car_cap);
    CHECK(taj_physics_speed_cap(car_cap, stock_boost_peak, FALSE) == car_cap);

    /* Boosting past the cap: the cap rises to exactly the stock magnitude, so
     * the clamp can no longer eat the pad. This is the assertion that fails
     * against the pre-fix unconditional clamp. */
    CHECK(taj_physics_speed_cap(car_cap, stock_boost_peak, TRUE) >
          car_cap);
    CHECK(taj_physics_speed_cap(car_cap, stock_boost_peak, TRUE) ==
          -stock_boost_peak);

    /* ...and no further: the exemption is a floor, not a bypass, so Taj's own
     * multipliers may not stack on top of a stock boost. */
    CHECK(taj_physics_speed_cap(car_cap, stock_boost_peak, TRUE) <
          -stock_boost_peak * 1.0001f + 0.0001f);

    /* Boosting but still BELOW the cruise cap (a boost taken from a standstill,
     * or a weak pad): the cap must not be lowered to the current speed, or the
     * boost would cap Taj at whatever he happened to be doing. */
    CHECK(taj_physics_speed_cap(car_cap, -5.0f, TRUE) == car_cap);
    CHECK(taj_physics_speed_cap(car_cap, -car_cap, TRUE) == car_cap);

    /* Degenerate inputs stay bounded. */
    CHECK(taj_physics_speed_cap(car_cap, 0.0f, TRUE) == car_cap);
    CHECK(taj_physics_speed_cap(car_cap, 3.0f, TRUE) == car_cap);
}

int main(void) {
    TajPhysicsDashState dash = { 0, 0 };

    taj_physics_reset();
    CHECK(!taj_physics_run_is_noncanonical());
    CHECK(taj_physics_accelerated_speed(-4.0f, -5.0f, 1) == -5.5f);
    CHECK(taj_physics_accelerated_speed(-4.0f, -5.0f, 0) == -5.0f);
    CHECK(taj_physics_retained_turn_speed(-10.0f, -7.0f, 1) == -8.5f);
    CHECK(taj_physics_retained_turn_speed(-10.0f, -9.0f, 1) == -9.0f);
    CHECK(taj_physics_sustained_speed(12.0f) == 16.2f);
    CHECK(taj_physics_horizontal_speed_component(5.0f, 0.6f, -4.0f) == 2.6f);
    CHECK(taj_physics_horizontal_speed_component(49.0f, 1.0f, 4.0f) == 50.0f);

    taj_physics_advance_dash(&dash, 1, 1);
    CHECK(dash.dashTicks == TAJ_PHYSICS_DASH_TICKS && dash.cooldownTicks == 0);
    taj_physics_advance_dash(&dash, 0, TAJ_PHYSICS_DASH_TICKS);
    CHECK(dash.dashTicks == 0 && dash.cooldownTicks == TAJ_PHYSICS_DASH_COOLDOWN_TICKS);
    taj_physics_advance_dash(&dash, 1, TAJ_PHYSICS_DASH_COOLDOWN_TICKS);
    CHECK(dash.dashTicks == 0 && dash.cooldownTicks == 0);
    taj_physics_advance_dash(&dash, 1, 1);
    CHECK(dash.dashTicks == TAJ_PHYSICS_DASH_TICKS);

    taj_physics_set_racer_predicate(player_one_is_taj);
    {
        Object object;
        Object_Racer racer;
        init_racer(&object, &racer, VEHICLE_CAR);
        racer.attackType = ATTACK_EXPLOSION;
        racer.spinout_timer = 12;
        racer.squish_timer = 13;
        racer.bubbleTrapTimer = 14;
        racer.unk188 = 2;
        CHECK(taj_physics_absorb_attack(&racer, ATTACK_BUBBLE));
        CHECK(racer.attackType == ATTACK_NONE && racer.spinout_timer == 0 &&
              racer.squish_timer == 0 && racer.bubbleTrapTimer == 0 &&
              racer.unk188 == 0);
        racer.playerIndex = PLAYER_TWO;
        CHECK(!taj_physics_absorb_attack(&racer, ATTACK_EXPLOSION));
    }
    test_real_vehicle_path(VEHICLE_CAR);
    test_real_vehicle_path(VEHICLE_HOVERCRAFT);
    test_real_vehicle_path(VEHICLE_PLANE);
    test_car_turn_and_dash();
    test_identity_handoff_and_negative_control();
    test_invalid_racer_index_does_not_alias_p1();
    test_boost_survives_the_sustained_cap();

    if (failures != 0) {
        fprintf(stderr, "taj-physics tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("taj-physics tests: PASS");
    return 0;
}
