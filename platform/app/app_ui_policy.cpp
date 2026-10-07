#include "app_ui_policy.h"

#include <cmath>
#include <cstdlib>
#include <cstring>


AppUiDestination AppUi_destinationForPanel(int panel) {
    switch (panel) {
        case kLauncherPanelOnlineRoom:        return AppUiDestination::Play;
        case kLauncherPanelCharacterWorkshop: return AppUiDestination::Content;
        case kLauncherPanelContent:           return AppUiDestination::Content;
        case kLauncherPanelSettings:          return AppUiDestination::Settings;
        case kLauncherPanelDiagnostics:       return AppUiDestination::Support;
        case kLauncherPanelAbout:             return AppUiDestination::Support;
        default:                              return AppUiDestination::Play;
    }
}

int AppUi_defaultPanelForDestination(AppUiDestination destination) {
    switch (destination) {
        case AppUiDestination::Content:  return kLauncherPanelContent;
        case AppUiDestination::Settings: return kLauncherPanelSettings;
        // About, not Diagnostics: the support surface opens on what the build
        // IS, and carries the diagnostics report below it.
        case AppUiDestination::Support:  return kLauncherPanelAbout;
        case AppUiDestination::Play:     break;
    }
    return kLauncherPanelPlay;
}

const char *AppUi_destinationLabel(AppUiDestination destination) {
    switch (destination) {
        case AppUiDestination::Content:  return "Content";
        case AppUiDestination::Settings: return "Settings";
        case AppUiDestination::Support:  return "About & support";
        case AppUiDestination::Play:     break;
    }
    return "Play";
}

bool AppUi_destinationIsFooter(AppUiDestination destination) {
    return destination == AppUiDestination::Support;
}

bool AppUi_destinationSelected(AppUiDestination destination, int activePanel) {
    return AppUi_destinationForPanel(activePanel) == destination;
}

OverlayBackState AppUi_overlayBackTransition(
    OverlayBackState current, OverlayBackInput input,
    bool popupOpen, bool keyRepeat) {
    if (popupOpen || (input == OverlayBackInput::Escape && keyRepeat)) {
        return current;
    }
    if (current.confirmation) current.confirmation = false;
    else if (current.settings) current.settings = false;
    else current.open = false;
    return current;
}

AppUiButtonPairLayout AppUi_fitButtonPair(
    float availableWidth, float spacing, float firstWidth, float secondWidth,
    float minimumWidth) {
    if (availableWidth < 1.0f) availableWidth = 1.0f;
    if (spacing < 0.0f) spacing = 0.0f;
    if (firstWidth + spacing + secondWidth <= availableWidth) {
        return {firstWidth, secondWidth, true};
    }
    const float pairedWidth = (availableWidth - spacing) * 0.5f;
    if (pairedWidth >= minimumWidth) {
        return {pairedWidth, pairedWidth, true};
    }
    return {availableWidth, availableWidth, false};
}

AppUiRomPanelVisibility AppUi_romPanelVisibility(
    bool haveRom, bool ready, bool validationPending, bool changing) {
    const bool awaitingInitialVerdict = validationPending && !ready;
    return {
        haveRom && !awaitingInitialVerdict,
        changing || (!ready && !awaitingInitialVerdict),
    };
}

bool AppUi_romCandidateFeedbackVisible(
    bool candidateVisible, bool validationPending) {
    return candidateVisible && !validationPending;
}

AppUiRomPlayRequest AppUi_romPlayRequest(
    bool ready, bool validationPending, bool playValidationPending) {
    if (!ready || playValidationPending) {
        return AppUiRomPlayRequest::Ignore;
    }
    return validationPending
        ? AppUiRomPlayRequest::AwaitReplacementCheck
        : AppUiRomPlayRequest::StartFinalCheck;
}

