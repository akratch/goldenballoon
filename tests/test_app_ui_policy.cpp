#include "app_ui_policy.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

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

    AppUiDpiState dpi;
    expect(!AppUi_applyDpiTransition(&dpi, 1.0f), "same-DPI frame is a no-op");
    expect(AppUi_applyDpiTransition(&dpi, 2.0f), "1x to 2x rebuilds atlas");
    expect(dpi.atlasGeneration == 2 && std::fabs(dpi.framebufferScale - 2.0f) < 0.001f,
           "2x atlas generation recorded exactly once");
    expect(!AppUi_applyDpiTransition(&dpi, 2.01f), "DPI rounding does not churn atlas");
    expect(AppUi_applyDpiTransition(&dpi, 1.0f), "2x to 1x rebuilds atlas");
    expect(dpi.atlasGeneration == 3,
           "1x to 2x to 1x produces two, non-compounding rebuilds");

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
    expect(AppUi_validateSmokeInput(nullptr, nullptr, nullptr, nullptr) ==
               SmokeMode::Disabled,
           "no synthetic-input environment leaves smoke input disabled");
    expect(AppUi_validateSmokeInput("24", "240", "gamepad",
                                    "mdkr64-app-ui-input-v1") ==
               SmokeMode::Gamepad,
           "complete versioned gamepad contract is accepted");
    expect(AppUi_validateSmokeInput("18", "240", "keyboard",
                                    "mdkr64-app-ui-input-v1") ==
               SmokeMode::Keyboard,
           "complete versioned keyboard contract is accepted");
    expect(AppUi_validateSmokeInput(nullptr, "240", "gamepad",
                                    "mdkr64-app-ui-input-v1") ==
               SmokeMode::Invalid,
           "missing smoke frame contract is rejected");
    expect(AppUi_validateSmokeInput("24", "240", "gamepad", nullptr) ==
               SmokeMode::Invalid,
           "missing authorization token is rejected");
    expect(AppUi_validateSmokeInput("24", "240", "gamepad", "stale") ==
               SmokeMode::Invalid,
           "stale authorization token is rejected");
    expect(AppUi_validateSmokeInput("24", nullptr, "gamepad",
                                    "mdkr64-app-ui-input-v1") ==
               SmokeMode::Invalid,
           "partial controller environment is rejected");

    if (failures) {
        std::printf("%d app UI policy failure(s)\n", failures);
        return 1;
    }
    std::printf("app UI policies passed\n");
    return 0;
}
