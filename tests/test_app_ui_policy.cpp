#include "app_ui_policy.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>

namespace {
int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}
}

int main() {
    using Input = OverlayBackInput;

    const OverlayBackState confirmation = {true, true, true};
    const OverlayBackState settings = {true, true, false};
    const OverlayBackState root = {true, false, false};
    const OverlayBackState closed = {false, false, false};
    auto same = [](OverlayBackState a, OverlayBackState b) {
        return a.open == b.open && a.settings == b.settings &&
               a.confirmation == b.confirmation;
    };
    expect(same(AppUi_overlayBackTransition(confirmation, Input::Escape,
                                            true, false), confirmation),
           "popup consumes Escape before shell stack");
    expect(same(AppUi_overlayBackTransition(confirmation, Input::Escape,
                                            false, true), confirmation),
           "repeated Escape is ignored");
    expect(same(AppUi_overlayBackTransition(confirmation, Input::Escape,
                                            false, false), settings),
           "Escape dismisses confirmation");
    expect(same(AppUi_overlayBackTransition(settings, Input::Escape,
                                            false, false), root),
           "Escape leaves Settings");
    expect(same(AppUi_overlayBackTransition(root, Input::Escape,
                                            false, false), closed),
           "Escape closes root overlay");
    for (OverlayBackState state : {confirmation, settings, root, closed}) {
        expect(same(AppUi_overlayBackTransition(state, Input::Escape, false, false),
                    AppUi_overlayBackTransition(state, Input::ControllerB,
                                                false, false)),
               "controller B matches Escape at every stack level");
    }

    const AppUiButtonPairLayout roomy =
        AppUi_fitButtonPair(1000.0f, 16.0f, 420.0f, 380.0f, 280.0f);
    expect(roomy.sameLine && roomy.firstWidth == 420.0f &&
               roomy.secondWidth == 380.0f,
           "roomy overlay keeps authored action widths");
    const AppUiButtonPairLayout compact =
        AppUi_fitButtonPair(592.0f, 16.0f, 420.0f, 420.0f, 280.0f);
    expect(compact.sameLine && compact.firstWidth == 288.0f &&
               compact.secondWidth == 288.0f,
           "640px 2x overlay shrinks paired actions inside its content width");
    const AppUiButtonPairLayout narrow =
        AppUi_fitButtonPair(540.0f, 16.0f, 420.0f, 420.0f, 280.0f);
    expect(!narrow.sameLine && narrow.firstWidth == 540.0f &&
               narrow.secondWidth == 540.0f,
           "narrow overlay stacks both actions at full content width");

    const AppUiRomPanelVisibility firstRun =
        AppUi_romPanelVisibility(false, false, false, false);
    expect(!firstRun.showVerdict && firstRun.showAcquisition,
           "first run leads with ROM acquisition and no verdict");
    const AppUiRomPanelVisibility rememberedCheck =
        AppUi_romPanelVisibility(true, false, true, false);
    expect(!rememberedCheck.showVerdict && !rememberedCheck.showAcquisition,
           "remembered ROM check flashes neither rejection nor replacement UI");
    const AppUiRomPanelVisibility readyRom =
        AppUi_romPanelVisibility(true, true, false, false);
    expect(readyRom.showVerdict && !readyRom.showAcquisition,
           "verified ROM shows its verdict without acquisition clutter");
    const AppUiRomPanelVisibility replacement =
        AppUi_romPanelVisibility(true, true, true, true);
    expect(replacement.showVerdict && replacement.showAcquisition,
           "replacement check retains the proven verdict and change controls");
    const AppUiRomPanelVisibility refused =
        AppUi_romPanelVisibility(true, false, false, false);
    expect(refused.showVerdict && refused.showAcquisition,
           "completed refusal shows its verdict and recovery controls");
    expect(AppUi_romCandidateFeedbackVisible(true, false),
           "settled candidate feedback remains visible");
    expect(!AppUi_romCandidateFeedbackVisible(true, true),
           "a new check hides stale candidate feedback");

    expect(AppUi_romPlayRequest(false, false, false) ==
               AppUiRomPlayRequest::Ignore,
           "Play ignores a launcher with no proven active ROM");
    expect(AppUi_romPlayRequest(true, false, false) ==
               AppUiRomPlayRequest::StartFinalCheck,
           "Play starts the final check for an idle proven ROM");
    expect(AppUi_romPlayRequest(true, true, false) ==
               AppUiRomPlayRequest::SupersedeReplacementCheck,
           "Play supersedes an unresolved replacement with the active ROM");
    expect(AppUi_romPlayRequest(true, true, true) ==
               AppUiRomPlayRequest::Ignore,
           "Play cannot duplicate an in-flight final check");

    bool dirty = false;
    int durableTransactions = 0;
    for (int update = 0; update < 100; ++update) {
        if (AppUi_deferredCommit(true, false, &dirty)) ++durableTransactions;
    }
    if (AppUi_deferredCommit(false, true, &dirty)) ++durableTransactions;
    expect(durableTransactions == 1,
           "100 slider previews coalesce to one durable transaction");
    expect(dirty, "failed durable transaction remains retryable");
    // Staged state outlives the commit request: only a successful persistence
    // result clears it, and that result is owned by the caller, not the policy.
    dirty = false;
    expect(!AppUi_deferredCommit(false, true, &dirty),
           "cleared staged state requests no second durable transaction");
    expect(!dirty, "a commit request never stages state on its own");
    bool sameFrame = false;
    expect(AppUi_deferredCommit(true, true, &sameFrame) && sameFrame,
           "a preview and its deactivation in one frame commit exactly once");

    for (const char *text : {"0.75", "1.00", "1.50", "2.00"}) {
        float scale = 0.0f;
        expect(AppUi_parseScale(text, &scale), "qualified UI scale parses");
        expect(scale >= 0.75f && scale <= 2.0f,
               "qualified UI scale remains in range");
    }
    float rejected = 1.0f;
    expect(!AppUi_parseScale("0.74", &rejected), "scale below range rejected");
    expect(!AppUi_parseScale("2.01", &rejected), "scale above range rejected");
    expect(!AppUi_parseScale("1.5junk", &rejected), "malformed scale rejected");

    expect(!AppUi_videoSettingVisible(MDKR_VIDEO_TEXTURE_PACK, true),
           "unimplemented texture-pack setting is hidden");
    expect(!AppUi_videoSettingVisible(MDKR_VIDEO_WIDESCREEN, true),
           "internal widescreen compatibility switch is hidden");
    // No preset selects the legacy-stretch branch, so the only way a player
    // reaches it is a saved or command-line 0 -- and then they need the control
    // that is otherwise hidden, or the stretched image is inescapable from the
    // settings screen.
    expect(AppUi_videoSettingVisible(MDKR_VIDEO_WIDESCREEN, true, true),
           "widescreen switch appears while the config is on legacy stretch");
    expect(!AppUi_videoSettingVisible(MDKR_VIDEO_WIDESCREEN, false, false),
           "widescreen switch disappears again once widescreen is engaged");
    expect(AppUi_videoSettingVisible(MDKR_VIDEO_MOTION_SMOOTHING, true) &&
           AppUi_videoSettingVisible(MDKR_VIDEO_MOTION_SMOOTHING, false),
           "production motion smoothing is visible on every renderer");
    // The settings panel is the only way to opt into camera correction without
    // setting an environment variable, so it may not be renderer-conditional.
    // The same goes for the reduced-motion option beside it: an accessibility
    // control that some renderers hide is not an accessibility control.
    expect(AppUi_videoSettingVisible(MDKR_VIDEO_CAMERA_OBSTRUCTION, true) &&
               AppUi_videoSettingVisible(MDKR_VIDEO_CAMERA_OBSTRUCTION, false),
           "the camera choice is offered on every renderer");
    expect(AppUi_videoSettingVisible(MDKR_VIDEO_CAMERA_COMFORT, true) &&
               AppUi_videoSettingVisible(MDKR_VIDEO_CAMERA_COMFORT, false),
           "reduced motion is offered on every renderer");
    expect(!AppUi_videoSettingVisible(MDKR_VIDEO_MSAA, true),
           "inert MSAA setting is hidden on WebGPU");
    expect(AppUi_videoSettingVisible(MDKR_VIDEO_MSAA, false),
           "implemented MSAA setting remains available to non-WebGPU renderers");
    for (int key = 0; key < MDKR_VIDEO_KEY_COUNT; ++key) {
        if (key == MDKR_VIDEO_TEXTURE_PACK || key == MDKR_VIDEO_WIDESCREEN ||
            key == MDKR_VIDEO_MSAA) continue;
        expect(AppUi_videoSettingVisible(static_cast<MdkrVideoKey>(key), true),
               "implemented video setting remains visible");
    }

    // --- Settings sections -------------------------------------------------
    // Routing, not visibility: an enhancement is drawn in its own section and
    // is still a visible setting, so the loop above must keep passing for
    // every one of these keys. Assert that explicitly, because the cheap way
    // to stop a row appearing twice is to hide it, and hiding it would take a
    // setting away from the player instead of moving it.
    const MdkrVideoKey enhancementKeys[] = {
        MDKR_ENH_SPEEDOMETER, MDKR_ENH_DRAW_DISTANCE, MDKR_ENH_LOD_BIAS,
        MDKR_ENH_AI_DIFFICULTY,
    };
    for (MdkrVideoKey key : enhancementKeys) {
        expect(AppUi_settingsSection(key) == AppUiSettingsSection::Enhancements,
               "every enhancement key is drawn in the Enhancements section");
        expect(AppUi_videoSettingVisible(key, true) &&
                   AppUi_videoSettingVisible(key, false),
               "an enhancement moved to its own section is still visible");
    }
    // The leak the section exists to prevent. These four keys declare three
    // different schema categories between them — Interface, Advanced Graphics
    // and Frame Rate & Motion — so without the routing above, "Opponent skill"
    // sits between Frame limit and Allow Tearing and the speedometer is not
    // reachable at all.
    for (int key = 0; key < MDKR_VIDEO_KEY_COUNT; ++key) {
        const MdkrVideoKey typed = static_cast<MdkrVideoKey>(key);
        bool isEnhancement = false;
        for (MdkrVideoKey enhancement : enhancementKeys) {
            if (enhancement == typed) isEnhancement = true;
        }
        expect(isEnhancement ==
                   (AppUi_settingsSection(typed) ==
                    AppUiSettingsSection::Enhancements),
               "no key outside the enhancement table claims the Enhancements "
               "section");
        if (isEnhancement) continue;
        expect(AppUi_settingsSection(typed) !=
                   AppUiSettingsSection::Enhancements,
               "no enhancement key leaks into Presentation or Fidelity");
    }
    expect(AppUi_settingsSection(MDKR_CONTENT_PACKS_ENABLED) ==
                   AppUiSettingsSection::Content &&
               AppUi_settingsSection(MDKR_CONTENT_PACK_DISABLED) ==
                   AppUiSettingsSection::Content,
           "both content-pack keys are drawn in the Content section");
    expect(AppUi_settingsSection(MDKR_VIDEO_RENDER_SCALE) ==
                   AppUiSettingsSection::Category &&
               AppUi_settingsSection(MDKR_VIDEO_MODE) ==
                   AppUiSettingsSection::Category &&
               AppUi_settingsSection(MDKR_AUDIO_MASTER_VOLUME) ==
                   AppUiSettingsSection::Category,
           "an ordinary setting is still drawn under its schema category");

    // --- The Accessibility section -----------------------------------------
    // The membership claim the section exists to make: everything a player with
    // an access need has to find is in ONE list. These keys declare two
    // different schema categories between them -- the four speech settings are
    // Interface and reduced motion is Presentation -- so without this routing
    // "Camera motion" sits between Motion smoothing and Allow tearing while the
    // speech settings sit four headers away, and a player who needs both has to
    // already know the panel to find either.
    const MdkrVideoKey accessibilityKeys[] = {
        MDKR_A11Y_SPEECH, MDKR_A11Y_SPEECH_RATE, MDKR_A11Y_SPEECH_VOLUME,
        MDKR_A11Y_SPEECH_RACE, MDKR_VIDEO_CAMERA_COMFORT,
    };
    for (MdkrVideoKey key : accessibilityKeys) {
        expect(AppUi_settingsSection(key) ==
                   AppUiSettingsSection::Accessibility,
               "every accessibility key is drawn in the Accessibility section");
        // Routing, not hiding -- the same trap the enhancement block names. The
        // cheap way to stop a row appearing twice is to hide it, and hiding an
        // accessibility control takes it away from the player who needs it.
        expect(AppUi_videoSettingVisible(key, true) &&
                   AppUi_videoSettingVisible(key, false),
               "an accessibility key moved to its own section is still visible");
    }
    // THE NO-DUPLICATION CLAIM. Section membership is exclusive by
    // construction: one key, one answer. So asserting that every accessibility
    // key answers Accessibility is the same as asserting that none of them is
    // still routed to the schema category its old section enumerated -- the
    // Interface block and the category loop both draw a key only when it
    // answers Category. The converse direction is asserted too, so a key that
    // is not an accessibility option cannot be swept into the section.
    int accessibilitySectionKeys = 0;
    for (int key = 0; key < MDKR_VIDEO_KEY_COUNT; ++key) {
        const MdkrVideoKey typed = static_cast<MdkrVideoKey>(key);
        bool isAccessibility = false;
        for (MdkrVideoKey member : accessibilityKeys) {
            if (member == typed) isAccessibility = true;
        }
        expect(isAccessibility ==
                   (AppUi_settingsSection(typed) ==
                    AppUiSettingsSection::Accessibility),
               "no key outside the accessibility table claims the "
               "Accessibility section");
        if (AppUi_settingsSection(typed) ==
            AppUiSettingsSection::Accessibility) {
            ++accessibilitySectionKeys;
        }
    }
    expect(accessibilitySectionKeys ==
               static_cast<int>(std::size(accessibilityKeys)),
           "the Accessibility section holds exactly the accessibility keys");
    // Reduced motion left the camera group and the speech settings left
    // Interface. Name the neighbours they used to sit beside and require those
    // to have stayed put, so a future edit cannot drag half of Presentation
    // into the accessibility list along with Camera.Comfort.
    expect(AppUi_settingsSection(MDKR_VIDEO_CAMERA_OBSTRUCTION) ==
                   AppUiSettingsSection::Category &&
               AppUi_settingsSection(MDKR_WINDOW_MODE) ==
                   AppUiSettingsSection::Category &&
               AppUi_settingsSection(MDKR_APP_UPDATE_CHECK) ==
                   AppUiSettingsSection::Category &&
               AppUi_settingsSection(MDKR_TOOLS_ENABLED) ==
                   AppUiSettingsSection::Category,
           "the rows the accessibility keys sat beside stay in their category");
    // UI scale has no schema key, so nothing above can route it. It is the one
    // control that was hand-placed in Interface, and the whole reason a second
    // routing function exists: the panel asks this question at both candidate
    // sections, so exactly one of them draws the slider.
    expect(AppUi_shellPreferenceSection(AppUiShellPreference::UiScale) ==
               AppUiSettingsSection::Accessibility,
           "UI scale is drawn in the Accessibility section");
    expect(AppUi_shellPreferenceSection(AppUiShellPreference::UiScale) !=
               AppUiSettingsSection::Category,
           "UI scale is no longer drawn under Interface as well");
    // An accessibility choice is not one of "the extras", so the reset beside
    // the enhancements must not switch a player's voice or reduced motion off.
    for (MdkrVideoKey key : accessibilityKeys) {
        expect(!AppUi_enhancementResetIncludes(key),
               "Reset enhancements leaves the accessibility options untouched");
    }

    // --- Reset enhancements ------------------------------------------------
    // The scoping IS the action. A reset button beside a list of extras that
    // also threw away the frame limit, the volume levels and a remapped
    // controller would be a trap, so name one key from each of those families
    // and require it to survive.
    for (MdkrVideoKey key : enhancementKeys) {
        expect(AppUi_enhancementResetIncludes(key),
               "Reset enhancements restores every enhancement key");
    }
    expect(!AppUi_enhancementResetIncludes(MDKR_VIDEO_FRAME_LIMIT),
           "Reset enhancements leaves the presentation pacing key untouched");
    expect(!AppUi_enhancementResetIncludes(MDKR_VIDEO_MODE),
           "Reset enhancements leaves the presentation mode untouched");
    expect(!AppUi_enhancementResetIncludes(MDKR_AUDIO_MASTER_VOLUME),
           "Reset enhancements leaves the audio levels untouched");
    expect(!AppUi_enhancementResetIncludes(MDKR_INPUT_CONTROLLER_A),
           "Reset enhancements leaves a remapped controller untouched");
    expect(!AppUi_enhancementResetIncludes(MDKR_INPUT_RUMBLE_PROFILE),
           "Reset enhancements leaves the rumble strength untouched");
    // Installed content is not an enhancement setting: a pack is content the
    // player put on disk, and switching it off is not what "reset the extras"
    // offers to do.
    expect(!AppUi_enhancementResetIncludes(MDKR_CONTENT_PACKS_ENABLED) &&
               !AppUi_enhancementResetIncludes(MDKR_CONTENT_PACK_DISABLED),
           "Reset enhancements leaves installed content packs untouched");
    int resetKeys = 0;
    for (int key = 0; key < MDKR_VIDEO_KEY_COUNT; ++key) {
        if (AppUi_enhancementResetIncludes(static_cast<MdkrVideoKey>(key))) {
            ++resetKeys;
        }
    }
    expect(resetKeys == static_cast<int>(std::size(enhancementKeys)),
           "Reset enhancements touches only enhancement keys");

    AppUiDpiState dpi;
    expect(!AppUi_applyDpiTransition(&dpi, 1.0f), "same-DPI frame is a no-op");
    expect(AppUi_applyDpiTransition(&dpi, 2.0f), "1x to 2x rebuilds atlas");
    expect(dpi.atlasGeneration == 2 && std::fabs(dpi.framebufferScale - 2.0f) < 0.001f,
           "2x atlas generation recorded exactly once");
    expect(!AppUi_applyDpiTransition(&dpi, 2.01f), "DPI rounding does not churn atlas");
    expect(AppUi_applyDpiTransition(&dpi, 1.0f), "2x to 1x rebuilds atlas");
    expect(dpi.atlasGeneration == 3,
           "1x to 2x to 1x produces two, non-compounding rebuilds");

    // --- Cursor visibility (issue #45) --------------------------------------
    expect(!AppUi_cursorVisible(false, false),
           "cursor stays hidden during ordinary racing");
    expect(AppUi_cursorVisible(true, false),
           "cursor is shown while the pause overlay is open");
    expect(AppUi_cursorVisible(false, true),
           "cursor is shown while a dev-tool window is open");
    expect(AppUi_cursorVisible(true, true),
           "cursor is shown while both the overlay and a dev tool are open");

    unsigned zeroDrawableAttempts = 0;
    unsigned occludedAttempts = 0;
    unsigned elapsed = 0;
    while (elapsed < 250) {
        const AppUiIdleDecision zero = AppUi_idleDecision(false, false);
        const AppUiIdleDecision occluded = AppUi_idleDecision(true, true);
        zeroDrawableAttempts += zero.buildFrame ? 1u : 0u;
        occludedAttempts += occluded.buildFrame ? 1u : 0u;
        expect(zero.waitMilliseconds == 25 && occluded.waitMilliseconds == 25,
               "unavailable surfaces use bounded event waits");
        elapsed += 25;
    }
    expect(zeroDrawableAttempts == 0,
           "zero drawable constructs no frames during 250 ms idle interval");
    expect(occludedAttempts <= 10,
           "occluded surface retries no more than once per 25 ms interval");
    const AppUiIdleDecision restored = AppUi_idleDecision(true, false);
    expect(restored.buildFrame && restored.waitMilliseconds == 0,
           "restored surface resumes immediately");

    using SmokeMode = AppUiSmokeInputMode;
    expect(AppUi_validateSmokeInput(nullptr, nullptr, nullptr, nullptr,
                                    nullptr) == SmokeMode::Disabled,
           "no synthetic-input environment leaves smoke input disabled");
    expect(AppUi_validateSmokeInput("24", "240", "gamepad",
                                    "mdkr64-app-ui-input-v1", nullptr) ==
               SmokeMode::Gamepad,
           "complete versioned gamepad contract is accepted");
    expect(AppUi_validateSmokeInput("18", "240", "keyboard",
                                    "mdkr64-app-ui-input-v1", nullptr) ==
               SmokeMode::Keyboard,
           "complete versioned keyboard contract is accepted");
    expect(AppUi_validateSmokeInput(nullptr, "240", "gamepad",
                                    "mdkr64-app-ui-input-v1", nullptr) ==
               SmokeMode::Invalid,
           "missing smoke frame contract is rejected");
    expect(AppUi_validateSmokeInput("24", "240", "gamepad", nullptr,
                                    nullptr) == SmokeMode::Invalid,
           "missing authorization token is rejected");
    expect(AppUi_validateSmokeInput("24", "240", "gamepad", "stale",
                                    nullptr) == SmokeMode::Invalid,
           "stale authorization token is rejected");
    expect(AppUi_validateSmokeInput("24", nullptr, "gamepad",
                                    "mdkr64-app-ui-input-v1", nullptr) ==
               SmokeMode::Invalid,
           "partial controller environment is rejected");
    // The presentation-pace script is the second scripted selection, and the
    // contract has to be exactly as strict about it as about the first.
    expect(AppUi_validateSmokeInput("10", nullptr, "keyboard",
                                    "mdkr64-app-ui-input-v1", "smooth") ==
               SmokeMode::Keyboard,
           "complete versioned presentation-pace contract is accepted");
    expect(AppUi_validateSmokeInput("10", nullptr, "keyboard",
                                    "mdkr64-app-ui-input-v1", "custom") ==
               SmokeMode::Invalid,
           "custom is not a pace a script may select");
    expect(AppUi_validateSmokeInput("10", "240", "keyboard",
                                    "mdkr64-app-ui-input-v1", "smooth") ==
               SmokeMode::Invalid,
           "two scripted selections in one run are rejected");
    expect(AppUi_validateSmokeInput("10", nullptr, nullptr,
                                    "mdkr64-app-ui-input-v1", "smooth") ==
               SmokeMode::Invalid,
           "a pace selection without an input mode is rejected");

    if (failures) {
        std::printf("%d app UI policy failure(s)\n", failures);
        return 1;
    }
    std::printf("app UI policies passed\n");
    return 0;
}
