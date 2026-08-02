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
    const char *token) {
    const bool anyInputContract =
        (selection && selection[0]) || (input && input[0]) || (token && token[0]);
    if (!anyInputContract) return AppUiSmokeInputMode::Disabled;
    if (!frames || !frames[0] || !selection || !input || !token ||
        std::strcmp(selection, "240") != 0 ||
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
        std::getenv("MDKR_APP_SMOKE_INPUT_TOKEN"));
}

bool AppUi_videoSettingVisible(MdkrVideoKey key) {
    switch (key) {
        // Keep the reserved config key parseable, but hide it until a
        // texture-pack loader actually consumes it.
        case MDKR_VIDEO_TEXTURE_PACK: return false;
        default: return true;
    }
}
