// app_overlay_hooks.c — see app_overlay_hooks.h.
#include "app_overlay_hooks.h"

#include <stddef.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(int, browserPartyOverlayOpen, (), {
    return globalThis.__mdkrPartyOverlayOpen === true ? 1 : 0;
});
#endif

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
#ifdef __EMSCRIPTEN__
    /* The browser's launcher-owned Local Party sheet uses the same engine
     * boundary as the native F1 overlay. It never reaches through wasm to
     * mutate gameplay state; this read-only latch makes the next ordinary
     * input capture neutral while the modal owns focus. */
    return browserPartyOverlayOpen();
#endif
    /* Rendering a diagnostic overlay is not input capture. In particular, the
     * browser backend's otherwise-inert overlay fault gate must never pause or
     * swallow input in a build that has no app-shell hooks registered. */
    return 0;
}

int platformOverlayWantsPause(void) {
    if (g_haveHooks && g_hooks.wants_pause) return g_hooks.wants_pause();
#ifdef __EMSCRIPTEN__
    /* Phone setup is local-only, so pausing is honest and prevents the race
     * from continuing behind an opaque modal. Future online chrome must use a
     * separate non-pausing session intent and must not set this latch. */
    return browserPartyOverlayOpen();
#endif
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

/* Deliberately a separate slot rather than a sixth AppOverlayHooks member: the
 * overlay hook set is registered once when the shell adopts the window, and it
 * is the ENGINE that decides whether an overlay exists. This one is claimed by
 * whichever tool is currently substituting at presentation depth, so it has to
 * be settable and clearable independently of the overlay's lifetime. */
static AppPresentationHookFn g_presentationHook = NULL;

void platformSetPresentationHook(AppPresentationHookFn hook) {
    g_presentationHook = hook;
}

void platformPresentationHook(void) {
    if (g_presentationHook != NULL) g_presentationHook();
}

static AppPresentationHookFn g_presentationEndHook = NULL;

void platformSetPresentationEndHook(AppPresentationHookFn hook) {
    g_presentationEndHook = hook;
}

void platformPresentationEndHook(void) {
    if (g_presentationEndHook != NULL) g_presentationEndHook();
}
