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
    for trigger in ("MDKR64_HIDDEN", "MDKR_APP_SMOKE_FRAMES",
                    "MDKR_APP_AUTOPLAY", "MDKR_APP_FILEDIALOG_SELFTEST"):
        require(trigger in header,
                f"automated surface policy must cover {trigger}")
    # Desktop safety is a rendering policy, not a refusal to start. Automation
    # must never be gated on an environment capability again.
    for removed in ("MDKR_APP_TESTS_ALLOWED", "MDKR_DEDICATED_TEST_DESKTOP",
                    "rejectUnauthorizedAutomationSurface"):
        require(removed not in header and removed not in main_app,
                f"{removed} must not gate native surface creation")
    background_function = between(
        header, "inline bool AppActivation_backgroundAutomation() {", "}\n"
    )
    for trigger in ("MDKR64_HIDDEN", "MDKR_APP_SMOKE_FRAMES",
                    "MDKR_APP_AUTOPLAY", "MDKR_APP_FILEDIALOG_SELFTEST"):
        require(trigger in background_function,
                f"background policy must cover {trigger} without relying on a runner")
    for removed in ("MDKR_APP_TESTS_ALLOWED", "MDKR_DEDICATED_TEST_DESKTOP"):
        require(removed not in engine_sdl,
                f"{removed} must not gate the direct engine SDL boundary")
    sdl_hint_guard = engine_sdl.index("if (sdl_automation_surface_requested()) {")
    sdl_init = engine_sdl.index("SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)")
    require(sdl_hint_guard < sdl_init,
            "direct-engine background hints must be chosen before SDL_Init")
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
    # The pre-SDL policy is now the first thing the automation path does: it
    # must still precede every route that can create or activate a surface.
    main_prepare = main_app.index("AppActivation_prepareProcess();")
    engine_dispatch = main_app.index(
        "return mdkr64_headless_main(argc, argv);", main_prepare)
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
    # --jobs serialization policy (updated with the runner's GPU-aware pooling
    # fix). Desktop safety is enforced in the window layer — hidden surfaces,
    # the background policy, and the lowered priority asserted above — not by
    # serializing every engine role. So only the roles that share ONE build
    # tree or bind a fixed local port stay serial; rom/native/release/asan
    # pool, with just their render/GPU/measurement checks pulled out by name
    # (GPU_SERIAL_NAMES/SERIAL_NAMES). APP_ROLES still names every app-executing
    # role for the focus-policy reasoning below.
    serial = between(runner, "SERIAL_ROLES = frozenset({", "})")
    app_roles = between(runner, "APP_ROLES = (", ")\n")
    require('"rom",' in app_roles,
            "ROM-backed checks execute the native app and must stay named in "
            "APP_ROLES for the focus-policy reasoning")
    for role in ("ctest", "instrumented", "layout",
                 "wasm", "browser", "browser_save", "browser_local"):
        require(f'"{role}",' in serial,
                f"build-tree/port-sharing role {role} must be serialized")
    for role in ("rom", "native", "release", "asan"):
        require(f'"{role}",' not in serial,
                f"role {role} must not be serialized wholesale; only its "
                "GPU/measurement checks serialize, by name")
    require('option(MDKR_ENABLE_GPU_TESTS' in cmake,
            "native GPU/window CTests must require a configure-time opt-in")
    require('if(MDKR_ENABLE_GPU_TESTS AND NOT MDKR_SKIP_GPU_TESTS)' in cmake,
            "native GPU/window CTests must be absent by default")
    # The suite has no human gate: a bare run covers every class. The
    # --with-* flags survive only as accepted no-ops.
    for flag in ('"--with-browser-tests"', '"--with-app-tests"',
                 '"--with-compiled-tests"', '"--with-gpu-tests"'):
        require(flag in runner,
                f"{flag} must still be accepted for compatibility")
    require("deprecated no-op" in runner,
            "the --with-* flags must document that they no longer gate anything")
    for removed in ('if app_checks and not args.with_app_tests:',
                    'args.app_tests_excluded',
                    'environment["MDKR_APP_TESTS_ALLOWED"] = "1"',
                    'environment["MDKR_BROWSER_TESTS_ALLOWED"] = "1"',
                    'MDKR_DEDICATED_TEST_DESKTOP',
                    'command += ["-LE", "gpu|app_process|browser"]'):
        require(removed not in runner,
                f"runner must no longer gate or exclude via {removed}")
    for removed in ("MDKR_APP_TESTS_ALLOWED", "MDKR_DEDICATED_TEST_DESKTOP",
                    "_native_process_tests_allowed"):
        require(removed not in harness,
                f"harness must not gate the native product on {removed}")
    # The yield-to-desktop guarantee is per-child, never self-applied: an
    # inherited nice/Darwin-background band cannot be given back, and on
    # Apple Silicon it confines every wall-clock verdict to the efficiency
    # cores (measured on an idle M4 Max: rollback p99 2x over ceiling,
    # audio sink drains, browser step timeouts). Pinned both directions.
    require('return ["taskpolicy", "-b"]' in runner,
            "runner must launch bulk children under Darwin background policy")
    require('check.name in SERIAL_NAMES or check.role in FOREGROUND_ROLES'
            in runner,
            "runner must exempt wall-clock and browser checks from the band")
    require('str(os.getpid())' not in runner and 'current_nice' not in runner,
            "runner must not demote itself; children inherit and cannot undo it")
    local_ci = (ROOT / "tools/ci/ci_local.sh").read_text(encoding="utf-8")
    require('taskpolicy -b "$@"' in local_ci,
            "local CI must launch bulk steps under Darwin background policy")
    require('-p "$$"' not in local_ci,
            "local CI must not demote itself; run_checks and the audio "
            "contract inherit and cannot undo it")
    require("-E '^audio_sink_contract$'" in local_ci and
            "-R '^audio_sink_contract$'" in local_ci,
            "local CI must run the audio sink contract foreground, outside "
            "the yielded CTest lane")
    require('RUN_COMPILED_TESTS=0' in local_ci and
            '--with-compiled-tests' in local_ci and
            'if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ "$RUN_COMPILED_TESTS" -eq 1 ]; then'
            in local_ci,
            "local CI must skip every compiled test by default")
    safe_lane = between(local_ci, 'step "ROM-free CTest suite"',
                        '# These tests open native graphics surfaces.')
    require("MDKR64_HIDDEN=1" in safe_lane and
            "SDL_MAC_BACKGROUND_APP=1" in safe_lane and
            "SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1" in safe_lane,
            "local CTest lane must run its surfaces hidden and non-activating")
    require('RUN_GPU_TESTS=0' in local_ci and
            'MDKR_CI_WITH_GPU_TESTS' not in local_ci,
            "ambient environment must not enable local GPU/window tests")
    for removed in ("MDKR_BROWSER_TESTS_ALLOWED", "MDKR_DEDICATED_TEST_DESKTOP"):
        require(removed not in browser,
                f"Chromium harness must not gate on {removed}")
    require('return ["nice", "-n", "10"]' in runner,
            "runner must lower bulk child CPU priority on non-Darwin POSIX")
    print("app background activation contract passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"app background activation contract failed: {error}")
        raise SystemExit(1)
