/* In-process authoritative range snapshots. Never serialized or networked. */
#ifndef MDKR_ROLLBACK_SNAPSHOT_H
#define MDKR_ROLLBACK_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ROLLBACK_SNAPSHOT_VERSION 1u
#define MDKR_ROLLBACK_MAX_RANGES 192u
#define MDKR_ROLLBACK_MAX_REBUILD_HOOKS 16u
#define MDKR_ROLLBACK_RANGE_HOST_HANDLE 0x01u
#define MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION 0x02u

typedef void (*MdkrRollbackRebuildHook)(void *context);

typedef struct MdkrRollbackRange {
    void *address;
    size_t size;
    uint32_t tag;
    uint32_t flags;
} MdkrRollbackRange;

typedef struct MdkrRollbackRangeSpec {
    void *address;
    size_t size;
    uint32_t tag;
    uint32_t flags;
} MdkrRollbackRangeSpec;

typedef struct MdkrRollbackRebuildEntry {
    MdkrRollbackRebuildHook hook;
    void *context;
} MdkrRollbackRebuildEntry;

typedef struct MdkrRollbackSnapshotRegistry {
    MdkrRollbackRange ranges[MDKR_ROLLBACK_MAX_RANGES];
    MdkrRollbackRebuildEntry rebuild[MDKR_ROLLBACK_MAX_REBUILD_HOOKS];
    size_t total_bytes;
    uint64_t process_cookie;
    uint64_t manifest_digest;
    uint64_t layout_digest;
    uint64_t live_allocation_generation;
    uint8_t range_count;
    uint8_t rebuild_count;
    bool frozen;
    bool live_allocation_generation_valid;
} MdkrRollbackSnapshotRegistry;

typedef struct MdkrRollbackSnapshotHeader {
    uint8_t magic[4];
    uint16_t version;
    uint16_t range_count;
    uint32_t tick;
    uint64_t process_cookie;
    uint64_t manifest_digest;
    uint64_t layout_digest;
    uint64_t payload_bytes;
    uint32_t checksum;
    uint32_t reserved;
} MdkrRollbackSnapshotHeader;

void mdkr_rollback_snapshot_registry_init(
    MdkrRollbackSnapshotRegistry *registry, uint64_t process_cookie);
bool mdkr_rollback_snapshot_register(
    MdkrRollbackSnapshotRegistry *registry, void *address, size_t size,
    uint32_t tag, uint32_t flags);
/* Validate and append all ranges atomically. A failed batch leaves the
 * registry byte-for-byte unchanged. */
bool mdkr_rollback_snapshot_register_batch(
    MdkrRollbackSnapshotRegistry *registry,
    const MdkrRollbackRangeSpec *ranges, size_t range_count);
bool mdkr_rollback_snapshot_register_rebuild(
    MdkrRollbackSnapshotRegistry *registry, MdkrRollbackRebuildHook hook,
    void *context);
bool mdkr_rollback_snapshot_freeze(
    MdkrRollbackSnapshotRegistry *registry, uint64_t manifest_digest);
size_t mdkr_rollback_snapshot_bytes(
    const MdkrRollbackSnapshotRegistry *registry);
bool mdkr_rollback_snapshot_capture(
    const MdkrRollbackSnapshotRegistry *registry, uint32_t tick,
    void *output, size_t capacity);
bool mdkr_rollback_snapshot_restore(
    const MdkrRollbackSnapshotRegistry *registry, uint32_t expected_tick,
    const void *snapshot, size_t size, bool at_tick_boundary);

#ifdef __cplusplus
}
#endif
#endif
