#!/usr/bin/env python3
"""Fail closed if the repository correctness workflow loses required coverage."""

from __future__ import annotations

import plistlib
import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "correctness.yml"
MACOS_WORKFLOW = ROOT / ".github" / "workflows" / "macos-release.yml"
DESKTOP_RELEASE_WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
WEB_DEMO_WORKFLOW = ROOT / ".github" / "workflows" / "web-demo.yml"
WINDOWS_VALIDATE_WORKFLOW = (
    ROOT / ".github" / "workflows" / "windows-validate.yml"
)
CI_LOCAL = ROOT / "tools" / "ci" / "ci_local.sh"
SOURCE_ARCHIVE_SMOKE = ROOT / "tools" / "smoke_public_source_archive.sh"
RUN_CHECKS = ROOT / "tools" / "run_checks.py"
WINDOWS_PACKAGER = ROOT / "tools" / "package_windows_zip.sh"
WINDOWS_IMPORT_GUARD = ROOT / "tools" / "check_windows_imports.sh"
LINUX_PACKAGER = ROOT / "tools" / "package_linux_appimage.sh"
PROVENANCE_STAMP = ROOT / "tools" / "release" / "stamp_provenance.sh"
PROVENANCE_VERIFY = ROOT / "tools" / "release" / "verify_provenance.sh"
MACOS_BUILDER = ROOT / "macos" / "Scripts" / "build_app_bundle.sh"
MACOS_SIGNER = ROOT / "macos" / "Scripts" / "sign_and_notarize.sh"
MACOS_NOTARY = ROOT / "macos" / "Scripts" / "notarize_artifact.sh"
MACOS_DMG = ROOT / "macos" / "Scripts" / "create_dmg.sh"
MACOS_SDL_BUILDER = ROOT / "macos" / "Scripts" / "build_release_sdl2.sh"
MACOS_UNSIGNED_VERIFY = ROOT / "macos" / "Scripts" / "verify_unsigned_release.sh"
MACOS_UNSIGNED_DMG_VERIFY = ROOT / "macos" / "Scripts" / "verify_unsigned_dmg.sh"
MACOS_DMG_MOUNT_HELPER = ROOT / "macos" / "Scripts" / "dmg_mount_cleanup.sh"
MACOS_LAUNCHSERVICES_PROBE = (
    ROOT / "macos" / "Scripts" / "run_launchservices_probe.py"
)
MACOS_BUNDLE_VERIFY = ROOT / "macos" / "Scripts" / "verify_gatekeeper_bundle.sh"
MACOS_ASSET_VERIFY = ROOT / "macos" / "Scripts" / "verify_asset_free.sh"
MACOS_PROVENANCE = ROOT / "macos" / "Scripts" / "stamp_macos_provenance.sh"
MACOS_SDL_CONFIG = ROOT / "macos" / "Scripts" / "release_sdl2_config.sh"
MACOS_ENTITLEMENTS = ROOT / "macos" / "Resources" / "Entitlements.plist"
MACOS_INFO_PLIST = ROOT / "macos" / "Resources" / "Info.plist"
MACOS_README = ROOT / "macos" / "README.md"
APP_PACING_CHECK = ROOT / "tests" / "check_app_adopted_pacing.py"
CMAKE_PROJECT = ROOT / "CMakeLists.txt"
UI_SETTINGS = ROOT / "platform" / "app" / "ui_settings.cpp"
APP_SOURCE_DIR = ROOT / "platform" / "app"
RELEASE_CHECKLIST = ROOT / "docs" / "RELEASE_CHECKLIST.md"
PINNED_ACTION_RE = re.compile(
    r"^\s*(?:-\s+)?uses:\s+([^@\s]+)@([^\s#]+)", re.MULTILINE
)
CLEAN_FAILURE_FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|SIGSEGV|Segmentation fault|SIGABRT|Abort trap"
)


def active_shell_source(source: str) -> str:
    """Return command text with shell/YAML comment-only content removed."""
    active: list[str] = []
    for raw_line in source.splitlines():
        if raw_line.lstrip().startswith("#"):
            continue
        # Remove a shell/YAML comment only when # begins a new, unquoted word.
        # Do not corrupt legitimate quoted parameter expansions such as
        # "${#APPS[@]}" while preventing `true # required command` from
        # satisfying a structural execution contract.
        quote: str | None = None
        escaped = False
        comment_at = len(raw_line)
        for index, character in enumerate(raw_line):
            if escaped:
                escaped = False
                continue
            if character == "\\" and quote != "'":
                escaped = True
                continue
            if character in ("'", '"'):
                if quote is None:
                    quote = character
                elif quote == character:
                    quote = None
                continue
            if (
                character == "#"
                and quote is None
                and (index == 0 or raw_line[index - 1].isspace() or
                     raw_line[index - 1] in ";|&()")
            ):
                comment_at = index
                break
        active.append(raw_line[:comment_at])
    return "\n".join(active)


def is_clean_rejection(result: subprocess.CompletedProcess[str],
                       required_stderr: str) -> bool:
    """Accept only the scripts' deliberate EXIT_FAILURE rejection path."""
    output = result.stdout + result.stderr
    return (
        result.returncode == 1
        and required_stderr in result.stderr
        and CLEAN_FAILURE_FATAL_RE.search(output) is None
    )


def workflow_input_fields(source: str, input_name: str) -> dict[str, str]:
    """Read scalar fields from one workflow_dispatch input by indentation."""
    lines = source.splitlines()
    header = f"      {input_name}:"
    try:
        start = lines.index(header) + 1
    except ValueError:
        return {}

    fields: dict[str, str] = {}
    for line in lines[start:]:
        if re.match(r"^      [A-Za-z0-9_]+:\s*$", line):
            break
        match = re.match(r"^        ([A-Za-z0-9_]+):\s*(.*?)\s*$", line)
        if match:
            fields[match.group(1)] = match.group(2).strip("\"'")
    return fields


def validate(source: str) -> list[str]:
    required = {
        "push trigger": "\n  push:",
        "all-branch push coverage": "branches: ['**']",
        "pull-request trigger": "\n  pull_request:",
        "manual trigger": "\n  workflow_dispatch:",
        "read-only permissions": "\npermissions:\n  contents: read",
        "full-history clean-room checkout": "fetch-depth: 0",
        "clean-room history gate": "tools/check_clean_room.sh",
        "public tree/new-commit step": "name: Public tree and new-commit boundary",
        "public ref-tip tree gate": "tools/check_public_surface.py --tree",
        "public pull-request range gate": "origin/$BASE_REF..HEAD",
        "public push range gate": "$BEFORE_SHA..$CURRENT_SHA",
        "tag push range bypass": '[ "$REF_TYPE" != "tag" ]',
        "new-ref default-branch boundary": "origin/$DEFAULT_BRANCH..$CURRENT_SHA",
        "public-surface positive controls": "tests/test_public_surface.py",
        "native SDK surface gate": "tools/check_native_sdk_surface.py",
        "third-party notice gate": "tools/check_third_party_notices.py",
        "image metadata gate": "tools/check_image_metadata.py",
        "address-domain gate": "tests/check_address_domains.py",
        "Linux WebGPU cell": "label: Linux WebGPU",
        "Linux OpenGL cell": "label: Linux OpenGL-only",
        "explicit Linux GCC compiler": "compiler: gcc",
        "macOS WebGPU cell": "label: macOS WebGPU warnings",
        "matrix compiler selection": "CC: ${{ matrix.compiler }}",
        "warning register": "-Wsometimes-uninitialized",
        "ROM-free CTest": "ctest --test-dir build-ci --output-on-failure",
        "ASan+UBSan": "-fsanitize=address,undefined",
        "linked wasm build": "tools/web/build_web.sh",
        "linked wasm layout": "tests/check_wave_visible_table.py",
        "browser save custody": "tests/check_browser_save_ui.py",
        "artifact ROM guard": "tools/check_no_rom.sh dist/web",
    }
    failures = [
        f"missing {label}: {needle!r}"
        for label, needle in required.items()
        if needle not in source
    ]
    actions = PINNED_ACTION_RE.findall(source)
    if not actions:
        failures.append("workflow has no actions")
    for action, revision in actions:
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            failures.append(
                f"{action} is not pinned to an immutable 40-character SHA: "
                f"{revision!r}"
            )
    forbidden = {
        "write-all permissions": "write-all",
        "repository write permission": "contents: write",
        "ROM secret/input": "MDKR_ROM",
        "untrusted pull-request target": "pull_request_target",
    }
    failures.extend(
        f"forbidden {label}: {needle!r}"
        for label, needle in forbidden.items()
        if needle in source
    )
    return failures


def validate_app_source_manifest(
    cmake: str, app_cpp_files: tuple[str, ...] | None = None
) -> list[str]:
    """Keep owned launcher sources explicit and platform selection complete."""
    failures: list[str] = []
    if app_cpp_files is None:
        app_cpp_files = tuple(sorted(path.name for path in APP_SOURCE_DIR.glob("*.cpp")))

    platform_variants = {"file_dialog_stub.cpp", "file_dialog_win.cpp"}
    expected_common = sorted(set(app_cpp_files) - platform_variants)
    manifest = re.search(
        r"set\(APP_SOURCES(?P<body>.*?)\)\n\s*\n\s*"
        r"# Native \"open file\" panel",
        cmake,
        re.DOTALL,
    )
    if "file(GLOB APP_SOURCES" in cmake:
        failures.append("APP_SOURCES must be an explicit manifest, not a glob")
    if manifest is None:
        failures.append("CMake no longer defines the explicit APP_SOURCES manifest")
        return failures

    listed_common = sorted(re.findall(
        r"\$\{CMAKE_SOURCE_DIR\}/platform/app/([A-Za-z0-9_]+\.cpp)",
        manifest.group("body"),
    ))
    if listed_common != expected_common:
        failures.append(
            "APP_SOURCES does not exactly match the reviewed common launcher "
            f"sources (expected {expected_common}, found {listed_common})"
        )

    app_section = cmake[manifest.end():]
    app_section = app_section.split("add_library(mdkr64_app STATIC", 1)[0]
    required_platform_routes = {
        "macOS activation bridge": "platform/app/app_activation_mac.mm",
        "macOS ROM dialog": "platform/app/file_dialog_mac.mm",
        "Windows ROM dialog": "platform/app/file_dialog_win.cpp",
        "portable stub ROM dialog": "platform/app/file_dialog_stub.cpp",
    }
    for label, source in required_platform_routes.items():
        if source not in app_section:
            failures.append(f"APP_SOURCES platform routing omits {label}: {source}")

    platform_warning_blocks = re.findall(
        r'set_source_files_properties\(\$\{PLATFORM_SOURCES\} PROPERTIES\s+'
        r'COMPILE_OPTIONS "([^"]+)"\)', cmake)
    if sorted(platform_warning_blocks) != sorted(("/W4;/WX", "-Wall;-Wextra;-Werror")):
        failures.append(
            "owned platform C sources no longer restore strict warnings after "
            "the decomp warning suppression"
        )
    app_warning_blocks = re.findall(
        r'set_source_files_properties\(\$\{APP_SOURCES\} PROPERTIES\s+'
        r'COMPILE_OPTIONS "([^"]+)"\)', cmake)
    if sorted(app_warning_blocks) != sorted(("/W4;/WX", "-Wall;-Wextra;-Werror")):
        failures.append(
            "owned app-shell C++ sources no longer enforce strict warnings "
            "without applying them to vendored Dear ImGui"
        )
    return failures


def validate_gpu_test_routing(sources: dict[str, str]) -> list[str]:
    """Keep real-GPU smoke out of headless CTest without losing release proof."""
    failures: list[str] = []
    cmake = sources["cmake"]
    smoke = re.search(
        r"set_tests_properties\(app_shell_smoke PROPERTIES(?P<body>.*?)\)",
        cmake,
        re.DOTALL,
    )
    settings_smoke = re.search(
        r"set_tests_properties\(app_settings_smoke PROPERTIES(?P<body>.*?)\)",
        cmake,
        re.DOTALL,
    )
    if "add_test(NAME app_shell_smoke COMMAND ${MDKR_TARGET})" not in cmake:
        failures.append("app_shell_smoke no longer executes the production app target")
    if smoke is None or re.search(r"\bLABELS\s+gpu\b", smoke.group("body")) is None:
        failures.append("app_shell_smoke is not labelled gpu")
    if "add_test(NAME app_settings_smoke COMMAND ${MDKR_TARGET})" not in cmake:
        failures.append("app_settings_smoke no longer executes the production app target")
    if (settings_smoke is None or
            re.search(r"\bLABELS\s+gpu\b", settings_smoke.group("body")) is None):
        failures.append("app_settings_smoke is not labelled gpu")
    if settings_smoke is None or "MDKR_APP_PANEL=Settings" not in settings_smoke.group("body"):
        failures.append("app_settings_smoke does not render the Settings panel")

    # GPU-labelled CTests run against the process-global graphics stack. They
    # may be excluded in headless CI, but whenever a maintainer invokes plain
    # `ctest -j`, every one must share this lock. The CMake helper discovers the
    # labels after registration, avoiding a fragile duplicated hand-maintained
    # list. Keep the invocation after the final label so a newly added GPU test
    # cannot fall outside the sweep.
    lock_function = re.search(
        r"function\(mdkr_serialize_gpu_ctests\)(?P<body>.*?)endfunction\(\)",
        cmake,
        re.DOTALL,
    )
    lock_calls = [
        match.start()
        for match in re.finditer(r"^\s*mdkr_serialize_gpu_ctests\(\)\s*$", cmake, re.MULTILINE)
    ]
    gpu_labels = list(re.finditer(r"\bLABELS\s+gpu\b", cmake))
    if lock_function is None:
        failures.append("CMake no longer defines the native GPU CTest lock sweep")
    else:
        lock_body = lock_function.group("body")
        for required_lock_fragment in (
            "DIRECTORY PROPERTY TESTS",
            "PROPERTY LABELS",
            'list(FIND MDKR_CTEST_LABELS "gpu" MDKR_GPU_LABEL_INDEX)',
            "RESOURCE_LOCK mdkr_native_gpu",
        ):
            if required_lock_fragment not in lock_body:
                failures.append(
                    "native GPU CTest lock sweep is missing "
                    f"{required_lock_fragment!r}"
                )
    if len(lock_calls) != 1:
        failures.append("CMake must invoke the native GPU CTest lock sweep exactly once")
    elif any(label.start() > lock_calls[0] for label in gpu_labels):
        failures.append("a GPU-labelled CTest is registered after the lock sweep")
    elif not gpu_labels:
        failures.append("CMake has no GPU-labelled CTests for the lock sweep to cover")

    required = {
        "native matrix GPU exclusion": (
            sources["correctness"],
            "ctest --test-dir build-ci --output-on-failure -LE gpu",
        ),
        "sanitizer GPU exclusion": (
            sources["correctness"],
            "ctest --test-dir build-sanitize --output-on-failure -LE gpu",
        ),
        "WebGPU callback ThreadSanitizer compile": (
            sources["correctness"],
            "tests/test_webgpu_callback_latch.c",
        ),
        "WebGPU request ThreadSanitizer compile": (
            sources["correctness"],
            "tests/test_webgpu_async_request.c",
        ),
        "WebGPU callback ThreadSanitizer runtime": (
            sources["correctness"],
            "TSAN_OPTIONS=halt_on_error=1",
        ),
        "Windows validation GPU exclusion": (
            sources["windows_validate"],
            "ctest --test-dir build --output-on-failure -LE gpu -E 'audio|mixer'",
        ),
        "Windows release targeted non-GPU sentinel": (
            sources["desktop_release"],
            "ctest --test-dir build --output-on-failure --no-tests=error -R '^memory_allocator$'",
        ),
        "local CI GPU exclusion": (
            sources["ci_local"],
            'ctest --test-dir "$BUILD_DIR" --output-on-failure -LE gpu',
        ),
        "source-archive GPU exclusion": (
            sources["source_archive"],
            'ctest --test-dir "$srcdir/build" --output-on-failure -LE gpu',
        ),
        "packaged app GPU verifier": (
            sources["macos_release"],
            "verify_unsigned_release.sh",
        ),
        "mounted DMG GPU verifier": (
            sources["macos_release"],
            "verify_unsigned_dmg.sh",
        ),
        "packaged Settings panel selection": (
            sources["macos_unsigned_verify"],
            "--env MDKR_APP_PANEL=Settings",
        ),
        "packaged safe frame-limit default proof": (
            sources["macos_unsigned_verify"],
            "Video\\.FrameLimit[[:space:]]+original",
        ),
        "packaged safe gameplay cadence proof": (
            sources["macos_unsigned_verify"],
            "Gameplay\\.SimulationCadence[[:space:]]+original",
        ),
        "packaged safe smoothing proof": (
            sources["macos_unsigned_verify"],
            "Video\\.MotionSmoothing[[:space:]]+off",
        ),
        "packaged visible frame-rate proof": (
            sources["macos_unsigned_verify"],
            "frame-rate-controls visible=1 gameplay-accuracy-separated=1",
        ),
        "packaged Original frame-limit proof": (
            sources["macos_unsigned_verify"],
            "frame-limit value=original",
        ),
        "packaged frame-limit guidance": (
            sources["macos_unsigned_verify"],
            'frame-limit UI contract: recommended="Original (recommended)" group="Higher refresh rates" caveat="Original presents each authored image once.',
        ),
        "schema and smoke frame-limit contract": (
            sources["cmake"],
            "MDKR_FRAME_LIMIT_UI_CONTRACT_REGEX",
        ),
        "visible frame-rate presentation smoke": (
            sources["cmake"],
            'PASS_REGULAR_EXPRESSION "frame-rate-controls visible=1 gameplay-accuracy-separated=1"',
        ),
        "recommended frame-limit label": (
            sources["ui_settings"],
            "Original (recommended)",
        ),
        "modern frame-limit group": (
            sources["ui_settings"],
            "ImGui::SeparatorText(kModernFrameLimitGroup)",
        ),
        "fixed-gameplay frame-limit guidance": (
            sources["ui_settings"],
            "Gameplay speed does not change. Higher rates can use more ",
        ),
        "interpolated image guidance": (
            sources["ui_settings"],
            "images when smoothing is Off, or create in-between images when it is ",
        ),
        "browser uncapped fallback guidance": (
            sources["ui_settings"],
            "browser always maps Uncapped to Match Display.",
        ),
    }
    failures.extend(
        f"missing {label}: {needle!r}"
        for label, (source, needle) in required.items()
        if needle not in active_shell_source(source)
    )
    for value in (
            "display", "30", "60", "90", "120", "144", "165", "240",
            "uncapped"):
        if f'{{"{value}",' not in sources["ui_settings"]:
            failures.append(f"native frame-limit option {value!r} is missing")
    ctest_route = re.search(
        r'if check\.role == "ctest":(?P<body>.*?)\n\s*cmd =',
        sources["run_checks"],
        re.DOTALL,
    )
    if ctest_route is None or '"--output-on-failure"' not in ctest_route.group("body"):
        failures.append("run_checks ROM-free CTest route is missing")
    elif '"-LE"' in ctest_route.group("body") or '"gpu"' in ctest_route.group("body"):
        failures.append("run_checks complete local CTest must include GPU-labelled tests")
    if sources["cmake"].count(
        'PASS_REGULAR_EXPRESSION "${MDKR_FRAME_LIMIT_UI_CONTRACT_REGEX}"'
    ) != 1 or sources["cmake"].count(
        'PASS_REGULAR_EXPRESSION "frame-rate-controls visible=1 gameplay-accuracy-separated=1"'
    ) != 1:
        failures.append(
            "schema must assert every frame-limit label and rendered Settings "
            "must assert visible frame-rate controls separated from gameplay cadence"
        )
    return failures


