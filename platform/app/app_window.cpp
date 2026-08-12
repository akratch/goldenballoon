#include "app_window.h"

#include "app_activation.h"

#include <cstdio>
#include <cstring>

namespace {

struct PendingWindowMode {
    SDL_Window            *window        = nullptr;
    char                   mode[16]      = {0};
    bool                   pending       = false;
    bool                   completed     = false;
    Uint64                 completedTick = 0;
    MdkrVideoRuntimeResult result        = MDKR_VIDEO_RUNTIME_INVALID;
};

PendingWindowMode g_pending;

/* Settings is the only consumer of a completion, and it can stay closed for the
 * rest of the session. Past this window the result still has to be consumed —
 * the panel resynchronizes from it — but it is no longer something the player
 * is waiting on, so it must not be announced as if it just happened. */
constexpr Uint64  kCompletionFreshnessMs = 4000u;

void              publishCompleted(SDL_Window *window, MdkrVideoRuntimeResult result) {
    g_pending.window        = window;
    g_pending.pending       = false;
    g_pending.completed     = true;
    g_pending.completedTick = SDL_GetTicks64();
    g_pending.result        = result;
    g_pending.mode[0]       = '\0';
}

bool isFullscreen(SDL_Window *window) {
    return window != nullptr &&
           (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

bool isShortcut(const SDL_KeyboardEvent &key) {
    return key.keysym.sym == SDLK_F11 ||
           ((key.keysym.sym == SDLK_RETURN || key.keysym.sym == SDLK_KP_ENTER) &&
            (key.keysym.mod & KMOD_ALT) != 0);
}

bool restoreWindowFlags(SDL_Window *window, Uint32 oldFlags) {
    const Uint32 fullscreen = oldFlags & SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (SDL_SetWindowFullscreen(window, fullscreen) != 0) {
        std::fprintf(stderr,
                     "[app] fullscreen rollback failed: %s\n",
                     SDL_GetError());
        return false;
    }
    SDL_SetWindowBordered(
        window,
        (oldFlags & SDL_WINDOW_BORDERLESS) ? SDL_FALSE : SDL_TRUE);
    return true;
}

} // namespace

Uint32 AppWindow_creationFlags() {
    const MdkrVideoConfig *config = mdkr_video_config_current();
    return config != nullptr &&
                   std::strcmp(config->values[MDKR_WINDOW_MODE].text, "fullscreen") == 0
               ? SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS
               : 0u;
}

MdkrVideoRuntimeResult AppWindow_applyMode(
    SDL_Window *window,
    const char *requestedMode) {
    const char *mode = mdkr_window_mode_canonical(requestedMode);
    if (window == nullptr) return MDKR_VIDEO_RUNTIME_UNAVAILABLE;
    if (mode == nullptr) return MDKR_VIDEO_RUNTIME_INVALID;
    if (mdkr_video_config_runtime_locked(MDKR_WINDOW_MODE)) {
        return MDKR_VIDEO_RUNTIME_LOCKED;
    }

    const Uint32 oldFlags       = SDL_GetWindowFlags(window);
    const bool   wasFullscreen  = (oldFlags & SDL_WINDOW_FULLSCREEN) != 0;
    const bool   wantFullscreen = std::strcmp(mode, "fullscreen") == 0;
    /* Screenshot/input automation owns a deliberately non-key background
     * surface. An F11 event or persisted fullscreen choice must not turn that
     * surface into a foreground Space. Preserve the requested config result
     * for UI/persistence coverage while leaving the automation window alone. */
    if (AppActivation_backgroundAutomation() &&
        wasFullscreen != wantFullscreen) {
        return mdkr_video_config_runtime_set(MDKR_WINDOW_MODE, mode);
    }
    if (wasFullscreen != wantFullscreen) {
        const Uint32 fullscreenFlag =
            wantFullscreen
                ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP)
                : Uint32{0};
        if (SDL_SetWindowFullscreen(window, fullscreenFlag) != 0) {
            std::fprintf(stderr, "[app] fullscreen transition failed: %s\n", SDL_GetError());
            return MDKR_VIDEO_RUNTIME_APPLY_FAILED;
        }
        /* SDL owns placement and restores the prior window rectangle. Make the
         * Windows decoration state explicit too: the reported bug was a
         * desktop-sized overlapped window whose normal border remained visible. */
        SDL_SetWindowBordered(window,
                              wantFullscreen ? SDL_FALSE : SDL_TRUE);
        if (!AppActivation_backgroundAutomation()) SDL_RaiseWindow(window);
    }

    const MdkrVideoRuntimeResult result =
        mdkr_video_config_runtime_set(MDKR_WINDOW_MODE, mode);
    if (!mdkr_video_runtime_result_applied(result)) {
        if (wasFullscreen != wantFullscreen) {
            if (!restoreWindowFlags(window, oldFlags)) {
                return MDKR_VIDEO_RUNTIME_ROLLBACK_FAILED;
            }
        }
        return result;
    }
    return result;
}

MdkrVideoRuntimeResult AppWindow_requestMode(
    SDL_Window *window,
    const char *requestedMode) {
    const char *mode = mdkr_window_mode_canonical(requestedMode);
    if (window == nullptr) {
        return MDKR_VIDEO_RUNTIME_UNAVAILABLE;
    }
    if (mode == nullptr) {
        return MDKR_VIDEO_RUNTIME_INVALID;
    }
    if (mdkr_video_config_runtime_locked(MDKR_WINDOW_MODE)) {
        return MDKR_VIDEO_RUNTIME_LOCKED;
    }
    /* A settings combobox can receive more than one valid selection before
     * the next safe frame boundary.  Keep the newest intent; rejecting it as
     * INVALID made a real selection look malformed and could apply a stale
     * intermediate mode. Report the replacement distinctly rather than as a
     * plain PENDING: the caller can then say that the earlier queued choice was
     * dropped in favor of this one, which is not the same event as queueing the
     * first request of the frame. */
    const bool replaced = g_pending.pending;
    std::snprintf(g_pending.mode, sizeof(g_pending.mode), "%s", mode);
    g_pending.window    = window;
    g_pending.pending   = true;
    g_pending.completed = false;
    return replaced ? MDKR_VIDEO_RUNTIME_SUPERSEDED
                    : MDKR_VIDEO_RUNTIME_PENDING;
}

void AppWindow_servicePending() {
    if (!g_pending.pending) return;
    SDL_Window *window = g_pending.window;
    char        mode[sizeof(g_pending.mode)];
    std::snprintf(mode, sizeof(mode), "%s", g_pending.mode);
    publishCompleted(window, AppWindow_applyMode(window, mode));
}

bool AppWindow_consumeCompleted(MdkrVideoRuntimeResult *result, bool *fresh) {
    if (!g_pending.completed || result == nullptr) return false;
    *result = g_pending.result;
    if (fresh != nullptr) {
        *fresh = SDL_GetTicks64() - g_pending.completedTick <=
                 kCompletionFreshnessMs;
    }
    g_pending.completed = false;
    g_pending.window    = nullptr;
    g_pending.mode[0]   = '\0';
    return true;
}

bool AppWindow_handleEvent(SDL_Window *window, const SDL_Event &event) {
    if ((event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) ||
        !isShortcut(event.key)) {
        return false;
    }
    if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        const char                  *next   = isFullscreen(window) ? "windowed" : "fullscreen";
        const MdkrVideoRuntimeResult result = AppWindow_applyMode(window, next);
        /* A shortcut is the newest window-mode intent. Publish its result over
         * any older queued completion so Settings resynchronizes to the actual
         * desired value instead of briefly reporting stale success/failure. */
        publishCompleted(window, result);
        std::fprintf(stderr,
                     "[app] fullscreen shortcut requested %s (result=%d)\n",
                     next,
                     static_cast<int>(result));
    }
    return true;
}
