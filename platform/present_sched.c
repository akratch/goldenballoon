/*
 * present_sched.c — presentation facade for the authoritative host driver.
 */
#include "present_sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host_frame_driver.h"
#include "pacing_policy.h"
#include "platform_os.h"
#include "presentation_snapshot.h"
#include "fast3d/gfx_presentation_packet.h"
#include "fast3d/gfx_shadow_frame.h"

int g_simTickCounter = 0;

static HostFrameDriver s_driver;
static int      s_ready = 0;
static int      s_trace = -1;

/* Clock/ticket census: due-ticket burst shape and maximum unissued debt. */
static uint64_t s_entries = 0;
static uint64_t s_zero_due = 0;      /* entries the accumulator called idle */
static uint64_t s_multi_due = 0;     /* entries it wanted to catch up on */
static long long s_worst_lead = 0;   /* max (ticks - presents) */
static long long s_worst_lag = 0;    /* min (ticks - presents) */
static uint64_t s_interpolated = 0;  /* presents drawn between two ticks */
static uint64_t s_interpolated_views = 0; /* cameras actually substituted */
static uint64_t s_stale = 0;         /* presents that found a stale list */
static uint64_t s_elided = 0;        /* catch-up ticks not submitted/presented */
static bool s_render_elided;
static bool s_surface_elided;
static uint64_t s_game_updates = 0;
static uint64_t s_bad_game_updates = 0;
static unsigned s_game_update_min = 0;
static unsigned s_game_update_max = 0;
static unsigned s_bootstrap_update = 0;

static void present_sched_lazy_init(void) {
    if (s_ready) {
        return;
    }
    /* The pacer's resolved field clock, not platform_source_field_hz(): a
     * MDKR_FIELD_HZ diagnostic override must move both or the accumulator
     * would model a different second than the pacer is enforcing. */
    host_frame_driver_init(
        &s_driver, (unsigned)platform_pace_field_hz(),
        (unsigned)platform_sim_tick_fields());
    s_ready = 1;
}

static MdkrPresentPolicy s_present_policy = {
    MDKR_PRESENT_ORIGINAL, 0u
};
static MdkrPresentPolicy s_present_requested_policy = {
    MDKR_PRESENT_ORIGINAL, 0u
};
static int s_present_policy_ready;
static int s_test_replay_walk = -1;
static int s_snapshot_forced = 0;
static int s_smoothing = -1;

static void present_policy_lazy_init(void) {
    const char *value;

    if (s_present_policy_ready) {
        return;
    }
    value = getenv("MDKR_PRESENT_RATE");
    if (value == NULL || value[0] == '\0' ||
        !mdkr_present_policy_parse(value, &s_present_requested_policy)) {
        s_present_requested_policy.kind = MDKR_PRESENT_ORIGINAL;
        s_present_requested_policy.rate = 0u;
    }
    s_present_policy = s_present_requested_policy;
#ifdef __EMSCRIPTEN__
    if (s_present_policy.kind == MDKR_PRESENT_UNCAPPED) {
        /* rAF is the browser's presentation ceiling. `uncapped` can arrive
         * from a shared config file or diagnostic env, but must never imply a
         * native-style unbounded swapchain in this build. */
        s_present_policy.kind = MDKR_PRESENT_DISPLAY;
        s_present_policy.rate = 0u;
        fprintf(stderr,
                "[PRESENT-POLICY] platform=web requested=uncapped "
                "effective=display reason=raf-ceiling\n");
    }
#endif
    s_present_policy_ready = 1;
}

unsigned present_sched_present_rate(void) {
    present_policy_lazy_init();
    return s_present_policy.kind == MDKR_PRESENT_CAPPED
        ? s_present_policy.rate : 0u;
}

MdkrPresentPolicyKind present_sched_present_kind(void) {
    present_policy_lazy_init();
    return s_present_policy.kind;
}

