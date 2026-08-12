#ifndef MDKR_ROLLBACK_ENGINE_REGISTRY_H
#define MDKR_ROLLBACK_ENGINE_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#include "memory.h"
#include "rollback_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ROLLBACK_TAG_OBJECT_POOL_DESCRIPTOR UINT32_C(0x4f424a01)
#define MDKR_ROLLBACK_TAG_OBJECT_POOL_BACKING UINT32_C(0x4f424a02)
#define MDKR_ROLLBACK_TAG_MODEL_POOL_DESCRIPTOR UINT32_C(0x4d4f4401)
#define MDKR_ROLLBACK_TAG_MODEL_POOL_BACKING UINT32_C(0x4d4f4402)

typedef struct MdkrRollbackAllocationSpec {
    const void *interior;
    uint32_t tag;
} MdkrRollbackAllocationSpec;

/* Resolve an interior pointer through the main allocator and register the
 * complete live allocation, never a caller-guessed byte count. Nested mutable
 * pools use mdkr_rollback_register_mempool_state instead. */
bool mdkr_rollback_register_live_allocation(
    MdkrRollbackSnapshotRegistry *registry, const void *interior,
    uint32_t tag);
bool mdkr_rollback_register_live_allocations(
    MdkrRollbackSnapshotRegistry *registry,
    const MdkrRollbackAllocationSpec *allocations,
    size_t allocation_count);

/* Register one dynamic engine sub-pool as two atomic ranges: its mutable pool
 * descriptor and its bounded backing allocation (slot metadata plus data). */
bool mdkr_rollback_register_mempool_state(
    MdkrRollbackSnapshotRegistry *registry, MemoryPools pool_index,
    uint32_t descriptor_tag, uint32_t backing_tag);

/* Concrete first engine authority range: the dynamic object/particle pool. */
bool mdkr_rollback_register_object_pool_state(
    MdkrRollbackSnapshotRegistry *registry);

/* Every allocator-owned range must still resolve to the same complete live
 * allocation. The main allocator's monotonic topology generation makes an
 * unchanged tick O(1); a changed topology triggers the full exact-span scan.
 * Unrelated cache activity is accepted because live blocks never move. */
bool mdkr_rollback_validate_live_allocations(
    MdkrRollbackSnapshotRegistry *registry);

#ifdef __cplusplus
}
#endif
#endif
