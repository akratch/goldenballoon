#include "pacing_policy.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_SECOND UINT64_C(1000000000)

static int present_ci_equal(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return 0;
    }
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

int mdkr_present_policy_parse(const char *value, MdkrPresentPolicy *out) {
    MdkrPresentPolicy parsed = { MDKR_PRESENT_ORIGINAL, 0u };
    char *end = NULL;
    unsigned long rate;

    if (value == NULL || out == NULL || value[0] == '\0') {
        return 0;
    }
    if (present_ci_equal(value, "original")) {
        *out = parsed;
        return 1;
    }
    if (present_ci_equal(value, "display")) {
        parsed.kind = MDKR_PRESENT_DISPLAY;
        *out = parsed;
        return 1;
    }
    if (present_ci_equal(value, "uncapped")) {
        parsed.kind = MDKR_PRESENT_UNCAPPED;
        *out = parsed;
        return 1;
    }
    {
        const unsigned char *digit = (const unsigned char *)value;
        while (*digit != '\0' && isdigit(*digit)) {
            digit++;
        }
        if (digit == (const unsigned char *)value || *digit != '\0') {
            return 0;
        }
    }
    errno = 0;
    rate = strtoul(value, &end, 10);
    if (errno != 0 || end == value || end == NULL || end[0] != '\0' ||
        rate < MDKR_PRESENT_RATE_MIN || rate > MDKR_PRESENT_RATE_MAX) {
        return 0;
    }
    parsed.kind = MDKR_PRESENT_CAPPED;
    parsed.rate = (unsigned)rate;
    *out = parsed;
    return 1;
}

int mdkr_present_policy_equal(const MdkrPresentPolicy *left,
                              const MdkrPresentPolicy *right) {
    return left != NULL && right != NULL && left->kind == right->kind &&
           left->rate == right->rate;
}

MdkrPresentSync mdkr_present_policy_sync(const MdkrPresentPolicy *policy,
                                         unsigned display_rate) {
    if (policy == NULL) {
        return MDKR_PRESENT_SYNC_BLOCKING;
    }
    if (policy->kind == MDKR_PRESENT_UNCAPPED) {
        return MDKR_PRESENT_SYNC_LATEST;
    }
    /* A cap at or below the refresh is served exactly by the blocking queue:
     * the deadline grid below already spaces the presents, and the queue drains
     * faster than that grid fills it, so it never becomes a second limiter. An
     * unknown refresh keeps the blocking queue rather than guessing. */
    if (policy->kind == MDKR_PRESENT_CAPPED && display_rate != 0u &&
        policy->rate > display_rate) {
        return MDKR_PRESENT_SYNC_LATEST;
    }
    return MDKR_PRESENT_SYNC_BLOCKING;
}

int mdkr_present_policy_needs_subloop(const MdkrPresentPolicy *policy,
                                      unsigned tick_rate) {
    if (policy == NULL || policy->kind == MDKR_PRESENT_ORIGINAL) {
        return 0;
    }
    if (policy->kind == MDKR_PRESENT_CAPPED) {
        return policy->rate > tick_rate;
    }
    return 1;
}

int mdkr_present_policy_needs_held_frame_deadline(
    const MdkrPresentPolicy *policy, int smoothing_enabled) {
    if (policy == NULL || policy->kind == MDKR_PRESENT_ORIGINAL ||
        smoothing_enabled) {
        return 0;
    }
    return policy->kind == MDKR_PRESENT_DISPLAY ||
           policy->kind == MDKR_PRESENT_UNCAPPED;
}

uint64_t mdkr_present_quantize_phase(uint64_t phase_units,
                                     uint64_t tick_units,
                                     uint64_t quantum_units) {
    uint64_t index;
    uint64_t quantized;

    if (quantum_units == 0u || tick_units == 0u || phase_units == 0u) {
        return phase_units;
    }
    /* A refresh at or slower than the tick has no sub-tick grid to project
     * onto: every opportunity is already an endpoint. */
    if (quantum_units >= tick_units) {
        return phase_units;
    }
    /* Nearest grid index. The pacer wakes just AFTER a vblank, so the measured
     * phase sits a little above its grid point and rounding (not flooring)
     * is what recovers the index the display used. */
    index = (phase_units + quantum_units / 2u) / quantum_units;
    if (index == 0u) {
        /* Inside the first half-refresh, yet past the endpoint that already
         * went out at phase zero. This opportunity is still the next grid
         * point; collapsing it onto the endpoint would show the same image
         * twice. */
        index = 1u;
    }
    quantized = index * quantum_units;
    if (quantized >= tick_units) {
        /* The projection crossed the tick boundary. The accumulator is the
         * authority on where a tick ends, so defer to the measured phase
         * rather than pin the frame to a boundary it has not reached. */
        return phase_units;
    }
    return quantized;
}

