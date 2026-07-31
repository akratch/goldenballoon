// app_overlay_hooks.c — see app_overlay_hooks.h.
#include "app_overlay_hooks.h"

#include <stddef.h>

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

void platformOverlayProcessEvent(const void *sdl_event) {
    if (g_haveHooks && g_hooks.process_event) g_hooks.process_event(sdl_event);
}

int platformOverlayWantsInput(void) {
    return (g_haveHooks && g_hooks.wants_input) ? g_hooks.wants_input() : 0;
}

void platformOverlayRender(void) {
    if (g_haveHooks && g_hooks.render) g_hooks.render();
}
