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

int mdkr_present_deadline_init(MdkrPresentDeadlineClock *clock,
                               unsigned rate);
uint64_t mdkr_present_deadline_target(MdkrPresentDeadlineClock *clock,
                                      uint64_t now_ns);
void mdkr_present_deadline_commit(MdkrPresentDeadlineClock *clock,
                                  uint64_t now_ns);

int mdkr_pacing_clock_init(MdkrPacingClock *clock, int field_hz,
                           int min_fields, int max_fields);
uint64_t mdkr_pacing_clock_target(MdkrPacingClock *clock, uint64_t now_ns);
int mdkr_pacing_clock_commit(MdkrPacingClock *clock, uint64_t now_ns,
                             int *rebased);

#endif
