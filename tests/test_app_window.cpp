#include "app_window.h"

#include <cstdio>
#include <cstring>

namespace {
int                    failures;
Uint32                 windowFlags;
Uint32                 lastFullscreenRequest;
int                    fullscreenCalls;
int                    fullscreenFailure;
int                    fullscreenFailOnCall;
int                    borderedCalls;
SDL_bool               lastBordered;
int                    raiseCalls;
int                    configLocked;
int                    configSetCalls;
MdkrVideoRuntimeResult configResult = MDKR_VIDEO_RUNTIME_LIVE;
MdkrVideoConfig        config;

void                   expect(bool condition, const char *message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        failures++;
    }
}

void reset() {
    windowFlags           = 0;
    lastFullscreenRequest = 0;
    fullscreenCalls       = 0;
    fullscreenFailure     = 0;
    fullscreenFailOnCall  = 0;
    borderedCalls         = 0;
    lastBordered          = SDL_TRUE;
    raiseCalls            = 0;
    configLocked          = 0;
    configSetCalls        = 0;
    configResult          = MDKR_VIDEO_RUNTIME_LIVE;
    std::memset(&config, 0, sizeof(config));
    std::snprintf(config.values[MDKR_WINDOW_MODE].text,
                  sizeof(config.values[MDKR_WINDOW_MODE].text),
                  "windowed");
}

SDL_Window *fakeWindow() {
    return reinterpret_cast<SDL_Window *>(static_cast<uintptr_t>(1));
}
} // namespace

extern "C" {
Uint32 SDL_GetWindowFlags(SDL_Window *) {
    return windowFlags;
}
int SDL_SetWindowFullscreen(SDL_Window *, Uint32 flags) {
    fullscreenCalls++;
    lastFullscreenRequest = flags;
    if (fullscreenFailure || fullscreenCalls == fullscreenFailOnCall) return -1;
    windowFlags &= ~(SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS);
    windowFlags |= flags;
    if (flags != 0) windowFlags |= SDL_WINDOW_BORDERLESS;
    return 0;
}
void SDL_SetWindowBordered(SDL_Window *, SDL_bool bordered) {
    borderedCalls++;
    lastBordered = bordered;
    if (bordered)
        windowFlags &= ~SDL_WINDOW_BORDERLESS;
    else
        windowFlags |= SDL_WINDOW_BORDERLESS;
}
void SDL_RaiseWindow(SDL_Window *) {
    raiseCalls++;
}
const char *SDL_GetError(void) {
    return "injected fullscreen failure";
}
// Completion freshness is measured against this clock. Holding it at zero keeps
// every completion these cases publish inside the freshness window.
Uint64 SDL_GetTicks64(void) {
    return 0u;
}

const MdkrVideoConfig *mdkr_video_config_current(void) {
    return &config;
}
int mdkr_video_config_runtime_locked(MdkrVideoKey) {
    return configLocked;
}
MdkrVideoRuntimeResult mdkr_video_config_runtime_set(
    MdkrVideoKey key,
    const char  *value) {
    configSetCalls++;
    if (configResult == MDKR_VIDEO_RUNTIME_LIVE) {
        std::snprintf(config.values[key].text,
                      sizeof(config.values[key].text),
                      "%s",
                      value);
    }
    return configResult;
}
const char *mdkr_window_mode_canonical(const char *value) {
    if (value && (!std::strcmp(value, "windowed") ||
                  !std::strcmp(value, "window"))) return "windowed";
    if (value && (!std::strcmp(value, "fullscreen") ||
                  !std::strcmp(value, "borderless"))) return "fullscreen";
    return nullptr;
}
}

