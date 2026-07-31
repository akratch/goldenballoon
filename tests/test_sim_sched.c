/* Fixed-authority clock/accumulator unit (spec §5, §4.3; Phase 1). */
#include <stdio.h>

#include "sim_sched.h"

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    SimSched sched;

    /* NTSC: exactly 30 ticks per simulated second, forever, no drift.
     * Feed one 60 Hz frame at a time using the worst-case integer split
     * (16666666 ns and 16666667 ns alternating misses real 1/60 s, so feed
     * exact thirds: 3 frames == 50 ms). */
    sim_sched_init(&sched, 60);
    {
        unsigned long long ticks = 0;
        /* 3600 seconds at 20 ms host frames: 50 fps host against a 30 Hz
         * authority — steps alternate 0/1/1... with EXACT accounting. */
        for (unsigned long long frame = 0; frame < 3600ull * 50ull; frame++) {
            ticks += sim_sched_advance(&sched, 20000000ull, 0);
        }
        expect(ticks == 3600ull * 30ull,
               "NTSC: exactly 30 ticks per second over an hour");
        expect(sched.accumulator_units == 0,
               "NTSC: zero residue after an exact hour");
    }

    /* PAL: 25 ticks per second on the 50-field clock. */
    sim_sched_init(&sched, 50);
    {
        unsigned long long ticks = 0;
        for (unsigned long long frame = 0; frame < 3600ull * 50ull; frame++) {
            ticks += sim_sched_advance(&sched, 20000000ull, 0);
        }
        expect(ticks == 3600ull * 25ull,
               "PAL: exactly 25 ticks per second over an hour");
        expect(sched.accumulator_units == 0,
               "PAL: zero residue after an exact hour");
    }

    /* Irregular host cadence conserves time exactly: any sequence of
     * elapsed values summing to N tick-durations yields N ticks. */
    sim_sched_init(&sched, 60);
    {
        unsigned long long ticks = 0;
        const unsigned long long chunks[] = {
            1000000ull, 32333333ull, 1ull, 66666666ull, 33333333ull,
            /* tops up to exactly 4 ticks (4 * 33333333.33..): total
             * 133333333.33 -> use units math: feed remainder chunk. */
        };
        for (unsigned index = 0;
             index < sizeof(chunks) / sizeof(chunks[0]); index++) {
            ticks += sim_sched_advance(&sched, chunks[index], 0);
        }
        /* 133333333 ns * 60 = 7999999980 units = 3 ticks + 1999999980. */
        expect(ticks == 3, "irregular: floor(elapsed/tick) ticks");
        expect(sched.accumulator_units == 1999999980ull,
               "irregular: exact residue retained");
    }

    /* Catch-up budget: debt is preserved, never discarded. */
    sim_sched_init(&sched, 60);
    {
        unsigned steps = sim_sched_advance(&sched, 200000000ull, 3);
        expect(steps == 3, "budget: capped at max_steps");
        expect(sched.stats.render_skips_owed == 1,
               "budget: render-skip owed while debt remains");
        steps = sim_sched_advance(&sched, 0, 0);
        expect(steps == 3,
               "budget: remaining whole ticks delivered next frame "
               "(200ms == 6 ticks exactly)");
        expect(sched.accumulator_units == 0, "budget: no residue lost");
    }

    /* Suspension rebases instead of catching up. */
    sim_sched_init(&sched, 60);
    {
        unsigned steps = sim_sched_advance(&sched, 5ull * 1000000000ull, 0);
        expect(steps == 0 && sched.stats.rebases == 1,
               "stall: a 5s gap rebases and delivers zero ticks");
    }

    /* Alpha is an exact rational of the residue. */
    sim_sched_init(&sched, 60);
    {
        unsigned long long numerator, denominator;
        (void)sim_sched_advance(&sched, 10000000ull, 0); /* 10 ms */
        sim_sched_alpha(&sched, &numerator, &denominator);
        expect(numerator == 600000000ull && denominator == 2000000000ull,
               "alpha: 10ms into a 33.3ms tick is exactly 3/10");
    }

    if (failures != 0) {
        fprintf(stderr, "%d sim-sched test(s) failed\n", failures);
        return 1;
    }
    puts("PASS sim sched");
    return 0;
}
