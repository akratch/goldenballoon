// app_activation.h — make the native launcher a foreground application.
#ifndef MDKR64_APP_ACTIVATION_H
#define MDKR64_APP_ACTIVATION_H

#include <SDL.h>

#include <cstdlib>
#include <cstring>

/* Render/screenshot automation still needs a real GPU surface, but must never
 * activate the app or steal the user's keyboard focus. Treat every supported
 * automation trigger as background work: relying on MDKR64_HIDDEN alone left
 * a direct smoke/autoplay invocation able to create a foreground window. */
inline bool AppActivation_backgroundAutomation() {
    return std::getenv("MDKR64_HIDDEN") != nullptr ||
           std::getenv("MDKR_APP_SMOKE_FRAMES") != nullptr ||
           std::getenv("MDKR_APP_AUTOPLAY") != nullptr ||
           std::getenv("MDKR_APP_FILEDIALOG_SELFTEST") != nullptr;
}

/* Automation never seizes the desktop because its windows are created hidden
 * or ordered behind, not because the process refuses to start. This predicate
 * selects that rendering policy; it is not an authorization check. */
inline bool AppActivation_automatedSurfaceRequested() {
    return AppActivation_backgroundAutomation();
}

#if defined(__APPLE__)
/* Must run before SDL_Init: Cocoa can activate while SDL creates NSApplication,
 * before a window exists for AppActivation_requestForeground() to demote. */
void AppActivation_prepareProcess();
void AppActivation_requestForeground(SDL_Window *window, void *nativeView = nullptr);
#else
inline void AppActivation_prepareProcess() {
    if (!AppActivation_backgroundAutomation()) return;
    SDL_SetHintWithPriority(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN,
                            "1", SDL_HINT_OVERRIDE);
}
inline void AppActivation_requestForeground(SDL_Window *window, void *nativeView = nullptr) {
    (void)nativeView;
    if (AppActivation_backgroundAutomation()) return;
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
}
#endif

#endif // MDKR64_APP_ACTIVATION_H