static uint64_t present_grid_time_ns(const MdkrPresentDeadlineClock *clock,
                                     uint64_t index) {
    uint64_t whole_seconds = index / (uint64_t)clock->rate;
    uint64_t remainder = index % (uint64_t)clock->rate;

    if (whole_seconds > (UINT64_MAX - clock->origin_ns) / NS_PER_SECOND) {
        return UINT64_MAX;
    }
    return clock->origin_ns + whole_seconds * NS_PER_SECOND +
           (remainder * NS_PER_SECOND) / (uint64_t)clock->rate;
}

int mdkr_present_deadline_init(MdkrPresentDeadlineClock *clock,
                               unsigned rate) {
    if (clock == NULL || rate < MDKR_PRESENT_RATE_MIN ||
        rate > MDKR_PRESENT_RATE_MAX) {
        return 0;
    }
    memset(clock, 0, sizeof(*clock));
    clock->rate = rate;
    return 1;
}

uint64_t mdkr_present_deadline_target(MdkrPresentDeadlineClock *clock,
                                      uint64_t now_ns) {
    if (clock == NULL || clock->rate == 0u) {
        return now_ns;
    }
    if (!clock->initialized) {
        clock->origin_ns = now_ns;
        clock->next_index = 1u;
        clock->initialized = 1;
    }
    return present_grid_time_ns(clock, clock->next_index);
}

uint64_t mdkr_present_grid_next(MdkrPresentDeadlineClock *clock,
                                uint64_t now_ns) {
    uint64_t elapsed_ns;
    uint64_t elapsed_intervals;

    if (clock == NULL || clock->rate == 0u) {
        return now_ns;
    }
    if (!clock->initialized) {
        clock->origin_ns = now_ns;
        clock->next_index = 1u;
        clock->initialized = 1;
        return present_grid_time_ns(clock, 1u);
    }
    elapsed_ns = now_ns > clock->origin_ns ? now_ns - clock->origin_ns : 0u;
    /* floor(elapsed * rate / 1e9), overflow-safe for host-time ranges. */
    elapsed_intervals =
        (elapsed_ns / NS_PER_SECOND) * (uint64_t)clock->rate +
        ((elapsed_ns % NS_PER_SECOND) * (uint64_t)clock->rate) /
            NS_PER_SECOND;
    {
        uint64_t next = present_grid_time_ns(clock, elapsed_intervals + 1u);
        if (next <= now_ns) {
            /* Grid times are truncated to whole nanoseconds, so `now` can land
             * exactly on the point this index names. A floor that returns the
             * present instant is not a floor; take the following one. Grid
             * spacing is milliseconds, so one step always clears it. */
            next = present_grid_time_ns(clock, elapsed_intervals + 2u);
        }
        return next;
    }
}

void mdkr_present_deadline_commit(MdkrPresentDeadlineClock *clock,
                                  uint64_t now_ns) {
    uint64_t elapsed_ns;
    uint64_t elapsed_intervals;

    if (clock == NULL || !clock->initialized || clock->rate == 0u) {
        return;
    }
    elapsed_ns = now_ns > clock->origin_ns ? now_ns - clock->origin_ns : 0u;
    /* floor(elapsed * rate / 1e9), overflow-safe for host-time ranges. */
    elapsed_intervals =
        (elapsed_ns / NS_PER_SECOND) * (uint64_t)clock->rate +
        ((elapsed_ns % NS_PER_SECOND) * (uint64_t)clock->rate) /
            NS_PER_SECOND;
    if (elapsed_intervals >= clock->next_index) {
        clock->next_index = elapsed_intervals + 1u;
    } else {
        clock->next_index++;
    }
}

int mdkr_pacing_cadence_valid(const char *value) {
    return value != NULL &&
           (strcmp(value, "original") == 0 || strcmp(value, "enhanced") == 0);
}

int mdkr_pacing_min_fields(const char *value) {
    return value != NULL && strcmp(value, "enhanced") == 0 ? 1 : 2;
}

int mdkr_pacing_field_hz(int source_field_hz, const char *diagnostic_override) {
    char *end = NULL;
    long parsed;

    if (source_field_hz != 50 && source_field_hz != 60) {
        source_field_hz = 60;
    }
    if (diagnostic_override == NULL || diagnostic_override[0] == '\0') {
        return source_field_hz;
    }
    errno = 0;
    parsed = strtol(diagnostic_override, &end, 10);
    if (errno != 0 || end == diagnostic_override || *end != '\0' ||
        parsed < MDKR_PACING_MIN_FIELD_HZ ||
        parsed > MDKR_PACING_MAX_FIELD_HZ) {
        return source_field_hz;
    }
    return (int)parsed;
}

int mdkr_pacing_synthetic_fields(int requested_fields, int min_fields,
                                 int max_fields) {
    if (min_fields < 1) {
        min_fields = 1;
    }
    if (max_fields < min_fields) {
        max_fields = min_fields;
    }
    if (requested_fields <= 0) {
        return min_fields;
    }
    if (requested_fields < min_fields) {
        return min_fields;
    }
    if (requested_fields > max_fields) {
        return max_fields;
    }
    return requested_fields;
}

