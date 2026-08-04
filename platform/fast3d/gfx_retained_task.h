/*
 * Immutable renderer task retained for presentation-only replay.
 *
 * DKR authors display lists and their transitive matrix/vertex/texture data in
 * a reusable RDRAM stand-in. By the time a high-refresh presentation wants to
 * draw an intermediate image, the game may already have reused that memory for
 * the next task. A command pointer is therefore not ownership.
 *
 * This module publishes one complete authored task transactionally. The arena
 * image, top-level list, segment bases, and explicitly observed non-arena
 * dependencies all belong to the same immutable authored-tick token. Readers
 * either acquire that whole task or fail; a partial capture is never visible.
 */
#ifndef MDKR_GFX_RETAINED_TASK_H
#define MDKR_GFX_RETAINED_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_RETAINED_TASK_SEGMENTS 16u

typedef struct GfxRetainedTaskView {
    uint64_t authored_tick;
    const void *original_arena;
    const void *retained_arena;
    size_t arena_size;
    const void *display_list;
    uintptr_t segments[GFX_RETAINED_TASK_SEGMENTS];
} GfxRetainedTaskView;

typedef struct GfxRetainedTaskStats {
    uint64_t capture_begins;
    uint64_t captures;
    uint64_t capture_failures;
    uint64_t acquisitions;
    uint64_t acquisition_rejects;
    uint64_t live_arena_poison_replays;
    uint64_t arena_resolutions;
    uint64_t external_resolutions;
    uint64_t external_dependencies;
    uint64_t external_bytes;
    uint64_t arena_bytes;
    uint64_t arena_budget_rejections;
    /* Payload bytes, not allocator capacity. These make the replay-memory
     * ceiling observable in normal presentation telemetry. */
    size_t arena_copy_budget;
    size_t arena_peak;
    size_t resident_bytes;
    size_t resident_peak;
    size_t external_peak;
} GfxRetainedTaskStats;

/* Start staging the dependencies observed by one real HLE walk. This never
 * disturbs the last complete published task. */
bool gfx_retained_task_capture_begin(uint64_t authored_tick,
                                     const void *arena, size_t arena_size);

/* Copy a dependency that does not belong to the arena. Repeated observations
 * of the same address retain the largest complete byte range. Arena pointers
 * are already covered by the arena image and are accepted as no-ops. */
bool gfx_retained_task_capture_dependency(const void *address,
                                          const void *bytes, size_t size);

/* Publish the staged task atomically. `display_list` and every arena-backed
 * segment are rebased into the private image. Non-arena list/segment pointers
 * remain valid only when their storage is immutable or was copied explicitly. */
bool gfx_retained_task_capture_commit(
    const void *display_list,
    const uintptr_t segments[GFX_RETAINED_TASK_SEGMENTS]);

/* Acquire only the exact task requested by the replay owner. */
bool gfx_retained_task_acquire(uint64_t authored_tick,
                               GfxRetainedTaskView *out);

/* Translate between the retained and original address identities. Registry
 * ownership remains keyed to the original task addresses even while bytes are
 * read from the private image. */
const void *gfx_retained_task_original_address(const void *retained_address);

/* Resolve the raw/flipped 32-bit arena encodings used by DKR against the
 * retained task's ORIGINAL arena window, then return the matching private byte
 * address. Registry/global resolution must still run before this helper so a
 * non-arena pointer whose low bits collide with the arena remains authoritative.
 */
void *gfx_retained_task_resolve_arena_token(uint32_t token);

/* Retrieve the exact copied bytes for an explicitly observed external
 * dependency. The original address is the stable lookup identity. A size of
 * one is sufficient when the caller only needs the retained base address and
 * the command-specific consumer will perform the bounded read later. */
bool gfx_retained_task_lookup_dependency(const void *original_address,
                                         size_t size, const void **out);
/* Return the readable suffix of one private external dependency. This lets the
 * HLE apply the same span checks to retained external bytes as to arena bytes. */
bool gfx_retained_task_dependency_room(const void *retained_address,
                                       size_t *out);

/* Internal adversarial-gate telemetry: one replay completed while every byte
 * of the live arena was temporarily poisoned and later restored. */
void gfx_retained_task_note_live_arena_poison(void);

void gfx_retained_task_invalidate(void);
void gfx_retained_task_get_stats(GfxRetainedTaskStats *out);
void gfx_retained_task_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_GFX_RETAINED_TASK_H */
