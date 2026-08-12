#include "rollback_ring.h"

#include <stdlib.h>
#include <string.h>

bool mdkr_rollback_ring_init(
    MdkrRollbackRing *ring, const MdkrRollbackSnapshotRegistry *registry,
    unsigned slots) {
    size_t snapshot_bytes;
    if (ring == NULL || registry == NULL || slots == 0u ||
        slots > MDKR_ROLLBACK_RING_MAX_SLOTS) return false;
    snapshot_bytes = mdkr_rollback_snapshot_bytes(registry);
    if (snapshot_bytes == 0u || snapshot_bytes > SIZE_MAX / slots ||
        snapshot_bytes > MDKR_ROLLBACK_MAX_RING_BYTES / slots) return false;
    memset(ring, 0, sizeof(*ring));
    ring->storage = (uint8_t *)calloc(slots, snapshot_bytes);
    if (ring->storage == NULL) return false;
    ring->registry = registry;
    ring->snapshot_bytes = snapshot_bytes;
    ring->slot_count = slots;
    ring->next_generation = 1u;
    ring->stats.allocated_bytes = snapshot_bytes * slots;
    return true;
}

void mdkr_rollback_ring_destroy(MdkrRollbackRing *ring) {
    if (ring == NULL) return;
    free(ring->storage);
    memset(ring, 0, sizeof(*ring));
}

void mdkr_rollback_ring_set_clock(
    MdkrRollbackRing *ring, MdkrRollbackClockFn clock, void *context) {
    if (ring == NULL) return;
    ring->clock = clock;
    ring->clock_context = context;
}

static uint64_t clock_now(const MdkrRollbackRing *ring) {
    return ring->clock != NULL ? ring->clock(ring->clock_context) : 0u;
}

static uint64_t elapsed(uint64_t start, uint64_t end) {
    return end >= start ? end - start : 0u;
}

