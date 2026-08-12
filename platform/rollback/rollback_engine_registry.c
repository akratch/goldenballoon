#include "rollback_engine_registry.h"

#include <stdio.h>

bool mdkr_rollback_register_live_allocation(
    MdkrRollbackSnapshotRegistry *registry, const void *interior,
    uint32_t tag) {
    const MdkrRollbackAllocationSpec allocation = {interior, tag};
    return mdkr_rollback_register_live_allocations(registry, &allocation, 1u);
}

bool mdkr_rollback_register_live_allocations(
    MdkrRollbackSnapshotRegistry *registry,
    const MdkrRollbackAllocationSpec *allocations,
    size_t allocation_count) {
    MdkrRollbackRangeSpec ranges[MDKR_ROLLBACK_MAX_RANGES];
    size_t index;
    if (registry == NULL || allocations == NULL || allocation_count == 0u ||
        allocation_count > MDKR_ROLLBACK_MAX_RANGES) {
        return false;
    }
    for (index = 0u; index < allocation_count; index++) {
        void *base = NULL;
        size_t size = 0u;
        if (!mdkr_mempool_allocation_span_in_pool(
                POOL_MAIN, allocations[index].interior, &base, &size)) {
            return false;
        }
        ranges[index].address = base;
        ranges[index].size = size;
        ranges[index].tag = allocations[index].tag;
        ranges[index].flags = MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION;
    }
    return mdkr_rollback_snapshot_register_batch(
        registry, ranges, allocation_count);
}

bool mdkr_rollback_register_mempool_state(
    MdkrRollbackSnapshotRegistry *registry, MemoryPools pool_index,
    uint32_t descriptor_tag, uint32_t backing_tag) {
    MdkrMemorySpan spans[MDKR_MEMPOOL_STATE_SPAN_COUNT];
    MdkrRollbackRangeSpec ranges[MDKR_MEMPOOL_STATE_SPAN_COUNT];
    unsigned index;

    if (!mdkr_mempool_pool_state_spans(pool_index, spans)) {
        return false;
    }
    for (index = 0u; index < MDKR_MEMPOOL_STATE_SPAN_COUNT; index++) {
        ranges[index].address = spans[index].base;
        ranges[index].size = spans[index].size;
        ranges[index].tag = index == 0u ? descriptor_tag : backing_tag;
        ranges[index].flags = index == 1u
                                  ? MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION
                                  : 0u;
    }
    return mdkr_rollback_snapshot_register_batch(
        registry, ranges, MDKR_MEMPOOL_STATE_SPAN_COUNT);
}

bool mdkr_rollback_register_object_pool_state(
    MdkrRollbackSnapshotRegistry *registry) {
    return mdkr_rollback_register_mempool_state(
        registry, POOL_OBJECT,
        MDKR_ROLLBACK_TAG_OBJECT_POOL_DESCRIPTOR,
        MDKR_ROLLBACK_TAG_OBJECT_POOL_BACKING);
}

bool mdkr_rollback_validate_live_allocations(
    MdkrRollbackSnapshotRegistry *registry) {
    unsigned index;
    u64 generation = 0u;
    if (registry == NULL ||
        registry->range_count > MDKR_ROLLBACK_MAX_RANGES) {
        return false;
    }
    if (mdkr_mempool_topology_generation(POOL_MAIN, &generation) &&
        registry->live_allocation_generation_valid &&
        registry->live_allocation_generation == generation) {
        return true;
    }
    for (index = 0u; index < registry->range_count; index++) {
        const MdkrRollbackRange *range = &registry->ranges[index];
        void *base = NULL;
        size_t size = 0u;
        if ((range->flags & MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION) == 0u) {
            continue;
        }
        if (!mdkr_mempool_allocation_span_in_pool(
                POOL_MAIN, range->address, &base, &size) ||
            base != range->address || size != range->size) {
            fprintf(stderr,
                    "[ROLLBACK] live allocation mismatch tag=%08x "
                    "expected=%p/%zu actual=%p/%zu\n",
                    (unsigned)range->tag, range->address, range->size,
                    base, size);
            return false;
        }
    }
    registry->live_allocation_generation = generation;
    registry->live_allocation_generation_valid = true;
    return true;
}
