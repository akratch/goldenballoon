// ui_overlay.h — the in-game overlay (F1): live settings, quit, FPS readout.
#ifndef MDKR64_UI_OVERLAY_H
#define MDKR64_UI_OVERLAY_H

struct SDL_Window;

enum class OverlayExitRequest {
    None,
    ReturnToLauncher,
    RestartGame,
};

// Register the overlay hooks with the engine. Call before mdkr64_engine_boot().
void Overlay_install(SDL_Window *window);

// Consume an orderly launcher/restart request made by the overlay. The overlay
// only asks the engine to quit; main_app performs renderer/audio/log teardown
// before it replaces the process.
OverlayExitRequest Overlay_consumeExitRequest();

// The gamepad button reserved for toggling the overlay, as an
// SDL_GameControllerButton value. Single source of truth so nothing else can
// bind it.
int Overlay_gamepadToggleButton();

#endif  // MDKR64_UI_OVERLAY_H