def validate_macos_release(source: str) -> list[str]:
    active_source = active_shell_source(source)
    required = {
        "manual trigger": "\n  workflow_dispatch:",
        "read-only default permission": "\npermissions:\n  contents: read",
        "protected release environment": "environment: macos-release",
        "serialized release runs": "group: macos-release",
        "self-contained SDL bundle": "--bundle-sdl2",
        "explicit macOS 13 deployment target": "--deployment-target 13.0",
        "strict deployment-target enforcement": "--strict-deployment-target",
        "pinned standalone SDL2 build": "build_release_sdl2.sh",
        "standalone SDL2 pkg-config selection": "export PKG_CONFIG_PATH=",
        "compiled source provenance": '--build-stamp "$GITHUB_SHA"',
        "1.0.4 patch-release default": 'default: "1.0.4"',
        "shared SDL2 release configuration": "release_sdl2_config.sh",
        "exact version release tag": 'EXPECTED_RELEASE_TAG="v${RELEASE_VERSION}"',
        "package tag commit resolution": 'git rev-parse --verify "${RELEASE_TAG}^{commit}"',
        "publish tag commit resolution": "repos/${GITHUB_REPOSITORY}/commits/${RELEASE_TAG}",
        "package tag/source binding": '[[ "$TAG_COMMIT" == "$GITHUB_SHA" ]]',
        "publish tag/source binding": '[[ "$TAG_COMMIT" == "$SOURCE_COMMIT" ]]',
        "explicit unsigned filename": (
            "Golden-Balloon-${RELEASE_VERSION}-macos-arm64-unsigned.dmg"
        ),
        "explicit trusted filename": (
            "Golden-Balloon-${RELEASE_VERSION}-macos-arm64-signed-notarized.dmg"
        ),
        "canonical macOS workflow artifact": (
            "Golden-Balloon-${{ inputs.version }}-macos-arm64-"
        ),
        "unsigned signing provenance": "--signing ad-hoc-unsigned",
        "trusted signing provenance": "--signing developer-id-notarized",
        "explicit workflow-artifact signing identity": "signed-notarized' || 'unsigned",
        "publish signing provenance validation": '"macos_signing": os.environ["EXPECTED_SIGNING"]',
        "publish exports expected signing mode": "export EXPECTED_SIGNING",
        "certificate secret": "MACOS_CERTIFICATE_P12_BASE64",
        "certificate password secret": "MACOS_CERTIFICATE_PASSWORD",
        "Developer ID identity variable": "DEVELOPER_ID_APPLICATION",
        "notary API private key": "APPLE_API_PRIVATE_KEY_BASE64",
        "notary API key ID": "APPLE_API_KEY_ID",
        "notary API issuer": "APPLE_API_ISSUER_ID",
        "temporary keychain cleanup": "security delete-keychain",
        "inside-out app signing": "sign_and_notarize.sh",
        "distribution bundle verification": "--distribution dist/mdkr64.app",
        "DMG packaging": "create_dmg.sh",
        "DMG signing": "codesign --force --sign",
        "DMG notarization": 'notarize_artifact.sh "$DMG_PATH"',
        "DMG integrity": 'hdiutil verify "$DMG_PATH"',
        "asset-free gate": "verify_asset_free.sh dist/mdkr64.app",
        "artifact upload": "actions/upload-artifact@",
        "isolated publish dependency": "needs: package",
        "macOS artifact download": "actions/download-artifact@",
        "publish-only write permission": "\n    permissions:\n      contents: write",
        "publish provenance validation": '"provenance sha256 mismatch"',
        "trusted tagged publication rejected":
            "trusted signing artifacts cannot be published until the documented post-sign LaunchServices/WebGPU runtime gate is automated",
        "publish job excludes trusted artifacts":
            "if: ${{ inputs.release_tag != '' && !inputs.trusted_signing }}",
        "publish script rejects trusted artifacts":
            "trusted macOS publication is disabled until post-sign runtime qualification is automated",
    }
    failures = [
        f"missing macOS release {label}: {needle!r}"
        for label, needle in required.items()
        if needle not in active_source
    ]
    active_required = {
        "basename-only checksum sidecars": (
            'shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256"'
        ),
        "checksum verification beside artifact": (
            'shasum -a 256 -c "$DMG_NAME.sha256"'
        ),
        "mounted unsigned DMG WebGPU command": "./macos/Scripts/verify_unsigned_dmg.sh",
        "unsigned WebGPU release command": "./macos/Scripts/verify_unsigned_release.sh",
    }
    failures.extend(
        f"missing active macOS release {label}: {needle!r}"
        for label, needle in active_required.items()
        if needle not in active_source
    )
    trusted_signing = workflow_input_fields(source, "trusted_signing")
    if trusted_signing.get("type") != "boolean":
        failures.append("trusted_signing input is not structurally typed boolean")
    if trusted_signing.get("default") != "false":
        failures.append("trusted_signing input does not structurally default false")
    # One target belongs to SDL2 and one to mdkr64 itself. This remains
    # deterministic even when the hosted macos-14 runner is Intel.
    if source.count("--arch arm64") < 2:
        failures.append("macOS release does not pin both SDL2 and mdkr64 to arm64")
    if source.count('EXPECTED_RELEASE_TAG="v${RELEASE_VERSION}"') < 2:
        failures.append("both macOS package and publish jobs must bind vVERSION")
    if source.count('[[ "$RELEASE_TAG" == "$EXPECTED_RELEASE_TAG" ]]') < 2:
        failures.append("both macOS package and publish jobs must reject tag drift")
    if source.count("signed-notarized' || 'unsigned") < 2:
        failures.append("macOS upload/download artifact names omit signing identity")
    if active_source.count('shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256"') < 2:
        failures.append("both macOS checksum sidecars must record only the DMG basename")
    if active_source.count('cd "$DMG_DIR"') < 3:
        failures.append("macOS checksum generation/verification is not artifact-local")
    if active_source.count('--repo "$GITHUB_REPOSITORY"') < 2:
        failures.append("macOS publish commands do not name the release repository")
    trusted_order = [
        source.find('notarize_artifact.sh "$DMG_PATH"'),
        source.find("--signing developer-id-notarized"),
    ]
    if any(index < 0 for index in trusted_order) or trusted_order != sorted(
        trusted_order
    ):
        failures.append("trusted macOS provenance can be stamped before notarization")
    for action, revision in PINNED_ACTION_RE.findall(source):
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            failures.append(
                f"macOS release {action} is not pinned to an immutable SHA: "
                f"{revision!r}"
            )
    forbidden = {
        "automatic push trigger": "\n  push:",
        "automatic pull-request trigger": "\n  pull_request:",
        "notarization bypass": "--skip-notarize",
        "Gatekeeper bypass": "--allow-spctl-fail",
        "Homebrew SDL2 release dependency": "brew install sdl2",
        "ambiguous generic macOS DMG filename": (
            "Golden-Balloon-${RELEASE_VERSION}.dmg"
        ),
    }
    failures.extend(
        f"forbidden macOS release {label}: {needle!r}"
        for label, needle in forbidden.items()
        if needle in source
    )
    if "\n  package:" in source and "\n  publish:" in source:
        package_source = source.split("\n  package:", 1)[1].split("\n  publish:", 1)[0]
        if "contents: write" in package_source:
            failures.append(
                "macOS package job combines Apple credentials with contents: write"
            )
    else:
        failures.append("macOS release must isolate package and publish jobs")
    return failures


