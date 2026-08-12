#include "rollback_snapshot.h"

#include <string.h>

#define SNAPSHOT_MAGIC_0 ((uint8_t)'G')
#define SNAPSHOT_MAGIC_1 ((uint8_t)'B')
#define SNAPSHOT_MAGIC_2 ((uint8_t)'R')
#define SNAPSHOT_MAGIC_3 ((uint8_t)'S')

static uint64_t hash64(uint64_t hash, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;
    for (index = 0u; index < size; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint32_t hash32(uint32_t hash, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    /* Snapshot checksums are an in-process corruption oracle, not a wire or
     * cryptographic format. Fold complete 32-bit words with memcpy-safe loads
     * and retain byte folding for the tail. This examines every input bit while
     * avoiding one wasm multiply per payload byte; a 400+ KiB authority image
     * is checksummed every authored tick, so the byte-at-a-time loop could push
     * an otherwise 30 Hz browser frame over its next rAF quantum. */
    while (size >= sizeof(uint32_t)) {
        uint32_t word;
        memcpy(&word, bytes, sizeof(word));
        hash ^= word;
        hash *= UINT32_C(16777619);
        bytes += sizeof(word);
        size -= sizeof(word);
    }
    while (size-- > 0u) {
        hash ^= *bytes;
        hash *= UINT32_C(16777619);
        bytes++;
    }
    return hash;
}

void mdkr_rollback_snapshot_registry_init(
    MdkrRollbackSnapshotRegistry *registry, uint64_t process_cookie) {
    if (registry == NULL) return;
    memset(registry, 0, sizeof(*registry));
    registry->process_cookie = process_cookie != 0u ? process_cookie : 1u;
}

bool mdkr_rollback_snapshot_register(
    MdkrRollbackSnapshotRegistry *registry, void *address, size_t size,
    uint32_t tag, uint32_t flags) {
    const MdkrRollbackRangeSpec range = {address, size, tag, flags};
    return mdkr_rollback_snapshot_register_batch(registry, &range, 1u);
}

bool mdkr_rollback_snapshot_register_batch(
    MdkrRollbackSnapshotRegistry *registry,
    const MdkrRollbackRangeSpec *ranges, size_t range_count) {
    size_t total;
    size_t index;
    if (registry == NULL || registry->frozen || ranges == NULL ||
        range_count == 0u || registry->range_count > MDKR_ROLLBACK_MAX_RANGES ||
        range_count > MDKR_ROLLBACK_MAX_RANGES - registry->range_count) {
        return false;
    }
    total = registry->total_bytes;
    for (index = 0u; index < range_count; index++) {
        const uintptr_t begin = (uintptr_t)ranges[index].address;
        uintptr_t end;
        size_t other;
        if (ranges[index].address == NULL || ranges[index].size == 0u ||
            ranges[index].tag == 0u ||
            (ranges[index].flags & ~MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION) != 0u ||
            ranges[index].size > UINTPTR_MAX - begin ||
            ranges[index].size > SIZE_MAX - total) {
            return false;
        }
        end = begin + ranges[index].size;
        for (other = 0u; other < registry->range_count; other++) {
            const uintptr_t other_begin =
                (uintptr_t)registry->ranges[other].address;
            const uintptr_t other_end =
                other_begin + registry->ranges[other].size;
            if ((begin < other_end && other_begin < end) ||
                registry->ranges[other].tag == ranges[index].tag) {
                return false;
            }
        }
        for (other = 0u; other < index; other++) {
            const uintptr_t other_begin = (uintptr_t)ranges[other].address;
            const uintptr_t other_end = other_begin + ranges[other].size;
            if ((begin < other_end && other_begin < end) ||
                ranges[other].tag == ranges[index].tag) {
                return false;
            }
        }
        total += ranges[index].size;
    }
    for (index = 0u; index < range_count; index++) {
        registry->ranges[registry->range_count++] = (MdkrRollbackRange){
            ranges[index].address, ranges[index].size, ranges[index].tag,
            ranges[index].flags};
    }
    registry->total_bytes = total;
    return true;
}

bool mdkr_rollback_snapshot_register_rebuild(
    MdkrRollbackSnapshotRegistry *registry, MdkrRollbackRebuildHook hook,
    void *context) {
    if (registry == NULL || registry->frozen || hook == NULL ||
        registry->rebuild_count >= MDKR_ROLLBACK_MAX_REBUILD_HOOKS) return false;
    registry->rebuild[registry->rebuild_count++] =
        (MdkrRollbackRebuildEntry){hook, context};
    return true;
}

bool mdkr_rollback_snapshot_freeze(
    MdkrRollbackSnapshotRegistry *registry, uint64_t manifest_digest) {
    uint64_t digest = UINT64_C(1469598103934665603);
    unsigned index;
    if (registry == NULL || registry->frozen || registry->range_count == 0u ||
        manifest_digest == 0u) return false;
    for (index = 0u; index < registry->range_count; index++) {
        const uintptr_t address = (uintptr_t)registry->ranges[index].address;
        digest = hash64(digest, &address, sizeof(address));
        digest = hash64(digest, &registry->ranges[index].size,
                        sizeof(registry->ranges[index].size));
        digest = hash64(digest, &registry->ranges[index].tag,
                        sizeof(registry->ranges[index].tag));
        digest = hash64(digest, &registry->ranges[index].flags,
                        sizeof(registry->ranges[index].flags));
    }
    registry->manifest_digest = manifest_digest;
    registry->layout_digest = digest;
    registry->frozen = true;
    return true;
}

size_t mdkr_rollback_snapshot_bytes(
    const MdkrRollbackSnapshotRegistry *registry) {
    if (registry == NULL || !registry->frozen ||
        registry->total_bytes > SIZE_MAX - sizeof(MdkrRollbackSnapshotHeader)) return 0u;
    return sizeof(MdkrRollbackSnapshotHeader) + registry->total_bytes;
}

static uint32_t snapshot_checksum(
    MdkrRollbackSnapshotHeader header, const uint8_t *payload) {
    uint32_t checksum = UINT32_C(2166136261);
    header.checksum = 0u;
    checksum = hash32(checksum, &header, sizeof(header));
    return hash32(checksum, payload, (size_t)header.payload_bytes);
}

static bool snapshot_buffer_disjoint(
    const MdkrRollbackSnapshotRegistry *registry, const void *buffer,
    size_t size) {
    const uintptr_t begin = (uintptr_t)buffer;
    uintptr_t end;
    unsigned index;
    if (buffer == NULL || size == 0u || size > UINTPTR_MAX - begin) return false;
    end = begin + size;
    for (index = 0u; index < registry->range_count; index++) {
        const uintptr_t range_begin = (uintptr_t)registry->ranges[index].address;
        const uintptr_t range_end = range_begin + registry->ranges[index].size;
        if (begin < range_end && range_begin < end) return false;
    }
    return true;
}

bool mdkr_rollback_snapshot_capture(
    const MdkrRollbackSnapshotRegistry *registry, uint32_t tick,
    void *output, size_t capacity) {
    MdkrRollbackSnapshotHeader header;
    uint8_t *cursor;
    unsigned index;
    const size_t required = mdkr_rollback_snapshot_bytes(registry);
    if (required == 0u || output == NULL || capacity < required ||
        !snapshot_buffer_disjoint(registry, output, required)) return false;
    memset(&header, 0, sizeof(header));
    header.magic[0] = SNAPSHOT_MAGIC_0; header.magic[1] = SNAPSHOT_MAGIC_1;
    header.magic[2] = SNAPSHOT_MAGIC_2; header.magic[3] = SNAPSHOT_MAGIC_3;
    header.version = MDKR_ROLLBACK_SNAPSHOT_VERSION;
    header.range_count = registry->range_count;
    header.tick = tick;
    header.process_cookie = registry->process_cookie;
    header.manifest_digest = registry->manifest_digest;
    header.layout_digest = registry->layout_digest;
    header.payload_bytes = registry->total_bytes;
    cursor = (uint8_t *)output + sizeof(header);
    for (index = 0u; index < registry->range_count; index++) {
        memcpy(cursor, registry->ranges[index].address, registry->ranges[index].size);
        cursor += registry->ranges[index].size;
    }
    header.checksum = snapshot_checksum(
        header, (const uint8_t *)output + sizeof(header));
    memcpy(output, &header, sizeof(header));
    return true;
}

bool mdkr_rollback_snapshot_restore(
    const MdkrRollbackSnapshotRegistry *registry, uint32_t expected_tick,
    const void *snapshot, size_t size, bool at_tick_boundary) {
    MdkrRollbackSnapshotHeader header;
    const uint8_t *cursor;
    unsigned index;
    if (registry == NULL || !registry->frozen || snapshot == NULL ||
        !at_tick_boundary || size != mdkr_rollback_snapshot_bytes(registry) ||
        !snapshot_buffer_disjoint(registry, snapshot, size)) return false;
    memcpy(&header, snapshot, sizeof(header));
    if (header.magic[0] != SNAPSHOT_MAGIC_0 || header.magic[1] != SNAPSHOT_MAGIC_1 ||
        header.magic[2] != SNAPSHOT_MAGIC_2 || header.magic[3] != SNAPSHOT_MAGIC_3 ||
        header.version != MDKR_ROLLBACK_SNAPSHOT_VERSION ||
        header.range_count != registry->range_count || header.tick != expected_tick ||
        header.process_cookie != registry->process_cookie ||
        header.manifest_digest != registry->manifest_digest ||
        header.layout_digest != registry->layout_digest ||
        header.payload_bytes != registry->total_bytes ||
        header.reserved != 0u ||
        header.checksum != snapshot_checksum(
            header, (const uint8_t *)snapshot + sizeof(header))) return false;
    cursor = (const uint8_t *)snapshot + sizeof(header);
    for (index = 0u; index < registry->range_count; index++) {
        memcpy(registry->ranges[index].address, cursor, registry->ranges[index].size);
        cursor += registry->ranges[index].size;
    }
    for (index = 0u; index < registry->rebuild_count; index++) {
        registry->rebuild[index].hook(registry->rebuild[index].context);
    }
    return true;
}
