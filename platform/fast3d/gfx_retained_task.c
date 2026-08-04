#include "gfx_retained_task.h"
#include "gfx_retained_budget_policy.h"
#include "address_domains.h"

#include <stdlib.h>
#include <string.h>

#define RETAINED_EXTERNAL_INITIAL 16u
#define RETAINED_EXTERNAL_MAX 16384u
#define RETAINED_EXTERNAL_BYTES_MAX (64u * 1024u * 1024u)

typedef struct RetainedDependency {
    const void *address;
    uint8_t *bytes;
    size_t size;
} RetainedDependency;

typedef struct RetainedDependencyList {
    RetainedDependency *entries;
    size_t count;
    size_t capacity;
    size_t bytes;
} RetainedDependencyList;

typedef struct RetainedTask {
    uint64_t authored_tick;
    const uint8_t *original_arena;
    uint8_t *arena;
    size_t arena_size;
    const void *display_list;
    uintptr_t segments[GFX_RETAINED_TASK_SEGMENTS];
    RetainedDependencyList dependencies;
    bool valid;
} RetainedTask;

typedef struct RetainedCapture {
    uint64_t authored_tick;
    const uint8_t *arena;
    size_t arena_size;
    RetainedDependencyList dependencies;
    bool active;
    bool failed;
    bool budget_rejected;
} RetainedCapture;

static RetainedTask s_task;
static RetainedCapture s_capture;
static GfxRetainedTaskStats s_stats;
static uint8_t *s_spare_arena;
static size_t s_spare_arena_size;
static size_t s_arena_copy_budget;
static bool s_arena_copy_budget_ready;

static size_t retained_arena_copy_budget(void) {
    const char *value;

    if (s_arena_copy_budget_ready) {
        return s_arena_copy_budget;
    }
    value = getenv("MDKR_RETAINED_ARENA_COPY_BUDGET_BYTES");
    s_arena_copy_budget = gfx_retained_arena_copy_budget_parse(value);
    s_arena_copy_budget_ready = true;
    return s_arena_copy_budget;
}

static size_t retained_resident_bytes(void) {
    size_t total = 0u;
    const size_t parts[] = {
        s_task.arena_size, s_spare_arena_size, s_task.dependencies.bytes,
        s_capture.dependencies.bytes
    };

    for (size_t index = 0u; index < sizeof(parts) / sizeof(parts[0]); index++) {
        if (parts[index] > SIZE_MAX - total) {
            return SIZE_MAX;
        }
        total += parts[index];
    }
    return total;
}

static bool address_in_range(const void *address, const void *base,
                             size_t size, size_t *offset) {
    uintptr_t value = (uintptr_t)address;
    uintptr_t begin = (uintptr_t)base;

    if (address == NULL || base == NULL || size == 0u || value < begin ||
        value - begin >= size) {
        return false;
    }
    if (offset != NULL) {
        *offset = (size_t)(value - begin);
    }
    return true;
}

static bool span_in_range(const void *address, size_t span,
                          const void *base, size_t size, size_t *offset) {
    size_t found;

    if (span == 0u || !address_in_range(address, base, size, &found) ||
        span > size - found) {
        return false;
    }
    if (offset != NULL) {
        *offset = found;
    }
    return true;
}

static void dependency_list_clear(RetainedDependencyList *list) {
    size_t index;

    if (list == NULL) {
        return;
    }
    for (index = 0u; index < list->count; index++) {
        free(list->entries[index].bytes);
        list->entries[index].bytes = NULL;
        list->entries[index].size = 0u;
        list->entries[index].address = NULL;
    }
    list->count = 0u;
    list->bytes = 0u;
}

static void dependency_list_shutdown(RetainedDependencyList *list) {
    if (list == NULL) {
        return;
    }
    dependency_list_clear(list);
    free(list->entries);
    memset(list, 0, sizeof(*list));
}

static bool dependency_list_grow(RetainedDependencyList *list, size_t need) {
    size_t capacity;
    RetainedDependency *grown;

    if (list == NULL || need > RETAINED_EXTERNAL_MAX) {
        return false;
    }
    if (list->capacity >= need) {
        return true;
    }
    capacity = list->capacity != 0u ? list->capacity
                                    : RETAINED_EXTERNAL_INITIAL;
    while (capacity < need) {
        if (capacity > RETAINED_EXTERNAL_MAX / 2u) {
            capacity = RETAINED_EXTERNAL_MAX;
        } else {
            capacity *= 2u;
        }
        if (capacity < need && capacity == RETAINED_EXTERNAL_MAX) {
            return false;
        }
    }
    grown = (RetainedDependency *)realloc(
        list->entries, capacity * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    memset(grown + list->capacity, 0,
           (capacity - list->capacity) * sizeof(*grown));
    list->entries = grown;
    list->capacity = capacity;
    return true;
}

static bool dependency_list_capture(RetainedDependencyList *list,
                                    const void *address, const void *bytes,
                                    size_t size) {
    RetainedDependency *entry = NULL;
    uint8_t *copy;
    size_t index;
    size_t next_bytes;

    if (list == NULL || address == NULL || bytes == NULL || size == 0u) {
        return false;
    }
    for (index = 0u; index < list->count; index++) {
        if (list->entries[index].address == address) {
            entry = &list->entries[index];
            break;
        }
    }
    if (entry != NULL && entry->size >= size) {
        memcpy(entry->bytes, bytes, size);
        return true;
    }
    next_bytes = list->bytes - (entry != NULL ? entry->size : 0u);
    if (size > RETAINED_EXTERNAL_BYTES_MAX - next_bytes) {
        return false;
    }
    copy = (uint8_t *)malloc(size);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, bytes, size);
    if (entry == NULL) {
        if (!dependency_list_grow(list, list->count + 1u)) {
            free(copy);
            return false;
        }
        entry = &list->entries[list->count++];
        entry->address = address;
    } else {
        free(entry->bytes);
    }
    entry->bytes = copy;
    entry->size = size;
    list->bytes = next_bytes + size;
    return true;
}

