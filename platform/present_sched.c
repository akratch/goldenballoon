/*
 * present_sched.c — parallel authoritative accumulator for the retrace branch.
 * See present_sched.h for why it is fed the pacer's committed field count.
 */
#include "present_sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform_os.h"
#include "presentation_snapshot.h"
#include "sim_sched.h"

int g_simTickCounter = 0;

static SimSched s_sched;
static int      s_ready = 0;
static int      s_trace = -1;

/* Divergence census: how often the accumulator disagreed with the containment
 * loop's "one branch entry == one tick", and by how much at worst. */
static uint64_t s_entries = 0;
static uint64_t s_zero_due = 0;      /* entries the accumulator called idle */
static uint64_t s_multi_due = 0;     /* entries it wanted to catch up on */
static long long s_worst_lead = 0;   /* max (ticks - presents) */
static long long s_worst_lag = 0;    /* min (ticks - presents) */
static uint64_t s_interpolated = 0;  /* presents drawn between two ticks */
static uint64_t s_interpolated_views = 0; /* cameras actually substituted */
static uint64_t s_stale = 0;         /* presents that found a stale list */

static void present_sched_lazy_init(void) {
    if (s_ready) {
        return;
    }
    /* The pacer's resolved field clock, not platform_source_field_hz(): a
     * MDKR_FIELD_HZ diagnostic override must move both or the accumulator
     * would model a different second than the pacer is enforcing. */
    sim_sched_init(&s_sched, (unsigned)platform_pace_field_hz());
    s_ready = 1;
}

static int s_present_rate = -1;
static int s_test_replay_walk = -1;
static int s_snapshot_forced = 0;
static int s_smoothing = -1;

unsigned present_sched_present_rate(void) {
    if (s_present_rate < 0) {
        const char *value = getenv("MDKR_PRESENT_RATE");
        int parsed = (value != NULL && value[0] != '\0') ? atoi(value) : 0;
        /* Bounded like the pacing policy's field clock: a nonsense rate must
         * become "off", never an unbounded present subloop. */
        if (parsed < 0 || parsed > 480) {
            parsed = 0;
        }
        s_present_rate = parsed;
    }
    return (unsigned)s_present_rate;
}

static void replay_walk_lazy_init(void) {
    if (s_test_replay_walk >= 0) {
        return;
    }
    {
        const char *value = getenv("MDKR_TEST_REPLAY_WALK");
        if (value == NULL || value[0] == '\0') {
            s_test_replay_walk = 0;
        } else if (strcmp(value, "recompose") == 0) {
            s_test_replay_walk = 2;
        } else if (value[0] == '1') {
            s_test_replay_walk = 1;
        } else {
            s_test_replay_walk = 0;
        }
    }
}

bool present_sched_test_replay_walk(void) {
    replay_walk_lazy_init();
    return s_test_replay_walk != 0;
}

bool present_sched_test_force_recompose(void) {
    replay_walk_lazy_init();
    return s_test_replay_walk == 2;
}

bool present_sched_smoothing_enabled(void) {
    if (s_smoothing < 0) {
        const char *value = getenv("MDKR_PRESENT_SMOOTHING");
        s_smoothing = !(value != NULL &&
                        (strcmp(value, "off") == 0 ||
                         strcmp(value, "0") == 0));
    }
    return s_smoothing != 0;
}

void mdkr_present_set_frame_limit(const char *value) {
    /* "60" -> 60 presents/sec (the only engaged rate slice 2 supports);
     * anything else, including "original", is the off value present_sched
     * already treats as historical one-present-per-tick behaviour. */
    s_present_rate = (value != NULL && strcmp(value, "60") == 0) ? 60 : 0;
}

void mdkr_present_set_motion_smoothing(const char *value) {
    s_smoothing = (value != NULL && strcmp(value, "off") == 0) ? 0 : 1;
}

