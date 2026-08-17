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

static void present_interval_sort(uint64_t *values, unsigned count) {
    unsigned i;

    for (i = 1u; i < count; i++) {
        const uint64_t value = values[i];
        unsigned position = i;
        while (position > 0u && values[position - 1u] > value) {
            values[position] = values[position - 1u];
            position--;
        }
        values[position] = value;
    }
}

static uint64_t present_interval_jitter_ppm(
    const MdkrPresentIntervalClassifier *classifier) {
    uint64_t ordered[MDKR_PRESENT_INTERVAL_WINDOW];
    uint64_t sum = 0u;
    uint64_t sum_sq = 0u;
    uint64_t mean;
    uint64_t mean_sq;
    uint64_t scaled_mean_sq;
    unsigned i;
    const unsigned first = MDKR_PRESENT_INTERVAL_TRIM;
    const unsigned last = MDKR_PRESENT_INTERVAL_WINDOW -
                          MDKR_PRESENT_INTERVAL_TRIM;
    const unsigned retained = last - first;

    if (classifier == NULL ||
        classifier->count < MDKR_PRESENT_INTERVAL_WINDOW) {
        return 0u;
    }
    memcpy(ordered, classifier->intervals_ns, sizeof(ordered));
    present_interval_sort(ordered, MDKR_PRESENT_INTERVAL_WINDOW);
    for (i = first; i < last; i++) {
        sum += ordered[i];
    }
    mean = sum / retained;
    if (mean == 0u || mean > UINT32_MAX) {
        return 0u;
    }
    for (i = first; i < last; i++) {
        const uint64_t sample = ordered[i];
        const uint64_t delta = sample > mean ? sample - mean : mean - sample;
        if (delta > UINT32_MAX) {
            return UINT64_MAX;
        }
        sum_sq += delta * delta;
    }
    mean_sq = mean * mean;
    scaled_mean_sq = mean_sq / UINT64_C(1000000);
    if (scaled_mean_sq == 0u) {
        return 0u;
    }
    return (sum_sq / retained) / scaled_mean_sq;
}

void mdkr_present_interval_reset(MdkrPresentIntervalClassifier *classifier) {
    if (classifier != NULL) {
        memset(classifier, 0, sizeof(*classifier));
    }
}

void mdkr_present_interval_note(MdkrPresentIntervalClassifier *classifier,
                                uint64_t elapsed_ns) {
    MdkrPresentIntervalKind next;
    unsigned required;

    if (classifier == NULL || elapsed_ns == 0u) {
        return;
    }
    if (mdkr_pacing_interval_requires_rebase(elapsed_ns)) {
        mdkr_present_interval_reset(classifier);
        return;
    }
    classifier->intervals_ns[classifier->head] = elapsed_ns;
    classifier->head = (classifier->head + 1u) %
                       MDKR_PRESENT_INTERVAL_WINDOW;
    if (classifier->count < MDKR_PRESENT_INTERVAL_WINDOW) {
        classifier->count++;
    }
    if (classifier->count < MDKR_PRESENT_INTERVAL_WINDOW) {
        classifier->kind = MDKR_PRESENT_INTERVAL_WARMING;
        classifier->jitter_ppm = 0u;
        return;
    }

    classifier->jitter_ppm = present_interval_jitter_ppm(classifier);
    if (classifier->kind == MDKR_PRESENT_INTERVAL_WARMING) {
        classifier->kind =
            classifier->jitter_ppm > MDKR_PRESENT_INTERVAL_VARIABLE_PPM
                ? MDKR_PRESENT_INTERVAL_VARIABLE
                : MDKR_PRESENT_INTERVAL_FIXED;
        classifier->contrary_count = 0u;
        return;
    }

    next = classifier->kind;
    required = 0u;
    if (classifier->kind == MDKR_PRESENT_INTERVAL_FIXED &&
        classifier->jitter_ppm > MDKR_PRESENT_INTERVAL_VARIABLE_PPM) {
        next = MDKR_PRESENT_INTERVAL_VARIABLE;
        required = MDKR_PRESENT_INTERVAL_VARIABLE_CONFIRM;
    } else if (classifier->kind == MDKR_PRESENT_INTERVAL_VARIABLE &&
               classifier->jitter_ppm < MDKR_PRESENT_INTERVAL_FIXED_PPM) {
        next = MDKR_PRESENT_INTERVAL_FIXED;
        required = MDKR_PRESENT_INTERVAL_FIXED_CONFIRM;
    }

    if (next == classifier->kind) {
        classifier->contrary_count = 0u;
        return;
    }
    classifier->contrary_count++;
    if (classifier->contrary_count >= required) {
        classifier->kind = next;
        classifier->contrary_count = 0u;
        classifier->transitions++;
    }
}

