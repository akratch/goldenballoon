// app_ui_policy.h — small, deterministic policies shared by UI and tests.
#ifndef MDKR64_APP_UI_POLICY_H
#define MDKR64_APP_UI_POLICY_H

#include "../video_config.h"

enum class OverlayBackInput { Escape, ControllerB };

struct OverlayBackState {
    bool open;
    bool settings;
    bool confirmation;
};

struct AppUiButtonPairLayout {
    float firstWidth;
    float secondWidth;
    bool sameLine;
};

struct AppUiRomPanelVisibility {
    bool showVerdict;
    bool showAcquisition;
};

enum class AppUiRomPlayRequest {
    Ignore,
    StartFinalCheck,
    SupersedeReplacementCheck,
};

// An in-flight first or remembered-ROM check has no verdict yet. Do not flash
// a false rejection or replacement controls while that asynchronous state is
// unresolved; a proven active ROM remains visible during replacement checks.
AppUiRomPanelVisibility AppUi_romPanelVisibility(
    bool haveRom, bool ready, bool validationPending, bool changing);
bool AppUi_romCandidateFeedbackVisible(
    bool candidateVisible, bool validationPending);

// A proven active ROM remains playable while a replacement is being checked.
// Play therefore supersedes only that candidate check; an initial/remembered
// check or an already-running final Play check remains non-actionable.
AppUiRomPlayRequest AppUi_romPlayRequest(
    bool ready, bool validationPending, bool playValidationPending);

// Preserve requested button widths when they fit, shrink both actions evenly
// when they remain comfortably usable, and otherwise stack full-width actions.
AppUiButtonPairLayout AppUi_fitButtonPair(
    float availableWidth, float spacing, float firstWidth, float secondWidth,
    float minimumWidth);

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
//
// `selection` is the scripted Frame limit value and `pace` the scripted
// Presentation pace choice. EXACTLY ONE must be present: they are two pointer
// scripts for two controls, and a run claiming both would be driving neither
// deterministically.
AppUiSmokeInputMode AppUi_validateSmokeInput(
    const char *frames, const char *selection, const char *input,
    const char *token, const char *pace);
AppUiSmokeInputMode AppUi_smokeInputMode();

// Reserved config keys remain parseable for forward compatibility, but the
// launcher must not advertise settings that the running product cannot apply.
// `legacyStretchActive` is whether the resolved config currently sits on the
// pre-widescreen stretch path; a setting that is otherwise hidden because no
// preset ever chooses it still has to be reachable when a saved config already
// did. Defaulted so tests can assert the ordinary, not-stretched product.
bool AppUi_videoSettingVisible(MdkrVideoKey key, bool webGpuRenderer,
                               bool legacyStretchActive = false);

// Which settings section draws a key.
//
// Nearly every key is drawn under the header its schema category names, and
// Category says exactly that. Two families cut across those categories and are
// drawn in their own section instead: the enhancements are spread over
// Interface, Advanced Graphics and Frame Rate & Motion, and the content-pack
// keys sit in Advanced Graphics beside render scale and filtering. A player
// looking for "the extras I switched on" or "the packs I installed" looks for
// one list, not for four rows scattered through three headers.
//
// This is a ROUTING decision and never a visibility one:
// AppUi_videoSettingVisible stays true for every key below, because none of
// them is hidden — they are drawn somewhere else.
enum class AppUiSettingsSection { Category, Enhancements, Content };

AppUiSettingsSection AppUi_settingsSection(MdkrVideoKey key);

// True for exactly the keys "Reset enhancements" restores to their schema
// defaults. The scoping is the whole point of the action: a player resetting
// the extras must keep the presentation, audio and controller preferences they
// set for comfort, and the content packs they deliberately installed.
bool AppUi_enhancementResetIncludes(MdkrVideoKey key);

#endif  // MDKR64_APP_UI_POLICY_H