bool present_sched_replay_armed(void) {
    if (present_sched_test_replay_walk()) {
        return true;
    }
    /* A present rate at or below the authoritative tick rate needs no replay:
     * the one real walk per tick already produces every image. */
    if (present_sched_present_rate() <=
        (unsigned)(platform_pace_field_hz() /
                   (int)PRESENT_SCHED_FIELDS_PER_TICK)) {
        return false;
    }
    /*
     * An interpolated present needs the camera snapshot pair, so asking for a
     * present rate above the tick rate implies asking for the snapshot. An
     * EXPLICIT MDKR_PRESENT_SNAPSHOT still wins, which is what lets a gate run
     * the subloop with interpolation switched off as a positive control:
     * without a pair every present redraws the tick's own camera, and the
     * intermediate frames become byte-identical to the ones bracketing them.
     */
    if (!present_sched_smoothing_enabled()) {
        /* Repeating the tick's image needs no replay and no snapshot. */
        return false;
    }
    if (!s_snapshot_forced) {
        s_snapshot_forced = 1;
        if (getenv("MDKR_PRESENT_SNAPSHOT") == NULL) {
            presentation_snapshot_set_enabled(true);
        }
    }
    return true;
}

bool present_sched_trace_enabled(void) {
    if (s_trace < 0) {
        const char *value = getenv("MDKR_PRESENT_SCHED_TRACE");
        s_trace = value != NULL && value[0] == '1';
    }
    return s_trace != 0;
}

unsigned present_sched_advance_fields(unsigned fields) {
    unsigned due;
    present_sched_lazy_init();
    if (fields == 0) {
        fields = 1;
    }
    /* One source field is exactly 1e9 accumulator units — see
     * sim_sched_advance_units on why this must not route through nanoseconds. */
    due = sim_sched_advance_units(
        &s_sched, (unsigned long long)fields * 1000000000ull, 0u);
    s_entries++;
    if (due == 0) {
        s_zero_due++;
    } else if (due > 1) {
        s_multi_due++;
    }
    return due;
}

void present_sched_note_interpolated(unsigned viewports) {
    s_interpolated++;
    s_interpolated_views += viewports;
}

void present_sched_note_stale(void) {
    s_stale++;
}

void present_sched_alpha(uint64_t *numerator, uint64_t *denominator) {
    unsigned long long num = 0;
    unsigned long long den = 1;
    present_sched_lazy_init();
    sim_sched_alpha(&s_sched, &num, &den);
    if (numerator != NULL) {
        *numerator = (uint64_t)num;
    }
    if (denominator != NULL) {
        *denominator = (uint64_t)den;
    }
}

uint64_t present_sched_ticks(void) {
    present_sched_lazy_init();
    return (uint64_t)s_sched.stats.ticks;
}

void present_sched_trace_entry(unsigned fields, unsigned due,
                               int frame_counter) {
    uint64_t num = 0;
    uint64_t den = 1;
    long long delta;

    present_sched_lazy_init();
    delta = (long long)s_sched.stats.ticks - (long long)frame_counter;
    if (delta > s_worst_lead) {
        s_worst_lead = delta;
    }
    if (delta < s_worst_lag) {
        s_worst_lag = delta;
    }
    if (!present_sched_trace_enabled()) {
        return;
    }
    present_sched_alpha(&num, &den);
    /*
     * `ticks` is the accumulator's opinion, `frame` is g_frameCounter (the
     * presents the containment loop actually performed). Slice 0's whole
     * purpose is that delta stays 0 on the fixture route.
     */
    fprintf(stderr,
            "[PRESENTSCHED] entry=%llu fields=%u due=%u ticks=%llu frame=%d "
            "delta=%lld alpha=%llu/%llu catchup=%llu skips=%llu rebases=%llu\n",
            (unsigned long long)s_entries, fields, due,
            (unsigned long long)s_sched.stats.ticks, frame_counter, delta,
            (unsigned long long)num, (unsigned long long)den,
            (unsigned long long)s_sched.stats.catchup_steps,
            (unsigned long long)s_sched.stats.render_skips_owed,
            (unsigned long long)s_sched.stats.rebases);
    fflush(stderr);
}

/* ---- present-path cost census (see present_sched.h) ---------------------- */

static int      s_perf = -1;
static uint64_t s_perf_ns[PRESENT_PERF_SECTION_COUNT];
static uint64_t s_perf_hits[PRESENT_PERF_SECTION_COUNT];