static uint64_t add_saturating(uint64_t left, uint64_t right) {
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

static void timing_add(
    uint64_t bins[MDKR_ROLLBACK_TIMING_BIN_COUNT], uint64_t duration,
    uint64_t *overflow, uint64_t *over_p99, uint64_t *over_tail) {
    uint64_t bucket = duration / MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS;
    if (duration > MDKR_ROLLBACK_TIMING_P99_BUDGET_NS)
        *over_p99 = add_saturating(*over_p99, 1u);
    if (duration > MDKR_ROLLBACK_TIMING_TAIL_BUDGET_NS)
        *over_tail = add_saturating(*over_tail, 1u);
    if (bucket >= MDKR_ROLLBACK_TIMING_BIN_COUNT) {
        bucket = MDKR_ROLLBACK_TIMING_BIN_COUNT - 1u;
        *overflow = add_saturating(*overflow, 1u);
    }
    bins[bucket] = add_saturating(bins[bucket], 1u);
}

static uint64_t timing_percentile(
    const uint64_t bins[MDKR_ROLLBACK_TIMING_BIN_COUNT], uint64_t samples,
    uint64_t overflow, uint64_t maximum, unsigned percentile) {
    uint64_t target;
    uint64_t seen = 0u;
    unsigned bucket;
    if (bins == NULL || samples == 0u || percentile == 0u ||
        percentile > 100u) return 0u;
    target = (samples / 100u) * percentile +
        (((samples % 100u) * percentile + 99u) / 100u);
    for (bucket = 0u; bucket < MDKR_ROLLBACK_TIMING_BIN_COUNT; bucket++) {
        seen = add_saturating(seen, bins[bucket]);
        if (seen >= target) {
            if (bucket + 1u == MDKR_ROLLBACK_TIMING_BIN_COUNT &&
                overflow != 0u) return maximum;
            return (uint64_t)(bucket + 1u) *
                MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS;
        }
    }
    return maximum;
}

void mdkr_rollback_timing_record(
    MdkrRollbackTimingHistogram *histogram, uint64_t duration_ns) {
    if (histogram == NULL) return;
    histogram->samples = add_saturating(histogram->samples, 1u);
    histogram->ns_total = add_saturating(histogram->ns_total, duration_ns);
    if (duration_ns > histogram->ns_max) histogram->ns_max = duration_ns;
    timing_add(
        histogram->bins, duration_ns, &histogram->overflow,
        &histogram->over_p99_budget, &histogram->over_tail_budget);
}

uint64_t mdkr_rollback_timing_percentile_ns(
    const MdkrRollbackTimingHistogram *histogram, unsigned percentile) {
    if (histogram == NULL) return 0u;
    return timing_percentile(
        histogram->bins, histogram->samples, histogram->overflow,
        histogram->ns_max, percentile);
}

uint64_t mdkr_rollback_ring_percentile_ns(
    const MdkrRollbackRingStats *stats, bool capture, unsigned percentile) {
    const uint64_t *bins;
    const uint64_t samples = stats == NULL ? 0u :
        (capture ? stats->captures : stats->restores);
    const uint64_t overflow = stats == NULL ? 0u :
        (capture ? stats->capture_timing_overflow :
                   stats->restore_timing_overflow);
    const uint64_t maximum = stats == NULL ? 0u :
        (capture ? stats->capture_ns_max : stats->restore_ns_max);
    if (stats == NULL || samples == 0u || percentile == 0u ||
        percentile > 100u) return 0u;
    bins = capture ? stats->capture_timing_bins : stats->restore_timing_bins;
    return timing_percentile(
        bins, samples, overflow, maximum, percentile);
}

static int find_slot(const MdkrRollbackRing *ring, uint32_t tick);

bool mdkr_rollback_ring_capture(MdkrRollbackRing *ring, uint32_t tick) {
    MdkrRollbackRingSlot *slot;
    uint8_t *destination;
    int existing;
    unsigned target;
    uint64_t started;
    uint64_t duration;
    if (ring == NULL || ring->storage == NULL) return false;
    started = clock_now(ring);
    existing = find_slot(ring, tick);
    target = existing >= 0 ? (unsigned)existing : ring->next_slot;
    slot = &ring->slots[target];
    destination = ring->storage + target * ring->snapshot_bytes;
    slot->valid = false;
    if (!mdkr_rollback_snapshot_capture(
            ring->registry, tick, destination, ring->snapshot_bytes)) return false;
    if (ring->next_generation == 0u) ring->next_generation = 1u;
    slot->tick = tick;
    slot->generation = ring->next_generation++;
    slot->valid = true;
    if (existing < 0) ring->next_slot = (ring->next_slot + 1u) % ring->slot_count;
    ring->stats.captures++;
    ring->stats.high_water_bytes = ring->stats.allocated_bytes;
    if (ring->clock != NULL) {
        duration = elapsed(started, clock_now(ring));
        ring->stats.capture_ns_total = add_saturating(
            ring->stats.capture_ns_total, duration);
        if (duration > ring->stats.capture_ns_max)
            ring->stats.capture_ns_max = duration;
        timing_add(
            ring->stats.capture_timing_bins, duration,
            &ring->stats.capture_timing_overflow,
            &ring->stats.capture_over_p99_budget,
            &ring->stats.capture_over_tail_budget);
        ring->stats.timing_samples = add_saturating(
            ring->stats.timing_samples, 1u);
    }
    return true;
}

static int find_slot(const MdkrRollbackRing *ring, uint32_t tick) {
    unsigned index;
    if (ring == NULL || ring->storage == NULL) return -1;
    for (index = 0u; index < ring->slot_count; index++) {
        if (ring->slots[index].valid && ring->slots[index].tick == tick) return (int)index;
    }
    return -1;
}

bool mdkr_rollback_ring_restore(MdkrRollbackRing *ring, uint32_t tick,
                                bool at_tick_boundary) {
    const int index = find_slot(ring, tick);
    uint64_t started;
    uint64_t duration;
    if (index < 0) {
        if (ring != NULL) ring->stats.misses++;
        return false;
    }
    started = clock_now(ring);
    if (!mdkr_rollback_snapshot_restore(ring->registry, tick,
            ring->storage + (size_t)index * ring->snapshot_bytes,
            ring->snapshot_bytes, at_tick_boundary)) return false;
    ring->stats.restores++;
    if (ring->clock != NULL) {
        duration = elapsed(started, clock_now(ring));
        ring->stats.restore_ns_total = add_saturating(
            ring->stats.restore_ns_total, duration);
        if (duration > ring->stats.restore_ns_max)
            ring->stats.restore_ns_max = duration;
        timing_add(
            ring->stats.restore_timing_bins, duration,
            &ring->stats.restore_timing_overflow,
            &ring->stats.restore_over_p99_budget,
            &ring->stats.restore_over_tail_budget);
    }
    return true;
}

bool mdkr_rollback_ring_has(const MdkrRollbackRing *ring, uint32_t tick) {
    return find_slot(ring, tick) >= 0;
}

bool mdkr_rollback_ring_copy(
    const MdkrRollbackRing *ring, uint32_t tick, void *output,
    size_t output_size) {
    const int index = find_slot(ring, tick);
    if (index < 0 || output == NULL || output_size != ring->snapshot_bytes)
        return false;
    memcpy(output,
           ring->storage + (size_t)index * ring->snapshot_bytes,
           ring->snapshot_bytes);
    return true;
}