def validate_macos_packaging(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    builder = sources["builder"]
    signer = sources["signer"]
    notary = sources["notary"]
    dmg = sources["dmg"]
    mount_helper = sources["mount_helper"]
    entitlements = sources["entitlements"]
    sdl_builder = sources["sdl_builder"]
    unsigned_verify = sources["unsigned_verify"]
    unsigned_dmg_verify = sources["unsigned_dmg_verify"]
    launch_probe = sources["launch_probe"]
    app_pacing = sources["app_pacing"]
    bundle_verify = sources["bundle_verify"]
    asset_verify = sources["asset_verify"]
    provenance = sources["provenance"]
    sdl_config = sources["sdl_config"]
    info_plist = sources["info_plist"]
    cmake_project = sources["cmake_project"]
    macos_readme = sources["macos_readme"]
    active_dmg = active_shell_source(dmg)
    active_mount_helper = active_shell_source(mount_helper)
    active_unsigned_dmg_verify = active_shell_source(unsigned_dmg_verify)
    required = {
        "builder 1.0.4 default": (builder, 'APP_VERSION="1.0.4"'),
        "CMake 1.0.4 default": (cmake_project, 'set(MDKR_VERSION "1.0.4"'),
        "Info.plist 1.0.4 default": (info_plist, "<string>1.0.4</string>"),
        "pinned SDL2 version": (sdl_config, 'MDKR_RELEASE_SDL2_VERSION="2.32.10"'),
        "pinned SDL2 source hash": (
            sdl_config,
            'MDKR_RELEASE_SDL2_SOURCE_SHA256="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"',
        ),
        "pinned SDL2 license hash": (
            sdl_config,
            'MDKR_RELEASE_SDL2_LICENSE_SHA256="97f35b302b361680ec1e891e95d2d52097bb95abff361434916d99dc1305f127"',
        ),
        "official SDL2 source": (sdl_config, "https://www.libsdl.org/release/SDL2-"),
        "SDL2 builder sources shared pins": (
            sdl_builder,
            'source "${SCRIPT_DIR}/release_sdl2_config.sh"',
        ),
        "SDL2 verifier sources shared pins": (
            unsigned_verify,
            'source "${SCRIPT_DIR}/release_sdl2_config.sh"',
        ),
        "SDL2 source hash verification": (
            sdl_builder,
            '"${ACTUAL_SHA}" == "${MDKR_RELEASE_SDL2_SOURCE_SHA256}"',
        ),
        "SDL2 license hash verification": (
            sdl_builder,
            '"${ACTUAL_LICENSE_SHA}" == "${MDKR_RELEASE_SDL2_LICENSE_SHA256}"',
        ),
        "SDL2 builder records exact dylib identity": (
            sdl_builder,
            'dylib_sha256=%s',
        ),
        "SDL2 deployment target": (
            sdl_builder,
            '-DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}"',
        ),
        "SDL2 source paths are normalized": (
            sdl_builder,
            "-ffile-prefix-map=${SOURCE_DIR}=SDL2-${MDKR_RELEASE_SDL2_VERSION}",
        ),
        "SDL2 macro paths are normalized": (
            sdl_builder,
            "-fmacro-prefix-map=${SOURCE_DIR}=SDL2-${MDKR_RELEASE_SDL2_VERSION}",
        ),
        "SDL2 debug paths are normalized": (
            sdl_builder,
            "-fdebug-prefix-map=${SOURCE_DIR}=SDL2-${MDKR_RELEASE_SDL2_VERSION}",
        ),
        "SDL2 dylib rejects local build paths": (
            sdl_builder,
            "installed SDL2 contains an absolute developer/build path",
        ),
        "SDL2 system-dependency closure": (
            sdl_builder,
            "installed SDL2 has a non-system runtime dependency",
        ),
        "builder rejects sdl2-compat": (
            builder,
            "pkg-config resolves sdl2 to sdl2-compat",
        ),
        "builder requires WebGPU": (builder, "-DMDKR_WEBGPU_BACKEND=ON"),
        "builder pins compiled provenance": (
            builder,
            '-DMDKR_BUILD_STAMP="${BUILD_STAMP}"',
        ),
        "builder rejects executable source/build paths": (
            builder,
            "Final app executable contains an absolute source/build path.",
        ),
        "builder removes absolute runtime search paths": (
            builder,
            'install_name_tool -delete_rpath "${rpath}" "${ENGINE_PATH}"',
        ),
        "builder selects only absolute runtime search paths": (
            builder,
            '[[ "${rpath}" == /* ]] && ABSOLUTE_RPATHS+=("${rpath}")',
        ),
        "builder strips final payload debug records": (
            builder,
            'strip -S "${ENGINE_PATH}"',
        ),
        "builder rechecks final runtime search paths": (
            builder,
            "Final app executable still contains an absolute runtime search path.",
        ),
        "builder rechecks final SDL2 build path": (
            builder,
            "Final app executable contains the build-time SDL2 path.",
        ),
        "builder carries SDL2 license": (
            builder,
            "Contents/Resources/ThirdParty/SDL2-LICENSE.txt",
        ),
        "builder carries SDL2 manifest": (
            builder,
            "Contents/Resources/ThirdParty/SDL2-MANIFEST.txt",
        ),
        "builder requires controller database": (
            builder,
            "Tracked SDL game-controller mapping database is missing or empty",
        ),
        "builder requires branded release icon": (
            builder,
            "Tracked branded icon source is required for a Release app bundle",
        ),
        "asset guard permits only the SDL2 manifest": (
            asset_verify,
            "Contents/Resources/ThirdParty/SDL2-MANIFEST.txt",
        ),
        "builder verifies SDL2 input manifest": (
            builder,
            '"${SDL2_ACTUAL_DYLIB_SHA256}" == "${SDL2_INPUT_DYLIB_SHA256}"',
        ),
        "builder seals final SDL2 hash provenance": (
            builder,
            'bundled_dylib_sha256=%s',
        ),
        "builder canonical output guard": (builder, "canonical_output_app_path"),
        "builder safe validation mode": (builder, "--validate-output-only"),
        "builder rejects broad output targets": (builder, "refusing broad target"),
        "builder requires exact output basename": (
            builder,
            "target basename must be mdkr64.app",
        ),
        "builder identifies an existing output bundle": (
            builder,
            "existing target belongs to another application",
        ),
        "builder rejects build-tree ancestors": (
            builder,
            "target must not equal or contain the CMake build directory",
        ),
        "builder clears inherited xattrs": (builder, 'xattr -cr "${OUTPUT_APP}"'),
        "builder signs nested SDL": (
            builder,
            'codesign --force --sign - "${SDL2_BUNDLED_PATH}"',
        ),
        "builder seals outer app": (
            builder,
            'codesign --force --sign - "${OUTPUT_APP}"',
        ),
        "builder verifies final bundle": (builder, "verify_gatekeeper_bundle.sh"),
        "signer enumerates nested dylibs": (
            signer,
            "find \"${FRAMEWORKS_DIR}\" -depth -type f -name '*.dylib'",
        ),
        "signer requires Developer ID certificate type": (
            signer,
            '"Developer ID Application: "*',
        ),
        "signer enables Hardened Runtime": (signer, "--options runtime --timestamp"),
        "signer refreshes post-sign SDL2 hash": (
            signer,
            'print "bundled_dylib_sha256=" hash',
        ),
        "signer delegates notarization": (
            signer,
            'notarize_artifact.sh" "${APP_PATH}"',
        ),
        "notary requires exact acceptance": (notary, '"${STATUS}" != "Accepted"'),
        "notary staples": (notary, 'xcrun stapler staple "${TARGET_PATH}"'),
        "notary validates staple": (notary, 'xcrun stapler validate "${TARGET_PATH}"'),
        "DMG verifies checksum": (dmg, 'hdiutil verify "${OUTPUT_DMG}"'),
        "DMG mounts read-only": (
            active_dmg,
            "hdiutil attach -readonly -nobrowse",
        ),
        "DMG uses deterministic system layout": (
            dmg,
            "Creating DMG with the system hdiutil",
        ),
        "DMG canonical output guard": (dmg, "canonical_dmg_output"),
        "DMG canonical source-app guard": (dmg, "canonical_source_app"),
        "DMG rejects source-app symlinks": (
            dmg,
            "the source .app itself must not be a symbolic link",
        ),
        "DMG safe validation mode": (dmg, "--validate-output-only"),
        "DMG refuses unsafe targets": (
            dmg,
            "Refusing unsafe DMG output target",
        ),
        "DMG re-verifies packaged app": (
            dmg,
            'verify_gatekeeper_bundle.sh" "${PACKAGED_APP}"',
        ),
        "DMG packager shares mount-state cleanup": (
            active_dmg,
            'source "${SCRIPT_DIR}/dmg_mount_cleanup.sh"',
        ),
        "DMG cleanup derives current filesystem mount state": (
            active_mount_helper,
            'mount_device="$(/usr/bin/stat -f \'%d\' "${mount_dir}"',
        ),
        "DMG cleanup compares mount and parent devices": (
            active_mount_helper,
            '[[ "${mount_device}" != "${parent_device}" ]]',
        ),
        "DMG cleanup bounds detach retries": (
            active_mount_helper,
            "for attempt in 1 2 3",
        ),
        "DMG cleanup force-detaches only its exact mount": (
            active_mount_helper,
            'hdiutil detach -force "${mount_dir}"',
        ),
        "DMG cleanup confirms detach from current mount state": (
            active_mount_helper,
            '! mdkr_dmg_mount_is_active "${mount_dir}"',
        ),
        "DMG cleanup never removes a live mount": (
            active_mount_helper,
            'mdkr_dmg_mount_is_active "${mount_dir}" && return 1',
        ),
        "DMG cleanup narrowly removes detached mount directory": (
            active_mount_helper,
            'rmdir "${mount_dir}"',
        ),
        "DMG packager handles termination signals": (
            active_dmg,
            "trap 'exit 143' TERM",
        ),
        "SDL2 builder canonical path guard": (sdl_builder, "canonical_sdl_paths"),
        "SDL2 builder path-only validation mode": (
            sdl_builder,
            "--validate-paths-only",
        ),
        "SDL2 builder requires versioned work directory": (
            sdl_builder,
            "work directory basename must be sdl2-",
        ),
        "SDL2 builder requires exact private install prefix": (
            sdl_builder,
            "prefix must be the work directory's exact install child",
        ),
        "local macOS recipe fails on shell errors": (
            macos_readme,
            "set -euo pipefail",
        ),
        "local macOS recipe rejects untracked source": (
            macos_readme,
            "git status --porcelain=v1 --untracked-files=all",
        ),
        "bundle verifier rejects sdl2-compat": (
            bundle_verify,
            "bundled SDL2 is the SDL3-loading sdl2-compat shim",
        ),
        "bundle verifier checks nested load paths": (
            bundle_verify,
            'verify_mach_o_contract "${dylib}"',
        ),
        "bundle verifier enforces declared minimum": (
            bundle_verify,
            "newer than declared",
        ),
        "unsigned verifier requires exact arm64": (
            unsigned_verify,
            "--expected-arch arm64",
        ),
        "unsigned verifier requires exact macOS 13 target": (
            unsigned_verify,
            "--expected-min-os 13.0",
        ),
        "bundle verifier enforces exact architecture": (
            bundle_verify,
            "expected exactly ${EXPECTED_ARCH}",
        ),
        "bundle verifier enforces exact minimum OS": (
            bundle_verify,
            "expected ${EXPECTED_MINOS}",
        ),
        "unsigned verifier checks short version": (
            unsigned_verify,
            "CFBundleShortVersionString",
        ),
        "unsigned verifier checks build version": (
            unsigned_verify,
            "CFBundleVersion",
        ),
        "unsigned verifier requires ad-hoc seal": (unsigned_verify, "Signature=adhoc"),
        "unsigned verifier checks nested SDL ad-hoc seal": (
            unsigned_verify,
            "printf '%s\\n' \"${SDL2_SIGNATURE_DETAILS}\" | grep -Fq 'Signature=adhoc'",
        ),
        "unsigned DMG verifier checks Applications link": (
            unsigned_dmg_verify,
            '[[ "$(readlink "${APPLICATIONS_LINK}")" == "/Applications" ]]',
        ),
        "unsigned DMG verifier rejects extra root payload": (
            unsigned_dmg_verify,
            "mounted DMG contains unexpected root entry",
        ),
        "unsigned verifier requires WebGPU default": (
            unsigned_verify,
            "launcher did not select WebGPU by default",
        ),
        "unsigned verifier uses bounded LaunchServices controller": (
            unsigned_verify,
            '"${SCRIPT_DIR}/run_launchservices_probe.py"',
        ),
        "LaunchServices controller uses Finder boundary": (
            launch_probe,
            '"/usr/bin/open", "-n", "-F", "-W"',
        ),
        "LaunchServices controller has a monotonic deadline": (
            launch_probe,
            "deadline = time.monotonic() + args.timeout",
        ),
        "LaunchServices controller matches exact executable identity": (
            launch_probe,
            "if command == expected",
        ),
        "LaunchServices controller handles termination signals": (
            launch_probe,
            "signal.SIGINT, signal.SIGTERM, signal.SIGHUP",
        ),
        "LaunchServices controller attempts graceful app termination": (
            launch_probe,
            "signal_exact_processes(pids, executable, signal.SIGTERM)",
        ),
        "LaunchServices controller escalates hung app termination": (
            launch_probe,
            "signal_exact_processes(remaining, executable, signal.SIGKILL)",
        ),
        "LaunchServices controller rejects cleanup survivors": (
            launch_probe,
            "launched app process survived cleanup",
        ),
        "LaunchServices controller rejects app surviving open": (
            launch_probe,
            "unexpected_survivors = True",
        ),
        "FPS overlay gate shares the LaunchServices controller": (
            app_pacing,
            "str(LAUNCHSERVICES_PROBE)",
        ),
        "unsigned verifier requires real surface present": (
            unsigned_verify,
            "launcher smoke did not prove a WebGPU surface present",
        ),
        "unsigned verifier requires clean AppHost telemetry": (
            unsigned_verify,
            "launcher shutdown telemetry reported a rendering or capture failure",
        ),
        "unsigned verifier requires one telemetry row": (
            unsigned_verify,
            '"${TELEMETRY_COUNT}" == "1"',
        ),
        "unsigned verifier fully parses telemetry": (
            unsigned_verify,
            "TELEMETRY_RE=",
        ),
        "unsigned verifier accounts every attempt": (
            unsigned_verify,
            "PRESENT_ATTEMPTS == PRESENTED_FRAMES + UNAVAILABLE_FRAMES",
        ),
        "unsigned verifier requires four actual presents": (
            unsigned_verify,
            "PRESENTED_FRAMES >= 4",
        ),
        "unsigned verifier cross-checks present telemetry": (
            unsigned_verify,
            "SMOKE_PRESENTED_FRAMES == PRESENTED_FRAMES",
        ),
        "unsigned verifier bounds unavailable frames": (
            unsigned_verify,
            "UNAVAILABLE_FRAMES <= 64",
        ),
        "unsigned verifier requires successful last status": (
            unsigned_verify,
            '"${LAST_SURFACE_STATUS}" == "1" || "${LAST_SURFACE_STATUS}" == "2"',
        ),
        "unsigned verifier detects locked console": (
            unsigned_verify,
            "CGSSessionScreenIsLocked",
        ),
        "unsigned verifier rejects SDL3/Homebrew loads": (
            unsigned_verify,
            "launcher loaded a Homebrew or SDL3 runtime dependency",
        ),
        "unsigned verifier checks SDL2 license": (
            unsigned_verify,
            "bundled SDL2 license does not match",
        ),
        "unsigned verifier checks one final SDL2 dylib": (
            unsigned_verify,
            '"${#SDL2_DYLIBS[@]}" -eq 1',
        ),
        "unsigned verifier checks final SDL2 hash": (
            unsigned_verify,
            '"${SDL2_ACTUAL_SHA256}" == "${SDL2_BUNDLED_SHA256}"',
        ),
        "unsigned verifier requires controller database": (
            unsigned_verify,
            "bundled game-controller mapping database is missing or empty",
        ),
        "unsigned verifier requires branded app icon": (
            unsigned_verify,
            "bundled branded app icon is missing or empty",
        ),
        "unsigned DMG verifier mounts read-only": (
            active_unsigned_dmg_verify,
            "hdiutil attach -readonly -nobrowse",
        ),
        "unsigned DMG verifier requires one app": (
            unsigned_dmg_verify,
            '[[ "${#APPS[@]}" -eq 1 ]]',
        ),
        "unsigned DMG verifier requires physical app": (
            unsigned_dmg_verify,
            '[[ -d "${APPS[0]}" && ! -L "${APPS[0]}" ]]',
        ),
        "unsigned DMG verifier runs full mounted-app qualification": (
            active_unsigned_dmg_verify,
            '"${SCRIPT_DIR}/verify_unsigned_release.sh"',
        ),
        "unsigned DMG verifier shares mount-state cleanup": (
            active_unsigned_dmg_verify,
            'source "${SCRIPT_DIR}/dmg_mount_cleanup.sh"',
        ),
        "unsigned DMG verifier handles termination signals": (
            active_unsigned_dmg_verify,
            "trap 'exit 143' TERM",
        ),
        "macOS provenance delegates common stamp": (
            provenance,
            "tools/release/stamp_provenance.sh",
        ),
        "macOS provenance restricts signing modes": (
            provenance,
            "ad-hoc-unsigned|developer-id-notarized",
        ),
        "macOS provenance records signing mode": (
            provenance,
            'payload["macos_signing"] = sys.argv[2]',
        ),
    }
    failures.extend(
        f"missing macOS packaging {label}: {needle!r}"
        for label, (source, needle) in required.items()
        if needle not in active_shell_source(source)
    )
    ordered = [
        builder.find("install_name_tool -change"),
        builder.find("install_name_tool -delete_rpath"),
        builder.find('strip -S "${ENGINE_PATH}"'),
        builder.find(
            "Final app executable still contains an absolute runtime search path."
        ),
        builder.find(
            "Final app executable contains an absolute source/build path."
        ),
        builder.find("Final app executable contains the build-time SDL2 path."),
        builder.find('codesign --force --sign - "${SDL2_BUNDLED_PATH}"'),
        builder.find('codesign --force --sign - "${OUTPUT_APP}"'),
        builder.find("verify_gatekeeper_bundle.sh"),
    ]
    if any(index < 0 for index in ordered) or ordered != sorted(ordered):
        failures.append(
            "macOS builder no longer rewrites, removes absolute rpaths, strips, "
            "scans the final payload, signs inside-out, and verifies in that order"
        )
    output_guard = builder.find('SAFE_OUTPUT_APP="$(canonical_output_app_path')
    destructive_replace = builder.find('rm -rf "${OUTPUT_APP}"')
    if (
        output_guard < 0
        or destructive_replace < 0
        or output_guard > destructive_replace
    ):
        failures.append("macOS builder does not validate --output before rm -rf")
    dmg_output_guard = dmg.find('SAFE_OUTPUT_DMG="$(canonical_dmg_output')
    dmg_destructive_replace = dmg.find('rm -f -- "${OUTPUT_DMG}"')
    if (
        dmg_output_guard < 0
        or dmg_destructive_replace < 0
        or dmg_output_guard > dmg_destructive_replace
    ):
        failures.append("macOS DMG packager does not validate output before rm -f")
    if "command -v create-dmg" in dmg or re.search(r"^\s*create-dmg\b", dmg, re.MULTILINE):
        failures.append("macOS DMG packager can execute unpinned create-dmg from PATH")
    for entitlement in (
        "com.apple.security.cs.disable-library-validation",
        "com.apple.security.cs.allow-unsigned-executable-memory",
    ):
        if entitlement in entitlements:
            failures.append(f"forbidden high-risk macOS entitlement: {entitlement}")
    return failures


def validate_desktop_release(sources: dict[str, str]) -> list[str]:
    """Pin portable artifacts, Linux GPU qualification, and Windows CI limits."""
    workflow = sources["workflow"]
    windows_packager = sources["windows_packager"]
    windows_import_guard = sources["windows_import_guard"]
    linux_packager = sources["linux_packager"]
    provenance_stamp = sources["provenance_stamp"]
    provenance_verify = sources["provenance_verify"]
    required = {
        "manual-only trigger": (workflow, "\n  workflow_dispatch:"),
        "read-only permissions": (workflow, "\npermissions:\n  contents: read"),
        "central version validation job": (workflow, "\n  validate:"),
        "full-history tag validation checkout": (workflow, "fetch-depth: 0"),
        "validated version output": (
            workflow,
            "version: ${{ steps.version.outputs.version }}",
        ),
        "bare-semver validation": (
            workflow,
            "version must be dev or bare semver",
        ),
        "portable release tag input": (workflow, "      release_tag:"),
        "dev artifacts reject a release tag": (
            workflow,
            "release_tag must be empty when version=dev",
        ),
        "portable exact vVERSION tag": (
            workflow,
            'EXPECTED_RELEASE_TAG="v${RELEASE_VERSION}"',
        ),
        "portable tag commit resolution": (
            workflow,
            'git rev-parse --verify "${RELEASE_TAG}^{commit}"',
        ),
        "portable tag/source binding": (
            workflow,
            '[[ "$TAG_COMMIT" == "$GITHUB_SHA" ]]',
        ),
        "Windows import guard": (
            workflow,
            "./tools/check_windows_imports.sh build/mdkr64.exe",
        ),
        "Windows asset-free guard": (
            workflow,
            "./macos/Scripts/verify_asset_free.sh build/mdkr64.exe",
        ),
        "Windows embedded manifest identity": (
            workflow,
            "embedded manifest identity",
        ),
        "Windows UTF-8 and long-path manifest": (
            workflow,
            "embedded manifest Windows settings: UTF-8 + long paths",
        ),
        "Windows manifest version normalization": (
            workflow,
            "manifest_version=\"${manifest_major}.${manifest_minor}.${manifest_patch}.0\"",
        ),
        "Windows ROM-free tests": (
            workflow,
            "ctest --test-dir build --output-on-failure",
        ),
        "Windows test build": (workflow, "-DBUILD_TESTING=ON"),
        "Windows MinGW portability target": (
            workflow,
            "--target mdkr64 mdkr_memory_allocator_test",
        ),
        "Windows MinGW portability execution": (
            workflow,
            "--no-tests=error -R '^memory_allocator$'",
        ),
        "Linux asset-free guard": (
            workflow,
            "./macos/Scripts/verify_asset_free.sh build/mdkr64",
        ),
        "Linux release test build": (workflow, "-DBUILD_TESTING=ON"),
        "deterministic Linux software GPU stack": (
            workflow,
            "xvfb xauth mesa-vulkan-drivers",
        ),
        "exact lavapipe ICD": (
            workflow,
            "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
        ),
        "Ubuntu 22.04 lavapipe loader compatibility": (
            workflow,
            "VK_ICD_FILENAMES: /usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
        ),
        "software OpenGL selection": (workflow, 'LIBGL_ALWAYS_SOFTWARE: "1"'),
        "Linux GPU-free CTest routing": (
            workflow,
            "ctest --test-dir build --output-on-failure -LE gpu",
        ),
        "built default WebGPU launcher proof": (
            workflow,
            "qualify_launcher webgpu \"\"",
        ),
        "default renderer environment isolation": (
            workflow,
            "local -a backend=(-u MDKR_RENDERER)",
        ),
        "built explicit GL launcher proof": (
            workflow,
            "qualify_launcher gl gl",
        ),
        "launcher WebGPU backend assertion": (
            workflow,
            "grep -Fq '[app] host: WebGPU'",
        ),
        "launcher GL backend assertion": (
            workflow,
            "grep -Fq '[app] host: OpenGL'",
        ),
        "launcher present assertion": (
            workflow,
            "MDKR_APP_REQUIRE_PRESENT=1",
        ),
        "launcher pixel content validation": (
            workflow,
            'python3 tests/check_app_capture.py "$work/launcher.bmp" --self-test',
        ),
        "Linux extracted AppRun": (
            workflow,
            'app_run="$extract/Golden-Balloon.AppDir/AppRun"',
        ),
        "Linux unrelated launch directory": (
            workflow,
            'cd "$unrelated"',
        ),
        "Linux bundled SDL resolution": (
            workflow,
            'LD_LIBRARY_PATH="$lib_dir" ldd "$binary"',
        ),
        "packaged default WebGPU launcher proof": (
            workflow,
            "qualify_package webgpu \"\"",
        ),
        "packaged explicit GL launcher proof": (
            workflow,
            "qualify_package gl gl",
        ),
        "Windows package script": (
            workflow,
            "./tools/package_windows_zip.sh",
        ),
        "Linux package script": (
            workflow,
            "./tools/package_linux_appimage.sh",
        ),
        "Windows archive inspection dependency": (
            workflow,
            "mingw-w64-x86_64-python zip unzip",
        ),
        "Windows declared Python probe": (workflow, "python3 --version"),
        "Windows extracted package startup": (
            workflow,
            'executable="$package_root/GoldenBalloon/GoldenBalloon.exe"',
        ),
        "Windows automatic-publication policy": (
            workflow,
            "Hosted Windows artifact not uploaded automatically; only a manually accepted exact archive may be attached.",
        ),
        "canonical Linux provenance input": (
            workflow,
            "dist/Golden-Balloon-*-linux-x86_64.*",
        ),
        "canonical Linux workflow artifact": (
            workflow,
            "name: Golden-Balloon-${{ needs.validate.outputs.version }}-linux-x86_64",
        ),
        "canonical Linux upload path": (
            workflow,
            "path: dist/Golden-Balloon-${{ needs.validate.outputs.version }}-linux-x86_64.*",
        ),
        "hosted provenance rejects dirty source": (
            provenance_stamp,
            "hosted release provenance requires a clean tracked source tree",
        ),
        "provenance records source dirtiness": (
            provenance_stamp,
            '"source_dirty": $source_dirty',
        ),
        "release verifier rejects dirty provenance": (
            provenance_verify,
            'rec.get("source_dirty") is not False',
        ),
        "renamed Windows executable": (
            windows_packager,
            '"$stage/GoldenBalloon.exe"',
        ),
        "canonical Windows zip filename": (
            windows_packager,
            'archive="$dist/Golden-Balloon-$version-windows-x64.zip"',
        ),
        "Windows exact archive verifier": (
            windows_packager,
            'verify_windows_archive "$archive"',
        ),
        "Windows exact archive manifest": (
            windows_packager,
            "Windows archive payload differs from the exact release manifest.",
        ),
        "Windows archive executable entry": (
            windows_packager,
            "GoldenBalloon/GoldenBalloon.exe",
        ),
        "Windows archive controller database entry": (
            windows_packager,
            "GoldenBalloon/gamecontrollerdb.txt",
        ),
        "Windows archive rejects DLLs": (
            windows_packager,
            "Windows archive contains a DLL; SDL2 must remain statically linked.",
        ),
        "Windows archive verifier self-test": (
            windows_packager,
            "Windows archive verifier accepted the SDL2.dll control.",
        ),
        "packager rechecks Windows imports": (
            windows_packager,
            './tools/check_windows_imports.sh "$binary"',
        ),
        "Windows scans final staging tree for ROM data": (
            windows_packager,
            './tools/check_no_rom.sh "$stage"',
        ),
        "Windows package cleanup trap": (
            windows_packager,
            "trap cleanup EXIT",
        ),
        "Windows import positive controls": (
            windows_import_guard,
            "--self-test",
        ),
        "Windows system-DLL allowlist": (
            windows_import_guard,
            "KERNEL32|USER32",
        ),
        "Windows SDL broken control": (
            windows_import_guard,
            "SDL2.dll",
        ),
        "Linux version validation": (
            linux_packager,
            "version must be dev or bare semver",
        ),
        "Linux package cleanup trap": (linux_packager, "trap cleanup EXIT"),
        "canonical Linux staging root": (
            linux_packager,
            'appdir="$work/Golden-Balloon.AppDir"',
        ),
        "canonical Linux artifact prefix": (
            linux_packager,
            'artifact_prefix="Golden-Balloon-$version-linux-x86_64"',
        ),
        "Linux exact tarball verifier": (
            linux_packager,
            'verify_linux_tarball "$tarball" "$appdir"',
        ),
        "Linux exact archive manifest": (
            linux_packager,
            "Linux tarball payload differs from the exact release manifest.",
        ),
        "Linux archive executable entry": (
            linux_packager,
            "Golden-Balloon.AppDir/usr/bin/mdkr64",
        ),
        "Linux archive controller database entry": (
            linux_packager,
            "Golden-Balloon.AppDir/usr/bin/gamecontrollerdb.txt",
        ),
        "Linux archive exact SDL2 count": (
            linux_packager,
            "Linux release tarball must contain exactly one SDL2 runtime.",
        ),
        "Linux archive verifier self-test": (
            linux_packager,
            "Linux tarball verifier accepted an unexpected archive entry.",
        ),
        "Linux pinned appimagetool version": (
            linux_packager, 'APPIMAGETOOL_VERSION="1.9.1"'),
        "Linux pinned appimagetool hash": (
            linux_packager,
            'APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"',
        ),
        "Linux verifies appimagetool before execution": (
            linux_packager, '[[ "$got" != "$APPIMAGETOOL_SHA256" ]]'),
        "Linux requires controller database": (
            linux_packager,
            "tracked controller mapping database is missing from release package",
        ),
        "Linux requires license and readme": (
            linux_packager,
            "tracked LICENSE and README are required in the release package",
        ),
        "Linux requires branded release icon": (
            linux_packager,
            "tracked branded icon source is required in the release package",
        ),
        "Linux scans final staging tree for ROM data": (
            linux_packager,
            './tools/check_no_rom.sh "$appdir"',
        ),
        "provenance exact Windows filename family": (
            provenance_stamp,
            'windows_zip="Golden-Balloon-${version}-windows-x64.zip"',
        ),
        "provenance exact Linux filename family": (
            provenance_stamp,
            'linux_appimage="Golden-Balloon-${version}-linux-x86_64.AppImage"',
        ),
        "provenance exact macOS filename family": (
            provenance_stamp,
            'macos_unsigned="Golden-Balloon-${version}-macos-arm64-unsigned.dmg"',
        ),
        "provenance verifier canonical enumeration": (
            provenance_verify,
            '"$dist"/Golden-Balloon-"$version"-*',
        ),
        "provenance verifier exact family gate": (
            provenance_verify,
            "unsupported release artifact filename",
        ),
    }
    failures = [
        f"missing desktop release {label}: {needle!r}"
        for label, (source, needle) in required.items()
        if needle not in active_shell_source(source)
    ]
    windows_manifest = re.search(
        r"expected=\"\$\(printf '%s\\n'(?P<body>.*?)\| LC_ALL=C sort\)\"",
        windows_packager,
        re.DOTALL,
    )
    expected_windows_entries = {
        "GoldenBalloon/",
        "GoldenBalloon/GoldenBalloon.exe",
        "GoldenBalloon/LICENSE",
        "GoldenBalloon/README.md",
        "GoldenBalloon/RUN_ME.txt",
        "GoldenBalloon/gamecontrollerdb.txt",
    }
    actual_windows_entries = (
        set(
            re.findall(
                r"^\s+(GoldenBalloon/\S*)",
                windows_manifest.group("body"),
                re.MULTILINE,
            )
        )
        if windows_manifest is not None
        else set()
    )
    if actual_windows_entries != expected_windows_entries:
        failures.append(
            "Windows archive verifier no longer pins the exact portable manifest"
        )
    linux_manifest = re.search(
        r"expected=\"\$\((?P<body>.*?)\n\s*\)\"",
        linux_packager,
        re.DOTALL,
    )
    expected_linux_entries = {
        "Golden-Balloon.AppDir/AppRun",
        "Golden-Balloon.AppDir/LICENSE",
        "Golden-Balloon.AppDir/README.md",
        "Golden-Balloon.AppDir/RUN_ME.txt",
        "Golden-Balloon.AppDir/mdkr64.desktop",
        "Golden-Balloon.AppDir/mdkr64.png",
        "Golden-Balloon.AppDir/usr/bin/gamecontrollerdb.txt",
        "Golden-Balloon.AppDir/usr/bin/mdkr64",
    }
    actual_linux_entries = (
        set(
            re.findall(
                r"^\s+(Golden-Balloon\.AppDir/\S+)",
                linux_manifest.group("body"),
                re.MULTILINE,
            )
        )
        if linux_manifest is not None
        else set()
    )
    if actual_linux_entries != expected_linux_entries:
        failures.append(
            "Linux archive verifier no longer pins the exact portable manifest"
        )
    if workflow.count("needs: validate") < 2:
        failures.append("both portable release jobs must depend on input validation")
    if workflow.count('-DMDKR_VERSION="$RELEASE_VERSION"') < 2:
        failures.append("both portable release binaries must compile the requested version")
    if workflow.count('test "$version_output" = "mdkr64 ${{ needs.validate.outputs.version }}"') < 2:
        failures.append("both portable release binaries must assert their exact version")
    if workflow.count("needs.validate.outputs.version") < 8:
        failures.append("portable artifact naming/stamping is not bound to validated version")
    if workflow.count(
        'python3 tests/check_app_capture.py "$work/launcher.bmp" --self-test'
    ) != 2:
        failures.append(
            "Linux release must content-validate built and packaged launcher pixels"
        )
    if workflow.count("MDKR_APP_REQUIRE_PRESENT=1") != 2:
        failures.append(
            "Linux release must require real presents from built and packaged launchers"
        )
    if workflow.count("local -a backend=(-u MDKR_RENDERER)") != 2:
        failures.append(
            "built and packaged default launcher gates must clear MDKR_RENDERER"
        )
    linux_order = (
        "Verify built executable identity and ROM-free surfaces",
        "Qualify built AppHost pixels",
        "Package AppImage + tar.gz",
        "Qualify extracted tar/AppRun",
        "Stamp provenance",
        "uses: actions/upload-artifact@",
    )
    linux_indexes = [workflow.find(marker) for marker in linux_order]
    if any(index < 0 for index in linux_indexes) or linux_indexes != sorted(linux_indexes):
        failures.append(
            "Linux release no longer qualifies built pixels, packages, qualifies "
            "the extracted AppRun, stamps, then uploads in that order"
        )
    windows_job = workflow.split("\n  windows:", 1)[-1]
    if "uses: actions/upload-artifact@" in active_shell_source(windows_job):
        failures.append(
            "Windows publication must remain held until a stable GPU runner "
            "content-validates built and extracted WebGPU/GL launchers"
        )
    if re.search(r"\bcp\b[^\n]*\.dll\b", windows_packager, re.IGNORECASE):
        failures.append("Windows packager copies a DLL into the portable package")
    if "command -v appimagetool" in linux_packager:
        failures.append("Linux packager can execute an unverified appimagetool from PATH")
    if '"$stage/mdkr64.exe"' in windows_packager:
        failures.append("Windows packager still emits the internal executable name")
    for label, source in (
        ("workflow", workflow),
        ("Windows packager", windows_packager),
        ("Linux packager", linux_packager),
        ("provenance stamp", provenance_stamp),
        ("provenance verifier", provenance_verify),
    ):
        for legacy in ("mdkr64-windows-", "mdkr64-linux-"):
            if legacy in source:
                failures.append(
                    f"{label} still exposes legacy portable artifact family: {legacy!r}"
                )
    actions = PINNED_ACTION_RE.findall(workflow)
    if not actions:
        failures.append("desktop release workflow has no actions")
    for action, revision in actions:
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            failures.append(
                f"desktop release {action} is not pinned to an immutable SHA: "
                f"{revision!r}"
            )
    for label, needle in {
        "automatic push trigger": "\n  push:",
        "automatic pull-request trigger": "\n  pull_request:",
        "repository write permission": "contents: write",
    }.items():
        if needle in workflow:
            failures.append(f"forbidden desktop release {label}: {needle!r}")
    return failures


def validate_windows_nonpublishing(source: str) -> list[str]:
    """Keep hosted Windows useful without mistaking it for a GPU release gate."""
    required = {
        "Git for public-surface fixtures": (
            "mingw-w64-x86_64-ninja zip unzip python git"
        ),
        "native Windows execution": "./build/mdkr64.exe --help",
        "GPU-free CTest routing": (
            "ctest --test-dir build --output-on-failure -LE gpu -E 'audio|mixer'"
        ),
        "exact portable packaging": "./tools/package_windows_zip.sh",
        "archive extraction": (
            "unzip -q dist/Golden-Balloon-dev-windows-x64.zip"
        ),
        "extracted executable": (
            'executable="$package_root/GoldenBalloon/GoldenBalloon.exe"'
        ),
        "unrelated launch directory": 'cd "$unrelated"',
        "publication hold": (
            "Windows artifact not uploaded: hosted GPU launcher qualification is unavailable."
        ),
    }
    active = active_shell_source(source)
    failures = [
        f"Windows validation missing {label}: {needle!r}"
        for label, needle in required.items()
        if needle not in active
    ]
    if "uses: actions/upload-artifact@" in active:
        failures.append(
            "Windows validation publishes an artifact without a stable GPU launcher gate"
        )
    return failures


def validate_web_demo(source: str) -> list[str]:
    """The independently publishable Pages path must gate its exact payload."""
    required = {
        "linked browser build": "tools/web/build_web.sh",
        "linked wave-table validation": (
            "python3 tests/check_wave_visible_table.py "
            "--wasm dist/web/mdkr64_web.wasm"
        ),
        "ROM absence guard": "tools/check_no_rom.sh dist/web",
        "Pages artifact upload": "uses: actions/upload-pages-artifact@",
    }
    failures = [
        f"web-demo missing {label}: {needle!r}"
        for label, needle in required.items()
        if needle not in source
    ]
    ordered = [source.find(needle) for needle in required.values()]
    if any(index < 0 for index in ordered) or ordered != sorted(ordered):
        failures.append(
            "web-demo no longer builds, validates linked layout, ROM-scans, "
            "and uploads the exact artifact in that order"
        )
    return failures


def validate_output_guard(builder: Path) -> list[str]:
    """Exercise output validation through the guaranteed no-write mode."""
    cases = (
        ("filesystem root", ["--output", "/"]),
        ("user home", ["--output", str(Path.home())]),
        ("repository root", ["--output", str(ROOT)]),
        ("non-app target", ["--output", str(ROOT / "dist" / "mdkr64")]),
        ("root-level empty app name", ["--output", "/.app"]),
        (
            "output containing build directory",
            [
                "--build-dir",
                str(ROOT / "contract-output.app" / "build"),
                "--output",
                str(ROOT / "contract-output.app"),
            ],
        ),
    )
    failures: list[str] = []
    for label, arguments in cases:
        result = subprocess.run(
            [str(builder), "--validate-output-only", *arguments],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if not is_clean_rejection(result, "Refusing unsafe --output target"):
            failures.append(f"unsafe macOS output unexpectedly accepted: {label}")

    with tempfile.TemporaryDirectory(prefix="mdkr-output-contract.") as temp_name:
        temp_root = Path(temp_name)
        safe_output = temp_root / "mdkr64.app"
        result = subprocess.run(
            [
                str(builder),
                "--validate-output-only",
                "--output",
                str(safe_output),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or "Safe app output:" not in result.stdout:
            failures.append("narrow macOS .app output failed validation-only control")
        if safe_output.exists():
            failures.append("macOS output validation-only mode wrote to the filesystem")

        unrelated = temp_root / "unrelated" / "mdkr64.app"
        unrelated_plist = unrelated / "Contents" / "Info.plist"
        unrelated_plist.parent.mkdir(parents=True)
        with unrelated_plist.open("wb") as stream:
            plistlib.dump(
                {
                    "CFBundleIdentifier": "org.example.unrelated",
                    "CFBundleExecutable": "other-app",
                },
                stream,
            )
        sentinel = unrelated / "must-survive.txt"
        sentinel.write_text("unrelated app remains intact\n", encoding="utf-8")
        result = subprocess.run(
            [
                str(builder),
                "--validate-output-only",
                "--output",
                str(unrelated),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if not is_clean_rejection(result, "Refusing unsafe --output target"):
            failures.append("existing unrelated .app output unexpectedly accepted")
        if sentinel.read_text(encoding="utf-8") != "unrelated app remains intact\n":
            failures.append("unrelated existing .app was changed during validation")

        correct = temp_root / "identified" / "mdkr64.app"
        correct_plist = correct / "Contents" / "Info.plist"
        correct_plist.parent.mkdir(parents=True)
        with correct_plist.open("wb") as stream:
            plistlib.dump(
                {
                    "CFBundleIdentifier": "com.mdkr64.app",
                    "CFBundleExecutable": "mdkr64",
                },
                stream,
            )
        correct_sentinel = correct / "must-survive.txt"
        correct_sentinel.write_text("identified app remains intact\n", encoding="utf-8")
        result = subprocess.run(
            [
                str(builder),
                "--validate-output-only",
                "--output",
                str(correct),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or "Safe app output:" not in result.stdout:
            failures.append("identified existing mdkr64.app failed output validation")
        if correct_sentinel.read_text(encoding="utf-8") != (
            "identified app remains intact\n"
        ):
            failures.append("identified existing mdkr64.app changed during validation")

        symlink_target = temp_root / "symlink-target"
        symlink_target.mkdir()
        symlink_sentinel = symlink_target / "must-survive.txt"
        symlink_sentinel.write_text("symlink target remains intact\n", encoding="utf-8")
        symlink_output = temp_root / "symlink" / "mdkr64.app"
        symlink_output.parent.mkdir()
        symlink_output.symlink_to(symlink_target, target_is_directory=True)
        result = subprocess.run(
            [
                str(builder),
                "--validate-output-only",
                "--output",
                str(symlink_output),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if not is_clean_rejection(result, "Refusing unsafe --output target"):
            failures.append("symlinked .app output unexpectedly accepted")
        if symlink_sentinel.read_text(encoding="utf-8") != (
            "symlink target remains intact\n"
        ):
            failures.append("symlink output target changed during validation")
    return failures


def validate_dmg_output_guard(packager: Path) -> list[str]:
    """Exercise DMG output validation without creating or replacing an image."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-dmg-contract.") as temp_name:
        temp_root = Path(temp_name)
        app = temp_root / "mdkr64.app"
        contents = app / "Contents"
        executable = contents / "MacOS" / "mdkr64"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"contract executable\n")
        executable.chmod(0o755)
        with (contents / "Info.plist").open("wb") as stream:
            plistlib.dump(
                {
                    "CFBundleIdentifier": "com.mdkr64.app",
                    "CFBundleExecutable": "mdkr64",
                },
                stream,
            )

        cases = (
            ("filesystem root", Path("/")),
            ("user home", Path.home()),
            ("source app directory", app),
            ("non-DMG target", temp_root / "contract-output.bin"),
            ("root-level DMG", Path("/contract-output.dmg")),
            ("empty DMG name", temp_root / ".dmg"),
            ("inside source", app / "contract-output.dmg"),
        )
        for label, output in cases:
            result = subprocess.run(
                [str(packager), "--validate-output-only", str(app), str(output)],
                cwd=ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            if not is_clean_rejection(result, "Refusing unsafe DMG output"):
                failures.append(f"unsafe DMG output unexpectedly accepted: {label}")

        safe_output = temp_root / "contract-safe-output.dmg"
        result = subprocess.run(
            [str(packager), "--validate-output-only", str(app), str(safe_output)],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or "Safe DMG output:" not in result.stdout:
            failures.append("narrow macOS DMG output failed validation-only control")
        if safe_output.exists():
            failures.append("macOS DMG validation-only mode wrote to the filesystem")

        target_sentinel = app / "must-survive.txt"
        target_sentinel.write_text("source target remains intact\n", encoding="utf-8")
        symlink_app = temp_root / "linked.app"
        symlink_app.symlink_to(app, target_is_directory=True)
        result = subprocess.run(
            [
                str(packager),
                "--validate-output-only",
                str(symlink_app),
                str(temp_root / "linked-output.dmg"),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if not is_clean_rejection(result, "Refusing unsafe source app"):
            failures.append("symlinked source .app unexpectedly accepted")
        if target_sentinel.read_text(encoding="utf-8") != (
            "source target remains intact\n"
        ):
            failures.append("source target changed during DMG validation")

        malformed = temp_root / "malformed.app"
        malformed.mkdir()
        result = subprocess.run(
            [
                str(packager),
                "--validate-output-only",
                str(malformed),
                str(temp_root / "malformed-output.dmg"),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if not is_clean_rejection(result, "Refusing unsafe source app"):
            failures.append("malformed source .app unexpectedly accepted")
    return failures


def validate_sdl_path_guard(builder: Path) -> list[str]:
    """Exercise SDL dependency path validation through its no-write mode."""
    failures: list[str] = []
    versioned_name = "sdl2-2.32.10"
    with tempfile.TemporaryDirectory(prefix="mdkr-sdl-contract.") as temp_name:
        temp_root = Path(temp_name)
        safe_work = temp_root / "safe" / versioned_name
        safe_prefix = safe_work / "install"
        cases = (
            ("filesystem root", Path("/"), Path("/install")),
            ("user home", Path.home(), Path.home() / "install"),
            ("repository root", ROOT, ROOT / "install"),
            (
                "unversioned work directory",
                temp_root / "unsafe-work",
                temp_root / "unsafe-work" / "install",
            ),
            (
                "filesystem-root versioned directory",
                Path("/") / versioned_name,
                Path("/") / versioned_name / "install",
            ),
            (
                "prefix outside work directory",
                safe_work,
                temp_root / "outside-install",
            ),
        )
        for label, work, prefix in cases:
            result = subprocess.run(
                [
                    str(builder),
                    "--validate-paths-only",
                    "--work-dir",
                    str(work),
                    "--prefix",
                    str(prefix),
                ],
                cwd=ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            if not is_clean_rejection(
                result, "refusing unsafe SDL2 work directory or prefix"
            ):
                failures.append(f"unsafe SDL2 build path unexpectedly accepted: {label}")

        work_target = temp_root / "work-target"
        work_target.mkdir()
        work_sentinel = work_target / "must-survive.txt"
        work_sentinel.write_text("work target remains intact\n", encoding="utf-8")
        linked_work = temp_root / "linked" / versioned_name
        linked_work.parent.mkdir()
        linked_work.symlink_to(work_target, target_is_directory=True)
        result = subprocess.run(
            [
                str(builder),
                "--validate-paths-only",
                "--work-dir",
                str(linked_work),
                "--prefix",
                str(linked_work / "install"),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if not is_clean_rejection(
            result, "refusing unsafe SDL2 work directory or prefix"
        ):
            failures.append("symlinked SDL2 work directory unexpectedly accepted")
        if work_sentinel.read_text(encoding="utf-8") != "work target remains intact\n":
            failures.append("symlinked SDL2 work target changed during validation")

        prefix_work = temp_root / "prefix-link" / versioned_name
        prefix_work.mkdir(parents=True)
        prefix_target = temp_root / "prefix-target"
        prefix_target.mkdir()
        prefix_sentinel = prefix_target / "must-survive.txt"
        prefix_sentinel.write_text("prefix target remains intact\n", encoding="utf-8")
        linked_prefix = prefix_work / "install"
        linked_prefix.symlink_to(prefix_target, target_is_directory=True)
        result = subprocess.run(
            [
                str(builder),
                "--validate-paths-only",
                "--work-dir",
                str(prefix_work),
                "--prefix",
                str(linked_prefix),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if not is_clean_rejection(
            result, "refusing unsafe SDL2 work directory or prefix"
        ):
            failures.append("symlinked SDL2 install prefix unexpectedly accepted")
        if prefix_sentinel.read_text(encoding="utf-8") != (
            "prefix target remains intact\n"
        ):
            failures.append("symlinked SDL2 prefix target changed during validation")

        result = subprocess.run(
            [
                str(builder),
                "--validate-paths-only",
                "--work-dir",
                str(safe_work),
                "--prefix",
                str(safe_prefix),
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or "Safe SDL2 work directory:" not in result.stdout:
            failures.append("narrow SDL2 work/prefix failed validation-only control")
        if safe_work.exists() or safe_prefix.exists():
            failures.append("SDL2 path validation-only mode wrote to the filesystem")
    return failures


def validate_release_checklist(source: str) -> list[str]:
    required_browser_tasks = (
        "wave_visible_table",
        "browser_save_ui",
        "browser_resource_plateau",
        "touch_controls",
        "browser_runtime",
        "browser_presentation_rates",
        "browser_taj_character_select",
        "browser_taj_persistence",
    )
    failures = [
        f"release checklist omits browser task: {task}"
        for task in required_browser_tasks
        if task not in source
    ]
    if "all eight runner tasks PASS" not in source:
        failures.append("release checklist browser task count is stale")
    if "After the final Developer ID signatures and stapling" not in source:
        failures.append("release checklist omits signed post-sign runtime gate")
    if source.count("-f release_tag=v1.0.4") < 2:
        failures.append(
            "release checklist must bind both portable and macOS publication "
            "to v1.0.4"
        )
    if source.count("--ref v1.0.4") < 2:
        failures.append(
            "release checklist must dispatch both portable and macOS publication "
            "from the exact v1.0.4 ref"
        )
    for label, needle in {
        "Windows automatic-publication hold": "Automatic Windows publication is intentionally disabled for this patch",
        "Windows manual GPU acceptance": "manual acceptance on Windows hardware",
        "Windows exact accepted archive": "Publish the exact checksum-verified archive and provenance sidecars",
        "Windows evidence record": "Record the tester, Windows version, GPU, archive SHA-256, and outcome",
        "Linux software-GPU gate": "Xvfb plus Mesa's pinned lavapipe ICD/llvmpipe",
        "Linux extracted AppRun gate": "launch through `AppRun`",
        "Linux failed-gate publication hold": "publish no Linux artifact",
    }.items():
        if needle not in source:
            failures.append(f"release checklist omits {label}")
    return failures


def main() -> int:
    source = WORKFLOW.read_text(encoding="utf-8")
    failures = validate(source)
    cmake_source = CMAKE_PROJECT.read_text(encoding="utf-8")
    failures.extend(validate_app_source_manifest(cmake_source))
    desktop_sources = {
        "workflow": DESKTOP_RELEASE_WORKFLOW.read_text(encoding="utf-8"),
        "windows_packager": WINDOWS_PACKAGER.read_text(encoding="utf-8"),
        "windows_import_guard": WINDOWS_IMPORT_GUARD.read_text(encoding="utf-8"),
        "linux_packager": LINUX_PACKAGER.read_text(encoding="utf-8"),
        "provenance_stamp": PROVENANCE_STAMP.read_text(encoding="utf-8"),
        "provenance_verify": PROVENANCE_VERIFY.read_text(encoding="utf-8"),
    }
    failures.extend(validate_desktop_release(desktop_sources))
    windows_validate_source = WINDOWS_VALIDATE_WORKFLOW.read_text(encoding="utf-8")
    failures.extend(validate_windows_nonpublishing(windows_validate_source))
    web_demo_source = WEB_DEMO_WORKFLOW.read_text(encoding="utf-8")
    failures.extend(validate_web_demo(web_demo_source))
    macos_source = MACOS_WORKFLOW.read_text(encoding="utf-8")
    failures.extend(validate_macos_release(macos_source))
    gpu_routing_sources = {
        "cmake": cmake_source,
        "correctness": source,
        "windows_validate": windows_validate_source,
        "desktop_release": desktop_sources["workflow"],
        "macos_release": macos_source,
        "macos_unsigned_verify": MACOS_UNSIGNED_VERIFY.read_text(encoding="utf-8"),
        "ci_local": CI_LOCAL.read_text(encoding="utf-8"),
        "source_archive": SOURCE_ARCHIVE_SMOKE.read_text(encoding="utf-8"),
        "run_checks": RUN_CHECKS.read_text(encoding="utf-8"),
        "ui_settings": UI_SETTINGS.read_text(encoding="utf-8"),
    }
    failures.extend(validate_gpu_test_routing(gpu_routing_sources))
    packaging_sources = {
        "builder": MACOS_BUILDER.read_text(encoding="utf-8"),
        "signer": MACOS_SIGNER.read_text(encoding="utf-8"),
        "notary": MACOS_NOTARY.read_text(encoding="utf-8"),
        "dmg": MACOS_DMG.read_text(encoding="utf-8"),
        "mount_helper": MACOS_DMG_MOUNT_HELPER.read_text(encoding="utf-8"),
        "entitlements": MACOS_ENTITLEMENTS.read_text(encoding="utf-8"),
        "sdl_builder": MACOS_SDL_BUILDER.read_text(encoding="utf-8"),
        "unsigned_verify": MACOS_UNSIGNED_VERIFY.read_text(encoding="utf-8"),
        "unsigned_dmg_verify": MACOS_UNSIGNED_DMG_VERIFY.read_text(encoding="utf-8"),
        "launch_probe": MACOS_LAUNCHSERVICES_PROBE.read_text(encoding="utf-8"),
        "app_pacing": APP_PACING_CHECK.read_text(encoding="utf-8"),
        "bundle_verify": MACOS_BUNDLE_VERIFY.read_text(encoding="utf-8"),
        "asset_verify": MACOS_ASSET_VERIFY.read_text(encoding="utf-8"),
        "provenance": MACOS_PROVENANCE.read_text(encoding="utf-8"),
        "sdl_config": MACOS_SDL_CONFIG.read_text(encoding="utf-8"),
        "info_plist": MACOS_INFO_PLIST.read_text(encoding="utf-8"),
        "cmake_project": cmake_source,
        "macos_readme": MACOS_README.read_text(encoding="utf-8"),
    }
    failures.extend(validate_macos_packaging(packaging_sources))
    failures.extend(validate_output_guard(MACOS_BUILDER))
    failures.extend(validate_dmg_output_guard(MACOS_DMG))
    failures.extend(validate_sdl_path_guard(MACOS_SDL_BUILDER))
    checklist_source = RELEASE_CHECKLIST.read_text(encoding="utf-8")
    failures.extend(validate_release_checklist(checklist_source))
    if failures:
        raise AssertionError("CI contract drift:\n  " + "\n  ".join(failures))

    # A gate that cannot reject regressions is documentation, not a control.
    controls = {
        "trigger": source.replace("\n  pull_request:", "\n  removed:", 1),
        "all-branch push coverage": source.replace(
            "branches: ['**']", "branches: [main]", 1
        ),
        "matrix": source.replace("label: Linux WebGPU", "label: removed", 1),
        "compiler": source.replace("compiler: gcc", "compiler: removed"),
        "action pin": re.sub(r"(@)[0-9a-f]{40}", r"\1v4", source, count=1),
        "ROM guard": source.replace("tools/check_no_rom.sh dist/web", "true", 1),
        "native SDK surface guard": source.replace(
            "tools/check_native_sdk_surface.py", "true", 1
        ),
        "third-party notice guard": source.replace(
            "tools/check_third_party_notices.py", "true", 1
        ),
        "image metadata guard": source.replace(
            "tools/check_image_metadata.py", "true", 1
        ),
        "public tree/new-commit step": source.replace(
            "name: Public tree and new-commit boundary",
            "name: Removed public boundary",
            1,
        ),
        "public ref-tip tree gate": source.replace(
            "tools/check_public_surface.py --tree", "true", 1
        ),
        "public pull-request range gate": source.replace(
            "origin/$BASE_REF..HEAD", "removed-pull-request-range", 1
        ),
        "public push range gate": source.replace(
            "$BEFORE_SHA..$CURRENT_SHA", "removed-push-range", 1
        ),
        "tag push range bypass": source.replace(
            '[ "$REF_TYPE" != "tag" ]', '[ "$REF_TYPE" = "tag" ]'
        ),
        "new-ref default-branch boundary": source.replace(
            "origin/$DEFAULT_BRANCH..$CURRENT_SHA",
            "removed-new-ref-boundary",
            1,
        ),
        "public-surface positive controls": source.replace(
            "tests/test_public_surface.py", "true", 1
        ),
    }
    escaped = [name for name, broken in controls.items() if not validate(broken)]
    if escaped:
        raise AssertionError(
            "CI positive controls unexpectedly passed: " + ", ".join(escaped)
        )

    app_manifest_controls = {
        "globbed app source manifest": cmake_source.replace(
            "set(APP_SOURCES", "file(GLOB APP_SOURCES", 1
        ),
        "missing common app source": cmake_source.replace(
            "platform/app/app_config.cpp",
            "platform/app/removed_app_config.cpp",
            1,
        ),
        "unreviewed new app source": cmake_source,
        "platform warning baseline deletion": cmake_source.replace(
            'set_source_files_properties(${PLATFORM_SOURCES} PROPERTIES',
            'set_source_files_properties(${IMGUI_SOURCES} PROPERTIES',
        ),
        "app warning baseline deletion": cmake_source.replace(
            'set_source_files_properties(${APP_SOURCES} PROPERTIES',
            'set_source_files_properties(${IMGUI_SOURCES} PROPERTIES',
        ),
    }
    app_manifest_escaped = [
        name
        for name, broken in app_manifest_controls.items()
        if not validate_app_source_manifest(
            broken,
            (tuple(sorted(path.name for path in APP_SOURCE_DIR.glob("*.cpp"))) +
             (("unreviewed_new_launcher.cpp",) if name == "unreviewed new app source" else ())),
        )
    ]
    if app_manifest_escaped:
        raise AssertionError(
            "app-source-manifest positive controls unexpectedly passed: "
            + ", ".join(app_manifest_escaped)
        )

    gpu_routing_controls = {
        "GPU label deletion": {
            **gpu_routing_sources,
            "cmake": re.sub(
                r"(set_tests_properties\(app_shell_smoke PROPERTIES.*?)\n"
                r"\s+LABELS gpu",
                r"\1",
                gpu_routing_sources["cmake"],
                count=1,
                flags=re.DOTALL,
            ),
        },
        "native GPU lock removal": {
            **gpu_routing_sources,
            "cmake": gpu_routing_sources["cmake"].replace(
                "RESOURCE_LOCK mdkr_native_gpu",
                "RESOURCE_LOCK removed_gpu_lock",
                1,
            ),
        },
        "GPU test registered after lock sweep": {
            **gpu_routing_sources,
            "cmake": gpu_routing_sources["cmake"].replace(
                "        mdkr_serialize_gpu_ctests()\n",
                "        mdkr_serialize_gpu_ctests()\n"
                "        set_tests_properties(app_shell_smoke PROPERTIES LABELS gpu)\n",
                1,
            ),
        },
        "app smoke target path mutation": {
            **gpu_routing_sources,
            "cmake": gpu_routing_sources["cmake"].replace(
                "add_test(NAME app_shell_smoke COMMAND ${MDKR_TARGET})",
                "add_test(NAME app_shell_smoke COMMAND missing-app)",
                1,
            ),
        },
        "Settings smoke deletion": {
            **gpu_routing_sources,
            "cmake": gpu_routing_sources["cmake"].replace(
                "add_test(NAME app_settings_smoke COMMAND ${MDKR_TARGET})",
                "add_test(NAME removed_settings_smoke COMMAND ${MDKR_TARGET})",
                1,
            ),
        },
        "Settings smoke panel path mutation": {
            **gpu_routing_sources,
            "cmake": gpu_routing_sources["cmake"].replace(
                "MDKR_APP_PANEL=Settings", "MDKR_APP_PANEL=About", 1
            ),
        },
        "Settings frame-rate visibility mutation": {
            **gpu_routing_sources,
            "cmake": gpu_routing_sources["cmake"].replace(
                "frame-rate-controls visible=1 gameplay-accuracy-separated=1",
                "frame-rate-controls visible=0 gameplay-accuracy-separated=1",
                1,
            ),
        },
        "native matrix GPU exclusion deletion": {
            **gpu_routing_sources,
            "correctness": gpu_routing_sources["correctness"].replace(
                "ctest --test-dir build-ci --output-on-failure -LE gpu",
                "ctest --test-dir build-ci --output-on-failure",
                1,
            ),
        },
        "sanitizer GPU exclusion deletion": {
            **gpu_routing_sources,
            "correctness": gpu_routing_sources["correctness"].replace(
                "ctest --test-dir build-sanitize --output-on-failure -LE gpu",
                "ctest --test-dir build-sanitize --output-on-failure",
                1,
            ),
        },
        "WebGPU callback TSan compile deletion": {
            **gpu_routing_sources,
            "correctness": gpu_routing_sources["correctness"].replace(
                "tests/test_webgpu_callback_latch.c",
                "tests/removed_webgpu_callback_latch.c",
                1,
            ),
        },
        "WebGPU request TSan compile deletion": {
            **gpu_routing_sources,
            "correctness": gpu_routing_sources["correctness"].replace(
                "tests/test_webgpu_async_request.c",
                "tests/removed_webgpu_async_request.c",
                1,
            ),
        },
        "WebGPU callback TSan runtime deletion": {
            **gpu_routing_sources,
            "correctness": gpu_routing_sources["correctness"].replace(
                "TSAN_OPTIONS=halt_on_error=1", "TSAN_OPTIONS="
            ),
        },
        "Windows validation GPU exclusion deletion": {
            **gpu_routing_sources,
            "windows_validate": gpu_routing_sources["windows_validate"].replace(
                " -LE gpu -E 'audio|mixer'", " -E 'audio|mixer'", 1
            ),
        },
        "Windows release targeted sentinel deletion": {
            **gpu_routing_sources,
            "desktop_release": gpu_routing_sources["desktop_release"].replace(
                " -R '^memory_allocator$'", " -R '^never$'", 1
            ),
        },
        "local CI GPU exclusion deletion": {
            **gpu_routing_sources,
            "ci_local": gpu_routing_sources["ci_local"].replace(
                " --output-on-failure -LE gpu", " --output-on-failure", 1
            ),
        },
        "source-archive GPU exclusion deletion": {
            **gpu_routing_sources,
            "source_archive": gpu_routing_sources["source_archive"].replace(
                " --output-on-failure -LE gpu", " --output-on-failure", 1
            ),
        },
        "run_checks GPU exclusion insertion": {
            **gpu_routing_sources,
            "run_checks": gpu_routing_sources["run_checks"].replace(
                '            "--output-on-failure",\n',
                '            "--output-on-failure",\n'
                '            "-LE",\n'
                '            "gpu",\n',
                1,
            ),
        },
        "packaged GPU verifier path mutation": {
            **gpu_routing_sources,
            "macos_release": gpu_routing_sources["macos_release"].replace(
                "verify_unsigned_release.sh",
                "missing_unsigned_release.sh",
                1,
            ),
        },
        "mounted DMG GPU verifier path mutation": {
            **gpu_routing_sources,
            "macos_release": gpu_routing_sources["macos_release"].replace(
                "verify_unsigned_dmg.sh",
                "missing_unsigned_dmg.sh",
                1,
            ),
        },
        "packaged Settings panel path mutation": {
            **gpu_routing_sources,
            "macos_unsigned_verify": gpu_routing_sources["macos_unsigned_verify"].replace(
                "--env MDKR_APP_PANEL=Settings",
                "--env MDKR_APP_PANEL=About",
                1,
            ),
        },
        "packaged frame-limit default proof deletion": {
            **gpu_routing_sources,
            "macos_unsigned_verify": gpu_routing_sources["macos_unsigned_verify"].replace(
                "Video\\.FrameLimit[[:space:]]+original",
                "frame-limit default unchecked",
                1,
            ),
        },
        "packaged gameplay cadence proof deletion": {
            **gpu_routing_sources,
            "macos_unsigned_verify": gpu_routing_sources["macos_unsigned_verify"].replace(
                "Gameplay\\.SimulationCadence[[:space:]]+original",
                "gameplay cadence unchecked",
                1,
            ),
        },
        "packaged smoothing default proof deletion": {
            **gpu_routing_sources,
            "macos_unsigned_verify": gpu_routing_sources["macos_unsigned_verify"].replace(
                "Video\\.MotionSmoothing[[:space:]]+off",
                "motion smoothing default unchecked",
                1,
            ),
        },
        "packaged frame-rate visibility proof deletion": {
            **gpu_routing_sources,
            "macos_unsigned_verify": gpu_routing_sources["macos_unsigned_verify"].replace(
                "frame-rate-controls visible=1 gameplay-accuracy-separated=1",
                "frame-rate controls unchecked",
                1,
            ),
        },
        "packaged Original frame-limit proof deletion": {
            **gpu_routing_sources,
            "macos_unsigned_verify": gpu_routing_sources["macos_unsigned_verify"].replace(
                "frame-limit value=original",
                "frame-limit default visibility unchecked",
                1,
            ),
        },
        "packaged frame-limit guidance deletion": {
            **gpu_routing_sources,
            "macos_unsigned_verify": gpu_routing_sources["macos_unsigned_verify"].replace(
                'frame-limit UI contract: recommended="Original (recommended)" group="Higher refresh rates"',
                "frame-limit guidance unchecked",
                1,
            ),
        },
        "schema frame-limit contract deletion": {
            **gpu_routing_sources,
            "cmake": gpu_routing_sources["cmake"].replace(
                "MDKR_FRAME_LIMIT_UI_CONTRACT_REGEX",
                "REMOVED_FRAME_LIMIT_UI_CONTRACT",
            ),
        },
        "recommended frame-limit label mutation": {
            **gpu_routing_sources,
            "ui_settings": gpu_routing_sources["ui_settings"].replace(
                "Original (recommended)",
                "Original",
            ),
        },
        "modern frame-limit grouping deletion": {
            **gpu_routing_sources,
            "ui_settings": gpu_routing_sources["ui_settings"].replace(
                "ImGui::SeparatorText(kModernFrameLimitGroup)",
                "ImGui::Separator()",
                1,
            ),
        },
        "interpolated-image guidance mutation": {
            **gpu_routing_sources,
            "ui_settings": gpu_routing_sources["ui_settings"].replace(
                "images when smoothing is Off, or create in-between images when it is ",
                "images are always repeated. ",
                1,
            ),
        },
        "fixed-gameplay guidance mutation": {
            **gpu_routing_sources,
            "ui_settings": gpu_routing_sources["ui_settings"].replace(
                "Gameplay speed does not change. Higher rates can use more ",
                "Gameplay speed changes. Higher rates can use more ",
                1,
            ),
        },
        "browser fallback guidance mutation": {
            **gpu_routing_sources,
            "ui_settings": gpu_routing_sources["ui_settings"].replace(
                "browser always maps Uncapped to Match Display.",
                "Unbounded in a browser.",
                1,
            ),
        },
    }
    gpu_routing_escaped = [
        name for name, broken in gpu_routing_controls.items()
        if not validate_gpu_test_routing(broken)
    ]
    if gpu_routing_escaped:
        raise AssertionError(
            "GPU-routing positive controls unexpectedly passed: "
            + ", ".join(gpu_routing_escaped)
        )

    windows_validate_controls = {
        "packaged startup": windows_validate_source.replace(
            "./tools/package_windows_zip.sh", "true", 1
        ),
        "unrelated CWD": windows_validate_source.replace(
            'cd "$unrelated"', "true", 1
        ),
        "publication hold": windows_validate_source.replace(
            "Windows artifact not uploaded: hosted GPU launcher qualification is unavailable.",
            "Windows artifact ready.",
            1,
        ),
        "forbidden upload": windows_validate_source.replace(
            "      - name: Package and verify extracted startup (not published)",
            "      - uses: actions/upload-artifact@65c4c4a1ddee5b72f698fdd19549f0f0fb45cf08\n"
            "      - name: Package and verify extracted startup (not published)",
            1,
        ),
    }
    windows_validate_escaped = [
        name
        for name, broken in windows_validate_controls.items()
        if not validate_windows_nonpublishing(broken)
    ]
    if windows_validate_escaped:
        raise AssertionError(
            "Windows non-publishing controls unexpectedly passed: "
            + ", ".join(windows_validate_escaped)
        )

    desktop_controls = {
        "central version validation": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "\n  validate:", "\n  unchecked:", 1
            ),
        },
        "portable release full-history checkout": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "fetch-depth: 0", "fetch-depth: 1", 1
            ),
        },
        "portable exact release tag": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                'EXPECTED_RELEASE_TAG="v${RELEASE_VERSION}"',
                'EXPECTED_RELEASE_TAG="${RELEASE_VERSION}"',
                1,
            ),
        },
        "portable release tag resolution": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                'git rev-parse --verify "${RELEASE_TAG}^{commit}"',
                'printf "%s" "$GITHUB_SHA"',
                1,
            ),
        },
        "portable release tag/source binding": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                '[[ "$TAG_COMMIT" == "$GITHUB_SHA" ]]', "true", 1
            ),
        },
        "dev release-tag rejection": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "release_tag must be empty when version=dev",
                "dev release tag accepted",
                1,
            ),
        },
        "immutable action pin": {
            **desktop_sources,
            "workflow": re.sub(
                r"(@)[0-9a-f]{40}",
                r"\1v4",
                desktop_sources["workflow"],
                count=1,
            ),
        },
        "compiled Linux version": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                '-DMDKR_VERSION="$RELEASE_VERSION"',
                '-DMDKR_VERSION="dev"',
                1,
            ),
        },
        "exact Linux binary version": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                'test "$version_output" = "mdkr64 ${{ needs.validate.outputs.version }}"',
                "test -n \"$version_output\"",
                1,
            ),
        },
        "canonical Linux workflow artifact": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "dist/Golden-Balloon-*-linux-x86_64.*",
                "dist/mdkr64-linux-x86_64.*",
                1,
            ),
        },
        "Windows archive inspection dependency": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "mingw-w64-x86_64-python zip unzip",
                "zip unzip",
                1,
            ),
        },
        "Windows declared Python probe": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "python3 --version", "true", 1
            ),
        },
        "Windows import verification": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "./tools/check_windows_imports.sh build/mdkr64.exe",
                "true",
                1,
            ),
        },
        "Windows MinGW portability target": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "--target mdkr64 mdkr_memory_allocator_test",
                "--target mdkr64",
                1,
            ),
        },
        "Windows MinGW portability execution": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "-R '^memory_allocator$'",
                "-R '^never$'",
                1,
            ),
        },
        "Linux software GPU stack": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "xvfb xauth mesa-vulkan-drivers",
                "xvfb xauth",
                1,
            ),
        },
        "Linux exact lavapipe selection": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
                "/tmp/unspecified-vulkan-driver.json",
            ),
        },
        "Ubuntu 22.04 lavapipe loader compatibility": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "VK_ICD_FILENAMES: /usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
                "VK_ICD_FILENAMES:",
            ),
        },
        "built WebGPU launcher gate": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                'qualify_launcher webgpu ""',
                'echo skipped-built-webgpu',
                1,
            ),
        },
        "default renderer environment isolation": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "local -a backend=(-u MDKR_RENDERER)",
                "local -a backend=()",
            ),
        },
        "built GL launcher gate": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "qualify_launcher gl gl",
                "echo skipped-built-gl",
                1,
            ),
        },
        "packaged WebGPU launcher gate": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                'qualify_package webgpu ""',
                'echo skipped-packaged-webgpu',
                1,
            ),
        },
        "packaged GL launcher gate": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "qualify_package gl gl",
                "echo skipped-packaged-gl",
                1,
            ),
        },
        "Linux launcher content validation": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                'python3 tests/check_app_capture.py "$work/launcher.bmp" --self-test',
                "true",
            ),
        },
        "Linux real-present requirement": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "MDKR_APP_REQUIRE_PRESENT=1",
                "MDKR_APP_REQUIRE_PRESENT=0",
            ),
        },
        "Linux packaged bundled SDL proof": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                'LD_LIBRARY_PATH="$lib_dir" ldd "$binary"',
                'ldd "$binary"',
                1,
            ),
        },
        "Windows automatic-publication policy": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "Hosted Windows artifact not uploaded automatically; only a manually accepted exact archive may be attached.",
                "Windows artifact ready.",
                1,
            ),
        },
        "Windows forbidden upload": {
            **desktop_sources,
            "workflow": desktop_sources["workflow"].replace(
                "      - name: Confirm automatic Windows publication is disabled",
                "      - uses: actions/upload-artifact@65c4c4a1ddee5b72f698fdd19549f0f0fb45cf08\n"
                "      - name: Confirm automatic Windows publication is disabled",
                1,
            ),
        },
        "renamed Windows executable": {
            **desktop_sources,
            "windows_packager": desktop_sources["windows_packager"].replace(
                '"$stage/GoldenBalloon.exe"',
                '"$stage/mdkr64.exe"',
                1,
            ),
        },
        "canonical Windows zip filename": {
            **desktop_sources,
            "windows_packager": desktop_sources["windows_packager"].replace(
                'archive="$dist/Golden-Balloon-$version-windows-x64.zip"',
                'archive="$dist/mdkr64-windows-$version.zip"',
                1,
            ),
        },
        "Windows exact archive verification": {
            **desktop_sources,
            "windows_packager": desktop_sources["windows_packager"].replace(
                'verify_windows_archive "$archive"',
                "true",
                1,
            ),
        },
        "Windows exact archive entry": {
            **desktop_sources,
            "windows_packager": desktop_sources["windows_packager"].replace(
                "GoldenBalloon/gamecontrollerdb.txt",
                "GoldenBalloon/unexpected.txt",
                1,
            ),
        },
        "Windows no-DLL staging": {
            **desktop_sources,
            "windows_packager": desktop_sources["windows_packager"].replace(
                'cp "$binary" "$stage/GoldenBalloon.exe"',
                'cp "$binary" "$stage/GoldenBalloon.exe"\n'
                'cp SDL2.dll "$stage/SDL2.dll"',
                1,
            ),
        },
        "Windows archive verifier self-test": {
            **desktop_sources,
            "windows_packager": desktop_sources["windows_packager"].replace(
                "Windows archive verifier accepted the SDL2.dll control.",
                "DLL control accepted.",
                1,
            ),
        },
        "packager import verification": {
            **desktop_sources,
            "windows_packager": desktop_sources["windows_packager"].replace(
                './tools/check_windows_imports.sh "$binary"',
                "true",
                1,
            ),
        },
        "Windows staged-package ROM guard": {
            **desktop_sources,
            "windows_packager": desktop_sources["windows_packager"].replace(
                './tools/check_no_rom.sh "$stage"',
                "true",
                1,
            ),
        },
        "Windows SDL broken control": {
            **desktop_sources,
            "windows_import_guard": desktop_sources["windows_import_guard"].replace(
                "SDL2.dll", "KERNEL32.dll", 1
            ),
        },
        "Windows system-DLL allowlist": {
            **desktop_sources,
            "windows_import_guard": desktop_sources["windows_import_guard"].replace(
                "KERNEL32|USER32", "REMOVED_SYSTEM_DLLS", 1
            ),
        },
        "Linux package version validation": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                "version must be dev or bare semver",
                "version accepted",
                1,
            ),
        },
        "canonical Linux archive filename": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                'artifact_prefix="Golden-Balloon-$version-linux-x86_64"',
                'artifact_prefix="mdkr64-linux-$version"',
                1,
            ),
        },
        "Linux exact archive verification": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                'verify_linux_tarball "$tarball" "$appdir"',
                "true",
                1,
            ),
        },
        "Linux exact archive entry": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                "Golden-Balloon.AppDir/usr/bin/gamecontrollerdb.txt",
                "Golden-Balloon.AppDir/usr/bin/unexpected.txt",
                1,
            ),
        },
        "Linux archive verifier self-test": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                "Linux tarball verifier accepted an unexpected archive entry.",
                "Unexpected archive entry accepted.",
                1,
            ),
        },
        "pinned Linux appimagetool hash": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                'APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"',
                'APPIMAGETOOL_SHA256="unverified"',
                1,
            ),
        },
        "Linux appimagetool digest comparison": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                '[[ "$got" != "$APPIMAGETOOL_SHA256" ]]',
                '[[ -z "$got" ]]',
                1,
            ),
        },
        "Linux controller database requirement": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                "tracked controller mapping database is missing from release package",
                "controller database unavailable",
                1,
            ),
        },
        "Linux license and readme requirement": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                "tracked LICENSE and README are required in the release package",
                "documentation unavailable",
                1,
            ),
        },
        "Linux branded icon requirement": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                "tracked branded icon source is required in the release package",
                "icon unavailable",
                1,
            ),
        },
        "Linux staged-package ROM guard": {
            **desktop_sources,
            "linux_packager": desktop_sources["linux_packager"].replace(
                './tools/check_no_rom.sh "$appdir"',
                "true",
                1,
            ),
        },
        "provenance version-bound filename": {
            **desktop_sources,
            "provenance_stamp": desktop_sources["provenance_stamp"].replace(
                'windows_zip="Golden-Balloon-${version}-windows-x64.zip"',
                'windows_zip="Golden-Balloon-*-windows-x64.zip"',
                1,
            ),
        },
        "provenance exact artifact family": {
            **desktop_sources,
            "provenance_verify": desktop_sources["provenance_verify"].replace(
                "unsupported release artifact filename",
                "artifact accepted",
                1,
            ),
        },
        "provenance dirty-source record": {
            **desktop_sources,
            "provenance_stamp": desktop_sources["provenance_stamp"].replace(
                '"source_dirty": $source_dirty',
                '"source_dirty": false',
                1,
            ),
        },
        "release dirty-provenance rejection": {
            **desktop_sources,
            "provenance_verify": desktop_sources["provenance_verify"].replace(
                'rec.get("source_dirty") is not False',
                'False',
                1,
            ),
        },
    }
    desktop_escaped = [
        name
        for name, broken in desktop_controls.items()
        if not validate_desktop_release(broken)
    ]
    if desktop_escaped:
        raise AssertionError(
            "desktop-release positive controls unexpectedly passed: "
            + ", ".join(desktop_escaped)
        )

    web_demo_controls = {
        "linked wave table": web_demo_source.replace(
            "python3 tests/check_wave_visible_table.py "
            "--wasm dist/web/mdkr64_web.wasm",
            "true",
            1,
        ),
    }
    web_demo_escaped = [
        name for name, broken in web_demo_controls.items()
        if not validate_web_demo(broken)
    ]
    if web_demo_escaped:
        raise AssertionError(
            "web-demo positive controls unexpectedly passed: "
            + ", ".join(web_demo_escaped)
        )

    macos_controls = {
        "read-only signing job": macos_source.replace(
            "\npermissions:\n  contents: read",
            "\npermissions:\n  contents: write",
            1,
        ),
        "certificate": macos_source.replace(
            "MACOS_CERTIFICATE_P12_BASE64", "REMOVED_CERTIFICATE"
        ),
        "notary API": macos_source.replace(
            "APPLE_API_PRIVATE_KEY_BASE64", "REMOVED_NOTARY_KEY"
        ),
        "app signing": macos_source.replace(
            "sign_and_notarize.sh", "removed-signing.sh", 1
        ),
        "DMG notarization": macos_source.replace(
            'notarize_artifact.sh "$DMG_PATH"', "true", 1
        ),
        "Gatekeeper verification": macos_source.replace(
            "--distribution dist/mdkr64.app", "dist/mdkr64.app", 1
        ),
        "strict deployment target": macos_source.replace(
            "--strict-deployment-target", "--allow-newer-dependency", 1
        ),
        "explicit deployment target": macos_source.replace(
            "--deployment-target 13.0", "--deployment-target latest"
        ),
        "arm64 dependency and app": macos_source.replace(
            "--arch arm64", "--arch native", 1
        ),
        "pinned standalone SDL2": macos_source.replace(
            "build_release_sdl2.sh", "brew install sdl2", 1
        ),
        "trusted signing defaults false": macos_source.replace(
            "        type: boolean\n        default: false",
            "        type: boolean\n        default: true",
            1,
        ),
        "exact vVERSION tag": macos_source.replace(
            'EXPECTED_RELEASE_TAG="v${RELEASE_VERSION}"',
            'EXPECTED_RELEASE_TAG="${RELEASE_VERSION}"',
            1,
        ),
        "package tag resolution": macos_source.replace(
            'git rev-parse --verify "${RELEASE_TAG}^{commit}"',
            'printf "%s" "$GITHUB_SHA"',
            1,
        ),
        "publish tag resolution": macos_source.replace(
            "repos/${GITHUB_REPOSITORY}/commits/${RELEASE_TAG}",
            "repos/${GITHUB_REPOSITORY}/commits/${SOURCE_COMMIT}",
            1,
        ),
        "package tag/source binding": macos_source.replace(
            '[[ "$TAG_COMMIT" == "$GITHUB_SHA" ]]', "true", 1
        ),
        "publish tag/source binding": macos_source.replace(
            '[[ "$TAG_COMMIT" == "$SOURCE_COMMIT" ]]', "true", 1
        ),
        "explicit unsigned identity": macos_source.replace(
            "macos-arm64-unsigned.dmg", "macos-arm64.dmg"
        ),
        "explicit trusted identity": macos_source.replace(
            "macos-arm64-signed-notarized.dmg", "macos-arm64.dmg"
        ),
        "unsigned signing provenance": macos_source.replace(
            "--signing ad-hoc-unsigned", "--signing unspecified", 1
        ),
        "trusted signing provenance": macos_source.replace(
            "--signing developer-id-notarized", "--signing unspecified", 1
        ),
        "workflow-artifact signing identity": macos_source.replace(
            "signed-notarized' || 'unsigned", "unspecified", 1
        ),
        "unsigned WebGPU verification": macos_source.replace(
            "verify_unsigned_release.sh", "true", 1
        ),
        "active unsigned WebGPU verification": macos_source.replace(
            "./macos/Scripts/verify_unsigned_release.sh",
            "true # ./macos/Scripts/verify_unsigned_release.sh",
            1,
        ),
        "mounted DMG WebGPU verification": macos_source.replace(
            "verify_unsigned_dmg.sh", "true", 1
        ),
        "active mounted DMG WebGPU verification": macos_source.replace(
            "./macos/Scripts/verify_unsigned_dmg.sh",
            "true # ./macos/Scripts/verify_unsigned_dmg.sh",
            1,
        ),
        "basename-only checksum generation": macos_source.replace(
            'shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256"',
            'shasum -a 256 "$DMG_PATH" > "$DMG_PATH.sha256"',
            1,
        ),
        "active checksum generation": macos_source.replace(
            'shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256"',
            'true # shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256"',
            1,
        ),
        "artifact-local checksum verification": macos_source.replace(
            'shasum -a 256 -c "$DMG_NAME.sha256"',
            'shasum -a 256 -c "${DMG_PATH}.sha256"',
            1,
        ),
        "active checksum verification": macos_source.replace(
            'shasum -a 256 -c "$DMG_NAME.sha256"',
            'true # shasum -a 256 -c "$DMG_NAME.sha256"',
            1,
        ),
        "checksum working directory": macos_source.replace(
            'cd "$DMG_DIR"', "true", 1
        ),
        "1.0.4 patch-release default": macos_source.replace(
            'default: "1.0.4"', 'default: "1.0.1"', 1
        ),
        "publish isolation": macos_source.replace(
            "\n  publish:", "\n  removed-publish:", 1
        ),
        "trusted tagged publication rejection": macos_source.replace(
            "trusted signing artifacts cannot be published until the documented post-sign LaunchServices/WebGPU runtime gate is automated",
            "trusted artifact accepted",
            1,
        ),
        "publish excludes trusted artifact": macos_source.replace(
            "if: ${{ inputs.release_tag != '' && !inputs.trusted_signing }}",
            "if: ${{ inputs.release_tag != '' }}",
            1,
        ),
        "publish script rejects trusted artifact": macos_source.replace(
            "trusted macOS publication is disabled until post-sign runtime qualification is automated",
            "trusted artifact accepted",
            1,
        ),
        "explicit publish repository": macos_source.replace(
            '--repo "$GITHUB_REPOSITORY"', ""
        ),
    }
    macos_escaped = [
        name
        for name, broken in macos_controls.items()
        if not validate_macos_release(broken)
    ]
    if macos_escaped:
        raise AssertionError(
            "macOS CI positive controls unexpectedly passed: "
            + ", ".join(macos_escaped)
        )

    packaging_controls = {
        "Developer ID certificate type": {
            **packaging_sources,
            "signer": packaging_sources["signer"].replace(
                '"Developer ID Application: "*', '"Apple Development: "*', 1
            ),
        },
        "outer integrity seal": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                'codesign --force --sign - "${OUTPUT_APP}"', "true", 1
            ),
        },
        "sdl2-compat rejection": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "pkg-config resolves sdl2 to sdl2-compat", "unsupported dependency", 1
            ),
        },
        "pinned SDL2 hash": {
            **packaging_sources,
            "sdl_config": packaging_sources["sdl_config"].replace(
                "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165",
                "unverified",
                1,
            ),
        },
        "pinned SDL2 license hash": {
            **packaging_sources,
            "sdl_config": packaging_sources["sdl_config"].replace(
                "97f35b302b361680ec1e891e95d2d52097bb95abff361434916d99dc1305f127",
                "unverified",
                1,
            ),
        },
        "shared SDL2 pin source": {
            **packaging_sources,
            "sdl_builder": packaging_sources["sdl_builder"].replace(
                'source "${SCRIPT_DIR}/release_sdl2_config.sh"',
                "pins omitted",
                1,
            ),
        },
        "SDL2 source path normalization": {
            **packaging_sources,
            "sdl_builder": packaging_sources["sdl_builder"].replace(
                "-ffile-prefix-map=${SOURCE_DIR}=SDL2-${MDKR_RELEASE_SDL2_VERSION}",
                "-fno-file-prefix-map",
                1,
            ),
        },
        "SDL2 macro path normalization": {
            **packaging_sources,
            "sdl_builder": packaging_sources["sdl_builder"].replace(
                "-fmacro-prefix-map=${SOURCE_DIR}=SDL2-${MDKR_RELEASE_SDL2_VERSION}",
                "-fno-macro-prefix-map",
                1,
            ),
        },
        "SDL2 debug path normalization": {
            **packaging_sources,
            "sdl_builder": packaging_sources["sdl_builder"].replace(
                "-fdebug-prefix-map=${SOURCE_DIR}=SDL2-${MDKR_RELEASE_SDL2_VERSION}",
                "-fno-debug-prefix-map",
                1,
            ),
        },
        "SDL2 local path rejection": {
            **packaging_sources,
            "sdl_builder": packaging_sources["sdl_builder"].replace(
                "installed SDL2 contains an absolute developer/build path",
                "installed SDL2 accepted",
                1,
            ),
        },
        "executable local path rejection": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "Final app executable contains an absolute source/build path.",
                "Final app executable accepted.",
                1,
            ),
        },
        "absolute rpath selection": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                '[[ "${rpath}" == /* ]] && ABSOLUTE_RPATHS+=("${rpath}")',
                '[[ -n "${rpath}" ]] && ABSOLUTE_RPATHS+=("${rpath}")',
                1,
            ),
        },
        "absolute rpath removal": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                'install_name_tool -delete_rpath "${rpath}" "${ENGINE_PATH}"',
                "true",
                1,
            ),
        },
        "final payload strip": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                'strip -S "${ENGINE_PATH}"', "true", 1
            ),
        },
        "post-normalization rpath scan": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "Final app executable still contains an absolute runtime search path.",
                "Final runtime search paths accepted.",
                1,
            ),
        },
        "post-normalization SDL2 path scan": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "Final app executable contains the build-time SDL2 path.",
                "Final SDL2 build path accepted.",
                1,
            ),
        },
        "normalization order": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                'strip -S "${ENGINE_PATH}" || die "Failed to strip compiler debug records."',
                "true # strip moved below signing",
                1,
            ).replace(
                'codesign --force --sign - "${OUTPUT_APP}"',
                'codesign --force --sign - "${OUTPUT_APP}"\n'
                'strip -S "${ENGINE_PATH}" || die "Failed to strip compiler debug records."',
                1,
            ),
        },
        "safe output validation": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "canonical_output_app_path", "unchecked_output_app_path"
            ),
        },
        "exact app output basename": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "target basename must be mdkr64.app",
                "target name accepted",
                1,
            ),
        },
        "existing app output identity": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "existing target belongs to another application",
                "existing target accepted",
                1,
            ),
        },
        "SDL2 build path validation": {
            **packaging_sources,
            "sdl_builder": packaging_sources["sdl_builder"].replace(
                "canonical_sdl_paths", "unchecked_sdl_paths"
            ),
        },
        "SDL2 versioned work directory": {
            **packaging_sources,
            "sdl_builder": packaging_sources["sdl_builder"].replace(
                "work directory basename must be sdl2-",
                "work directory accepted",
                1,
            ),
        },
        "SDL2 private install prefix": {
            **packaging_sources,
            "sdl_builder": packaging_sources["sdl_builder"].replace(
                "prefix must be the work directory's exact install child",
                "prefix accepted",
                1,
            ),
        },
        "local recipe untracked-source guard": {
            **packaging_sources,
            "macos_readme": packaging_sources["macos_readme"].replace(
                "git status --porcelain=v1 --untracked-files=all",
                "git diff --quiet",
                1,
            ),
        },
        "explicit signing provenance": {
            **packaging_sources,
            "provenance": packaging_sources["provenance"].replace(
                'payload["macos_signing"] = sys.argv[2]',
                "pass",
                1,
            ),
        },
        "bundled SDL2 license": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "Contents/Resources/ThirdParty/SDL2-LICENSE.txt",
                "license omitted",
                1,
            ),
        },
        "bundled SDL2 manifest": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "Contents/Resources/ThirdParty/SDL2-MANIFEST.txt",
                "manifest omitted",
                1,
            ),
        },
        "required macOS controller database": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "Tracked SDL game-controller mapping database is missing or empty",
                "controller database unavailable",
                1,
            ),
        },
        "required macOS branded icon": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                "Tracked branded icon source is required for a Release app bundle",
                "icon unavailable",
                1,
            ),
        },
        "verified macOS controller database": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "bundled game-controller mapping database is missing or empty",
                "controller database accepted",
                1,
            ),
        },
        "verified macOS branded icon": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "bundled branded app icon is missing or empty",
                "icon accepted",
                1,
            ),
        },
        "asset guard SDL2 manifest allowlist": {
            **packaging_sources,
            "asset_verify": packaging_sources["asset_verify"].replace(
                "Contents/Resources/ThirdParty/SDL2-MANIFEST.txt",
                "manifest omitted",
                1,
            ),
        },
        "final SDL2 binary identity": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                '"${SDL2_ACTUAL_SHA256}" == "${SDL2_BUNDLED_SHA256}"',
                '"${SDL2_ACTUAL_SHA256}" != ""',
                1,
            ),
        },
        "post-sign SDL2 identity refresh": {
            **packaging_sources,
            "signer": packaging_sources["signer"].replace(
                'print "bundled_dylib_sha256=" hash',
                'print "bundled_dylib_sha256=stale"',
                1,
            ),
        },
        "WebGPU default smoke": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "launcher did not select WebGPU by default", "launcher ran", 1
            ),
        },
        "bounded LaunchServices controller invocation": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                '"${SCRIPT_DIR}/run_launchservices_probe.py"',
                '"/usr/bin/open"',
                1,
            ),
        },
        "LaunchServices monotonic timeout": {
            **packaging_sources,
            "launch_probe": packaging_sources["launch_probe"].replace(
                "deadline = time.monotonic() + args.timeout",
                "deadline = float('inf')",
                1,
            ),
        },
        "LaunchServices exact executable identity": {
            **packaging_sources,
            "launch_probe": packaging_sources["launch_probe"].replace(
                "if command == expected", "if expected in command", 1
            ),
        },
        "LaunchServices graceful termination": {
            **packaging_sources,
            "launch_probe": packaging_sources["launch_probe"].replace(
                "signal_exact_processes(pids, executable, signal.SIGTERM)",
                "set(pids)",
                1,
            ),
        },
        "LaunchServices forced termination": {
            **packaging_sources,
            "launch_probe": packaging_sources["launch_probe"].replace(
                "signal_exact_processes(remaining, executable, signal.SIGKILL)",
                "set(remaining)",
                1,
            ),
        },
        "LaunchServices signal cleanup": {
            **packaging_sources,
            "launch_probe": packaging_sources["launch_probe"].replace(
                "signal.SIGINT, signal.SIGTERM, signal.SIGHUP",
                "signal.SIGINT, signal.SIGHUP",
                1,
            ),
        },
        "LaunchServices survivor rejection": {
            **packaging_sources,
            "launch_probe": packaging_sources["launch_probe"].replace(
                "launched app process survived cleanup",
                "cleanup complete",
                1,
            ),
        },
        "LaunchServices open/app lifetime agreement": {
            **packaging_sources,
            "launch_probe": packaging_sources["launch_probe"].replace(
                "unexpected_survivors = True",
                "unexpected_survivors = False",
                1,
            ),
        },
        "shared FPS LaunchServices controller": {
            **packaging_sources,
            "app_pacing": packaging_sources["app_pacing"].replace(
                "str(LAUNCHSERVICES_PROBE)", '"/usr/bin/open"', 1
            ),
        },
        "mounted DMG WebGPU qualification": {
            **packaging_sources,
            "unsigned_dmg_verify": packaging_sources["unsigned_dmg_verify"].replace(
                '"${SCRIPT_DIR}/verify_unsigned_release.sh"',
                "true",
                1,
            ),
        },
        "active mounted-app WebGPU qualification": {
            **packaging_sources,
            "unsigned_dmg_verify": packaging_sources["unsigned_dmg_verify"].replace(
                '"${SCRIPT_DIR}/verify_unsigned_release.sh"',
                'true # "${SCRIPT_DIR}/verify_unsigned_release.sh"',
                1,
            ),
        },
        "mounted DMG read-only qualification": {
            **packaging_sources,
            "unsigned_dmg_verify": packaging_sources["unsigned_dmg_verify"].replace(
                "hdiutil attach -readonly -nobrowse",
                "hdiutil attach -nobrowse",
                1,
            ),
        },
        "mounted DMG active-command semantics": {
            **packaging_sources,
            "unsigned_dmg_verify": packaging_sources["unsigned_dmg_verify"].replace(
                "hdiutil attach -readonly -nobrowse",
                "true # hdiutil attach -readonly -nobrowse",
                1,
            ),
        },
        "mounted DMG single-app identity": {
            **packaging_sources,
            "unsigned_dmg_verify": packaging_sources["unsigned_dmg_verify"].replace(
                '[[ "${#APPS[@]}" -eq 1 ]]',
                '[[ "${#APPS[@]}" -ge 1 ]]',
                1,
            ),
        },
        "mounted DMG physical-app identity": {
            **packaging_sources,
            "unsigned_dmg_verify": packaging_sources["unsigned_dmg_verify"].replace(
                '[[ -d "${APPS[0]}" && ! -L "${APPS[0]}" ]]',
                "true",
                1,
            ),
        },
        "mounted DMG Applications target": {
            **packaging_sources,
            "unsigned_dmg_verify": packaging_sources["unsigned_dmg_verify"].replace(
                '[[ "$(readlink "${APPLICATIONS_LINK}")" == "/Applications" ]]',
                "true",
                1,
            ),
        },
        "mounted DMG extra-root rejection": {
            **packaging_sources,
            "unsigned_dmg_verify": packaging_sources["unsigned_dmg_verify"].replace(
                '*) die "mounted DMG contains unexpected root entry: $(basename "${entry}")" ;;',
                "*) ;;",
                1,
            ),
        },
        "nested SDL ad-hoc signature": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "printf '%s\\n' \"${SDL2_SIGNATURE_DETAILS}\" | grep -Fq 'Signature=adhoc' ||",
                "true ||",
                1,
            ),
        },
        "mounted DMG shared cleanup": {
            **packaging_sources,
            "unsigned_dmg_verify": packaging_sources[
                "unsigned_dmg_verify"
            ].replace(
                'source "${SCRIPT_DIR}/dmg_mount_cleanup.sh"',
                "cleanup helpers omitted",
                1,
            ),
        },
        "DMG cleanup actual mount state": {
            **packaging_sources,
            "mount_helper": packaging_sources["mount_helper"].replace(
                'mount_device="$(/usr/bin/stat -f \'%d\' "${mount_dir}"',
                'mount_device="same-as-parent',
                1,
            ),
        },
        "DMG cleanup mount/parent comparison": {
            **packaging_sources,
            "mount_helper": packaging_sources["mount_helper"].replace(
                '[[ "${mount_device}" != "${parent_device}" ]]',
                "return 1",
                1,
            ),
        },
        "DMG cleanup bounded detach retries": {
            **packaging_sources,
            "mount_helper": packaging_sources["mount_helper"].replace(
                "for attempt in 1 2 3", "for attempt in 1", 1
            ),
        },
        "DMG cleanup exact forced detach": {
            **packaging_sources,
            "mount_helper": packaging_sources["mount_helper"].replace(
                'hdiutil detach -force "${mount_dir}"', "false", 1
            ),
        },
        "DMG cleanup verifies inactive mount": {
            **packaging_sources,
            "mount_helper": packaging_sources["mount_helper"].replace(
                '! mdkr_dmg_mount_is_active "${mount_dir}"',
                "true",
            ),
        },
        "DMG cleanup preserves live mount": {
            **packaging_sources,
            "mount_helper": packaging_sources["mount_helper"].replace(
                'mdkr_dmg_mount_is_active "${mount_dir}" && return 1',
                "true",
                1,
            ),
        },
        "DMG cleanup narrow directory removal": {
            **packaging_sources,
            "mount_helper": packaging_sources["mount_helper"].replace(
                'rmdir "${mount_dir}"',
                'rm -rf "${mount_dir}"',
                1,
            ),
        },
        "exact AppHost telemetry cardinality": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                '"${TELEMETRY_COUNT}" == "1"',
                '"${TELEMETRY_COUNT}" -ge "1"',
                1,
            ),
        },
        "AppHost attempt accounting": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "PRESENT_ATTEMPTS == PRESENTED_FRAMES + UNAVAILABLE_FRAMES",
                "PRESENT_ATTEMPTS >= PRESENTED_FRAMES",
                1,
            ),
        },
        "four actual surface presents": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "PRESENTED_FRAMES >= 4", "PRESENTED_FRAMES >= 1", 1
            ),
        },
        "surface-present telemetry agreement": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "SMOKE_PRESENTED_FRAMES == PRESENTED_FRAMES",
                "SMOKE_PRESENTED_FRAMES >= 1",
                1,
            ),
        },
        "bounded unavailable surface": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "UNAVAILABLE_FRAMES <= 64", "UNAVAILABLE_FRAMES >= 0", 1
            ),
        },
        "successful final surface status": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                '"${LAST_SURFACE_STATUS}" == "1" || "${LAST_SURFACE_STATUS}" == "2"',
                '"${LAST_SURFACE_STATUS}" != ""',
                1,
            ),
        },
        "exact arm64 verification": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "--expected-arch arm64", "--expected-arch native", 1
            ),
        },
        "exact macOS 13 verification": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "--expected-min-os 13.0", "--expected-min-os latest", 1
            ),
        },
        "plist build version verification": {
            **packaging_sources,
            "unsigned_verify": packaging_sources["unsigned_verify"].replace(
                "CFBundleVersion", "RemovedBundleVersion", 1
            ),
        },
        "exact architecture implementation": {
            **packaging_sources,
            "bundle_verify": packaging_sources["bundle_verify"].replace(
                "expected exactly ${EXPECTED_ARCH}",
                "architecture accepted",
            ),
        },
        "exact notary acceptance": {
            **packaging_sources,
            "notary": packaging_sources["notary"].replace(
                '"${STATUS}" != "Accepted"', '"${STATUS}" != "Anything"', 1
            ),
        },
        "mounted DMG verification": {
            **packaging_sources,
            "dmg": packaging_sources["dmg"].replace(
                "hdiutil attach -readonly -nobrowse", "true", 1
            ),
        },
        "packaged DMG active-command semantics": {
            **packaging_sources,
            "dmg": packaging_sources["dmg"].replace(
                "hdiutil attach -readonly -nobrowse",
                "true # hdiutil attach -readonly -nobrowse",
                1,
            ),
        },
        "deterministic system DMG layout": {
            **packaging_sources,
            "dmg": packaging_sources["dmg"].replace(
                "Creating DMG with the system hdiutil",
                "Creating DMG",
                1,
            ),
        },
        "safe DMG output validation": {
            **packaging_sources,
            "dmg": packaging_sources["dmg"].replace(
                "canonical_dmg_output", "unchecked_dmg_output"
            ),
        },
        "safe DMG source validation": {
            **packaging_sources,
            "dmg": packaging_sources["dmg"].replace(
                "canonical_source_app", "unchecked_source_app"
            ),
        },
        "DMG source symlink rejection": {
            **packaging_sources,
            "dmg": packaging_sources["dmg"].replace(
                "the source .app itself must not be a symbolic link",
                "source accepted",
                1,
            ),
        },
        "packaged DMG shared cleanup": {
            **packaging_sources,
            "dmg": packaging_sources["dmg"].replace(
                'source "${SCRIPT_DIR}/dmg_mount_cleanup.sh"',
                "cleanup helpers omitted",
                1,
            ),
        },
        "packaged DMG signal cleanup": {
            **packaging_sources,
            "dmg": packaging_sources["dmg"].replace(
                "trap 'exit 143' TERM", "true", 1
            ),
        },
        "library-validation protection": {
            **packaging_sources,
            "entitlements": packaging_sources["entitlements"].replace(
                "<dict>",
                "<dict><key>com.apple.security.cs.disable-library-validation</key>",
                1,
            ),
        },
    }
    packaging_escaped = [
        name
        for name, broken in packaging_controls.items()
        if not validate_macos_packaging(broken)
    ]
    if packaging_escaped:
        raise AssertionError(
            "macOS packaging positive controls unexpectedly passed: "
            + ", ".join(packaging_escaped)
        )

    checklist_controls = {
        "browser presentation rates": checklist_source.replace(
            "browser_presentation_rates", "removed_browser_rate_gate"
        ),
        "browser task count": checklist_source.replace(
            "all eight runner tasks PASS", "all runner tasks PASS", 1
        ),
        "browser Taj picker": checklist_source.replace(
            "browser_taj_character_select", "removed_browser_taj_picker", 1
        ),
        "browser Taj persistence": checklist_source.replace(
            "browser_taj_persistence", "removed_taj_state_gate", 1
        ),
        "signed post-sign runtime": checklist_source.replace(
            "After the final Developer ID signatures and stapling",
            "Before signing",
            1,
        ),
        "portable release tag binding": checklist_source.replace(
            "-f release_tag=v1.0.4", "-f release_tag=", 1
        ),
        "release dispatch source ref": checklist_source.replace(
            "--ref v1.0.4", "--ref main", 1
        ),
        "Windows automatic-publication hold": checklist_source.replace(
            "Automatic Windows publication is intentionally disabled for this patch",
            "Automatic Windows publication is enabled for this patch",
            1,
        ),
        "Windows manual acceptance deletion": checklist_source.replace(
            "manual acceptance on Windows hardware",
            "headless acceptance in CI",
            1,
        ),
        "Linux software GPU qualification": checklist_source.replace(
            "Xvfb plus Mesa's pinned lavapipe ICD/llvmpipe",
            "a display",
            1,
        ),
        "Linux extracted AppRun qualification": checklist_source.replace(
            "launch through `AppRun`",
            "inspect `AppRun`",
            1,
        ),
        "Linux failed-gate publication hold": checklist_source.replace(
            "publish no Linux artifact",
            "publish the local Linux artifact",
            1,
        ),
    }
    checklist_escaped = [
        name
        for name, broken in checklist_controls.items()
        if not validate_release_checklist(broken)
    ]
    if checklist_escaped:
        raise AssertionError(
            "release-checklist positive controls unexpectedly passed: "
            + ", ".join(checklist_escaped)
        )

    action_count = len(PINNED_ACTION_RE.findall(source))
    macos_action_count = len(PINNED_ACTION_RE.findall(macos_source))
    desktop_action_count = len(
        PINNED_ACTION_RE.findall(desktop_sources["workflow"])
    )
    print(
        "check_ci_contract: PASS — push/PR/manual policy, "
        "3-cell native matrix, sanitizer lane, linked wasm/save custody, "
        "ROM guards, GPU-qualified Linux artifacts, non-publishing Windows validation, "
        "fail-closed unsigned WebGPU and optional trusted macOS releases, and "
        f"{action_count + macos_action_count + desktop_action_count} "
        "immutable action pins"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