/*
 * The pacer's own clock source, not a second opinion about time: the census
 * measures work that sits between two pace points, so it has to read the same
 * CLOCK_MONOTONIC the pacer targets. It is deliberately local rather than a
 * new platform entry point -- platform_sdl_min.c's pace_host_ns is static, and
 * exporting it for a diagnostic would widen a shipping seam for nothing.
 */
static uint64_t present_perf_host_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *const s_perf_names[PRESENT_PERF_SECTION_COUNT] = {
    "snapshot", "freeze", "interp", "replay", "present", "ipresent", "tickwall"
};

bool present_perf_enabled(void) {
    if (s_perf < 0) {
        const char *value = getenv("MDKR_PRESENT_PERF");
        s_perf = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    return s_perf != 0;
}

uint64_t present_perf_now(void) {
    if (!present_perf_enabled()) {
        return 0;
    }
    return present_perf_host_ns();
}

void present_perf_add(PresentPerfSection section, uint64_t start) {
    uint64_t now;
    if (start == 0u || (int)section < 0 ||
        (int)section >= PRESENT_PERF_SECTION_COUNT) {
        return;
    }
    now = present_perf_host_ns();
    if (now > start) {
        s_perf_ns[section] += now - start;
    }
    s_perf_hits[section]++;
}

/*
 * Mismatch magnitude histogram, binned by power of two of the worst per-element
 * s15.16 delta: bin k holds the mismatches with worst in [2^(k-1), 2^k), bin 0
 * holds worst == 0..1. One LSB is 1/65536 of a world unit, so bin 12 is a
 * sixteenth of a unit and bin 28 is four thousand units.
 *
 * A histogram rather than the four fixed buckets this started as, because the
 * question the buckets could not answer is the one that decides the tolerance:
 * WHERE does the benign population actually end, and where does the genuine
 * mis-association population begin? A bucket edge that happens to sit inside a
 * population hides exactly that. The full shape costs 33 counters.
 */
#define PRESENT_REJECT_BINS 34
static uint64_t s_reject_bin[PRESENT_REJECT_BINS];
static uint64_t s_reject_worst;
static uint64_t s_reject_total;

void present_perf_note_matrix_reject(uint64_t worst_lsb) {
    size_t bin = 0;
    uint64_t edge = 1u;
    while (bin + 1 < PRESENT_REJECT_BINS && worst_lsb > edge) {
        edge <<= 1;
        bin++;
    }
    s_reject_bin[bin]++;
    s_reject_total++;
    if (worst_lsb > s_reject_worst) {
        s_reject_worst = worst_lsb;
    }
}

void present_perf_summary(void) {
    int index;
    if (!present_perf_enabled()) {
        return;
    }
    fprintf(stderr, "[PRESENTREJECT] total=%llu worstlsb=%llu",
            (unsigned long long)s_reject_total,
            (unsigned long long)s_reject_worst);
    for (index = 0; index < PRESENT_REJECT_BINS; index++) {
        /* Only the populated bins: an empty bin between two populated ones is
         * the separation this census exists to show, and printing 34 zeros
         * around it buries it. */
        if (s_reject_bin[index] != 0u) {
            fprintf(stderr, " lt2e%d=%llu", index,
                    (unsigned long long)s_reject_bin[index]);
        }
    }
    fputc('\n', stderr);
    for (index = 0; index < PRESENT_PERF_SECTION_COUNT; index++) {
        /*
         * ns is the TOTAL, hits is the sample count; the mean is printed for
         * readability but the total is what a budget is compared against. A
         * section with hits=0 is printed anyway: its absence is a finding.
         */
        fprintf(stderr,
                "[PRESENTPERF] section=%s hits=%llu ns=%llu meanns=%llu\n",
                s_perf_names[index], (unsigned long long)s_perf_hits[index],
                (unsigned long long)s_perf_ns[index],
                (unsigned long long)(s_perf_hits[index] != 0
                                         ? s_perf_ns[index] / s_perf_hits[index]
                                         : 0));
    }
    fflush(stderr);
}

/* platform/fast3d — declared rather than included: the F3DDKR headers pull in
 * the whole gbi command set for two counters. */
extern void gfx_dkr_replay_get_stats(
    uint64_t *walks, uint64_t *matrix_hits, uint64_t *matrix_misses,
    uint64_t *matrix_rejects, uint64_t *real_walks);
extern void gfx_dkr_replay_get_reject_stats(
    uint64_t *tolerant, uint64_t *tolerant_worst, uint64_t *reject_least,
    bool *reject_least_valid);
extern uint64_t gfx_dkr_shadow_stale_tenant_count(void);
extern void gfx_shadow_replay_get_stats(
    uint64_t *freezes, uint64_t *restores, uint64_t *failures,
    uint64_t *restore_failures);

void present_sched_trace_summary(void) {
    if (!present_sched_trace_enabled()) {
        return;
    }
    present_sched_lazy_init();
    {
        uint64_t walks = 0, hits = 0, misses = 0, rejects = 0;
        uint64_t real_walks = 0;
        uint64_t freezes = 0, restores = 0, failures = 0;
        uint64_t restore_failures = 0;
        uint64_t tolerant = 0, tolerant_worst = 0, reject_least = 0;
        bool reject_least_valid = false;
        gfx_dkr_replay_get_stats(&walks, &hits, &misses, &rejects,
                                 &real_walks);
        gfx_dkr_replay_get_reject_stats(&tolerant, &tolerant_worst,
                                        &reject_least, &reject_least_valid);
        gfx_shadow_replay_get_stats(&freezes, &restores, &failures,
                                    &restore_failures);
        /*
         * `mtxreject` counts gameplay matrices whose (world, view_projection)
         * decomposition failed to reproduce the display list even to within the
         * geometric tolerance, and which therefore kept the list's own matrix.
         * It is a fidelity ceiling, not an error: those objects hold their
         * authoritative pose through an interpolated frame.
         *
         * `mtxtol` is the part of `mtxhit` that was accepted by the tolerance
         * rather than bit-exactly, and the two magnitudes bracket the threshold
         * from both sides: `mtxtolworst` is the largest mismatch accepted,
         * `mtxrejectleast` the smallest refused. They are printed even when the
         * tolerance never fired, because "the tolerance accepted nothing" and
         * "the tolerance is not in the build" have to be distinguishable.
         * `mtxrejectleast=-1` means nothing measurable was rejected at all.
         */
        fprintf(stderr,
                "[REPLAY-SUMMARY] walks=%llu realwalks=%llu mtxhit=%llu "
                "mtxmiss=%llu mtxreject=%llu mtxtol=%llu mtxtolworst=%llu "
                "mtxrejectleast=%lld staletenants=%llu freezes=%llu "
                "restores=%llu freezefail=%llu restorefail=%llu\n",
                (unsigned long long)walks, (unsigned long long)real_walks,
                (unsigned long long)hits, (unsigned long long)misses,
                (unsigned long long)rejects,
                (unsigned long long)tolerant,
                (unsigned long long)tolerant_worst,
                reject_least_valid ? (long long)reject_least : -1,
                (unsigned long long)gfx_dkr_shadow_stale_tenant_count(),
                (unsigned long long)freezes, (unsigned long long)restores,
                (unsigned long long)failures,
                (unsigned long long)restore_failures);
    }
    fprintf(stderr,
            "[PRESENTSCHED-SUMMARY] entries=%llu ticks=%llu presents=%d "
            "simticks=%d interp=%llu interpviews=%llu stale=%llu zerodue=%llu "
            "multidue=%llu lead=%lld lag=%lld "
            "catchup=%llu skips=%llu rebases=%llu fieldhz=%u\n",
            (unsigned long long)s_entries,
            (unsigned long long)s_sched.stats.ticks,
            g_frameCounter, g_simTickCounter,
            (unsigned long long)s_interpolated,
            (unsigned long long)s_interpolated_views,
            (unsigned long long)s_stale,
            (unsigned long long)s_zero_due,
            (unsigned long long)s_multi_due,
            s_worst_lead, s_worst_lag,
            (unsigned long long)s_sched.stats.catchup_steps,
            (unsigned long long)s_sched.stats.render_skips_owed,
            (unsigned long long)s_sched.stats.rebases,
            s_sched.field_hz);
    fflush(stderr);
}
