#include "app_ui_policy.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

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
        ? AppUiRomPlayRequest::SupersedeReplacementCheck
        : AppUiRomPlayRequest::StartFinalCheck;
}

bool AppUi_deferredCommit(bool previewChanged, bool deactivated, bool *dirty) {
    if (!dirty) return false;
    if (previewChanged) *dirty = true;
    return deactivated && *dirty;
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
    const char *token, const char *pace) {
    // Exactly one scripted selection, and it must be one of the two the
    // launcher has scripts for. Naming them explicitly is what keeps an
    // inherited variable from attaching synthetic input to a normal session,
    // and requiring exactly one keeps two pointer scripts from sharing a run.
    const bool selectsFrameLimit = selection && selection[0];
    const bool selectsPace = pace && pace[0];
    const bool anyInputContract =
        selectsFrameLimit || selectsPace || (input && input[0]) ||
        (token && token[0]);
    if (!anyInputContract) return AppUiSmokeInputMode::Disabled;
    if (!frames || !frames[0] || !input || !token ||
        selectsFrameLimit == selectsPace ||
        (selectsFrameLimit && std::strcmp(selection, "240") != 0) ||
        (selectsPace && std::strcmp(pace, "original") != 0 &&
         std::strcmp(pace, "smooth") != 0) ||
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
        std::getenv("MDKR_APP_SMOKE_SELECT_PRESENTATION_PACE"));
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
