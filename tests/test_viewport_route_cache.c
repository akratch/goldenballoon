#include "viewport_route_cache.h"

#include <stdio.h>

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void test_split(unsigned viewports) {
    MdkrViewportRouteCache cache = { 0 };
    MdkrViewportRoute route;
    int object_a;
    int object_b;
    unsigned viewport;

    mdkr_viewport_route_cache_reset(&cache);
    for (viewport = 0; viewport < viewports; viewport++) {
        MdkrViewportRoutePass pass =
            viewport + 1u == viewports ? MDKR_VIEWPORT_ROUTE_BLEND :
                                         MDKR_VIEWPORT_ROUTE_OPAQUE;
        int opacity = viewport + 1u == viewports ? 64 : 255;
        expect("object A route stored",
               mdkr_viewport_route_cache_store(
                   &cache, &object_a, viewport, pass, opacity, opacity));
        /* A second pointer and opposite insertion order exercise the address
         * index independently of the draw order used to consume it. */
        expect("object B route stored",
               mdkr_viewport_route_cache_store(
                   &cache, &object_b, viewports - viewport - 1u,
                   MDKR_VIEWPORT_ROUTE_SPECIAL, 173, 1));
    }

    for (viewport = 0; viewport < viewports; viewport++) {
        MdkrViewportRoutePass pass =
            viewport + 1u == viewports ? MDKR_VIEWPORT_ROUTE_BLEND :
                                         MDKR_VIEWPORT_ROUTE_OPAQUE;
        int opacity = viewport + 1u == viewports ? 64 : 255;
        expect("object A per-viewport route loads",
               mdkr_viewport_route_cache_load(
                   &cache, &object_a, viewport, pass, &route));
        expect("object A per-viewport opacity is exact",
               route.opacity == opacity && route.visible == opacity);
        expect("object A is not admitted to the other pass",
               !mdkr_viewport_route_cache_load(
                   &cache, &object_a, viewport,
                   pass == MDKR_VIEWPORT_ROUTE_OPAQUE ?
                       MDKR_VIEWPORT_ROUTE_BLEND :
                       MDKR_VIEWPORT_ROUTE_OPAQUE,
                   &route));
        expect("object B survives reverse-order lookup",
               mdkr_viewport_route_cache_load(
                   &cache, &object_b, viewport,
                   MDKR_VIEWPORT_ROUTE_SPECIAL, &route) &&
               route.opacity == 173);
    }

    /* Exact broken-direction control: substituting the final viewport's shared
     * result for viewport zero changes opaque/pass/blend state. A single
     * viewport has no distinct zero to contaminate. */
    if (viewports > 1u) {
        expect("last-viewport contamination control is observable",
               mdkr_viewport_route_cache_load(
                   &cache, &object_a, viewports - 1u,
                   MDKR_VIEWPORT_ROUTE_BLEND, &route) &&
               route.opacity == 64 &&
               !mdkr_viewport_route_cache_load(
                   &cache, &object_a, 0u,
                   MDKR_VIEWPORT_ROUTE_BLEND, &route));
    }
}

int main(void) {
    test_split(1u);
    test_split(2u);
    test_split(3u);
    test_split(4u);
    if (s_failures != 0) {
        fprintf(stderr, "%d viewport route cache failure(s)\n", s_failures);
        return 1;
    }
    puts("viewport route cache: PASS (1P-4P + contamination control)");
    return 0;
}
