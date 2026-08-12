#ifndef MDKR_ROLLBACK_RING_H
#define MDKR_ROLLBACK_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rollback_snapshot.h"
#include "rollback_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ROLLBACK_RING_MAX_SLOTS MDKR_ROLLBACK_SNAPSHOT_SLOTS
#define MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS UINT64_C(10000)
#define MDKR_ROLLBACK_TIMING_BIN_COUNT 2048u
#define MDKR_ROLLBACK_TIMING_P99_BUDGET_NS UINT64_C(8333333)
#define MDKR_ROLLBACK_TIMING_TAIL_BUDGET_NS UINT64_C(16666667)

typedef uint64_t (*MdkrRollbackClockFn)(void *context);

/* Bounded, allocation-free timing distribution shared by snapshot and
 * resimulation diagnostics. Timings are observational only and must never feed
 * simulation decisions. */
typedef struct MdkrRollbackTimingHistogram {
    uint64_t samples;
    uint64_t ns_total;
    uint64_t ns_max;
    uint64_t bins[MDKR_ROLLBACK_TIMING_BIN_COUNT];
    uint64_t overflow;
    uint64_t over_p99_budget;
    uint64_t over_tail_budget;
} MdkrRollbackTimingHistogram;

typedef struct MdkrRollbackRingSlot {
    uint32_t tick;
    uint32_t generation;
    bool valid;
} MdkrRollbackRingSlot;

typedef struct MdkrRollbackRingStats {
    uint64_t captures;
    uint64_t restores;
    uint64_t misses;
    size_t allocated_bytes;
    size_t high_water_bytes;
    uint64_t capture_ns_total;
    uint64_t capture_ns_max;
    uint64_t restore_ns_total;
    uint64_t restore_ns_max;
    uint64_t timing_samples;
    uint64_t capture_timing_bins[MDKR_ROLLBACK_TIMING_BIN_COUNT];
    uint64_t restore_timing_bins[MDKR_ROLLBACK_TIMING_BIN_COUNT];
    uint64_t capture_timing_overflow;
    uint64_t restore_timing_overflow;
    uint64_t capture_over_p99_budget;
    uint64_t capture_over_tail_budget;
    uint64_t restore_over_p99_budget;
    uint64_t restore_over_tail_budget;
} MdkrRollbackRingStats;

typedef struct MdkrRollbackRing {
    const MdkrRollbackSnapshotRegistry *registry;
    uint8_t *storage;
    size_t snapshot_bytes;
    unsigned slot_count;
    unsigned next_slot;
    uint32_t next_generation;
    MdkrRollbackClockFn clock;
    void *clock_context;
    MdkrRollbackRingSlot slots[MDKR_ROLLBACK_RING_MAX_SLOTS];
    MdkrRollbackRingStats stats;
} MdkrRollbackRing;

bool mdkr_rollback_ring_init(
    MdkrRollbackRing *ring, const MdkrRollbackSnapshotRegistry *registry,
    unsigned slots);
void mdkr_rollback_ring_destroy(MdkrRollbackRing *ring);
/* Optional monotonic instrumentation. The ring never reads a wall clock unless
 * the launcher explicitly supplies one; simulation results cannot depend on it. */
void mdkr_rollback_ring_set_clock(
    MdkrRollbackRing *ring, MdkrRollbackClockFn clock, void *context);
bool mdkr_rollback_ring_capture(MdkrRollbackRing *ring, uint32_t tick);
bool mdkr_rollback_ring_restore(MdkrRollbackRing *ring, uint32_t tick,
                                bool at_tick_boundary);
bool mdkr_rollback_ring_has(const MdkrRollbackRing *ring, uint32_t tick);
/* Copy one immutable in-process snapshot for exact replay comparison. The
 * caller must provide precisely ring->snapshot_bytes of disjoint host memory;
 * no internal slot pointer escapes or remains valid across captures. */
bool mdkr_rollback_ring_copy(
    const MdkrRollbackRing *ring, uint32_t tick, void *output,
    size_t output_size);
/* Histogram upper-bound percentile. `capture=true` selects captures, false
 * restores. Overflow returns the observed maximum rather than pretending the
 * final finite bucket was precise. */
uint64_t mdkr_rollback_ring_percentile_ns(
    const MdkrRollbackRingStats *stats, bool capture, unsigned percentile);
void mdkr_rollback_timing_record(
    MdkrRollbackTimingHistogram *histogram, uint64_t duration_ns);
uint64_t mdkr_rollback_timing_percentile_ns(
    const MdkrRollbackTimingHistogram *histogram, unsigned percentile);

#ifdef __cplusplus
}
#endif
#endif
