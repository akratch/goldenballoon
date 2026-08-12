// app_activation.h — make the native launcher a foreground application.
#ifndef MDKR64_APP_ACTIVATION_H
#define MDKR64_APP_ACTIVATION_H

#include <SDL.h>

#include <cstdlib>

/* Render/screenshot automation still needs a real GPU surface, but must never
 * activate the app or steal the user's keyboard focus. Every native UI harness
 * already sets MDKR64_HIDDEN; keep the policy at the window boundary so a new
 * test cannot accidentally bypass it. */
inline bool AppActivation_backgroundAutomation() {
    return std::getenv("MDKR64_HIDDEN") != nullptr;
}

#if defined(__APPLE__)
void AppActivation_requestForeground(SDL_Window *window, void *nativeView = nullptr);
#else
inline void AppActivation_requestForeground(SDL_Window *window, void *nativeView = nullptr) {
    (void)nativeView;
    if (AppActivation_backgroundAutomation()) return;
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
}
#endif

#endif // MDKR64_APP_ACTIVATION_H