bool AppUi_deferredCommit(bool previewChanged, bool deactivated, bool *dirty) {
    if (!dirty) return false;
    if (previewChanged) *dirty = true;
    return deactivated && *dirty;
}

bool AppUi_cursorVisible(bool overlayOpen, bool anyDevToolOpen) {
    return overlayOpen || anyDevToolOpen;
}

AppUiIdleDecision AppUi_idleDecision(bool drawableAvailable, bool occluded) {
    if (!drawableAvailable) return {false, 25};
    if (occluded) return {true, 25};
    return {true, 0};
}

bool AppUi_parseScale(const char *text, float *scale) {
    if (!text || !scale) return false;
    char *end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(parsed) ||
        parsed < 0.75f || parsed > 2.0f) {
        return false;
    }
    *scale = parsed;
    return true;
}

bool AppUi_applyDpiTransition(AppUiDpiState *state, float framebufferScale) {
    if (!state) return false;
    if (framebufferScale < 1.0f) framebufferScale = 1.0f;
    if (std::fabs(framebufferScale - state->framebufferScale) < 0.05f) {
        return false;
    }
    state->framebufferScale = framebufferScale;
    ++state->atlasGeneration;
    return true;
}

AppUiSmokeInputMode AppUi_validateSmokeInput(
    const char *frames, const char *selection, const char *input,
    const char *token, const char *pace, const char *walk,
    const char *onlineAction, const char *browser, const char *packBrowser) {
    // Exactly one scripted selection, and it must be one of the selections the
    // launcher has scripts for. Naming them explicitly is what keeps an
    // inherited variable from attaching synthetic input to a normal session,
    // and requiring exactly one keeps two scripts from sharing a run.
    const bool selectsFrameLimit = selection && selection[0];
    const bool selectsPace = pace && pace[0];
    const bool selectsWalk = walk && walk[0];
    const bool selectsOnlineAction = onlineAction && onlineAction[0];
    const bool selectsBrowser = browser && browser[0];
    const bool selectsPackBrowser = packBrowser && packBrowser[0];
    const int scripts = (selectsFrameLimit ? 1 : 0) + (selectsPace ? 1 : 0) +
                        (selectsWalk ? 1 : 0) +
                        (selectsOnlineAction ? 1 : 0) +
                        (selectsBrowser ? 1 : 0) + (selectsPackBrowser ? 1 : 0);
    const bool anyInputContract =
        scripts > 0 || (input && input[0]) || (token && token[0]);
    if (!anyInputContract) return AppUiSmokeInputMode::Disabled;
    if (!frames || !frames[0] || !input || !token || scripts != 1 ||
        (selectsFrameLimit && std::strcmp(selection, "240") != 0) ||
        (selectsPace && std::strcmp(pace, "original") != 0 &&
         std::strcmp(pace, "smooth") != 0) ||
        (selectsWalk && std::strcmp(walk, "1") != 0) ||
        ((selectsBrowser || selectsPackBrowser) && std::strcmp(input, "gamepad") != 0) ||
        std::strcmp(token, "mdkr64-app-ui-input-v1") != 0) {
        return AppUiSmokeInputMode::Invalid;
    }
    char *end = nullptr;
    const long frameCount = std::strtol(frames, &end, 10);
    if (end == frames || *end != '\0' || frameCount < 1 || frameCount > 1000000) {
        return AppUiSmokeInputMode::Invalid;
    }
    if (std::strcmp(input, "keyboard") == 0) {
        return AppUiSmokeInputMode::Keyboard;
    }
    if (std::strcmp(input, "gamepad") == 0) {
        return AppUiSmokeInputMode::Gamepad;
    }
    return AppUiSmokeInputMode::Invalid;
}

