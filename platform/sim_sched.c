/*
 * sim_sched.c — fixed-authority frame clock and accumulator (spec §5, Phase 1).
 *
 * The authoritative tick is TWO source VI fields (updateRate == LOGIC_30FPS):
 * 2/60 s on NTSC/MPAL, 2/50 s on PAL. The accumulator is kept in exact integer
 * units of nanoseconds × field-rate, so one tick is exactly 2e9 units and no
 * floating-point drift can accumulate over any run length (spec §4.3, gate
 * 12.2.8: long-run drift bounded below one host clock quantum).
 *
 * Phase 1 scope: this module is the testable clock only. It does not change
 * the shipping loop; the containment complete-loop path continues to drive
 * gameplay until Phase 2's update/render split adopts it.
 */
#include "sim_sched.h"

#include <string.h>

/* One authoritative tick, in accumulator units (ns × field_hz):
 * tick_seconds = 2 / field_hz  ->  ns × field_hz = 2e9 exactly. */
#define SIM_SCHED_TICK_UNITS 2000000000ull

/* A gap this large (in ticks) is a suspension/debugger stall, not gameplay
 * time: rebase instead of catching up (spec §5). */
#define SIM_SCHED_REBASE_TICKS 30u

void sim_sched_init(SimSched *sched, unsigned field_hz) {
    memset(sched, 0, sizeof(*sched));
    sched->field_hz = field_hz == 0 ? 60u : field_hz;
}

void sim_sched_rebase(SimSched *sched) {
    sched->accumulator_units = 0;
    sched->stats.rebases++;
}

unsigned sim_sched_advance(
    SimSched *sched, unsigned long long elapsed_ns, unsigned max_steps) {
    /* Overflow-safe: elapsed_ns is bounded by the rebase clamp inside
     * sim_sched_advance_units long before multiplication can overflow
     * (30 ticks << 2^64 / 240). */
    return sim_sched_advance_units(
        sched, elapsed_ns * (unsigned long long)sched->field_hz, max_steps);
}

unsigned sim_sched_advance_units(
    SimSched *sched, unsigned long long units, unsigned max_steps) {
    unsigned long long gained;
    unsigned steps = 0;

    gained = units;
    if (gained > SIM_SCHED_REBASE_TICKS * SIM_SCHED_TICK_UNITS) {
        sim_sched_rebase(sched);
        return 0;
    }
    sched->accumulator_units += gained;
    if (sched->accumulator_units >
        SIM_SCHED_REBASE_TICKS * SIM_SCHED_TICK_UNITS) {
        sim_sched_rebase(sched);
        return 0;
    }

    while (sched->accumulator_units >= SIM_SCHED_TICK_UNITS) {
        if (max_steps != 0 && steps >= max_steps) {
            /* Budget reached with debt remaining: the caller must skip
             * rendering and continue stepping next frame (spec §5). Ordinary
             * gameplay time is never discarded. */
            sched->stats.render_skips_owed++;
            break;
        }
        sched->accumulator_units -= SIM_SCHED_TICK_UNITS;
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
    *numerator = sched->accumulator_units;
    *denominator = SIM_SCHED_TICK_UNITS;
}
