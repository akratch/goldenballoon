// app_window.h — durable native window mode and safe fullscreen transitions.
#ifndef MDKR64_APP_WINDOW_H
#define MDKR64_APP_WINDOW_H

#include "video_config.h"

#include <SDL.h>

// Flags added at SDL_CreateWindow time for the resolved Window.Mode.
Uint32                 AppWindow_creationFlags();

// Immediate transaction for a safe event-pump/startup boundary: apply the SDL
// transition, persist it, and roll SDL back if persistence fails.
MdkrVideoRuntimeResult AppWindow_applyMode(SDL_Window *window, const char *mode);

// Rendering the in-game overlay happens after WebGPU has acquired a drawable.
// Settings therefore queue window changes; the next event pump services them
// before a new frame starts. Completion can be consumed by the settings panel.
MdkrVideoRuntimeResult AppWindow_requestMode(SDL_Window *window, const char *mode);
void                   AppWindow_servicePending();
bool                   AppWindow_consumeCompleted(MdkrVideoRuntimeResult *result);

// F11 and Alt+Enter. Returns true for both press and release so game/ImGui input
// never sees the host shortcut; a non-repeated press toggles immediately.
bool                   AppWindow_handleEvent(SDL_Window *window, const SDL_Event &event);

#endif // MDKR64_APP_WINDOW_H
