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

int mdkr_pacing_cadence_valid(const char *value);
int mdkr_pacing_min_fields(const char *value);
int mdkr_pacing_field_hz(int source_field_hz, const char *diagnostic_override);
int mdkr_pacing_synthetic_fields(int requested_fields, int min_fields,
                                 int max_fields);
int mdkr_pacing_queue_refill(int pending_fields, int measured_fields,
                             int capacity);

int mdkr_present_policy_parse(const char *value, MdkrPresentPolicy *out);
int mdkr_present_policy_equal(const MdkrPresentPolicy *left,
                              const MdkrPresentPolicy *right);
int mdkr_present_policy_uses_vsync(const MdkrPresentPolicy *policy);
int mdkr_present_policy_needs_subloop(const MdkrPresentPolicy *policy,
                                      unsigned tick_rate);

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
