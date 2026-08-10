#include "input_latency_census.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 0.1 ms buckets to 100 ms, plus one overflow bucket. The terms being measured
 * live between 0 and ~35 ms on any host that is keeping up, so this resolves
 * every percentile that matters and still records a stall honestly instead of
 * clamping it into the top bucket's value -- the exact maximum is tracked
 * separately for that reason. */
#define CENSUS_BUCKET_NS   UINT64_C(100000)
#define CENSUS_BUCKETS     1001u
#define CENSUS_OVERFLOW    (CENSUS_BUCKETS - 1u)

typedef struct CensusSeries {
    uint64_t buckets[CENSUS_BUCKETS];
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
} CensusSeries;

static int s_enabled = -1;

static CensusSeries s_queue;    /* SDL event queue residency (stored as ns) */
static CensusSeries s_sample;   /* capture -> commit */
static CensusSeries s_tick;     /* commit -> commit */
static CensusSeries s_present;  /* commit -> swap return */

static uint64_t s_lastCaptureNs;
static uint64_t s_lastCommitNs;
static bool s_captureSinceCommit;
static bool s_presentPending;
static uint64_t s_noCapture;

static char s_policy[32] = "?";
static unsigned s_presentRate;
static unsigned s_tickFields;
static int s_swapInterval = -1;
static bool s_smoothing;
static bool s_jit;

bool input_latency_census_enabled(void) {
    if (s_enabled < 0) {
        const char *value = getenv("MDKR_INPUT_LATENCY");
        s_enabled = (value != NULL && value[0] != '\0' && value[0] != '0');
    }
    return s_enabled != 0;
}

static void series_add(CensusSeries *series, uint64_t ns) {
    uint64_t index = ns / CENSUS_BUCKET_NS;
    if (index >= CENSUS_OVERFLOW) {
        index = CENSUS_OVERFLOW;
    }
    series->buckets[index]++;
    series->count++;
    series->total_ns += ns;
    if (ns > series->max_ns) {
        series->max_ns = ns;
    }
}

/* Upper edge of the bucket holding the requested percentile, in milliseconds.
 * An upper edge never understates the term, which is the safe direction for a
 * latency claim. */
static double series_percentile(const CensusSeries *series, double fraction) {
    uint64_t want;
    uint64_t seen = 0;
    unsigned i;
    if (series->count == 0u) {
        return 0.0;
    }
    want = (uint64_t)((double)series->count * fraction);
    if (want == 0u) {
        want = 1u;
    }
    for (i = 0; i < CENSUS_BUCKETS; i++) {
        seen += series->buckets[i];
        if (seen >= want) {
            if (i == CENSUS_OVERFLOW) {
                return (double)series->max_ns / 1e6;
            }
            return (double)(i + 1u) * (double)CENSUS_BUCKET_NS / 1e6;
        }
    }
    return (double)series->max_ns / 1e6;
}

static void series_report(const char *name, const CensusSeries *series) {
    const double mean = series->count != 0u
        ? (double)series->total_ns / (double)series->count / 1e6
        : 0.0;
    fprintf(stderr,
            "[INPUT-LATENCY] term=%-7s n=%llu mean=%.2f p50=%.2f p95=%.2f "
            "p99=%.2f max=%.2f (ms)\n",
            name, (unsigned long long)series->count, mean,
            series_percentile(series, 0.50),
            series_percentile(series, 0.95),
            series_percentile(series, 0.99),
            (double)series->max_ns / 1e6);
}

void input_latency_census_note_event(unsigned queued_ms) {
    if (!input_latency_census_enabled()) {
        return;
    }
    series_add(&s_queue, (uint64_t)queued_ms * UINT64_C(1000000));
}

void input_latency_census_note_capture(uint64_t now_ns) {
    if (!input_latency_census_enabled()) {
        return;
    }
    s_lastCaptureNs = now_ns;
    s_captureSinceCommit = true;
}

void input_latency_census_note_commit(uint64_t now_ns) {
    if (!input_latency_census_enabled()) {
        return;
    }
    if (s_captureSinceCommit && now_ns >= s_lastCaptureNs) {
        series_add(&s_sample, now_ns - s_lastCaptureNs);
    } else if (!s_captureSinceCommit) {
        /* Script-only runs suppress live capture entirely, so this counter
         * being equal to the ticket count is the signal that the sample term
         * was not measurable rather than that it was zero. */
        s_noCapture++;
    }
    if (s_lastCommitNs != 0u && now_ns >= s_lastCommitNs) {
        series_add(&s_tick, now_ns - s_lastCommitNs);
    }
    s_lastCommitNs = now_ns;
    s_captureSinceCommit = false;
    s_presentPending = true;
}

void input_latency_census_note_present(uint64_t now_ns) {
    if (!input_latency_census_enabled()) {
        return;
    }
    if (!s_presentPending || s_lastCommitNs == 0u || now_ns < s_lastCommitNs) {
        return;
    }
    series_add(&s_present, now_ns - s_lastCommitNs);
    s_presentPending = false;
}

void input_latency_census_note_config(
    const char *present_policy, unsigned present_rate, unsigned tick_fields,
    int swap_interval, bool smoothing, bool jit) {
    if (!input_latency_census_enabled()) {
        return;
    }
    if (present_policy != NULL) {
        size_t len = strlen(present_policy);
        if (len >= sizeof(s_policy)) {
            len = sizeof(s_policy) - 1u;
        }
        memcpy(s_policy, present_policy, len);
        s_policy[len] = '\0';
    }
    s_presentRate = present_rate;
    s_tickFields = tick_fields;
    s_swapInterval = swap_interval;
    s_smoothing = smoothing;
    s_jit = jit;
}

void input_latency_census_summary(void) {
    if (!input_latency_census_enabled()) {
        return;
    }
    fprintf(stderr,
            "[INPUT-LATENCY] config policy=%s presentRate=%u tickFields=%u "
            "swapInterval=%d smoothing=%d jit=%d\n",
            s_policy, s_presentRate, s_tickFields, s_swapInterval,
            s_smoothing ? 1 : 0, s_jit ? 1 : 0);
    series_report("queue", &s_queue);
    series_report("sample", &s_sample);
    series_report("tick", &s_tick);
    series_report("present", &s_present);
    fprintf(stderr,
            "[INPUT-LATENCY] uncaptured-tickets=%llu\n",
            (unsigned long long)s_noCapture);
    fflush(stderr);
}
