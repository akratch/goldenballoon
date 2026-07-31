/*
 * present_sched.h — the presentation-side view of the authoritative clock
 * (spec §5/§13 Phase 3, design doc §1/§3, Wave B slice 0).
 *
 * WHY THIS EXISTS. Today one entry into the video-queue retrace branch
 * (stubs_dkr.c) is, by construction, exactly one authoritative tick AND
 * exactly one present: platform_vi_pace_measure() floors the frame to the
 * cadence minimum (2 source fields under `original`), so the two rates cannot
 * diverge. Wave B's goal is to let the present rate exceed the tick rate. The
 * first step is to grow a second, INDEPENDENT opinion about when a tick is due
 * — a SimSched accumulator — and prove it agrees with the pacer 1:1 before it
 * is ever allowed to drive anything (design §6, slice 0 gate).
 *
 * THE CLOCK SOURCE. The accumulator is fed the pacer's own committed field
 * count (g_viLastWallFields), not a second reading of the host clock. Two
 * independent samples of CLOCK_MONOTONIC would disagree by the time spent
 * between them and the accumulator would slowly lead or lag the pacer for
 * reasons that have nothing to do with the design under test. Feeding the
 * pacer's committed count means any divergence this module reports is a real
 * modelling difference, not clock skew — and, because a field is exactly 1e9
 * accumulator units, the arithmetic is exact (see sim_sched_advance_units).
 *
 * MDKR_VI_PACE=off deliberately lies about updateRate (g_viLastFields becomes
 * 1) while leaving the true elapsed count in g_viLastWallFields. The
 * accumulator follows the TRUE count: it models wall time, not what the game
 * is told.
 *
 * COST WHEN OFF. Slice 0 is measurement only. Nothing here changes what is
 * presented; the telemetry row is behind one cached getenv.
 */
#ifndef MDKR_PRESENT_SCHED_H
#define MDKR_PRESENT_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An authoritative tick is two source VI fields (LOGIC_30FPS), matching
 * pacing_policy.c's `original` minimum and sim_sched's 2e9-unit tick. */
#define PRESENT_SCHED_FIELDS_PER_TICK 2u

/*
 * Advance the parallel accumulator by one measured frame and return the
 * authoritative steps it believes are due (unbounded; the containment loop
 * still performs exactly one). Call once per video-queue branch entry,
 * immediately after platform_vi_pace_measure().
 */
unsigned present_sched_advance_fields(unsigned fields);

/* Interpolation alpha as sim_sched's exact rational (spec §7). */
void present_sched_alpha(uint64_t *numerator, uint64_t *denominator);

/* Total authoritative ticks the accumulator has issued since process start. */
uint64_t present_sched_ticks(void);

/*
 * Authoritative tick index, counted the way g_frameCounter counts presents:
 * bumped after the tick's present completes. While one tick == one present
 * these two are identical, which is precisely what slice 0's gate asserts.
 */
extern int g_simTickCounter;

/* ---- presentation replay / subloop seam (slices 1-2) --------------------- */

/*
 * MDKR_PRESENT_RATE=N — presents per second the subloop should aim for. 0 (the
 * default) keeps the historical one-present-per-tick behaviour exactly. Wave C
 * replaces this with the real Video.FrameLimit config key; the seam stays.
 */
unsigned present_sched_present_rate(void);

/*
 * True when something wants the captured display list re-walked: either the
 * zero-delta replay harness (MDKR_TEST_REPLAY_WALK=1, slice 1) or a present
 * rate above the authoritative tick rate (slice 2). The renderer checks this
 * before paying for the registry freeze, so an unarmed build pays one cached
 * getenv for the whole run.
 */
bool present_sched_replay_armed(void);

/*
 * MDKR_PRESENT_SMOOTHING=off — present at the requested rate but repeat the
 * tick's own image instead of interpolating (spec §11's
 * Video.MotionSmoothing=off). Also the positive control the smoothness gate
 * needs: with smoothing off the intermediate frames must be byte-identical to
 * the tick frames bracketing them, which is exactly what proves that with
 * smoothing on their difference is the interpolation and not noise.
 */
bool present_sched_smoothing_enabled(void);

/*
 * MDKR_TEST_REPLAY_WALK — slice 1's zero-delta harness.
 *
 *   1          re-walk every tick's list once with the captured
 *              view-projection, changing nothing. The replay uses the display
 *              list's own matrices, so the redraw is exact by construction.
 *   recompose  the same, but ALSO force every registered matrix through the
 *              world x view_projection recomposition slice 2 depends on, with
 *              the view-projection still unchanged. This is the arm that
 *              measures the recomposition itself; it is expected to differ
 *              from the list by at most the re-association error of
 *              mtx_head_rotation's decomposition (see gfx_pc_dkr.c's G_MTX
 *              replay branch), and the gate bounds that.
 */
bool present_sched_test_replay_walk(void);
bool present_sched_test_force_recompose(void);

/*
 * Telemetry (MDKR_PRESENT_SCHED_TRACE=1). One `[PRESENTSCHED]` row per branch
 * entry plus `[REPLAY-SUMMARY]` and `[PRESENTSCHED-SUMMARY]` rows at exit,
 * carrying the accumulator's tick total beside g_frameCounter so their
 * agreement is machine-checkable (tests/check_presentation_matrix.py) and the
 * replay's matrix/freeze counters beside them (tests/check_render_purity.py).
 */
/* One interpolated present went out, carrying `viewports` substituted camera
 * view-projections (0 == the snapshot pair could not be interpolated, so the
 * frame redrew the tick's own camera). */
