/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/rollback/rollback_snapshot.h"

typedef struct State { uint32_t rng; int racers[4]; uint8_t objects[97]; } State;

static void rebuild(void *context) { (*(int *)context)++; }

static uint32_t snapshot_test_hash(uint32_t hash, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    while (size >= sizeof(uint32_t)) {
        uint32_t word;
        memcpy(&word, bytes, sizeof(word));
        hash = (hash ^ word) * UINT32_C(16777619);
        bytes += sizeof(word);
        size -= sizeof(word);
    }
    while (size-- > 0u) {
        hash = (hash ^ *bytes++) * UINT32_C(16777619);
    }
    return hash;
}

static void test_atomic_batch_registration(void) {
    uint8_t first[8] = {0};
    uint8_t second[8] = {0};
    MdkrRollbackSnapshotRegistry registry;
    MdkrRollbackRangeSpec ranges[2] = {
        {first, sizeof(first), 11u, 0u},
        {first + 4, 4u, 12u, 0u},
    };

    mdkr_rollback_snapshot_registry_init(&registry, 1u);
    assert(!mdkr_rollback_snapshot_register_batch(&registry, ranges, 2u));
    assert(registry.range_count == 0u && registry.total_bytes == 0u);
    ranges[1].address = second;
    ranges[1].size = sizeof(second);
    ranges[1].tag = 11u;
    assert(!mdkr_rollback_snapshot_register_batch(&registry, ranges, 2u));
    assert(registry.range_count == 0u && registry.total_bytes == 0u);
    ranges[1].tag = 12u;
    assert(mdkr_rollback_snapshot_register_batch(&registry, ranges, 2u));
    assert(registry.range_count == 2u);
    assert(registry.total_bytes == sizeof(first) + sizeof(second));
    registry.range_count = UINT8_MAX;
    assert(!mdkr_rollback_snapshot_register_batch(&registry, ranges, 1u));
}

int main(void) {
    State state = {7u, {1, 2, 3, 4}, {0}};
    uint32_t global_tick = 42u;
    int rebuilds = 0;
    MdkrRollbackSnapshotRegistry registry;
    size_t bytes;
    uint8_t *blob;
    MdkrRollbackSnapshotHeader *header;

    test_atomic_batch_registration();
    state.objects[13] = 99u;
    mdkr_rollback_snapshot_registry_init(&registry, UINT64_C(0x12345678));
    assert(mdkr_rollback_snapshot_register(&registry, &state, sizeof(state), 1u, 0u));
    assert(!mdkr_rollback_snapshot_register(&registry, &state.rng,
                                            sizeof(state.rng), 2u, 0u));
    assert(!mdkr_rollback_snapshot_register(&registry, &global_tick,
        sizeof(global_tick), 3u, MDKR_ROLLBACK_RANGE_HOST_HANDLE));
    assert(!mdkr_rollback_snapshot_register(&registry, &global_tick,
        sizeof(global_tick), 3u, 0x80000000u));
    assert(mdkr_rollback_snapshot_register(
        &registry, &global_tick, sizeof(global_tick), 3u,
        MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION));
    assert(registry.ranges[1].flags ==
           MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION);
    assert(mdkr_rollback_snapshot_register_rebuild(&registry, rebuild, &rebuilds));
    assert(mdkr_rollback_snapshot_freeze(&registry, UINT64_C(0xaabbccdd)));
    assert(!mdkr_rollback_snapshot_register(&registry, &rebuilds,
                                             sizeof(rebuilds), 4u, 0u));
    bytes = mdkr_rollback_snapshot_bytes(&registry);
    blob = (uint8_t *)malloc(bytes);
    assert(blob != NULL && mdkr_rollback_snapshot_capture(&registry, 10u, blob, bytes));
    assert(!mdkr_rollback_snapshot_capture(
        &registry, 10u, &state, bytes));
    state.rng = 999u; state.racers[2] = -5; state.objects[13] = 0u; global_tick = 900u;
    assert(!mdkr_rollback_snapshot_restore(&registry, 10u, blob, bytes, false));
    assert(state.rng == 999u); /* failed restore is atomic */
    assert(mdkr_rollback_snapshot_restore(&registry, 10u, blob, bytes, true));
    assert(state.rng == 7u && state.racers[2] == 3 && state.objects[13] == 99u);
    assert(global_tick == 42u && rebuilds == 1);

    header = (MdkrRollbackSnapshotHeader *)blob;
    header->manifest_digest ^= 1u;
    state.rng = 55u;
    assert(!mdkr_rollback_snapshot_restore(&registry, 10u, blob, bytes, true));
    assert(state.rng == 55u);
    header->manifest_digest ^= 1u;
    header->reserved = 1u;
    header->checksum = 0u; /* Even a recomputed checksum cannot bless extensions. */
    {
        uint32_t checksum = UINT32_C(2166136261);
        const uint8_t *payload = (const uint8_t *)blob + sizeof(*header);
        checksum = snapshot_test_hash(checksum, header, sizeof(*header));
        checksum = snapshot_test_hash(
            checksum, payload, (size_t)header->payload_bytes);
        header->checksum = checksum;
    }
    assert(!mdkr_rollback_snapshot_restore(&registry, 10u, blob, bytes, true));
    header->reserved = 0u;
    /* Restore the original valid checksum before the payload mutation arm. */
    assert(mdkr_rollback_snapshot_capture(&registry, 10u, blob, bytes));
    ((uint8_t *)blob)[bytes - 1u] ^= 1u;
    assert(!mdkr_rollback_snapshot_restore(&registry, 10u, blob, bytes, true));

    assert(!mdkr_rollback_snapshot_restore(
        &registry, 10u, &state, bytes, true));

    /* Omitted-range mutation control: an unregistered scalar demonstrably does
     * not rewind, so a restore identity gate that hashes it must diverge. */
    rebuilds = 77;
    assert(!mdkr_rollback_snapshot_restore(&registry, 10u, blob, bytes, true));
    assert(rebuilds == 77);
    free(blob);
    puts("test_rollback_snapshot: PASS");
    return 0;
}
