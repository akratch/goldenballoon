/*
 * Pure simulation-cadence policy and clock. Kept independent of SDL and the
 * game so field-rate selection and the actual pacing mechanism can be tested
 * with injected timestamps.
 */
#ifndef MDKR64_PACING_POLICY_H
#define MDKR64_PACING_POLICY_H

#include <stdint.h>

#define MDKR_PACING_MIN_FIELD_HZ 20
#define MDKR_PACING_MAX_FIELD_HZ 240
#define MDKR_PACING_MAX_FIELDS 6
#define MDKR_PACING_STALL_REBASE_NS UINT64_C(200000000)
#define MDKR_PRESENT_RATE_MIN 30u
#define MDKR_PRESENT_RATE_MAX 1000u

typedef enum MdkrPresentPolicyKind {
    MDKR_PRESENT_ORIGINAL = 0,
    MDKR_PRESENT_CAPPED = 1,
    MDKR_PRESENT_DISPLAY = 2,
    MDKR_PRESENT_UNCAPPED = 3,
} MdkrPresentPolicyKind;

typedef struct MdkrPresentPolicy {
    MdkrPresentPolicyKind kind;
    unsigned rate;
} MdkrPresentPolicy;

/*
 * How a latched policy wants the backend to retire a present. BLOCKING is the
 * vblank queue every backend guarantees; LATEST asks for a queue that replaces
 * an undisplayed image instead of stalling the caller, which is what a rate
 * ABOVE the display can use and a blocking queue cannot serve. Both are
 * vblank-synchronized: neither value ever means a torn scanout, which is a
 * separate explicit opt-in the player owns.
 */
typedef enum MdkrPresentSync {
    MDKR_PRESENT_SYNC_BLOCKING = 0,
    MDKR_PRESENT_SYNC_LATEST = 1
} MdkrPresentSync;

/* Absolute rational deadline grid for a numeric presentation cap. */
typedef struct MdkrPresentDeadlineClock {
    uint64_t origin_ns;
    uint64_t next_index;
    unsigned rate;
    int initialized;
} MdkrPresentDeadlineClock;

typedef struct MdkrPacingClock {
    uint64_t origin_ns;
    uint64_t grid_fields;
    uint64_t next_grid_ns;
    int field_hz;
    int min_fields;
    int max_fields;
    int initialized;
} MdkrPacingClock;

/* Preserve the N64 COUNTER's strictly-increasing read contract without
 * rejecting a valid first sample whose high bit happens to be set. */
typedef struct MdkrCounterGuard {
    uint32_t last;
    int initialized;
} MdkrCounterGuard;

int mdkr_pacing_cadence_valid(const char *value);
int mdkr_pacing_min_fields(const char *value);
int mdkr_pacing_field_hz(int source_field_hz, const char *diagnostic_override);
int mdkr_pacing_synthetic_fields(int requested_fields, int min_fields,
                                 int max_fields);
int mdkr_pacing_queue_refill(int pending_fields, int measured_fields,
                             int capacity);
int mdkr_pacing_interval_requires_rebase(uint64_t elapsed_ns);
uint32_t mdkr_counter_guard_commit(MdkrCounterGuard *guard, uint32_t sample);

int mdkr_present_policy_parse(const char *value, MdkrPresentPolicy *out);
int mdkr_present_policy_equal(const MdkrPresentPolicy *left,
                              const MdkrPresentPolicy *right);
/* `display_rate` is the detected refresh of the display the window is on, or 0
 * when the host has no such number (the browser measures rAF instead). */
MdkrPresentSync mdkr_present_policy_sync(const MdkrPresentPolicy *policy,
                                         unsigned display_rate);
int mdkr_present_policy_needs_subloop(const MdkrPresentPolicy *policy,
                                      unsigned tick_rate);
int mdkr_present_policy_needs_held_frame_deadline(
    const MdkrPresentPolicy *policy, int smoothing_enabled);

