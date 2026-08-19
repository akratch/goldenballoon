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

// The OS cursor is drawn by ImGui's SDL2 backend, which only runs while
// onRender() builds a frame -- something ordinary racing skips entirely, so
// nothing ever calls SDL_ShowCursor and the pointer stays in SDL's
// default-visible state for the whole race (issue #45). This is the
// authoritative answer instead: the cursor is wanted exactly when something
// mouse-interactive is on screen to hit, i.e. the pause overlay or a dev-tool
// window. Not "is a frame being built" -- the F10 FPS readout and a
// Tools.Enabled session with nothing open also build a frame, and neither has
// anything on screen a click could land on.
bool AppUi_cursorVisible(bool overlayOpen, bool anyDevToolOpen);

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
// `selection` is the scripted Frame limit value, `pace` the scripted
// Presentation pace choice, `walk` the accessibility keyboard walk, and
// `onlineAction` a token-gated Online Room action id.
// EXACTLY ONE must be present: they are scripts for different
// jobs, and a run claiming two would be driving neither deterministically.
AppUiSmokeInputMode AppUi_validateSmokeInput(
    const char *frames, const char *selection, const char *input,
    const char *token, const char *pace, const char *walk = nullptr,
    const char *onlineAction = nullptr);
AppUiSmokeInputMode AppUi_smokeInputMode();

// True while the scripted accessibility walk is armed. The walk needs two
// things ordinary use does not -- every settings section already expanded, and
// nav able to cross from the launcher shell into its scrolling panel child --
// because a collapsed section draws no rows and Tab does not leave a window's
// focus scope. Both are test scaffolding for reaching the controls, not a
// change to what any control says: the announcements themselves are the
// ordinary production ones.
bool AppUi_a11yWalkArmed();

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
// Category says exactly that. Three families cut across those categories and
// are drawn in their own section instead: the enhancements are spread over
// Interface, Advanced Graphics and Frame Rate & Motion, the content-pack keys
// sit in Advanced Graphics beside render scale and filtering, and the
// accessibility options are split between Interface (the speech settings) and
// the camera group (reduced motion). A player looking for "the extras I
// switched on", "the packs I installed" or "the options that make this playable
// for me" looks for one list, not for rows scattered through three headers.
//
// Accessibility is the case where scattering costs the most. Someone who needs
// reduced motion, larger text and a voice needs all three before they can use
// the product at all, and asking them to find each one under a different header
// asks them to explore the very interface they cannot yet use.
//
// This is a ROUTING decision and never a visibility one:
// AppUi_videoSettingVisible stays true for every key below, because none of
// them is hidden — they are drawn somewhere else.
enum class AppUiSettingsSection {
    Category, Enhancements, Content, Accessibility
};

AppUiSettingsSection AppUi_settingsSection(MdkrVideoKey key);

// The same routing question for the shell preferences that have no schema key
// and so cannot answer it through AppUi_settingsSection.
//
// UI scale is the only one: it is stored in the launcher's own preferences
// rather than the video config, so nothing in MdkrVideoKey can say where it is
// drawn. Routing it here rather than hand-placing the widget is what makes "it
// is drawn in exactly one section" a property a test can read, instead of one
// that holds until somebody copies the slider into a second header.
enum class AppUiShellPreference { UiScale };

AppUiSettingsSection AppUi_shellPreferenceSection(AppUiShellPreference key);

// True for exactly the keys "Reset enhancements" restores to their schema
// defaults. The scoping is the whole point of the action: a player resetting
// the extras must keep the presentation, audio and controller preferences they
// set for comfort, and the content packs they deliberately installed.
bool AppUi_enhancementResetIncludes(MdkrVideoKey key);

#endif  // MDKR64_APP_UI_POLICY_H
