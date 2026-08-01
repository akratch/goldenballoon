/*
 * sim_sched.c — fixed-authority frame clock and accumulator (spec §5, Phase 1).
 *
 * The default/original authoritative tick is TWO source VI fields
 * (updateRate == LOGIC_30FPS): 2/60 s on NTSC/MPAL, 2/50 s on PAL. The explicit
 * enhanced compatibility cadence uses one field. The accumulator is kept in
 * exact integer units of nanoseconds × field-rate, so no floating-point drift
 * can accumulate. HostFrameDriver owns this clock and the native video-queue
 * adapter consumes the fixed tickets it issues.
 */
#include "sim_sched.h"

#include <limits.h>
#include <string.h>

#define SIM_SCHED_FIELD_UNITS 1000000000ull

/* A gap this large (in ticks) is a suspension/debugger stall, not gameplay
 * time: rebase instead of catching up (spec §5). */
#define SIM_SCHED_REBASE_TICKS 30u

void sim_sched_init(SimSched *sched, unsigned field_hz) {
    sim_sched_init_fields(sched, field_hz, 2u);
}

void sim_sched_init_fields(
    SimSched *sched, unsigned field_hz, unsigned fields_per_tick) {
    memset(sched, 0, sizeof(*sched));
    sched->field_hz = field_hz == 0 ? 60u : field_hz;
    /* Only the one-field enhanced cadence and the authored two-field cadence
     * are selected in production. Keep a small diagnostic range available,
     * but reject values large enough to weaken the suspension bound or
     * overflow its derived unit count. */
    sched->fields_per_tick =
        fields_per_tick >= 1u && fields_per_tick <= 6u
            ? fields_per_tick
            : 2u;
    sched->tick_units =
        (unsigned long long)sched->fields_per_tick * SIM_SCHED_FIELD_UNITS;
}

void sim_sched_rebase(SimSched *sched) {
    sched->accumulator_units = 0;
    sched->stats.rebases++;
}

unsigned sim_sched_advance(
    SimSched *sched, unsigned long long elapsed_ns, unsigned max_steps) {
    if (elapsed_ns > ULLONG_MAX / (unsigned long long)sched->field_hz) {
        sim_sched_rebase(sched);
        return 0;
    }
    return sim_sched_advance_units(
        sched, elapsed_ns * (unsigned long long)sched->field_hz, max_steps);
}

unsigned sim_sched_advance_units(
    SimSched *sched, unsigned long long units, unsigned max_steps) {
    unsigned long long gained;
    unsigned steps = 0;

    gained = units;
    const unsigned long long rebase_units =
        (unsigned long long)SIM_SCHED_REBASE_TICKS * sched->tick_units;

    if (gained > rebase_units) {
        sim_sched_rebase(sched);
        return 0;
    }
    if (gained > ULLONG_MAX - sched->accumulator_units) {
        sim_sched_rebase(sched);
        return 0;
    }
    sched->accumulator_units += gained;
    if (sched->accumulator_units > rebase_units) {
        sim_sched_rebase(sched);
        return 0;
    }

    while (sched->accumulator_units >= sched->tick_units) {
        if (max_steps != 0 && steps >= max_steps) {
            /* Budget reached with debt remaining: the caller must skip
             * rendering and continue stepping next frame (spec §5). Ordinary
             * gameplay time is never discarded. */
            sched->stats.render_skips_owed++;
            break;
        }
        sched->accumulator_units -= sched->tick_units;
        steps++;
        sched->stats.ticks++;
        if (steps > 1) {
            sched->stats.catchup_steps++;
        }
    }
    return steps;
}

/* Interpolation alpha as an exact rational: accumulator / tick. */
void sim_sched_alpha(
    const SimSched *sched,
    unsigned long long *numerator,
    unsigned long long *denominator) {
    /* Never extrapolate while whole-tick debt remains. A host driver must
     * drain that debt before using interpolation; zero is the safe endpoint
     * if a diagnostic asks early. */
    *numerator = sim_sched_has_debt(sched) ? 0u : sched->accumulator_units;
    *denominator = sched->tick_units;
}

int sim_sched_has_debt(const SimSched *sched) {
    return sched->accumulator_units >= sched->tick_units;
}
