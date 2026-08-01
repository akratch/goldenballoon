// app_activation.h — make the native launcher a foreground application.
#ifndef MDKR64_APP_ACTIVATION_H
#define MDKR64_APP_ACTIVATION_H

#include <SDL.h>

#if defined(__APPLE__)
void AppActivation_requestForeground(SDL_Window *window, void *nativeView = nullptr);
#else
inline void AppActivation_requestForeground(SDL_Window *window, void *nativeView = nullptr) {
    (void)nativeView;
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
}
#endif

#endif  // MDKR64_APP_ACTIVATION_H