unsigned present_sched_present_requested_rate(void) {
    present_policy_lazy_init();
    return s_present_requested_policy.kind == MDKR_PRESENT_CAPPED
        ? s_present_requested_policy.rate : 0u;
}

MdkrPresentPolicyKind present_sched_present_requested_kind(void) {
    present_policy_lazy_init();
    return s_present_requested_policy.kind;
}

bool present_sched_present_subloop(void) {
    const unsigned tick_rate =
        (unsigned)platform_pace_field_hz() /
        (unsigned)platform_sim_tick_fields();
    present_policy_lazy_init();
    return mdkr_present_policy_needs_subloop(
        &s_present_policy, tick_rate) != 0;
}

bool present_sched_backend_vsync_enabled(void) {
    present_policy_lazy_init();
    return mdkr_present_policy_uses_vsync(&s_present_policy) != 0;
}

const char *present_sched_present_policy_name(void) {
    present_policy_lazy_init();
    switch (s_present_policy.kind) {
        case MDKR_PRESENT_CAPPED: return "capped";
        case MDKR_PRESENT_DISPLAY: return "display";
        case MDKR_PRESENT_UNCAPPED: return "uncapped";
        case MDKR_PRESENT_ORIGINAL:
        default: return "original";
    }
}

const char *present_sched_present_requested_policy_name(void) {
    present_policy_lazy_init();
    switch (s_present_requested_policy.kind) {
        case MDKR_PRESENT_CAPPED: return "capped";
        case MDKR_PRESENT_DISPLAY: return "display";
        case MDKR_PRESENT_UNCAPPED: return "uncapped";
        case MDKR_PRESENT_ORIGINAL:
        default: return "original";
    }
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
    if (value == NULL ||
        !mdkr_present_policy_parse(value, &s_present_requested_policy)) {
        s_present_requested_policy.kind = MDKR_PRESENT_ORIGINAL;
        s_present_requested_policy.rate = 0u;
    }
    s_present_policy = s_present_requested_policy;
#ifdef __EMSCRIPTEN__
    if (s_present_policy.kind == MDKR_PRESENT_UNCAPPED) {
        s_present_policy.kind = MDKR_PRESENT_DISPLAY;
        s_present_policy.rate = 0u;
        fprintf(stderr,
                "[PRESENT-POLICY] platform=web requested=uncapped "
                "effective=display reason=raf-ceiling\n");
    }
#endif
    s_present_policy_ready = 1;
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
    if (!present_sched_present_subloop()) {
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

bool present_sched_defer_tick_present(void) {
    return present_sched_replay_armed() &&
           !present_sched_test_replay_walk() &&
           present_sched_smoothing_enabled();
}

bool present_sched_trace_enabled(void) {
    if (s_trace < 0) {
        const char *value = getenv("MDKR_PRESENT_SCHED_TRACE");
        s_trace = value != NULL && value[0] == '1';
    }
    return s_trace != 0;
}

unsigned present_sched_advance_fields(unsigned fields, bool rebase) {
    if (fields == 0) {
        fields = 1;
    }
    /* One source field is exactly 1e9 accumulator units — see
     * sim_sched_advance_units on why this must not route through nanoseconds. */
    return present_sched_advance_units(
        (uint64_t)fields * UINT64_C(1000000000), rebase);
}

unsigned present_sched_advance_units(uint64_t units, bool rebase) {
    unsigned due;
    present_sched_lazy_init();
    if (rebase) {
        /* Never interpolate across debugger/sleep/hidden-window time. The
         * first post-resume tick seeds a fresh snapshot history. */
        presentation_snapshot_stage_reset();
    }
    due = host_frame_driver_advance_units(&s_driver, units, rebase);
    s_entries++;
    if (due == 0) {
        s_zero_due++;
    } else if (due > 1) {
        s_multi_due++;
    }
    return due;
}

bool present_sched_take_tick(void) {
    present_sched_lazy_init();
    return host_frame_driver_take_tick(&s_driver);
}

uint64_t present_sched_pending_ticks(void) {
    present_sched_lazy_init();
    return host_frame_driver_pending_ticks(&s_driver);
}

uint64_t present_sched_issued_ticks(void) {
    present_sched_lazy_init();
    return host_frame_driver_issued_ticks(&s_driver);
}

unsigned present_sched_tick_fields(void) {
    present_sched_lazy_init();
    return host_frame_driver_fields_per_tick(&s_driver);
}

uint64_t present_sched_input_target_tick(void) {
    uint64_t clock_ticks;
    uint64_t issued_ticks;
    present_sched_lazy_init();
    clock_ticks = host_frame_driver_clock_ticks(&s_driver);
    issued_ticks = host_frame_driver_issued_ticks(&s_driver);
    if (clock_ticks > issued_ticks) {
        return clock_ticks;
    }
    return clock_ticks + 1u;
}

bool present_sched_should_elide_render(void) {
    present_sched_lazy_init();
    return host_frame_driver_pending_ticks(&s_driver) != 0u;
}

void present_sched_set_render_elided(bool elided) {
    s_render_elided = elided;
    if (elided) {
        /* main_game_loop suppresses the catch-up graphics task before it can
         * reach the dispatcher, so count that elision at the decision seam. */
        s_elided++;
    }
}

void present_sched_set_surface_elided(bool elided) {
    s_surface_elided = elided;
}

bool present_sched_render_elided(void) {
    return s_render_elided || s_surface_elided;
}

void present_sched_note_render_elided(void) {
    s_elided++;
}

void present_sched_note_game_update(unsigned update_rate) {
    present_sched_lazy_init();
    if (g_simTickCounter == 0) {
        s_bootstrap_update = update_rate;
        return;
    }
    if (s_game_updates == 0 || update_rate < s_game_update_min) {
        s_game_update_min = update_rate;
    }
    if (update_rate > s_game_update_max) {
        s_game_update_max = update_rate;
    }
    s_game_updates++;
    if (update_rate != host_frame_driver_fields_per_tick(&s_driver)) {
        s_bad_game_updates++;
    }
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
    host_frame_driver_alpha(&s_driver, &num, &den);
    if (numerator != NULL) {
        *numerator = (uint64_t)num;
    }
    if (denominator != NULL) {
        *denominator = (uint64_t)den;
    }
}

uint64_t present_sched_ticks(void) {
    present_sched_lazy_init();
    return host_frame_driver_clock_ticks(&s_driver);
}

void present_sched_trace_entry(unsigned fields, unsigned due,
                               int frame_counter) {
    uint64_t num = 0;
    uint64_t den = 1;
    long long delta;

    present_sched_lazy_init();
    delta = (long long)host_frame_driver_clock_ticks(&s_driver) -
            (long long)host_frame_driver_issued_ticks(&s_driver);
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
    /* `delta` is due clock ticks minus issued game-loop tickets. It is zero in
     * steady state and positive only while ordinary catch-up debt is draining;
     * `frame` is separately the presentation count. */
    fprintf(stderr,
            "[PRESENTSCHED] entry=%llu fields=%u due=%u ticks=%llu frame=%d "
            "delta=%lld alpha=%llu/%llu catchup=%llu skips=%llu rebases=%llu\n",
            (unsigned long long)s_entries, fields, due,
            (unsigned long long)host_frame_driver_clock_ticks(&s_driver),
            frame_counter, delta,
            (unsigned long long)num, (unsigned long long)den,
            (unsigned long long)s_driver.clock.stats.catchup_steps,
            (unsigned long long)s_driver.clock.stats.render_skips_owed,
            (unsigned long long)s_driver.clock.stats.rebases);
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
extern void gfx_dkr_replay_get_object_stats(uint64_t *hits, uint64_t *holds);
extern void gfx_dkr_replay_get_billboard_stats(
    uint64_t *matrix_hits, uint64_t *matrix_holds,
    uint64_t *vertex_hits, uint64_t *vertex_holds);
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
        uint64_t object_hits = 0, object_holds = 0;
        uint64_t billboard_matrix_hits = 0, billboard_matrix_holds = 0;
        uint64_t billboard_vertex_hits = 0, billboard_vertex_holds = 0;
        GfxPresentationOwnerStats owner_stats;
        GfxPresentationPacketStats packet_stats;
        bool reject_least_valid = false;
        memset(&owner_stats, 0, sizeof(owner_stats));
        memset(&packet_stats, 0, sizeof(packet_stats));
        gfx_dkr_replay_get_stats(&walks, &hits, &misses, &rejects,
                                 &real_walks);
        gfx_dkr_replay_get_reject_stats(&tolerant, &tolerant_worst,
                                        &reject_least, &reject_least_valid);
        gfx_dkr_replay_get_object_stats(&object_hits, &object_holds);
        gfx_dkr_replay_get_billboard_stats(
            &billboard_matrix_hits, &billboard_matrix_holds,
            &billboard_vertex_hits, &billboard_vertex_holds);
        gfx_shadow_presentation_owner_get_stats(&owner_stats);
        gfx_presentation_packet_get_stats(&packet_stats);
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
                "restores=%llu freezefail=%llu restorefail=%llu "
                "objhit=%llu objhold=%llu\n",
                (unsigned long long)walks, (unsigned long long)real_walks,
                (unsigned long long)hits, (unsigned long long)misses,
                (unsigned long long)rejects,
                (unsigned long long)tolerant,
                (unsigned long long)tolerant_worst,
                reject_least_valid ? (long long)reject_least : -1,
                (unsigned long long)gfx_dkr_shadow_stale_tenant_count(),
                (unsigned long long)freezes, (unsigned long long)restores,
                (unsigned long long)failures,
                (unsigned long long)restore_failures,
                (unsigned long long)object_hits,
                (unsigned long long)object_holds);
        fprintf(stderr,
                "[PRESENT-OWNERS] registrations=%llu roots=%llu children=%llu "
                "effects=%llu unowned=%llu\n",
                (unsigned long long)owner_stats.registrations,
                (unsigned long long)owner_stats.roots,
                (unsigned long long)owner_stats.children,
                (unsigned long long)owner_stats.effects,
                (unsigned long long)owner_stats.unowned);
        fprintf(stderr,
                "[PRESENT-PACKET] matrixreg=%llu vertexreg=%llu "
                "particlevertexreg=%llu freezes=%llu "
                "freezefail=%llu matrixhit=%llu matrixmiss=%llu "
                "vertexhit=%llu vertexmiss=%llu stale=%llu "
                "frozenmatrices=%zu frozenvertices=%zu matrixpeak=%zu "
                "vertexpeak=%zu matrixoverride=%llu matrixhold=%llu "
                "vertexoverride=%llu vertexhold=%llu deformreg=%llu "
                "deformhit=%llu deformoverride=%llu deformmiss=%llu "
                "deformincompatible=%llu deformcollision=%llu "
                "colorhit=%llu coloroverride=%llu "
                "particledeformhit=%llu particledeformoverride=%llu "
                "particlecolorhit=%llu particlecoloroverride=%llu "
                "primalphahit=%llu primalphaoverride=%llu "
                "particleprimalphahit=%llu "
                "particleprimalphaoverride=%llu "
                "effectreg=%llu effecthit=%llu effectoverride=%llu "
                "effectmiss=%llu effectcollision=%llu "
                "deformpeak=%zu\n",
                (unsigned long long)packet_stats.matrix_registrations,
                (unsigned long long)packet_stats.vertex_registrations,
                (unsigned long long)
                    packet_stats.particle_vertex_registrations,
                (unsigned long long)packet_stats.freezes,
                (unsigned long long)packet_stats.freeze_failures,
                (unsigned long long)packet_stats.matrix_hits,
                (unsigned long long)packet_stats.matrix_misses,
                (unsigned long long)packet_stats.vertex_hits,
                (unsigned long long)packet_stats.vertex_misses,
                (unsigned long long)packet_stats.stale_keys,
                packet_stats.frozen_matrices,
                packet_stats.frozen_vertices,
                packet_stats.matrix_peak,
                packet_stats.vertex_peak,
                (unsigned long long)billboard_matrix_hits,
                (unsigned long long)billboard_matrix_holds,
                (unsigned long long)billboard_vertex_hits,
                (unsigned long long)billboard_vertex_holds,
                (unsigned long long)packet_stats.deformation_registrations,
                (unsigned long long)packet_stats.deformation_hits,
                (unsigned long long)packet_stats.deformation_overrides,
                (unsigned long long)packet_stats.deformation_misses,
                (unsigned long long)packet_stats.deformation_incompatible,
                (unsigned long long)packet_stats.deformation_collisions,
                (unsigned long long)packet_stats.deformation_color_hits,
                (unsigned long long)packet_stats.deformation_color_overrides,
                (unsigned long long)
                    packet_stats.particle_deformation_hits,
                (unsigned long long)
                    packet_stats.particle_deformation_overrides,
                (unsigned long long)packet_stats.particle_color_hits,
                (unsigned long long)packet_stats.particle_color_overrides,
                (unsigned long long)packet_stats.primitive_alpha_hits,
                (unsigned long long)packet_stats.primitive_alpha_overrides,
                (unsigned long long)
                    packet_stats.particle_primitive_alpha_hits,
                (unsigned long long)
                    packet_stats.particle_primitive_alpha_overrides,
                (unsigned long long)packet_stats.effect_registrations,
                (unsigned long long)packet_stats.effect_hits,
                (unsigned long long)packet_stats.effect_overrides,
                (unsigned long long)packet_stats.effect_misses,
                (unsigned long long)packet_stats.effect_collisions,
                packet_stats.deformation_peak);
    }
    fprintf(stderr,
            "[PRESENTSCHED-SUMMARY] entries=%llu ticks=%llu presents=%d "
            "simticks=%d issued=%llu pending=%llu interp=%llu "
            "interpviews=%llu stale=%llu elided=%llu zerodue=%llu "
            "multidue=%llu lead=%lld lag=%lld "
            "catchup=%llu skips=%llu rebases=%llu blocked=%llu "
            "maxpending=%llu fieldhz=%u tickfields=%u "
            "updates=%llu updatebad=%llu updatemin=%u updatemax=%u "
            "presentkind=%d presentrate=%u requestedkind=%d "
            "requestedrate=%u bootstrap=%u\n",
            (unsigned long long)s_entries,
            (unsigned long long)host_frame_driver_clock_ticks(&s_driver),
            g_frameCounter, g_simTickCounter,
            (unsigned long long)host_frame_driver_issued_ticks(&s_driver),
            (unsigned long long)host_frame_driver_pending_ticks(&s_driver),
            (unsigned long long)s_interpolated,
            (unsigned long long)s_interpolated_views,
            (unsigned long long)s_stale,
            (unsigned long long)s_elided,
            (unsigned long long)s_zero_due,
            (unsigned long long)s_multi_due,
            s_worst_lead, s_worst_lag,
            (unsigned long long)s_driver.clock.stats.catchup_steps,
            (unsigned long long)s_driver.clock.stats.render_skips_owed,
            (unsigned long long)s_driver.clock.stats.rebases,
            (unsigned long long)s_driver.stats.blocked_advances,
            (unsigned long long)s_driver.stats.max_pending_tickets,
            s_driver.clock.field_hz,
            host_frame_driver_fields_per_tick(&s_driver),
            (unsigned long long)s_game_updates,
            (unsigned long long)s_bad_game_updates,
            s_game_update_min, s_game_update_max,
            (int)present_sched_present_kind(),
            present_sched_present_rate(),
            (int)present_sched_present_requested_kind(),
            present_sched_present_requested_rate(), s_bootstrap_update);
    fflush(stderr);
}
