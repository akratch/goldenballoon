/*
 * present_sched.h — presentation-side facade for the authoritative host-frame
 * driver.
 *
 * WHY THIS EXISTS. SimSched started as a parallel clock opinion and was
 * promoted only after its exact agreement gate passed. HostFrameDriver now
 * issues fixed-size game-loop tickets while this facade owns the presentation
 * controls, replay telemetry, and collapsed-libultra adapter API. Presentation
 * count may diverge from tick count; ticket width may not.
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
 * COST OF TELEMETRY. Trace rows remain behind one cached getenv. The driver and
 * ticket adapter themselves are shipping clock policy, not diagnostics.
 */
#ifndef MDKR_PRESENT_SCHED_H
#define MDKR_PRESENT_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#include "pacing_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Advance the authoritative driver by one committed host-time sample and
 * return newly due fixed-step tickets. Ordinary multi-tick debt is retained
 * until the adapter consumes it; `rebase` retires suspension time.
 */
unsigned present_sched_advance_fields(unsigned fields, bool rebase);
unsigned present_sched_advance_units(uint64_t units, bool rebase);

/* Fixed-step ticket adapter used by the collapsed libultra message loop. */
bool present_sched_take_tick(void);
uint64_t present_sched_pending_ticks(void);
uint64_t present_sched_issued_ticks(void);
unsigned present_sched_tick_fields(void);

/* Logical ticket assignment for host input captured at the current host
 * opportunity. During a burst, events belong to the last due interval; while
 * waiting between ticks, they belong to the next interval. */
uint64_t present_sched_input_target_tick(void);

/* Catch-up passes with another already-due ticket behind them may build the
 * latest list but must not submit/present an intermediate image. */
bool present_sched_should_elide_render(void);
void present_sched_set_render_elided(bool elided);
void present_sched_set_surface_elided(bool elided);
bool present_sched_render_elided(void);
void present_sched_note_render_elided(void);

/* Observe both clock authority and gameplay semantics. `ticket_width` is the
 * fixed HostFrameDriver ticket; `effective_rate` is the value input/menu/game/
 * audio/transition consume after the measured Original-cadence bootstrap
 * compatibility phase. Tick zero is reported separately. */
void present_sched_note_game_update(unsigned ticket_width,
                                    unsigned effective_rate);

/* Interpolation alpha as sim_sched's exact rational (spec §7). */
void present_sched_alpha(uint64_t *numerator, uint64_t *denominator);

/* Total authoritative ticks the clock has made due since process start. */
uint64_t present_sched_ticks(void);

/*
 * Authoritative tick index, independent of g_frameCounter's host-opportunity
 * count. Bumped after each complete fixed-step game pass.
 */
extern int g_simTickCounter;

/* ---- presentation replay / subloop seam (slices 1-2) --------------------- */

/*
 * MDKR_PRESENT_RATE accepts `original`, exact integer caps from 30 through
 * 1000, `display`, and `uncapped`. `original` keeps the historical one-present-
 * per-tick behaviour. Numeric caps use an absolute rational deadline grid;
 * display and uncapped policies are resolved by the platform backend.
 */
unsigned present_sched_present_rate(void);
MdkrPresentPolicyKind present_sched_present_kind(void);
unsigned present_sched_present_requested_rate(void);
MdkrPresentPolicyKind present_sched_present_requested_kind(void);
bool present_sched_present_subloop(void);
bool present_sched_backend_vsync_enabled(void);
const char *present_sched_present_policy_name(void);
const char *present_sched_present_requested_policy_name(void);

/*
 * True only when an explicit internal test seam wants the captured display
 * list re-walked. Production 1.0.1 never arms delayed replay from a frame-rate
 * or smoothing setting. The renderer checks this before paying for snapshot
 * and registry retention, so ordinary play pays only cached policy checks.
 */
bool present_sched_replay_armed(void);

/*
 * Production 1.0.1 always returns false. The public smoothing setting is
 * fail-closed because a delayed list walk cannot yet retain every mutable
 * dependency. Explicit internal replay seams can enable this for adversarial
 * diagnostics; host opportunities without a new image never swap or submit.
 */
bool present_sched_smoothing_enabled(void);

/* All replay test flags are inert unless this exact versioned capability is
 * present: MDKR_INTERNAL_TEST_TOKEN=mdkr64-presentation-replay-v1. */
bool present_sched_internal_replay_test_enabled(void);

/*
 * MDKR_TEST_REPLAY_WALK — slice 1's zero-delta harness. Also requires the
 * versioned MDKR_INTERNAL_TEST_TOKEN above.
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
/* One internal-test interpolated image was submitted, carrying `viewports`
 * substituted camera view-projections. Production 1.0.1 never calls this. */
void present_sched_note_interpolated(unsigned viewports);
/* A tick-boundary image was exposed. `replayed` is reserved for the delayed
 * replay negative control; production exposes the completed real walk. */
void present_sched_note_endpoint(bool replayed, uint64_t authored_tick);
/* No new image was produced for this host opportunity, so the prior authored
 * image stays on screen. The boundary does not swap or submit; explicit replay
 * diagnostics also use this when a retained walk safely refuses. */
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
 * WHAT THE SECTIONS ARE. They partition host pacing and the quarantined replay
 * mechanism. Replay-specific sections remain zero in production 1.0.1:
 *   SNAPSHOT  the per-TICK authoritative publish walk (presentation_snapshot_
 *             capture) — paid once per tick whether or not the subloop runs.
 *   FREEZE    the per-real-walk matrix-registry freeze (gfx_end_frame).
 *   INTERP    building the interpolated view-projections for one present.
 *   REPLAY    re-walking the held display list for one present.
 *   PRESENT   platform_frame_sync for the TICK's own present.
 *   IPRESENT  frame boundary for an extra host opportunity (a no-swap hold in
 *             production; a submitted midpoint only under the test seam).
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
 * present_pace_lazy_init() (platform_sdl_min.c) resolves the present policy
 * once into its deadline state and never revisits it, so engaging late costs the
 * freeze/snapshot work with no subloop to spend it on; and gfx_start_frame()
 * stops refreshing dkr_walk_entry_* the moment present_sched_replay_armed()
 * goes false, while the already-latched subloop keeps replaying -- memcpy'ing
 * a stale segment table into live HLE state. See video_config.c's schema rows
 * for the full argument. Making the seams re-resolvable belongs to the
 * live/interactive slice (design doc §6 slice 3).
 *
 * `value` is `original`, `display`, `uncapped`, or a validated integer cap
 * from 30 through 1000; smoothing is `off` or `interpolate`. Called only when
 * the resolved key did not come from the schema default (see
 * video_config_runtime.c), so an unset config still leaves the legacy raw-env
 * diagnostic path untouched.
 */
void mdkr_present_set_frame_limit(const char *value);
void mdkr_present_set_motion_smoothing(const char *value);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_PRESENT_SCHED_H */
