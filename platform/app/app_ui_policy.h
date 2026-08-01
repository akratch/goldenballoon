// app_ui_policy.h — small, deterministic policies shared by UI and tests.
#ifndef MDKR64_APP_UI_POLICY_H
#define MDKR64_APP_UI_POLICY_H

enum class OverlayBackInput { Escape, ControllerB };

struct OverlayBackState {
    bool open;
    bool settings;
    bool confirmation;
};

// Popup cancellation belongs to ImGui and leaves our stack unchanged. Escape
// repeats are ignored; controller B and a non-repeat Escape otherwise match.
OverlayBackState AppUi_overlayBackTransition(
    OverlayBackState current, OverlayBackInput input,
    bool popupOpen, bool keyRepeat);

// Coalesce any number of preview changes into one commit request on widget
// deactivation. `dirty` remains true after a failed commit so Retry can reuse it.
bool AppUi_deferredCommit(bool previewChanged, bool deactivated, bool *dirty);

struct AppUiIdleDecision {
    bool buildFrame;
    unsigned waitMilliseconds;
};

// A zero drawable sleeps and skips frame construction. Occlusion sleeps but
// retries one frame per cadence so restoration is detected.
AppUiIdleDecision AppUi_idleDecision(bool drawableAvailable, bool occluded);

// Strict persisted scale parser for the qualified 0.75x..2.00x range.
bool AppUi_parseScale(const char *text, float *scale);

struct AppUiDpiState {
    float framebufferScale = 1.0f;
    unsigned atlasGeneration = 1;
};

// Apply a meaningful framebuffer-scale transition. Returns true exactly when
// a new atlas generation is required.
bool AppUi_applyDpiTransition(AppUiDpiState *state, float framebufferScale);

enum class AppUiSmokeInputMode { Disabled, Keyboard, Gamepad, Invalid };

// Synthetic launcher input is enabled only by a complete, versioned test
// contract. Partial or stale environment combinations are invalid, so an
// inherited variable can never attach a virtual controller to normal gameplay.
AppUiSmokeInputMode AppUi_validateSmokeInput(
    const char *frames, const char *selection, const char *input,
    const char *token);
AppUiSmokeInputMode AppUi_smokeInputMode();

#endif  // MDKR64_APP_UI_POLICY_H