void present_sched_note_interpolated(unsigned viewports);
/* An interpolated present drew NOTHING, so the tick's own image stays on
 * screen. Three causes, all counted here: the held display list was stale (the
 * pass submitted no graphics task), motion smoothing is off, or the replay walk
 * itself refused (no frozen registry / no walk-entry state). The present does
 * NOT swap in this case -- see platform_frame_sync_no_swap. */
void present_sched_note_stale(void);

bool present_sched_trace_enabled(void);
void present_sched_trace_entry(unsigned fields, unsigned due, int frame_counter);
void present_sched_trace_summary(void);

/* ---- present-path cost census (MDKR_PRESENT_PERF=1) ---------------------- *
 *
 * WHY A SEPARATE SEAM FROM THE TRACE. present_sched_trace_entry() writes one
 * line per branch entry; measuring against it would measure the fprintf. This
 * census only accumulates, and prints one `[PRESENTPERF]` row at exit.
 *
 * WHAT IT COSTS WHEN OFF. present_perf_enabled() is one cached getenv, and
 * every call site is wrapped so an unarmed build never reads the clock. The
 * scope() helper returns 0 when disabled, which present_perf_add() then treats
 * as "no sample", so the sections cannot accumulate garbage from a build that
 * armed the flag mid-run (it cannot: the flag is cached at first use).
 *
 * WHAT THE SECTIONS ARE. They partition the work Phase 3 Wave B ADDED, so the
 * incremental cost of the interpolated presentation is the sum of them:
 *   SNAPSHOT  the per-TICK authoritative publish walk (presentation_snapshot_
 *             capture) — paid once per tick whether or not the subloop runs.
 *   FREEZE    the per-real-walk matrix-registry freeze (gfx_end_frame).
 *   INTERP    building the interpolated view-projections for one present.
 *   REPLAY    re-walking the held display list for one present.
 *   PRESENT   platform_frame_sync for the TICK's own present.
 *   IPRESENT  platform_frame_sync for an INTERPOLATED present.
 * TICKWALL is the whole retrace-branch entry, so PRESENT + the subloop's
 * per-present sections are bounded by it and the residue is the pacer's sleep.
 */
typedef enum PresentPerfSection {
    PRESENT_PERF_SNAPSHOT = 0,
    PRESENT_PERF_FREEZE,
    PRESENT_PERF_INTERP,
    PRESENT_PERF_REPLAY,
    PRESENT_PERF_PRESENT,
    PRESENT_PERF_IPRESENT,
    PRESENT_PERF_TICKWALL,
    PRESENT_PERF_SECTION_COUNT
} PresentPerfSection;

bool present_perf_enabled(void);
/* Monotonic nanoseconds, or 0 when the census is disarmed. */
uint64_t present_perf_now(void);
/* Accumulate now-start into `section`. A `start` of 0 is ignored. */
void present_perf_add(PresentPerfSection section, uint64_t start);
/*
 * One rejected recomposition, with the worst per-element distance between the
 * verification matrix and the display list's own, in s15.16 LSBs. Bucketed so
 * the reject population can be read as "float re-association noise" or "a
 * different association entirely" rather than one averaged number.
 */
void present_perf_note_matrix_reject(uint64_t worst_lsb);
void present_perf_summary(void);

/* ---- Video.FrameLimit / Video.MotionSmoothing config push (Wave C) ------- *
 *
 * present_sched_present_rate() and present_sched_smoothing_enabled() above are
 * a CACHED-ONCE getenv by design (see "COST WHEN OFF"): an unarmed run pays
 * one getenv for the whole process. That is exactly what makes them unsafe to
 * treat as reread-every-frame config, and exactly why video_config_runtime.c
 * calls these two instead of relying on MDKR_PRESENT_RATE/MDKR_PRESENT_SMOOTHING
 * being set as real environment variables: setenv() after the first read would
 * not be seen at all. These overwrite the cached value directly, so the value
 * a config file / CLI flag / launcher / schema-recognised env var resolved
 * reaches the seam at boot, through the same mdkr_video_config_publish() hook
 * every other key uses.
 *
 * CORRECTION (ship review): an earlier version of this comment claimed the two
 * keys were therefore "genuinely LIVE, not merely read once at boot dressed up
 * as LIVE". That was wrong, and both keys are SCOPE_RESTART now. Overwriting
 * these two cached ints is necessary for a mid-run change to take effect but
 * nowhere near sufficient: the consumers downstream of them latch.
 * present_pace_lazy_init() (platform_sdl_min.c) resolves the present period
 * once into s_presentFields and never revisits it, so engaging late costs the
 * freeze/snapshot work with no subloop to spend it on; and gfx_start_frame()
 * stops refreshing dkr_walk_entry_* the moment present_sched_replay_armed()
 * goes false, while the already-latched subloop keeps replaying -- memcpy'ing
 * a stale segment table into live HLE state. See video_config.c's schema rows
 * for the full argument. Making the seams re-resolvable belongs to the
 * live/interactive slice (design doc §6 slice 3).
 *
 * `value` is one of "original"|"60" for the frame limit, "off"|"interpolate"
 * for smoothing -- video_config.c's schema validator guarantees only those
 * strings ever reach here. Called only when the resolved key did NOT come
 * from the schema default (see video_config_runtime.c), so a raw diagnostic
 * MDKR_PRESENT_RATE=30/120/etc. that this schema does not recognise -- and
 * therefore never touches the config key at all -- keeps reaching
 * present_sched_present_rate()'s own getenv path completely untouched.
 */
void mdkr_present_set_frame_limit(const char *value);
void mdkr_present_set_motion_smoothing(const char *value);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_PRESENT_SCHED_H */