const char *mdkr_present_interval_kind_name(MdkrPresentIntervalKind kind) {
    switch (kind) {
        case MDKR_PRESENT_INTERVAL_FIXED:
            return "fixed";
        case MDKR_PRESENT_INTERVAL_VARIABLE:
            return "variable";
        case MDKR_PRESENT_INTERVAL_WARMING:
        default:
            return "warming";
    }
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
    if (present_ci_equal(value, "display-margin")) {
        parsed.kind = MDKR_PRESENT_DISPLAY_MARGIN;
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

unsigned mdkr_present_policy_display_margin_rate(unsigned display_rate) {
    if (display_rate == 0u) {
        return 0u;
    }
    if (display_rate <= MDKR_PRESENT_RATE_MIN + MDKR_PRESENT_DISPLAY_MARGIN_HZ) {
        return MDKR_PRESENT_RATE_MIN;
    }
    return display_rate - MDKR_PRESENT_DISPLAY_MARGIN_HZ;
}

MdkrPresentSync mdkr_present_policy_sync(const MdkrPresentPolicy *policy,
                                         unsigned display_rate) {
    if (policy == NULL) {
        return MDKR_PRESENT_SYNC_BLOCKING;
    }
    if (policy->kind == MDKR_PRESENT_UNCAPPED) {
        return MDKR_PRESENT_SYNC_LATEST;
    }
    /* display-margin is BELOW the refresh by construction, so the blocking
     * queue is exactly right for it and asking for a queue that drops an
     * undisplayed image would be asking for a capability it can never use. It
     * takes the fall-through below rather than a branch of its own. */
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
           policy->kind == MDKR_PRESENT_DISPLAY_MARGIN ||
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

uint64_t mdkr_present_slot_phase(MdkrPresentSlotState *state, uint64_t tick,
                                 uint64_t measured_units,
                                 uint64_t tick_units,
                                 uint64_t quantum_units) {
    uint64_t predicted;
    uint64_t drawn;
    uint64_t diff;
    uint64_t snap;

    uint64_t increment;

    if (state == NULL || quantum_units == 0u || tick_units == 0u ||
        quantum_units >= tick_units) {
        return measured_units;
    }
    if (state->last_tick != tick) {
        /* First replay of a tick. Its measured phase carries a per-tick
         * constant the projector must be invariant to — the endpoint lead
         * plus the tick-vs-slot residual — so there is no honest absolute
         * comparison to make here. The drawn phase starts the uniform
         * sequence; the measurement seeds the increment test below. */
        state->last_tick = tick;
        state->last_measured = measured_units;
        state->last_units = quantum_units;
        state->snaps++;
        return quantum_units;
    }
    predicted = state->last_units + quantum_units;
    if (predicted >= tick_units) {
        /* The tick/display beat produced an extra slot. Repeat just below
         * the boundary — content-identical to the incoming endpoint — and
         * do NOT fall through to the anchor comparison. */
        drawn = tick_units - 1u;
        state->snaps++;
    } else {
        /* Offset-invariant slot test: one displayed frame is one slot, so
         * the MEASURED phase should have advanced by one quantum since the
         * previous replay regardless of any constant offset. Judging the
         * increment (not the absolute position) is what keeps wake noise
         * and the per-tick offset out of the decision while a genuinely
         * missed slot (an increment near two quanta) still re-anchors. */
        increment = measured_units > state->last_measured
                        ? measured_units - state->last_measured
                        : 0u;
        diff = increment > quantum_units ? increment - quantum_units
                                         : quantum_units - increment;
        snap = (quantum_units / MDKR_PRESENT_SLOT_SNAP_DEN) *
               MDKR_PRESENT_SLOT_SNAP_NUM;
        if (diff <= snap) {
            drawn = predicted;
            state->snaps++;
        } else {
            drawn = mdkr_present_quantize_phase(measured_units, tick_units,
                                                quantum_units);
            if (drawn < quantum_units) {
                drawn = quantum_units;
            }
            if (drawn > tick_units - 1u) {
                drawn = tick_units - 1u;
            }
            state->anchors++;
        }
    }
    state->last_measured = measured_units;
    if (drawn < state->last_units) {
        drawn = state->last_units; /* census counts the repeat as a stall */
    }
    state->last_units = drawn;
    return drawn;
}

static uint64_t present_grid_time_ns(const MdkrPresentDeadlineClock *clock,
                                     uint64_t index) {
    uint64_t whole_seconds;
    uint64_t remainder;

    if (clock->period_override_fp != 0u) {
        /* Disciplined re-rate: the measured display period, 8.8 fp ns. The
         * <= 1/256 ns truncation per step is orders of magnitude below the
         * phase controller's CREEP and is continuously absorbed by it. */
        if (index > UINT64_MAX / clock->period_override_fp) {
            return UINT64_MAX;
        }
        return clock->origin_ns + ((index * clock->period_override_fp) >> 8);
    }
    whole_seconds = index / (uint64_t)clock->rate;
    remainder = index % (uint64_t)clock->rate;
    if (whole_seconds > (UINT64_MAX - clock->origin_ns) / NS_PER_SECOND) {
        return UINT64_MAX;
    }
    return clock->origin_ns + whole_seconds * NS_PER_SECOND +
           (remainder * NS_PER_SECOND) / (uint64_t)clock->rate;
}

/* The grid period currently in force, in 8.8 fixed-point nanoseconds. */
static uint64_t present_grid_period_fp(const MdkrPresentDeadlineClock *clock) {
    if (clock->period_override_fp != 0u) {
        return clock->period_override_fp;
    }
    if (clock->rate == 0u) {
        return 0u;
    }
    return (NS_PER_SECOND << 8) / (uint64_t)clock->rate;
}

/* floor(elapsed / period) for the effective grid, overflow-safe for
 * host-time ranges. Mirrors the legacy exact-rational form when no
 * override is active so those paths stay bit-for-bit. */
static uint64_t present_grid_elapsed_intervals(
    const MdkrPresentDeadlineClock *clock, uint64_t elapsed_ns) {
    if (clock->period_override_fp != 0u) {
        return (elapsed_ns << 8) / clock->period_override_fp;
    }
    return (elapsed_ns / NS_PER_SECOND) * (uint64_t)clock->rate +
           ((elapsed_ns % NS_PER_SECOND) * (uint64_t)clock->rate) /
               NS_PER_SECOND;
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
    elapsed_intervals = present_grid_elapsed_intervals(clock, elapsed_ns);
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
    elapsed_intervals = present_grid_elapsed_intervals(clock, elapsed_ns);
    if (elapsed_intervals >= clock->next_index) {
        clock->next_index = elapsed_intervals + 1u;
    } else {
        clock->next_index++;
    }
}

void mdkr_present_deadline_feedback(MdkrPresentDeadlineClock *clock,
                                    uint64_t block_ns, int unavailable,
                                    uint64_t now_ns) {
    uint64_t period_fp;
    int64_t delta;

    if (clock == NULL || !clock->initialized || clock->rate == 0u) {
        return;
    }
    period_fp = present_grid_period_fp(clock);
    if (period_fp == 0u) {
        return;
    }
    if (unavailable) {
        /* The queue overran and dropped an image: the grid is running ahead
         * of retirement. Back off hard — half a period — and forget the
         * bound-interval sample chain; the drop broke its continuity. */
        uint64_t backoff = (period_fp >> 8) / 2u;
        clock->origin_ns += backoff;
        clock->slew_total_ns += backoff;
        clock->last_bound_ns = 0u;
        clock->rerate_streak = 0u;
        return;
    }
    /* RATE: consecutive display-bound completions sample the display's own
     * period; sustained deviation re-rates the grid, and a measurement back
     * inside SNAP_PPM of nominal restores the exact rational grid. */
    if (block_ns >= MDKR_PRESENT_DISCIPLINE_PUNISH_NS) {
        /* The display pushed back: every early pull since the last block
         * was legitimate phase-seeking, not proof of a faster panel. */
        clock->early_credit_ns = 0u;
    } else {
        clock->early_credit_ns += MDKR_PRESENT_DISCIPLINE_CREEP_NS;
    }
    if (block_ns > MDKR_PRESENT_DISCIPLINE_BOUND_MIN_NS) {
        if (clock->last_bound_ns != 0u && now_ns > clock->last_bound_ns) {
            uint64_t sample_ns = now_ns - clock->last_bound_ns;
            if (sample_ns >= UINT64_C(2000000) &&
                sample_ns <= UINT64_C(50000000)) {
                uint64_t sample_fp = sample_ns << 8;
                if (clock->period_ema_fp == 0u) {
                    clock->period_ema_fp = period_fp;
                }
                clock->period_ema_fp +=
                    ((int64_t)sample_fp - (int64_t)clock->period_ema_fp) >>
                    MDKR_PRESENT_DISCIPLINE_EMA_SHIFT;
                {
                    uint64_t diff = clock->period_ema_fp > period_fp
                                        ? clock->period_ema_fp - period_fp
                                        : period_fp - clock->period_ema_fp;
                    uint64_t ppm = period_fp == 0u
                                       ? 0u
                                       : (diff * UINT64_C(1000000)) /
                                             period_fp;
                    if (ppm > MDKR_PRESENT_DISCIPLINE_RERATE_PPM) {
                        clock->rerate_streak++;
                        if (clock->rerate_streak >=
                            MDKR_PRESENT_DISCIPLINE_RERATE_CONFIRM) {
                            uint64_t nominal_fp =
                                (NS_PER_SECOND << 8) /
                                (uint64_t)clock->rate;
                            uint64_t ndiff =
                                clock->period_ema_fp > nominal_fp
                                    ? clock->period_ema_fp - nominal_fp
                                    : nominal_fp - clock->period_ema_fp;
                            uint64_t nppm =
                                (ndiff * UINT64_C(1000000)) / nominal_fp;
                            /* Re-anchor so the new period starts from the
                             * present instant; the phase controller trims
                             * the sub-period residue. */
                            clock->period_override_fp =
                                nppm <= MDKR_PRESENT_DISCIPLINE_SNAP_PPM
                                    ? 0u
                                    : clock->period_ema_fp;
                            clock->origin_ns = now_ns;
                            clock->next_index = 1u;
                            clock->rerate_streak = 0u;
                        }
                    } else {
                        clock->rerate_streak = 0u;
                    }
                }
            }
        }
        clock->last_bound_ns = now_ns;
    } else {
        clock->last_bound_ns = 0u;
        clock->rerate_streak = 0u;
    }
    /* Staleness: see the CREDIT_PERIODS comment in the header. Two full
     * periods of unpunished early pull prove the panel outruns the
     * override; decay a quarter of the gap toward nominal and let honest
     * blocks re-rate it back if this ever overshoots. */
    if (clock->period_override_fp != 0u &&
        clock->early_credit_ns >=
            (clock->period_override_fp >> 8) *
                (uint64_t)MDKR_PRESENT_DISCIPLINE_CREDIT_PERIODS) {
        const uint64_t nominal_fp =
            (NS_PER_SECOND << 8) / (uint64_t)clock->rate;
        uint64_t gap = clock->period_override_fp > nominal_fp
                           ? clock->period_override_fp - nominal_fp
                           : nominal_fp - clock->period_override_fp;
        gap >>= 1; /* halve the gap each decay step */
        {
            const uint64_t decayed = clock->period_override_fp > nominal_fp
                                         ? nominal_fp + gap
                                         : nominal_fp - gap;
            const uint64_t ppm = (gap * UINT64_C(1000000)) / nominal_fp;
            clock->rerate_expiries++;
            clock->early_credit_ns = 0u;
            clock->period_ema_fp = 0u;
            if (ppm <= MDKR_PRESENT_DISCIPLINE_SNAP_PPM) {
                clock->period_override_fp = 0u; /* home: exact grid */
            } else {
                clock->period_override_fp = decayed;
            }
            clock->origin_ns = now_ns;
            clock->next_index = 1u;
        }
    }
    /* PHASE: trend early by CREEP; arriving early enough to block past
     * TARGET pushes the origin later by a clamped fraction of the excess. */
    delta = -(int64_t)MDKR_PRESENT_DISCIPLINE_CREEP_NS;
    if (block_ns > MDKR_PRESENT_DISCIPLINE_TARGET_BLOCK_NS) {
        uint64_t push = (block_ns - MDKR_PRESENT_DISCIPLINE_TARGET_BLOCK_NS)
                        >> MDKR_PRESENT_DISCIPLINE_GAIN_SHIFT;
        if (push > MDKR_PRESENT_DISCIPLINE_MAX_SLEW_NS) {
            push = MDKR_PRESENT_DISCIPLINE_MAX_SLEW_NS;
        }
        delta += (int64_t)push;
    }
    if (delta < 0) {
        uint64_t back = (uint64_t)(-delta);
        if (clock->origin_ns > back) {
            clock->origin_ns -= back;
        } else {
            back = clock->origin_ns;
            clock->origin_ns = 0u;
        }
        clock->slew_total_ns += back;
    } else if (delta > 0) {
        clock->origin_ns += (uint64_t)delta;
        clock->slew_total_ns += (uint64_t)delta;
    }
}

uint64_t mdkr_present_deadline_slew_total(
    const MdkrPresentDeadlineClock *clock) {
    return clock != NULL ? clock->slew_total_ns : 0u;
}

uint64_t mdkr_present_deadline_period_ns(
    const MdkrPresentDeadlineClock *clock) {
    if (clock == NULL || clock->rate == 0u) {
        return 0u;
    }
    if (clock->period_override_fp != 0u) {
        return clock->period_override_fp >> 8;
    }
    return NS_PER_SECOND / (uint64_t)clock->rate;
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
