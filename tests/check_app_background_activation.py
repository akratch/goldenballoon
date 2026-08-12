#!/usr/bin/env python3
"""Keep native UI automation renderable without stealing desktop focus."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def between(text: str, start: str, end: str) -> str:
    require(start in text and end in text, f"missing contract boundary: {start}")
    return text.split(start, 1)[1].split(end, 1)[0]


def main() -> int:
    header = (ROOT / "platform/app/app_activation.h").read_text(encoding="utf-8")
    mac = (ROOT / "platform/app/app_activation_mac.mm").read_text(encoding="utf-8")
    host = (ROOT / "platform/app/app_host.cpp").read_text(encoding="utf-8")
    window = (ROOT / "platform/app/app_window.cpp").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    require('std::getenv("MDKR64_HIDDEN") != nullptr' in header,
            "background policy must use the existing automation variable")
    require("if (AppActivation_backgroundAutomation()) return;" in header,
            "non-mac automation must skip Show/Raise")
    background = between(mac,
        "if (AppActivation_backgroundAutomation()) {",
        "[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular]")
    for forbidden in ("activateIgnoringOtherApps", "makeKeyAndOrderFront",
                      "orderFrontRegardless", "SDL_RaiseWindow"):
        require(forbidden not in background,
                f"mac background branch must not call {forbidden}")
    require("orderBack:nil" in background and
            "NSApplicationActivationPolicyAccessory" in background,
            "mac automation surface must render behind other apps without Dock focus")
    require(host.count("backgroundAutomation ? SDL_WINDOW_HIDDEN") == 2,
            "both GL and WebGPU automation windows must start hidden")
    require(host.count("if (!backgroundAutomation) flags |= AppWindow_creationFlags();") == 2,
            "automation must not inherit persisted fullscreen creation flags")
    require("if (!AppActivation_backgroundAutomation()) SDL_RaiseWindow(window);" in window,
            "live window transitions must not raise automation")
    require("AppActivation_backgroundAutomation() &&" in window and
            "return mdkr_video_config_runtime_set(MDKR_WINDOW_MODE, mode);" in window,
            "automation fullscreen requests must persist without an OS transition")

    gpu_environment_rows = [line for line in cmake.splitlines()
                            if "ENVIRONMENT \"" in line and
                            "MDKR_APP_SMOKE" in line]
    require(len(gpu_environment_rows) >= 10,
            "expected native launcher smoke inventory")
    require(all("MDKR64_HIDDEN=1" in line for line in gpu_environment_rows),
            "every direct launcher smoke must opt into background automation")
    print("app background activation contract passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"app background activation contract failed: {error}")
        raise SystemExit(1)
