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

int mdkr_present_policy_uses_vsync(const MdkrPresentPolicy *policy) {
    return policy == NULL || policy->kind == MDKR_PRESENT_ORIGINAL ||
           policy->kind == MDKR_PRESENT_DISPLAY;
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
    raw_fields = now_ns > grid_ns
        ? elapsed_fields(clock, now_ns - grid_ns)
        : 0;
    if (raw_fields < (uint64_t)clock->min_fields) {
        fields = clock->min_fields;
    } else if (raw_fields > (uint64_t)clock->max_fields) {
        fields = clock->max_fields;
    } else {
        fields = (int)raw_fields;
    }
    clock->grid_fields += (uint64_t)fields;
    grid_ns = grid_time_ns(clock, clock->grid_fields);

    /*
     * A level load, debugger stop, or resume may leave more than four source
     * fields of debt even after the bounded update. Rebase instead of carrying
     * seconds of catch-up into subsequent frames.
     */
    if (now_ns > grid_ns &&
        elapsed_fields(clock, now_ns - grid_ns) > 4u) {
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
