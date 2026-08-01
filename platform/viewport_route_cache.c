#include "viewport_route_cache.h"

#include <string.h>

_Static_assert(
    (MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY &
     (MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY - 1u)) == 0u,
    "viewport route index capacity must be a power of two");
_Static_assert(
    MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY >=
        MDKR_VIEWPORT_ROUTE_MAX_OBJECTS * 2u,
    "viewport route index must keep load factor at or below one half");

static unsigned route_hash(const void *object) {
    uintptr_t value = (uintptr_t)object;
    value >>= 4;
#if UINTPTR_MAX > UINT32_MAX
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
#else
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
#endif
    return (unsigned)value & (MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY - 1u);
}

static int route_slot(
    const MdkrViewportRouteCache *cache, const void *object) {
    unsigned bucket;
    unsigned probes;

    if (cache == NULL || object == NULL) {
        return -1;
    }
    bucket = route_hash(object);
    for (probes = 0; probes < MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY; probes++) {
        uint16_t encoded = cache->index[bucket];
        if (encoded == 0u) {
            return -1;
        }
        if (cache->slots[encoded - 1u].object == object) {
            return (int)encoded - 1;
        }
        bucket = (bucket + 1u) &
                 (MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY - 1u);
    }
    return -1;
}

static int route_bind(MdkrViewportRouteCache *cache, const void *object) {
    unsigned bucket;
    unsigned probes;
    unsigned slot;
    int existing_slot;

    if (cache == NULL || object == NULL) {
        return -1;
    }
    existing_slot = route_slot(cache, object);
    if (existing_slot >= 0) {
        return existing_slot;
    }
    if (cache->count >= MDKR_VIEWPORT_ROUTE_MAX_OBJECTS) {
        return -1;
    }
    bucket = route_hash(object);
    for (probes = 0; probes < MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY; probes++) {
        if (cache->index[bucket] == 0u) {
            slot = cache->count++;
            memset(&cache->slots[slot], 0, sizeof(cache->slots[slot]));
            cache->slots[slot].object = object;
            cache->index[bucket] = (uint16_t)(slot + 1u);
            return (int)slot;
        }
        bucket = (bucket + 1u) &
                 (MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY - 1u);
    }
    return -1;
}

void mdkr_viewport_route_cache_reset(MdkrViewportRouteCache *cache) {
    if (cache == NULL) {
        return;
    }
    memset(cache->index, 0, sizeof(cache->index));
    cache->count = 0u;
}

bool mdkr_viewport_route_cache_store(
    MdkrViewportRouteCache *cache, const void *object, unsigned viewport,
    MdkrViewportRoutePass pass, int opacity, int visible) {
    MdkrViewportRoute *route;
    int slot;

    if (viewport >= MDKR_VIEWPORT_ROUTE_MAX_VIEWPORTS ||
        (unsigned)pass >= MDKR_VIEWPORT_ROUTE_PASS_COUNT) {
        return false;
    }
    slot = route_bind(cache, object);
    if (slot < 0) {
        return false;
    }
    route = &cache->slots[slot].routes[viewport][pass];
    route->opacity = (int16_t)opacity;
    route->visible = (int16_t)visible;
    route->admitted = true;
    return true;
}

bool mdkr_viewport_route_cache_load(
    const MdkrViewportRouteCache *cache, const void *object,
    unsigned viewport, MdkrViewportRoutePass pass,
    MdkrViewportRoute *route) {
    const MdkrViewportRoute *stored;
    int slot;

    if (route == NULL || viewport >= MDKR_VIEWPORT_ROUTE_MAX_VIEWPORTS ||
        (unsigned)pass >= MDKR_VIEWPORT_ROUTE_PASS_COUNT) {
        return false;
    }
    slot = route_slot(cache, object);
    if (slot < 0) {
        return false;
    }
    stored = &cache->slots[slot].routes[viewport][pass];
    if (!stored->admitted) {
        return false;
    }
    *route = *stored;
    return true;
}
