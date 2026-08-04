// app_overlay_hooks.c — see app_overlay_hooks.h.
#include "app_overlay_hooks.h"

#include <stddef.h>
#include <stdlib.h>

static AppOverlayHooks g_hooks;
static int g_haveHooks = 0;

void platformSetOverlayHooks(const AppOverlayHooks *hooks) {
    if (hooks) {
        g_hooks = *hooks;
        g_haveHooks = 1;
    } else {
        g_haveHooks = 0;
    }
}

int platformOverlayProcessEvent(const void *sdl_event) {
    return (g_haveHooks && g_hooks.process_event)
        ? g_hooks.process_event(sdl_event) : 0;
}

void platformOverlayService(void) {
    if (g_haveHooks && g_hooks.service) g_hooks.service();
}

int platformOverlayWantsInput(void) {
    if (g_haveHooks && g_hooks.wants_input) return g_hooks.wants_input();
    /* Rendering a diagnostic overlay is not input capture. In particular, the
     * browser backend's otherwise-inert overlay fault gate must never pause or
     * swallow input in a build that has no app-shell hooks registered. */
    return 0;
}

int platformOverlayWantsRender(void) {
    if (g_haveHooks && g_hooks.wants_render) return g_hooks.wants_render();
    /* Browser renderer fault tests need to request the otherwise absent output
     * overlay without linking the native ImGui shell. Production never sets
     * this diagnostic variable. */
    {
        const char *test = getenv("MDKR_TEST_WEBGPU_OVERLAY");
        return test != NULL && test[0] == '1';
    }
}

int platformOverlayRender(void) {
    return (g_haveHooks && g_hooks.render) ? g_hooks.render() : 1;
}