AppUiSmokeInputMode AppUi_smokeInputMode() {
    return AppUi_validateSmokeInput(
        std::getenv("MDKR_APP_SMOKE_FRAMES"),
        std::getenv("MDKR_APP_SMOKE_SELECT_FRAME_LIMIT"),
        std::getenv("MDKR_APP_SMOKE_INPUT"),
        std::getenv("MDKR_APP_SMOKE_INPUT_TOKEN"),
        std::getenv("MDKR_APP_SMOKE_SELECT_PRESENTATION_PACE"),
        std::getenv("MDKR_APP_SMOKE_A11Y_WALK"),
        std::getenv("MDKR_APP_SMOKE_ONLINE_ACTION"),
        std::getenv("MDKR_APP_SMOKE_ROM_BROWSER"),
        std::getenv("MDKR_APP_SMOKE_PACK_BROWSER"));
}

bool AppUi_a11yWalkArmed() {
    // Read once. Several draw paths ask this every frame, and the answer
    // cannot change inside a process.
    //
    // Two arming routes because there are two walks over the same rows: the
    // launcher's, which rides the complete versioned synthetic-input contract
    // so an inherited variable cannot quietly rearrange a player's settings
    // panel, and the in-game overlay's, which has no such contract to ride --
    // it lives inside an engine session and scripts itself from
    // ui_overlay.cpp's existing test schedule.
    static const bool armed =
        (AppUi_smokeInputMode() != AppUiSmokeInputMode::Disabled &&
         AppUi_smokeInputMode() != AppUiSmokeInputMode::Invalid &&
         std::getenv("MDKR_APP_SMOKE_A11Y_WALK") != nullptr) ||
        std::getenv("MDKR_TEST_OVERLAY_A11Y_WALK") != nullptr;
    return armed;
}

bool AppUi_videoSettingVisible(MdkrVideoKey key, bool webGpuRenderer,
                               bool legacyStretchActive) {
    switch (key) {
        // Keep the reserved config key parseable, but hide it until a
        // texture-pack loader actually consumes it.
        case MDKR_VIDEO_TEXTURE_PACK: return false;
        // NOT "implied by the preset": every preset pins this to 1, Pure
        // included, so no mode a player can pick ever selects the 0 branch.
        // 0 is the pre-widescreen compatibility path, which STRETCHES 4:3
        // content to fill the window (display_config.c's legacy_stretch
        // layout) -- a distorted image, not an alternative framing, and
        // Video.Aspect is what actually produces 4:3. Offering it as a normal
        // choice would be offering a wrong picture, so it stays CLI/config-only
        // (--legacy-stretch, hand-edited ini).
        //
        // It becomes visible for exactly one reason: a config that ALREADY
        // resolves to 0 leaves the player looking at a stretched image with no
        // control to undo it, and Video.Mode=custom means no preset will
        // re-pin it either. Surfacing the setting only in that state is the
        // escape hatch, and it disappears again the moment they leave it.
        case MDKR_VIDEO_WIDESCREEN: return legacyStretchActive;
        // Immutable interpolation is supported by both production backends.
        // Keep it visible beside Frame Limit so presentation rate and the
        // choice of authored holds versus unique midpoints stay independent.
        case MDKR_VIDEO_MOTION_SMOOTHING: return true;
        // WebGPU is the qualified default and has no multisampled scene path.
        // Keep the config key for GL/Metal and diagnostics, but never offer a
        // restart-required control that the active renderer ignores.
        case MDKR_VIDEO_MSAA: return !webGpuRenderer;
        default: return true;
    }
}