bool gfx_retained_task_capture_begin(uint64_t authored_tick,
                                     const void *arena, size_t arena_size) {
    const size_t budget = retained_arena_copy_budget();

    dependency_list_clear(&s_capture.dependencies);
    s_capture.authored_tick = authored_tick;
    s_capture.arena = (const uint8_t *)arena;
    s_capture.arena_size = arena_size;
    s_capture.active = arena != NULL && arena_size != 0u;
    s_capture.failed = !s_capture.active;
    s_capture.budget_rejected = s_capture.active && arena_size > budget;
    if (s_capture.budget_rejected) {
        s_capture.active = false;
        s_stats.arena_budget_rejections++;
    }
    s_stats.capture_begins++;
    s_stats.arena_copy_budget = budget;
    return s_capture.active;
}

bool gfx_retained_task_capture_dependency(const void *address,
                                          const void *bytes, size_t size) {
    if (!s_capture.active || s_capture.failed) {
        return false;
    }
    if (span_in_range(address, size, s_capture.arena,
                      s_capture.arena_size, NULL)) {
        return true;
    }
    /* A dependency that begins in the arena but crosses its end cannot be
     * represented by either ownership domain. Fail the complete publication
     * instead of advertising a truncated private image. */
    if (address_in_range(address, s_capture.arena,
                         s_capture.arena_size, NULL)) {
        s_capture.failed = true;
        return false;
    }
    if (!dependency_list_capture(
            &s_capture.dependencies, address, bytes, size)) {
        s_capture.failed = true;
        return false;
    }
    return true;
}

bool gfx_retained_task_capture_commit(
    const void *display_list,
    const uintptr_t segments[GFX_RETAINED_TASK_SEGMENTS]) {
    RetainedTask next = { 0 };
    RetainedDependencyList recycled_dependencies;
    uint8_t *recycled_arena;
    size_t recycled_arena_size;
    uint8_t *capture_arena;
    size_t display_offset;
    size_t index;

    if (s_capture.budget_rejected) {
        /* Expected load shedding, not a failed partial publication. The last
         * valid task remains available and the scheduler holds its endpoint. */
        s_capture.budget_rejected = false;
        return false;
    }
    if (!s_capture.active || s_capture.failed || display_list == NULL ||
        segments == NULL) {
        s_stats.capture_failures++;
        s_capture.active = false;
        return false;
    }
    capture_arena = s_spare_arena_size >= s_capture.arena_size
        ? s_spare_arena
        : (uint8_t *)malloc(s_capture.arena_size);
    if (capture_arena == NULL) {
        s_stats.capture_failures++;
        s_capture.active = false;
        return false;
    }
    next.arena = capture_arena;
    memcpy(next.arena, s_capture.arena, s_capture.arena_size);
    next.original_arena = s_capture.arena;
    next.arena_size = s_capture.arena_size;
    next.authored_tick = s_capture.authored_tick;

    if (address_in_range(display_list, s_capture.arena,
                         s_capture.arena_size, &display_offset)) {
        next.display_list = next.arena + display_offset;
    } else {
        next.display_list = display_list;
    }
    for (index = 0u; index < GFX_RETAINED_TASK_SEGMENTS; index++) {
        size_t segment_offset;
        const void *segment = (const void *)segments[index];
        if (address_in_range(segment, s_capture.arena,
                             s_capture.arena_size, &segment_offset)) {
            next.segments[index] = (uintptr_t)(next.arena + segment_offset);
        } else {
            next.segments[index] = segments[index];
        }
    }
    /* The staging list already owns private byte copies. Move it into the
     * publication instead of cloning every dependency once more; the previous
     * publication's list becomes the next staging allocation pool. */
    next.dependencies = s_capture.dependencies;
    memset(&s_capture.dependencies, 0, sizeof(s_capture.dependencies));
    next.valid = true;

    recycled_arena = s_task.arena;
    recycled_arena_size = s_task.arena_size;
    recycled_dependencies = s_task.dependencies;
    if (capture_arena != s_spare_arena) {
        free(s_spare_arena);
    }
    s_task = next;
    s_spare_arena = recycled_arena;
    s_spare_arena_size =
        recycled_arena != NULL ? recycled_arena_size : 0u;
    s_capture.dependencies = recycled_dependencies;
    s_capture.active = false;
    s_stats.captures++;
    s_stats.arena_bytes += s_task.arena_size;
    if (s_task.arena_size > s_stats.arena_peak) {
        s_stats.arena_peak = s_task.arena_size;
    }
    s_stats.external_dependencies += s_task.dependencies.count;
    s_stats.external_bytes += s_task.dependencies.bytes;
    if (s_task.dependencies.bytes > s_stats.external_peak) {
        s_stats.external_peak = s_task.dependencies.bytes;
    }
    {
        const size_t resident = retained_resident_bytes();
        if (resident > s_stats.resident_peak) {
            s_stats.resident_peak = resident;
        }
    }
    return true;
}