int mdkr_pacing_queue_refill(int pending_fields, int measured_fields,
                             int capacity) {
    if (capacity < 1) {
        return 0;
    }
    if (pending_fields < 0) {
        pending_fields = 0;
    }
    if (pending_fields > capacity) {
        pending_fields = capacity;
    }
    if (measured_fields < 1) {
        measured_fields = 1;
    }
    if (measured_fields > capacity - pending_fields) {
        return capacity;
    }
    return pending_fields + measured_fields;
}

int mdkr_pacing_interval_requires_rebase(uint64_t elapsed_ns) {
    return elapsed_ns >= MDKR_PACING_STALL_REBASE_NS;
}

uint32_t mdkr_counter_guard_commit(MdkrCounterGuard *guard, uint32_t sample) {
    if (guard == NULL) {
        return sample;
    }
    if (!guard->initialized) {
        guard->initialized = 1;
    } else if ((int32_t)(sample - guard->last) <= 0) {
        sample = guard->last + 1u;
    }
    guard->last = sample;
    return sample;
}

static uint64_t grid_time_ns(const MdkrPacingClock *clock,
                             uint64_t fields) {
    uint64_t whole_seconds = fields / (uint64_t)clock->field_hz;
    uint64_t remainder = fields % (uint64_t)clock->field_hz;

    if (whole_seconds > (UINT64_MAX - clock->origin_ns) / NS_PER_SECOND) {
        return UINT64_MAX;
    }
    return clock->origin_ns + whole_seconds * NS_PER_SECOND +
           (remainder * NS_PER_SECOND) / (uint64_t)clock->field_hz;
}

static uint64_t elapsed_fields(const MdkrPacingClock *clock,
                               uint64_t elapsed_ns) {
    uint64_t whole_seconds = elapsed_ns / NS_PER_SECOND;
    uint64_t remainder = elapsed_ns % NS_PER_SECOND;

    if (whole_seconds > UINT64_MAX / (uint64_t)clock->field_hz) {
        return UINT64_MAX;
    }
    return whole_seconds * (uint64_t)clock->field_hz +
           (remainder * (uint64_t)clock->field_hz) / NS_PER_SECOND;
}

int mdkr_pacing_clock_init(MdkrPacingClock *clock, int field_hz,
                           int min_fields, int max_fields) {
    if (clock == NULL ||
        field_hz < MDKR_PACING_MIN_FIELD_HZ ||
        field_hz > MDKR_PACING_MAX_FIELD_HZ ||
        min_fields < 1 || max_fields < min_fields) {
        return 0;
    }
    memset(clock, 0, sizeof(*clock));
    clock->field_hz = field_hz;
    clock->min_fields = min_fields;
    clock->max_fields = max_fields;
    return 1;
}

uint64_t mdkr_pacing_clock_target(MdkrPacingClock *clock, uint64_t now_ns) {
    if (clock == NULL || clock->field_hz <= 0) {
        return now_ns;
    }
    if (!clock->initialized) {
        clock->origin_ns = now_ns;
        clock->grid_fields = 0;
        clock->initialized = 1;
    }
    clock->next_grid_ns = grid_time_ns(
        clock, clock->grid_fields + (uint64_t)clock->min_fields);
    return clock->next_grid_ns;
}

int mdkr_pacing_clock_commit(MdkrPacingClock *clock, uint64_t now_ns,
                             int *rebased) {
    uint64_t elapsed_ns;
    uint64_t grid_ns;
    uint64_t raw_fields;
    int fields;

    if (rebased != NULL) {
        *rebased = 0;
    }
    if (clock == NULL || !clock->initialized || clock->field_hz <= 0) {
        return 1;
    }
    grid_ns = grid_time_ns(clock, clock->grid_fields);
    elapsed_ns = now_ns > grid_ns ? now_ns - grid_ns : 0u;
    raw_fields = elapsed_fields(clock, elapsed_ns);
    if (raw_fields < (uint64_t)clock->min_fields) {
        fields = clock->min_fields;
    } else if (raw_fields > (uint64_t)clock->max_fields) {
        fields = clock->max_fields;
    } else {
        fields = (int)raw_fields;
    }
    clock->grid_fields += (uint64_t)fields;

    /* Suspension/debugger/occlusion time is retired by the same exact policy
     * used by the presentation subloop. Equality is intentionally a rebase. */
    if (mdkr_pacing_interval_requires_rebase(elapsed_ns)) {
        clock->origin_ns = now_ns;
        clock->grid_fields = 0;
        if (rebased != NULL) {
            *rebased = 1;
        }
    }
    clock->next_grid_ns = grid_time_ns(
        clock, clock->grid_fields + (uint64_t)clock->min_fields);
    return fields;
}