/*
 * Quantize an interpolation phase onto the grid the DISPLAY retires presents
 * on (M3 slice 1).
 *
 * WHY. Interpolation alpha is measured from the wall clock at frame-BUILD
 * time, but the image is scanned out at the next vblank. Under a
 * vblank-quantized queue those two differ by the pacer's wake jitter -- the
 * FIFO/compositor slop between the vblank and the loop actually running -- and
 * that jitter lands directly in the phase the player sees, because uneven
 * phase is exactly what interpolated motion shows as stutter. The frames
 * themselves arrive on a perfectly regular grid; only the SAMPLING of time is
 * noisy.
 *
 * WHAT. `phase_units` is the measured phase and `tick_units` the authoritative
 * tick, both in the accumulator's exact units (see present_sched.h). Under a
 * queue that retires one present per refresh, the frame being built now is
 * displayed one refresh after the frame before it, so its phase is an exact
 * whole number of refresh periods -- `quantum_units` -- past the tick's own
 * endpoint. Rounding the measured phase to that grid recovers the number the
 * display will actually show it at. The constant one-refresh pipeline latency
 * is common to every frame and cancels in the difference, so this corrects the
 * phase's EVENNESS without adding or removing latency.
 *
 * WHAT IT DOES NOT DO. Nothing here feeds the accumulator. The tick that a
 * given host opportunity belongs to, and the moment ticks become due, are
 * untouched: this is a presentation-alpha sampling change only, which is what
 * the byte-identity gates (tests/check_arbitrary_presentation_rates.py) prove.
 *
 * WHEN IT DECLINES. `quantum_units` of 0 means the caller's presents are not
 * vblank-quantized at all (synthetic pacing, a software cadence cap, a tearing
 * queue, an unreported refresh) and the measured phase is returned unchanged.
 * So is a phase of 0 -- that is a tick endpoint, whose alpha is exact by
 * construction -- and any projection that lands at or past the next tick, where
 * the clock is the better authority.
 *
 * MONOTONICITY. Within one tick the result never runs backwards. Rounding is
 * monotone, and a later present that declines the grid declines it only from a
 * strictly higher grid index, so its measured phase is at least half a quantum
 * past the last quantized value. That matters because the pacing-quality census
 * gates `regressions=0` on the differenced phase series.
 */
uint64_t mdkr_present_quantize_phase(uint64_t phase_units,
                                     uint64_t tick_units,
                                     uint64_t quantum_units);

int mdkr_present_deadline_init(MdkrPresentDeadlineClock *clock,
                               unsigned rate);
uint64_t mdkr_present_deadline_target(MdkrPresentDeadlineClock *clock,
                                      uint64_t now_ns);
void mdkr_present_deadline_commit(MdkrPresentDeadlineClock *clock,
                                  uint64_t now_ns);

/*
 * The first grid point strictly after `now_ns`, on the same absolute rational
 * grid — for a caller that uses the grid as a FLOOR rather than a cadence.
 *
 * WHY THIS IS NOT target()/commit(). That pair is a schedule: it hands out
 * consecutive indices and expects the caller to reach every one of them, so
 * commit()'s "the deadline was met" branch advances by exactly one. A floor is
 * the opposite shape — most opportunities do not wait on it at all, and the
 * ones that do must not inherit an index left behind by the ones that did not.
 * Committing on every opportunity over-advances the index; committing on only
 * the waiting ones leaves it stale and the wait becomes a no-op. Projecting
 * from `now_ns` each time has neither failure: the grid's PHASE is fixed at the
 * anchor, so a run cannot accumulate drift, and its index is never carried.
 *
 * The anchor is this clock's origin, set on the first call after
 * mdkr_present_deadline_init (which is also how a suspension or a display
 * change re-anchors it). Returns `now_ns` unchanged for a rate of 0.
 */
uint64_t mdkr_present_grid_next(MdkrPresentDeadlineClock *clock,
                                uint64_t now_ns);

int mdkr_pacing_clock_init(MdkrPacingClock *clock, int field_hz,
                           int min_fields, int max_fields);
uint64_t mdkr_pacing_clock_target(MdkrPacingClock *clock, uint64_t now_ns);
int mdkr_pacing_clock_commit(MdkrPacingClock *clock, uint64_t now_ns,
                             int *rebased);

#endif