int main() {
    reset();
    expect(AppWindow_creationFlags() == 0,
           "windowed startup requests ordinary decorations");
    std::snprintf(config.values[MDKR_WINDOW_MODE].text,
                  sizeof(config.values[MDKR_WINDOW_MODE].text),
                  "fullscreen");
    expect(AppWindow_creationFlags() ==
               (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS),
           "fullscreen startup is desktop-sized and explicitly borderless");

    reset();
    expect(AppWindow_applyMode(fakeWindow(), "fullscreen") ==
               MDKR_VIDEO_RUNTIME_LIVE,
           "fullscreen applies and persists");
    expect(fullscreenCalls == 1 &&
               lastFullscreenRequest == SDL_WINDOW_FULLSCREEN_DESKTOP,
           "live fullscreen uses SDL desktop fullscreen");
    expect(borderedCalls == 1 && lastBordered == SDL_FALSE,
           "live fullscreen explicitly removes borders");
    expect(configSetCalls == 1 && raiseCalls == 1,
           "successful platform transition persists and raises the window");

    windowFlags = SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS;
    expect(AppWindow_applyMode(fakeWindow(), "windowed") ==
               MDKR_VIDEO_RUNTIME_LIVE,
           "windowed mode restores live");
    expect(lastFullscreenRequest == 0 && lastBordered == SDL_TRUE,
           "windowed mode restores decorations");

    reset();
    configResult = MDKR_VIDEO_RUNTIME_SAVE_FAILED;
    expect(AppWindow_applyMode(fakeWindow(), "fullscreen") ==
               MDKR_VIDEO_RUNTIME_SAVE_FAILED,
           "save failure is returned");
    expect(fullscreenCalls == 2 && windowFlags == 0,
           "save failure rolls the SDL transition back");

    reset();
    configResult = MDKR_VIDEO_RUNTIME_SAVE_FAILED;
    fullscreenFailOnCall = 2;
    expect(AppWindow_applyMode(fakeWindow(), "fullscreen") ==
               MDKR_VIDEO_RUNTIME_ROLLBACK_FAILED,
           "a rejected rollback is reported distinctly");
    expect(fullscreenCalls == 2 &&
               (windowFlags & SDL_WINDOW_FULLSCREEN) != 0 &&
               !std::strcmp(config.values[MDKR_WINDOW_MODE].text, "windowed"),
           "rollback failure leaves the saved preference truthful and exposes "
           "the degraded live window state");

    reset();
    fullscreenFailure = 1;
    expect(AppWindow_applyMode(fakeWindow(), "fullscreen") ==
               MDKR_VIDEO_RUNTIME_APPLY_FAILED,
           "OS fullscreen failure is distinct from invalid config");
    expect(configSetCalls == 0,
           "failed OS transition never persists a false fullscreen state");

    reset();
    configLocked = 1;
    expect(AppWindow_applyMode(fakeWindow(), "fullscreen") ==
               MDKR_VIDEO_RUNTIME_LOCKED,
           "locked window setting is rejected before touching SDL");
    expect(fullscreenCalls == 0 && configSetCalls == 0,
           "locked transition is side-effect free");

    // A missing window and a malformed value are different facts about a
    // request, and the settings panel prints a different sentence for each.
    // They shared MDKR_VIDEO_RUNTIME_INVALID until this split, which told a
    // player their perfectly legal choice was "not valid".
    reset();
    expect(AppWindow_applyMode(nullptr, "fullscreen") ==
               MDKR_VIDEO_RUNTIME_UNAVAILABLE,
           "no window to apply to is reported as unavailable, not invalid");
    expect(AppWindow_applyMode(fakeWindow(), "diagonal") ==
               MDKR_VIDEO_RUNTIME_INVALID,
           "an unknown window mode remains an invalid value");
    expect(AppWindow_requestMode(nullptr, "fullscreen") ==
               MDKR_VIDEO_RUNTIME_UNAVAILABLE,
           "a deferred request with no window is unavailable, not invalid");
    expect(AppWindow_requestMode(fakeWindow(), "diagonal") ==
               MDKR_VIDEO_RUNTIME_INVALID,
           "a deferred request with an unknown mode is an invalid value");
    expect(fullscreenCalls == 0 && configSetCalls == 0,
           "neither rejection touches SDL or persistence");

    reset();
    expect(AppWindow_requestMode(fakeWindow(), "fullscreen") ==
               MDKR_VIDEO_RUNTIME_PENDING,
           "settings request defers the transition");
    expect(fullscreenCalls == 0,
           "deferred request does not resize during UI rendering");
    AppWindow_servicePending();
    MdkrVideoRuntimeResult completed = MDKR_VIDEO_RUNTIME_INVALID;
    expect(AppWindow_consumeCompleted(&completed) &&
               completed == MDKR_VIDEO_RUNTIME_LIVE,
           "safe-boundary service reports durable completion");
    expect(fullscreenCalls == 1,
           "safe-boundary service performs exactly one transition");

    reset();
    expect(AppWindow_requestMode(fakeWindow(), "fullscreen") ==
               MDKR_VIDEO_RUNTIME_PENDING &&
               AppWindow_requestMode(fakeWindow(), "windowed") ==
               MDKR_VIDEO_RUNTIME_SUPERSEDED,
           "a newer valid settings request replaces the pending mode, and says "
           "so distinctly from the first queued request");
    AppWindow_servicePending();
    completed = MDKR_VIDEO_RUNTIME_INVALID;
    expect(AppWindow_consumeCompleted(&completed) &&
               completed == MDKR_VIDEO_RUNTIME_LIVE,
           "coalesced request completes normally rather than becoming invalid");
    expect(fullscreenCalls == 0 && configSetCalls == 1 &&
               !std::strcmp(config.values[MDKR_WINDOW_MODE].text, "windowed"),
           "only the newest queued windowed intent is persisted and applied");

    reset();
    configResult = MDKR_VIDEO_RUNTIME_SAVE_FAILED;
    expect(AppWindow_requestMode(fakeWindow(), "fullscreen") ==
               MDKR_VIDEO_RUNTIME_PENDING,
           "queued failure setup is accepted");
    AppWindow_servicePending();
    configResult = MDKR_VIDEO_RUNTIME_LIVE;
    SDL_Event latestEvent      = {};
    latestEvent.type           = SDL_KEYDOWN;
    latestEvent.key.keysym.sym = SDLK_F11;
    expect(AppWindow_handleEvent(fakeWindow(), latestEvent),
           "a shortcut after queued completion is consumed");
    completed = MDKR_VIDEO_RUNTIME_INVALID;
    expect(AppWindow_consumeCompleted(&completed) &&
               completed == MDKR_VIDEO_RUNTIME_LIVE,
           "the latest shortcut result supersedes stale queued failure");
    expect(!std::strcmp(config.values[MDKR_WINDOW_MODE].text, "fullscreen"),
           "shortcut completion and desired mode remain synchronized");

    reset();
    SDL_Event event      = {};
    event.type           = SDL_KEYDOWN;
    event.key.keysym.sym = SDLK_F11;
    expect(AppWindow_handleEvent(fakeWindow(), event),
           "F11 is consumed as a host shortcut");
    expect(lastFullscreenRequest == SDL_WINDOW_FULLSCREEN_DESKTOP,
           "F11 enters desktop fullscreen");
    event.type = SDL_KEYUP;
    expect(AppWindow_handleEvent(fakeWindow(), event),
           "F11 release is also consumed");
    event                = {};
    event.type           = SDL_KEYDOWN;
    event.key.keysym.sym = SDLK_RETURN;
    event.key.keysym.mod = KMOD_ALT;
    expect(AppWindow_handleEvent(fakeWindow(), event),
           "Alt+Enter is consumed as a host shortcut");

    if (failures) return 1;
    std::printf("app window policies passed\n");
    return 0;
}
