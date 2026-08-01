#ifndef MDKR_VIEWPORT_ROUTE_CACHE_H
#define MDKR_VIEWPORT_ROUTE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MDKR_VIEWPORT_ROUTE_MAX_OBJECTS 512u
#define MDKR_VIEWPORT_ROUTE_MAX_VIEWPORTS 4u
#define MDKR_VIEWPORT_ROUTE_PASS_COUNT 3u
#define MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY 1024u

typedef enum MdkrViewportRoutePass {
    MDKR_VIEWPORT_ROUTE_OPAQUE = 0,
    MDKR_VIEWPORT_ROUTE_SPECIAL,
    MDKR_VIEWPORT_ROUTE_BLEND,
} MdkrViewportRoutePass;

typedef struct MdkrViewportRoute {
    int16_t opacity;
    int16_t visible;
    bool admitted;
} MdkrViewportRoute;

typedef struct MdkrViewportRouteSlot {
    const void *object;
    MdkrViewportRoute routes[MDKR_VIEWPORT_ROUTE_MAX_VIEWPORTS]
                            [MDKR_VIEWPORT_ROUTE_PASS_COUNT];
} MdkrViewportRouteSlot;

typedef struct MdkrViewportRouteCache {
    MdkrViewportRouteSlot slots[MDKR_VIEWPORT_ROUTE_MAX_OBJECTS];
    /* Slot index + 1; zero is the empty sentinel. */
    uint16_t index[MDKR_VIEWPORT_ROUTE_INDEX_CAPACITY];
    uint16_t count;
} MdkrViewportRouteCache;

void mdkr_viewport_route_cache_reset(MdkrViewportRouteCache *cache);

/* Stores one admitted draw route. False means invalid input or object capacity
 * exhaustion; a caller must then fail closed rather than borrow another
 * viewport's state. */
bool mdkr_viewport_route_cache_store(
    MdkrViewportRouteCache *cache, const void *object, unsigned viewport,
    MdkrViewportRoutePass pass, int opacity, int visible);

bool mdkr_viewport_route_cache_load(
    const MdkrViewportRouteCache *cache, const void *object,
    unsigned viewport, MdkrViewportRoutePass pass,
    MdkrViewportRoute *route);

#endif