bool gfx_retained_task_acquire(uint64_t authored_tick,
                               GfxRetainedTaskView *out) {
    if (!s_task.valid || out == NULL || s_task.authored_tick != authored_tick) {
        s_stats.acquisition_rejects++;
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->authored_tick = s_task.authored_tick;
    out->original_arena = s_task.original_arena;
    out->retained_arena = s_task.arena;
    out->arena_size = s_task.arena_size;
    out->display_list = s_task.display_list;
    memcpy(out->segments, s_task.segments, sizeof(out->segments));
    s_stats.acquisitions++;
    return true;
}

const void *gfx_retained_task_original_address(const void *retained_address) {
    size_t offset;
    size_t index;

    if (s_task.valid &&
        address_in_range(retained_address, s_task.arena,
                         s_task.arena_size, &offset)) {
        return s_task.original_arena + offset;
    }
    if (s_task.valid) {
        for (index = 0u; index < s_task.dependencies.count; index++) {
            const RetainedDependency *entry =
                &s_task.dependencies.entries[index];
            if (address_in_range(retained_address, entry->bytes,
                                 entry->size, &offset)) {
                return (const uint8_t *)entry->address + offset;
            }
        }
    }
    return retained_address;
}

void *gfx_retained_task_resolve_arena_token(uint32_t token) {
    uint32_t candidates[2];
    uint32_t original_low;
    size_t index;

    if (!s_task.valid || token == 0u) {
        return NULL;
    }
    candidates[0] = token ^ UINT32_C(0x80000000);
    candidates[1] = token;
    original_low = mdkr_arena_token_from_host(s_task.original_arena);
    for (index = 0u; index < 2u; index++) {
        const uint32_t offset = candidates[index] - original_low;
        /* Modular subtraction deliberately handles a malloc window that
         * crosses a 4 GiB low-address boundary. The arena is necessarily
         * smaller than that token domain. */
        if ((uint64_t)offset < (uint64_t)s_task.arena_size) {
            s_stats.arena_resolutions++;
            return s_task.arena + offset;
        }
    }
    return NULL;
}

bool gfx_retained_task_lookup_dependency(const void *original_address,
                                         size_t size, const void **out) {
    size_t index;

    if (!s_task.valid || original_address == NULL || size == 0u || out == NULL) {
        return false;
    }
    for (index = 0u; index < s_task.dependencies.count; index++) {
        const RetainedDependency *entry = &s_task.dependencies.entries[index];
        size_t offset;
        if (span_in_range(original_address, size, entry->address,
                          entry->size, &offset)) {
            *out = entry->bytes + offset;
            s_stats.external_resolutions++;
            return true;
        }
    }
    return false;
}

bool gfx_retained_task_dependency_room(const void *retained_address,
                                       size_t *out) {
    size_t index;

    if (!s_task.valid || retained_address == NULL || out == NULL) {
        return false;
    }
    for (index = 0u; index < s_task.dependencies.count; index++) {
        const RetainedDependency *entry = &s_task.dependencies.entries[index];
        size_t offset;
        if (address_in_range(retained_address, entry->bytes,
                             entry->size, &offset)) {
            *out = entry->size - offset;
            return true;
        }
    }
    return false;
}

void gfx_retained_task_note_live_arena_poison(void) {
    s_stats.live_arena_poison_replays++;
}

void gfx_retained_task_invalidate(void) {
    s_task.valid = false;
    s_task.authored_tick = 0u;
    s_capture.active = false;
    s_capture.failed = false;
    s_capture.budget_rejected = false;
    dependency_list_clear(&s_capture.dependencies);
}

void gfx_retained_task_get_stats(GfxRetainedTaskStats *out) {
    if (out != NULL) {
        *out = s_stats;
        out->arena_copy_budget = retained_arena_copy_budget();
        out->resident_bytes = retained_resident_bytes();
    }
}

void gfx_retained_task_shutdown(void) {
    free(s_task.arena);
    free(s_spare_arena);
    dependency_list_shutdown(&s_task.dependencies);
    dependency_list_shutdown(&s_capture.dependencies);
    memset(&s_task, 0, sizeof(s_task));
    memset(&s_capture, 0, sizeof(s_capture));
    memset(&s_stats, 0, sizeof(s_stats));
    s_spare_arena = NULL;
    s_spare_arena_size = 0u;
}
