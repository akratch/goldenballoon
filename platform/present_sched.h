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

/*
 * How the latched policy wants the backend to retire a present, given the
 * refresh of the display the window is on (0 when the host has no such number).
 * Neither value permits tearing; Video.AllowTearing is the only thing that does,
 * and it is a separate player choice that applies to every policy.
 */
MdkrPresentSync present_sched_present_sync(unsigned display_rate);
bool present_sched_allow_tearing(void);
const char *present_sched_present_policy_name(void);
const char *present_sched_present_requested_policy_name(void);

/* True when a presentation subloop and Interpolated motion smoothing need an
 * immutable authored task. Internal endpoint controls also use this seam. The
 * renderer checks it before paying for snapshot/task retention, so Original or
 * smoothing-off play pays only cached policy checks. */
bool present_sched_replay_armed(void);

/* True only for the exact public values interpolate/1/on or the versioned
 * internal negative-control arm. Unknown values fail closed. A host opportunity
 * for which immutable replay cannot produce a complete image holds without a
 * swap or submission. */
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
/* One production or internal-control interpolated image was submitted,
 * carrying `viewports` substituted camera view-projections. */
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
 * WHAT THE SECTIONS ARE. They partition host pacing and immutable replay.
 * Replay-specific sections remain zero unless Interpolated smoothing is armed:
 *   SNAPSHOT  the per-TICK authoritative publish walk (presentation_snapshot_
 *             capture) — paid once per tick whether or not the subloop runs.
 *   FREEZE    the per-real-walk matrix-registry freeze (gfx_end_frame).
 *   INTERP    building the interpolated view-projections for one present.
 *   REPLAY    re-walking the held display list for one present.
 *   PRESENT   platform_frame_sync for the TICK's own present.
 *   IPRESENT  frame boundary for an extra host opportunity (a no-swap hold or
 *             a submitted production/control midpoint).
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

/* ---- pacing-quality census (M2) ----------------------------------------- *
 *
 * WHAT IT MEASURES. Presentation quality as three distributions rather than
 * one average, so a gate can talk about the tail a player actually notices:
 *   present-interval    wall microseconds between consecutive PRESENT
 *                       OPPORTUNITIES, displayed or held. This is the pacer's
 *                       own cadence.
 *   displayed-interval  wall microseconds between consecutive frames that
 *                       actually reached the screen. A held (no-swap) present
 *                       is absent from this series by construction, so a run
 *                       that holds every other opportunity shows the doubled
 *                       interval the player sees rather than the pacer's even
 *                       one.
 *   alpha-delta         interpolation phase advanced between consecutive
 *                       DISPLAYED frames, in parts-per-million of one
 *                       authoritative tick. Even spacing here is what makes
 *                       interpolated motion read as smooth; the series is the
 *                       monotone (ticks, alpha) phase differenced, so a tick
 *                       boundary is an ordinary advance rather than a wrap.
 *
 * WHY A HISTOGRAM. A fixed bucket array is the only shape that yields p95/p99
 * in bounded memory: keeping samples would grow without limit across a session,
 * and a running mean cannot see the tail at all. The bins are linear so the
 * resolution is uniform where the interesting mass sits.
 *
 * COST WHEN OFF. Gated on present_perf_enabled() exactly like [PRESENTPERF]:
 * one cached getenv, and the hook returns before reading the clock. The bins
 * are static storage, so an unarmed run pays no allocation either.
 *
 * IT ONLY READS. Nothing here feeds back into pacing, present, or simulation
 * state; the hooks take what the path already computed and accumulate.
 */

/*
 * One present opportunity. `displayed` is true only when the frame was actually
 * swapped to the surface. `alpha_num`/`alpha_den` are the interpolation phase
 * the present was drawn at, and are read only when `displayed`.
 */
void present_perf_note_present(bool displayed, uint64_t alpha_num,
                               uint64_t alpha_den);
/*
 * GPU frames in flight sampled AT a present, for the queue-depth latency
 * proxy. The renderer backend already maintains this counter for backpressure
 * admission; this is a read of it at the present boundary, not a new one.
 */
void present_perf_note_queue_depth(unsigned in_flight);

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
 * WHY THESE SETTERS ARE NOT, BY THEMSELVES, "LIVE". An early version of this
 * comment claimed they were -- that overwriting the cached ints made the keys
 * "genuinely LIVE, not merely read once at boot dressed up as LIVE". That was
 * wrong, and the reason is worth keeping: overwriting these two cached ints is
 * NECESSARY for a mid-run change to take effect and nowhere near SUFFICIENT,
 * because the consumers downstream of them latch. present_pace_lazy_init()
 * (platform_sdl_min.c) resolves the present policy once into its deadline state
 * and never revisits it, so engaging late costs the freeze/snapshot work with
 * no subloop to spend it on; and gfx_start_frame() stops refreshing
 * dkr_walk_entry_* the moment present_sched_replay_armed() goes false, while
 * the already-latched subloop keeps replaying -- memcpy'ing a stale segment
 * table into live HLE state.
 *
 * The keys ARE SCOPE_LIVE now, and none of the above stopped being true. What
 * changed is that these setters are no longer reachable from a settings panel
 * under a running engine at all. video_config.c's MDKR_VIDEO_APPLY_PRESENTATION
 * domain stages the change; platform_present_config_apply() calls these at the
 * host-frame boundary, after retiring the replay history and before re-latching
 * the pacer, as one ordered step. Calling them from anywhere else while the
 * engine runs would reintroduce exactly the hazard this paragraph describes.
 * See video_config.c's Video.FrameLimit row for the full argument.
 *
 * `value` is `original`, `display`, `uncapped`, or a validated integer cap
 * from 30 through 1000; smoothing is `off` or `interpolate`. Called only when
 * the resolved key did not come from the schema default (see
 * video_config_runtime.c), so an unset config still leaves the legacy raw-env
 * diagnostic path untouched.
 */
void mdkr_present_set_frame_limit(const char *value);
void mdkr_present_set_motion_smoothing(const char *value);
void mdkr_present_set_allow_tearing(const char *value);

/*
 * Re-arm the snapshot store's one-shot so the policy a live change is switching
 * TO gets its own arming decision rather than inheriting the one the previous
 * policy made. Called by platform_present_config_apply() only.
 */
void present_sched_replay_rearm(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_PRESENT_SCHED_H */