AppUiSettingsSection AppUi_settingsSection(MdkrVideoKey key) {
    // Written out rather than derived, because the two functions that could
    // derive it -- mdkr_video_key_is_enhancement() and
    // mdkr_video_key_is_content(), both in platform/video_config.c -- live in a
    // translation unit this policy is deliberately not linked against: these
    // policies are pure so the contract test can run without the config layer,
    // a ROM, a window, or a GPU. Keep the two lists in step; the settings panel
    // reads THIS one, so a key added there and forgotten here is drawn under
    // its category header rather than lost.
    switch (key) {
        case MDKR_ENH_SPEEDOMETER:
        case MDKR_ENH_DRAW_DISTANCE:
        case MDKR_ENH_LOD_BIAS:
        case MDKR_ENH_AI_DIFFICULTY:
        case MDKR_ENH_ADVENTURE_PARTY:
            return AppUiSettingsSection::Enhancements;
        case MDKR_CONTENT_PACKS_ENABLED:
        case MDKR_CONTENT_PACK_DISABLED:
            return AppUiSettingsSection::Content;
        // Every accessibility option in the product, in one place. The speech
        // keys declare the Interface category and Camera.Comfort declares a
        // camera one, which is correct for what they configure and wrong for
        // who is looking for them.
        case MDKR_A11Y_SPEECH:
        case MDKR_A11Y_SPEECH_RATE:
        case MDKR_A11Y_SPEECH_VOLUME:
        case MDKR_A11Y_SPEECH_RACE:
        case MDKR_VIDEO_CAMERA_COMFORT:
            return AppUiSettingsSection::Accessibility;
        default:
            return AppUiSettingsSection::Category;
    }
}

AppUiSettingsSection AppUi_shellPreferenceSection(AppUiShellPreference key) {
    switch (key) {
        // Text size belongs with the other access needs, not with the window
        // mode. It is also the one setting a player may have to change before
        // they can read anything else, so it sits in the section they will be
        // told to look for.
        case AppUiShellPreference::UiScale:
            return AppUiSettingsSection::Accessibility;
        // The button that opens the in-game menu belongs beside the other
        // controller rows. A player whose pad opens the menu on its own
        // (issue #55: SDL's fallback for the NSO N64 pad lands C-Right on
        // BACK) looks under Controls, not under an access-needs section.
        case AppUiShellPreference::MenuToggleButton:
            return AppUiSettingsSection::Category;
    }
    return AppUiSettingsSection::Category;
}

bool AppUi_launcherHoldOpensLauncher(AppUiLauncherHold hold) {
    // Shift is unambiguous at launch, so it stands alone. A pad has to hold
    // BOTH shoulders: L or R on its own is what a controller pressed against
    // the inside of a bag reports for hours, and treating that as a request
    // would quietly switch the setting off for anyone who owns one.
    return hold.shift || (hold.leftShoulder && hold.rightShoulder);
}

bool AppUi_launcherSkipArmed(bool settingEnabled, bool holdOpensLauncher) {
    // The hold is evaluated last and wins outright. That ordering is the whole
    // safety property: there is no combination of persisted state that can
    // leave a player unable to reach their launcher.
    return settingEnabled && !holdOpensLauncher;
}

bool AppUi_launcherSkipShouldBoot(AppUiLauncherSkipReadiness state) {
    if (!state.armed || state.dispatched) return false;
    // Undecided is not "no". While the remembered file is still being read
    // there is no verdict to act on, so the answer is "not yet" -- the launcher
    // keeps drawing and this is asked again next frame.
    if (state.validationPending || state.playValidationPending) return false;
    // No remembered game, or one that no longer verifies: the launcher is
    // exactly where the player needs to be, and it already says why.
    if (!state.romRemembered || !state.romValid) return false;
    // Something is on screen to be read (a failed boot, a save that could not
    // be written). Booting past it would throw the message away unseen.
    if (state.bootErrorVisible) return false;
    // A Workshop preview drives the same Play transition for its own reasons.
    // Two owners of one boot is one too many.
    return !state.otherWorkPending;
}

bool AppUi_enhancementResetIncludes(MdkrVideoKey key) {
    // Exactly the rows the Enhancements section draws, and nothing else. In
    // particular NOT the content keys: they share the section's "things you
    // opted into" character but a pack is installed content, and silently
    // switching it off is not what "reset the extras" offers to do.
    return AppUi_settingsSection(key) == AppUiSettingsSection::Enhancements;
}
