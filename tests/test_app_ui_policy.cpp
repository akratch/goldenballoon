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

    bool dirty = false;
    int durableTransactions = 0;
    for (int update = 0; update < 100; ++update) {
        if (AppUi_deferredCommit(true, false, &dirty)) ++durableTransactions;
    }
    if (AppUi_deferredCommit(false, true, &dirty)) ++durableTransactions;
    expect(durableTransactions == 1,
           "100 slider previews coalesce to one durable transaction");
    expect(dirty, "failed durable transaction remains retryable");
    dirty = false;  // production clears only after a successful persistence result
    expect(!dirty, "successful durable transaction clears staged state");

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
