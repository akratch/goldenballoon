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
    main_app = (ROOT / "platform/app/main_app.cpp").read_text(encoding="utf-8")
    window = (ROOT / "platform/app/app_window.cpp").read_text(encoding="utf-8")
    engine_sdl = (ROOT / "platform/platform_sdl_min.c").read_text(
        encoding="utf-8"
    )
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    runner = (ROOT / "tools/run_checks.py").read_text(encoding="utf-8")
    harness = (ROOT / "tests/harness_utils.py").read_text(encoding="utf-8")
    browser = (ROOT / "tests/check_browser_runtime.py").read_text(
        encoding="utf-8"
    )

    require('std::getenv("MDKR64_HIDDEN") != nullptr' in header,
            "background policy must use the existing automation variable")
    require('std::getenv("MDKR_APP_TESTS_ALLOWED")' in header and
            'std::getenv("MDKR_DEDICATED_TEST_DESKTOP")' in header and
            'std::strcmp(allowed, "1") == 0' in header and
            'std::strcmp(dedicated, "1") == 0' in header,
            "native surfaces must require class and desktop capabilities")
    for trigger in ("MDKR64_HIDDEN", "MDKR_APP_SMOKE_FRAMES",
                    "MDKR_APP_AUTOPLAY", "MDKR_APP_FILEDIALOG_SELFTEST"):
        require(trigger in header,
                f"automated surface guard must cover {trigger}")
    background_function = between(
        header, "inline bool AppActivation_backgroundAutomation() {", "}\n"
    )
    for trigger in ("MDKR64_HIDDEN", "MDKR_APP_SMOKE_FRAMES",
                    "MDKR_APP_AUTOPLAY", "MDKR_APP_FILEDIALOG_SELFTEST"):
        require(trigger in background_function,
                f"background policy must cover {trigger} without relying on a runner")
    require("if (AppActivation_rejectUnauthorizedAutomationSurface())" in main_app,
            "application must fail closed before native surface creation")
    guard_at = main_app.index(
        "if (AppActivation_rejectUnauthorizedAutomationSurface())")
    engine_dispatch = main_app.index(
        "return mdkr64_headless_main(argc, argv);", guard_at
    )
    require(guard_at < main_app.index("return runFileDialogSelfTest();") and
            guard_at < engine_dispatch and
            guard_at < main_app.index("AppHost host;"),
            "capability guard must precede engine dispatch, dialogs and AppHost")
    require('getenv("MDKR_APP_TESTS_ALLOWED")' in engine_sdl and
            'getenv("MDKR_DEDICATED_TEST_DESKTOP")' in engine_sdl and
            'strcmp(allowed, "1") == 0' in engine_sdl and
            'strcmp(dedicated, "1") == 0' in engine_sdl,
            "direct engine SDL boundary must require both capabilities")
    sdl_guard = engine_sdl.index("if (sdl_automation_surface_requested() &&")
    require(sdl_guard < engine_sdl.index("if (!mdkr_render_backend_available())"),
            "direct-engine capability guard must precede renderer/video setup")
    sdl_init = engine_sdl.index("SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)")
    for hint in ("SDL_HINT_MAC_BACKGROUND_APP",
                 "SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN"):
        require(hint in engine_sdl and engine_sdl.index(hint) < sdl_init,
                f"direct-engine {hint} must be set before SDL_Init")
    require("void AppActivation_prepareProcess()" in mac,
            "macOS must have a pre-SDL process activation policy")
    prepare = between(mac, "void AppActivation_prepareProcess() {",
                      "void AppActivation_requestForeground")
    require("NSApplicationActivationPolicyAccessory" in prepare and
            "SDL_HINT_MAC_BACKGROUND_APP" in prepare and
            "SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN" in prepare,
            "pre-SDL macOS policy must demote AppKit and set both SDL hints")
    main_prepare = main_app.index("AppActivation_prepareProcess();", guard_at)
    require(main_prepare < engine_dispatch and
            main_prepare < main_app.index("return runFileDialogSelfTest();") and
            main_prepare < main_app.index("AppHost host;"),
            "pre-SDL policy must precede engine, file dialog and launcher paths")
    require("AppActivation_prepareProcess();" in host[:host.index("SDL_Init(")],
            "reusable app host must prepare background process before SDL_Init")
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
            "AppActivation_prepareProcess();" in background,
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
    require('"SDL_MAC_BACKGROUND_APP": "1"' in runner,
            "runner must stop Cocoa promoting hermetic MDKR-scrubbing children")
    require('"SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN": "1"' in runner,
            "runner must stop SDL_ShowWindow activating automation")
    serial = between(runner, "SERIAL_ROLES = frozenset({", "})")
    app_roles = between(runner, "APP_ROLES = (", ")\n")
    require('"rom",' in app_roles,
            "ROM-backed checks can execute the native app and must require "
            "the app-test opt-in")
    for role in ("ctest", "rom", "native", "release", "asan", "instrumented", "layout",
                 "wasm", "browser", "browser_save", "browser_local"):
        require(f'"{role}",' in serial,
                f"workstation-intensive role {role} must be serialized")
    require('option(MDKR_ENABLE_GPU_TESTS' in cmake,
            "native GPU/window CTests must require a configure-time opt-in")
    require('if(MDKR_ENABLE_GPU_TESTS AND NOT MDKR_SKIP_GPU_TESTS)' in cmake,
            "native GPU/window CTests must be absent by default")
    require('"--with-browser-tests"' in runner,
            "runner must require an explicit browser-test opt-in")
    require('"--with-app-tests"' in runner,
            "runner must require an explicit native app-test opt-in")
    require('"--with-compiled-tests"' in runner,
            "runner must require an explicit compiled-test opt-in")
    require("if app_checks and not args.with_app_tests:" in runner,
            "runner must fail closed around native app/renderer roles")
    require("if check.role not in APP_ROLES" in runner,
            "ordinary runner use must exclude native app/renderer roles")
    require("if args.with_gpu_tests and (" in runner and
            "not args.with_app_tests or not args.with_compiled_tests" in runner,
            "GPU CTests must require native-app and compiled-test capabilities")
    require('environment["MDKR_APP_TESTS_ALLOWED"] = "1"' in runner,
            "runner app opt-in must be explicit in child environments")
    enhancement_authority = (
        ROOT / "tests/check_enhancement_authority.py"
    ).read_text(encoding="utf-8")
    require("resolve_binary(args.build)" in enhancement_authority,
            "enhancement authority must cross the shared native-process gate")
    require('os.environ.get("MDKR_DEDICATED_TEST_DESKTOP") == "1"' in runner and
            'environment["MDKR_DEDICATED_TEST_DESKTOP"] = "1"' in runner,
            "runner must require and only forward caller desktop attestation")
    require('product_names = {"mdkr64", "mdkr64.exe"}' in harness and
            'not _native_process_tests_allowed()' in harness,
            "direct Python harnesses must reject the product before launch")
    require('os.environ.get("MDKR_APP_TESTS_ALLOWED") == "1"' in harness and
            'os.environ.get("MDKR_BROWSER_TESTS_ALLOWED") == "1"' in harness and
            'os.environ.get("MDKR_DEDICATED_TEST_DESKTOP") == "1"' in harness,
            "harness launch guard must require class and desktop capabilities")
    require('command += ["-LE", "gpu|app_process|browser"]' in runner,
            "runner must omit all app/browser process labels by default")
    require('["taskpolicy", "-b", "-p", str(os.getpid())]' in runner,
            "macOS runner must enter inherited Darwin background policy")
    require('taskpolicy -b -p "$$"' in
            (ROOT / "tools/ci/ci_local.sh").read_text(encoding="utf-8"),
            "local CI must enter inherited Darwin background policy")
    local_ci = (ROOT / "tools/ci/ci_local.sh").read_text(encoding="utf-8")
    require('RUN_COMPILED_TESTS=0' in local_ci and
            '--with-compiled-tests' in local_ci and
            'if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ "$RUN_COMPILED_TESTS" -eq 1 ]; then'
            in local_ci,
            "local CI must skip every compiled test by default")
    safe_lane = between(local_ci, 'step "ROM-free CTest suite"',
                        '# These tests open native graphics surfaces.')
    require("env -u MDKR_APP_TESTS_ALLOWED -u MDKR_BROWSER_TESTS_ALLOWED" in
            safe_lane,
            "safe local CTest must strip inherited surface capabilities")
    require('RUN_GPU_TESTS=0' in local_ci and
            'MDKR_CI_WITH_GPU_TESTS' not in local_ci,
            "ambient environment must not enable local GPU/window tests")
    require('environment["MDKR_BROWSER_TESTS_ALLOWED"] = "1"' in runner,
            "runner browser opt-in must reach the Chromium launch boundary")
    require('os.environ.get("MDKR_BROWSER_TESTS_ALLOWED") != "1"' in browser,
            "direct Chromium harness use must fail closed")
    require('os.environ.get("MDKR_DEDICATED_TEST_DESKTOP") != "1"' in browser,
            "direct Chromium harness must require desktop attestation")
    require('if current_nice < 10:' in runner,
            "runner must lower its inherited CPU scheduling priority")
    print("app background activation contract passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"app background activation contract failed: {error}")
        raise SystemExit(1)
