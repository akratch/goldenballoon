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

/*
 * Classify the retirement cadence behind a blocking present queue.
 *
 * A fixed-refresh queue has a narrow interval distribution even when the host
 * wakes a little late. A VRR queue follows the application's varying delivery
 * times and therefore has a materially wider distribution. The classifier
 * trims isolated scheduling stalls, uses separate enter/leave thresholds, and
 * requires sustained contrary evidence before changing an established state.
 * It is intentionally a pure policy object: platform code feeds the same
 * interval it already measured for pacing, while unit tests can exercise the
 * decision without inventing a display.
 */
#define MDKR_PRESENT_INTERVAL_WINDOW 32u
#define MDKR_PRESENT_INTERVAL_TRIM 4u
#define MDKR_PRESENT_INTERVAL_VARIABLE_PPM UINT64_C(2500)
#define MDKR_PRESENT_INTERVAL_FIXED_PPM UINT64_C(900)
#define MDKR_PRESENT_INTERVAL_VARIABLE_CONFIRM 4u
#define MDKR_PRESENT_INTERVAL_FIXED_CONFIRM 16u

typedef enum MdkrPresentIntervalKind {
    MDKR_PRESENT_INTERVAL_WARMING = 0,
    MDKR_PRESENT_INTERVAL_FIXED = 1,
    MDKR_PRESENT_INTERVAL_VARIABLE = 2
} MdkrPresentIntervalKind;

typedef struct MdkrPresentIntervalClassifier {
    uint64_t intervals_ns[MDKR_PRESENT_INTERVAL_WINDOW];
    uint64_t jitter_ppm;
    uint64_t transitions;
    unsigned count;
    unsigned head;
    unsigned contrary_count;
    MdkrPresentIntervalKind kind;
} MdkrPresentIntervalClassifier;

/*
 * How far below the display's own refresh `display-margin` sits, in Hz.
 *
 * WHY THREE. A variable-refresh display holds its adaptive range only while the
 * application keeps arriving BEFORE the panel's minimum frame time. Sit exactly
 * on the refresh and every ordinary scheduling hiccup crosses it, the panel
 * falls back to its fixed ceiling for that frame, and the resulting alternation
 * is the judder VRR exists to remove. A small fixed step is the usual remedy,
 * and it is fixed rather than proportional because the thing being absorbed is
 * host-side jitter -- a scheduler slip is roughly the same number of
 * microseconds at 120 Hz as at 240, so a proportional margin would give away
 * far too much headroom at the high end and not enough at the low.
 */
#define MDKR_PRESENT_DISPLAY_MARGIN_HZ 3u

