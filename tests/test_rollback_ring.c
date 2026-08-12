/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/rollback/rollback_ring.h"

typedef struct FakeClock {
    uint64_t value;
    uint64_t step;
} FakeClock;

static uint64_t fake_clock(void *context) {
    FakeClock *clock = (FakeClock *)context;
    clock->value += clock->step;
    return clock->value;
}

int main(void) {
    int state = 0;
    MdkrRollbackSnapshotRegistry registry;
    MdkrRollbackRing ring;
    FakeClock clock = {0u, 125u};
    mdkr_rollback_snapshot_registry_init(&registry, 9u);
    assert(mdkr_rollback_snapshot_register(&registry, &state, sizeof(state), 1u, 0u));
    assert(mdkr_rollback_snapshot_freeze(&registry, 77u));
    {
        MdkrRollbackSnapshotRegistry oversized = registry;
        oversized.total_bytes = MDKR_ROLLBACK_MAX_RING_BYTES / 3u;
        memset(&ring, 0xa5, sizeof(ring));
        assert(!mdkr_rollback_ring_init(&ring, &oversized, 3u));
        /* Rejection is fail-atomic: no partial allocation escapes. */
        for (size_t byte = 0u; byte < sizeof(ring); byte++) {
            assert(((const unsigned char *)&ring)[byte] == 0xa5u);
        }
    }
    assert(mdkr_rollback_ring_init(&ring, &registry, 3u));
    mdkr_rollback_ring_set_clock(&ring, fake_clock, &clock);
    for (uint32_t tick = 10u; tick <= 13u; tick++) {
        state = (int)tick * 2;
        assert(mdkr_rollback_ring_capture(&ring, tick));
    }
    assert(!mdkr_rollback_ring_has(&ring, 10u));
    assert(mdkr_rollback_ring_has(&ring, 11u));
    {
        const size_t bytes = mdkr_rollback_snapshot_bytes(&registry);
        void *copy = malloc(bytes);
        assert(copy != NULL);
        assert(!mdkr_rollback_ring_copy(&ring, 10u, copy, bytes));
        assert(!mdkr_rollback_ring_copy(&ring, 11u, NULL, bytes));
        assert(!mdkr_rollback_ring_copy(&ring, 11u, copy, bytes - 1u));
        assert(mdkr_rollback_ring_copy(&ring, 11u, copy, bytes));
        assert(memcmp(copy, ring.storage + ring.snapshot_bytes, bytes) == 0);
        free(copy);
    }
    state = 999;
    assert(mdkr_rollback_ring_restore(&ring, 12u, true));
    assert(state == 24);
    assert(!mdkr_rollback_ring_restore(&ring, 10u, true));
    assert(ring.stats.captures == 4u && ring.stats.restores == 1u &&
           ring.stats.misses == 1u && ring.stats.allocated_bytes ==
             3u * mdkr_rollback_snapshot_bytes(&registry));
    assert(ring.stats.capture_ns_total == 4u * 125u &&
           ring.stats.capture_ns_max == 125u &&
           ring.stats.restore_ns_total == 125u &&
           ring.stats.restore_ns_max == 125u &&
           ring.stats.timing_samples == 4u);
    assert(mdkr_rollback_ring_percentile_ns(&ring.stats, true, 50u) ==
           MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS);
    assert(mdkr_rollback_ring_percentile_ns(&ring.stats, false, 99u) ==
           MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS);
    assert(mdkr_rollback_ring_percentile_ns(&ring.stats, true, 0u) == 0u);

    {
        MdkrRollbackTimingHistogram histogram = {0};
        mdkr_rollback_timing_record(&histogram, 100u);
        mdkr_rollback_timing_record(&histogram, 20000u);
        assert(histogram.samples == 2u && histogram.ns_total == 20100u &&
               histogram.ns_max == 20000u);
        assert(mdkr_rollback_timing_percentile_ns(&histogram, 50u) ==
               MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS);
        assert(mdkr_rollback_timing_percentile_ns(&histogram, 99u) ==
               3u * MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS);
        assert(mdkr_rollback_timing_percentile_ns(&histogram, 0u) == 0u);
        mdkr_rollback_timing_record(
            &histogram,
            MDKR_ROLLBACK_TIMING_BIN_COUNT *
                MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS + 1u);
        assert(histogram.overflow == 1u &&
               histogram.over_p99_budget == 1u &&
               histogram.over_tail_budget == 1u);
        assert(mdkr_rollback_timing_percentile_ns(&histogram, 99u) ==
               MDKR_ROLLBACK_TIMING_BIN_COUNT *
                   MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS + 1u);
    }

    /* The terminal bucket is explicit overflow and reports the observed max,
     * while budget counters make a pathological host pause non-decorative. */
    clock.step = MDKR_ROLLBACK_TIMING_BIN_COUNT *
        MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS + 1u;
    state = 28;
    assert(mdkr_rollback_ring_capture(&ring, 14u));
    assert(ring.stats.capture_timing_overflow == 1u &&
           ring.stats.capture_over_p99_budget == 1u &&
           ring.stats.capture_over_tail_budget == 1u);
    assert(mdkr_rollback_ring_percentile_ns(&ring.stats, true, 50u) ==
           MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS);
    assert(mdkr_rollback_ring_percentile_ns(&ring.stats, true, 95u) ==
           MDKR_ROLLBACK_TIMING_BIN_COUNT *
               MDKR_ROLLBACK_TIMING_BIN_WIDTH_NS + 1u);
    mdkr_rollback_ring_destroy(&ring);
    assert(ring.storage == NULL);
    puts("test_rollback_ring: PASS");
    return 0;
}
