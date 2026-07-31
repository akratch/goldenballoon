// ui_overlay.h — the in-game overlay (F1): live settings, quit, FPS readout.
#ifndef MDKR64_UI_OVERLAY_H
#define MDKR64_UI_OVERLAY_H

struct SDL_Window;

// Register the overlay hooks with the engine. Call before mdkr64_engine_boot().
// argv0 is the app executable path, used to re-exec back to the launcher.
void Overlay_install(SDL_Window *window, const char *argv0);

// The gamepad button reserved for toggling the overlay, as an
// SDL_GameControllerButton value. Single source of truth so nothing else can
// bind it.
int Overlay_gamepadToggleButton();

#endif  // MDKR64_UI_OVERLAY_H