typedef enum MdkrPresentPolicyKind {
    MDKR_PRESENT_ORIGINAL = 0,
    MDKR_PRESENT_CAPPED = 1,
    MDKR_PRESENT_DISPLAY = 2,
    MDKR_PRESENT_UNCAPPED = 3,
    /*
     * Follow the display, minus a small fixed margin. Like MDKR_PRESENT_DISPLAY
     * the player's choice carries no number: the rate is a property of the
     * monitor the window is currently on and is re-derived from it, including
     * when the window moves to another monitor.
     */
    MDKR_PRESENT_DISPLAY_MARGIN = 4,
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
/*
 * Closed-loop discipline for the software present-deadline clock.
 *
 * The deadline grid is open-loop against CLOCK_MONOTONIC; a real display
 * retires images on its own cadence, which is never exactly the reported
 * integer rate (119.88 vs 120) and, on adaptive panels, hops between rate
 * buckets. mdkr_present_deadline_feedback() closes the loop from the one
 * signal the presentation backend always has: how long the surface acquire
 * blocked before each present, plus whether it failed outright.
 *
 * Two nested controllers:
 *  - PHASE: each frame drifts the grid origin earlier by CREEP and later by
 *    a clamped fraction of any block beyond TARGET. Equilibrium holds the
 *    block near TARGET + (CREEP << GAIN_SHIFT): releases land slightly
 *    before retirement, the queue stays primed, and mismatches inside
 *    RERATE_PPM (e.g. the 1000ppm of a 119.88 panel) are absorbed with the
 *    exact integer-rational grid intact.
 *  - RATE: when consecutive display-bound frames (block > BOUND_MIN) show a
 *    period a sustained RERATE_PPM away from the grid's, the grid re-rates
 *    onto the measured period (8.8 fixed-point ns), snapping back to the
 *    exact integer-rational grid whenever the measurement returns to within
 *    SNAP_PPM of nominal. Paths that never call feedback keep period
 *    override zero and remain bit-for-bit on the legacy grid.
 */
#define MDKR_PRESENT_DISCIPLINE_TARGET_BLOCK_NS UINT64_C(1000000)
#define MDKR_PRESENT_DISCIPLINE_MAX_SLEW_NS     UINT64_C(200000)
#define MDKR_PRESENT_DISCIPLINE_CREEP_NS        UINT64_C(25000)
#define MDKR_PRESENT_DISCIPLINE_GAIN_SHIFT      3u
#define MDKR_PRESENT_DISCIPLINE_BOUND_MIN_NS    UINT64_C(200000)
#define MDKR_PRESENT_DISCIPLINE_RERATE_PPM      UINT64_C(1200)
#define MDKR_PRESENT_DISCIPLINE_SNAP_PPM        UINT64_C(600)
#define MDKR_PRESENT_DISCIPLINE_RERATE_CONFIRM  8u
#define MDKR_PRESENT_DISCIPLINE_EMA_SHIFT       3u

typedef struct MdkrPresentDeadlineClock {
    uint64_t origin_ns;
    uint64_t next_index;
    unsigned rate;
    int initialized;
    /* Discipline state; all zero (inert) until feedback is called. */
    uint64_t period_override_fp; /* 8.8 fixed-point ns; 0 = legacy grid */
    uint64_t period_ema_fp;      /* measured display period, 8.8 fp ns */
    uint64_t last_bound_ns;      /* completion time of last bound frame */
    uint64_t slew_total_ns;      /* cumulative |origin adjustment| */
    unsigned rerate_streak;
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

void mdkr_present_interval_reset(MdkrPresentIntervalClassifier *classifier);
void mdkr_present_interval_note(MdkrPresentIntervalClassifier *classifier,
                                uint64_t elapsed_ns);
const char *mdkr_present_interval_kind_name(MdkrPresentIntervalKind kind);

int mdkr_present_policy_parse(const char *value, MdkrPresentPolicy *out);
int mdkr_present_policy_equal(const MdkrPresentPolicy *left,
                              const MdkrPresentPolicy *right);

/*
 * The cadence `display-margin` resolves to for a display reporting
 * `display_rate` Hz: display_rate - MDKR_PRESENT_DISPLAY_MARGIN_HZ, floored at
 * MDKR_PRESENT_RATE_MIN so the result is always a rate the deadline grid and
 * the schema both accept.
 *
 * Returns 0 for a `display_rate` of 0 -- the host reports no refresh, there is
 * nothing to sit under, and the caller must fall back to plain `display`
 * behaviour rather than invent a number. A display at or below the floor
 * resolves to the floor, which is at or above its own refresh; that is the
 * honest answer, because a 30 Hz panel has no headroom to give back and the
 * blocking queue is already the limiter.
 */
unsigned mdkr_present_policy_display_margin_rate(unsigned display_rate);
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

/*
 * Slot-projected interpolation phase for a disciplined display cadence.
 *
 * Under a closed-loop FIFO pacer, one displayed frame IS one display slot,
 * so the drawn phase should advance by exactly one quantum per replay —
 * reading it off the wake clock instead bakes scheduler jitter into every
 * motion step (the measured p95 was 0.385 tick against an ideal constant
 * 0.25 at 120 Hz). This projector predicts last + quantum and keeps the
 * prediction whenever the measured phase agrees within SNAP; a larger
 * disagreement (a genuinely missed slot) re-anchors onto the measured grid
 * point and is counted. A prediction that would touch the tick boundary
 * clamps to tick_units - 1: the tick/display beat's extra slot becomes one
 * soft repeat of (nearly) the incoming endpoint image rather than a
 * mid-tick stutter.
 */
/*
 * The snap window separates wake noise from a genuinely missed display
 * slot. Under a blocking FIFO the glass cadence is even no matter how
 * noisy the CPU wakes are, so everything short of a whole-slot deviation
 * is noise to be rejected; a real miss shows up as a full quantum. 3/4
 * splits those: it absorbs the endpoint lead (<=8 ms cap, typically 3 ms)
 * plus worst-case scheduler jitter, while a +1.0-quantum miss still lands
 * outside and re-anchors.
 */
#define MDKR_PRESENT_SLOT_SNAP_NUM 3u
#define MDKR_PRESENT_SLOT_SNAP_DEN 4u

typedef struct MdkrPresentSlotState {
    uint64_t last_units;    /* last drawn phase, accumulator units */
    uint64_t last_measured; /* last measured phase, for the increment test */
    uint64_t last_tick;     /* census tick that phase belonged to */
    uint64_t snaps;         /* predicted-step frames (telemetry) */
    uint64_t anchors;       /* re-anchor events (telemetry) */
} MdkrPresentSlotState;

uint64_t mdkr_present_slot_phase(MdkrPresentSlotState *state, uint64_t tick,
                                 uint64_t measured_units,
                                 uint64_t tick_units,
                                 uint64_t quantum_units);

int mdkr_present_deadline_init(MdkrPresentDeadlineClock *clock,
                               unsigned rate);
uint64_t mdkr_present_deadline_target(MdkrPresentDeadlineClock *clock,
                                      uint64_t now_ns);
void mdkr_present_deadline_commit(MdkrPresentDeadlineClock *clock,
                                  uint64_t now_ns);
/*
 * Feed one presented frame's acquire observation back into the clock (see
 * the discipline comment above the struct). block_ns is how long the surface
 * acquire blocked before this present; unavailable is nonzero when the
 * acquire failed (queue overrun; the frame was dropped); now_ns is the
 * monotonic completion time of the acquire, used to sample the display's
 * real period across consecutive display-bound frames. Inert before the
 * first target() call. Nudges origin (and, on sustained evidence, the grid
 * period); never touches next_index.
 */
void mdkr_present_deadline_feedback(MdkrPresentDeadlineClock *clock,
                                    uint64_t block_ns, int unavailable,
                                    uint64_t now_ns);
/* Cumulative |origin adjustment| applied by feedback, for telemetry. */
uint64_t mdkr_present_deadline_slew_total(
    const MdkrPresentDeadlineClock *clock);
/* The grid period currently in force, whole nanoseconds (legacy rational
 * grid when no rate override is active). 0 for an invalid clock. */
uint64_t mdkr_present_deadline_period_ns(
    const MdkrPresentDeadlineClock *clock);

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
