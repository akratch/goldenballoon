#!/usr/bin/env python3
"""Fail closed if the repository correctness workflow loses required coverage.

This is the engine. The exact literals it pins -- workflow step names, refusal
messages, artifact filenames, pinned hashes, packaged UI text -- are data, in
`tests/ci_contract_manifest.py`, together with the deliberately broken sources
that prove each pin can still reject a regression. Reword a release script and
you edit the manifest, not this file.

What stays here is everything that is computed rather than transcribed: version
agreement between CMake and the player-facing notes, the three-way
kFrameLimitHelp comparison, occurrence counts, step ordering, the structural
regexes over CMake and the packagers' archive manifests, the no-write output
guards executed as subprocesses, and the tests/README coverage sweep.
"""

from __future__ import annotations

import plistlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from ci_contract_manifest import CONTROL_GROUPS, PIN_GROUPS, RELEASE_TAG, VERSION
from harness_utils import ABORT_MARKERS, fatal_re


ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "correctness.yml"
MACOS_WORKFLOW = ROOT / ".github" / "workflows" / "macos-release.yml"
DESKTOP_RELEASE_WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
WEB_DEMO_WORKFLOW = ROOT / ".github" / "workflows" / "web-demo.yml"
WINDOWS_VALIDATE_WORKFLOW = (
    ROOT / ".github" / "workflows" / "windows-validate.yml"
)
CI_LOCAL = ROOT / "tools" / "ci" / "ci_local.sh"


def run_checks_manifest():
    """tools/run_checks.py is the one place task names and roles are defined."""
    import importlib.util

    spec = importlib.util.spec_from_file_location("mdkr_run_checks", RUN_CHECKS)
    module = importlib.util.module_from_spec(spec)
    # @dataclass resolves its own module out of sys.modules while the class body
    # executes, so the module has to be registered before it is run.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module
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
RELEASE_NOTES = ROOT / "RELEASE_NOTES.md"
TESTS = ROOT / "tests"
TESTS_README = TESTS / "README.md"
PINNED_ACTION_RE = re.compile(
    r"^\s*(?:-\s+)?uses:\s+([^@\s]+)@([^\s#]+)", re.MULTILINE
)
CLEAN_FAILURE_FATAL_RE = fatal_re(*ABORT_MARKERS)

# Every file the contract reads, under the one name the manifest's `file` field
# and the positive controls both use. One name per file, so a pin on the macOS
# release workflow and a control that breaks it cannot drift apart.
SOURCES = {
    "correctness": WORKFLOW,
    "macos_release": MACOS_WORKFLOW,
    "desktop_release": DESKTOP_RELEASE_WORKFLOW,
    "web_demo": WEB_DEMO_WORKFLOW,
    "windows_validate": WINDOWS_VALIDATE_WORKFLOW,
    "ci_local": CI_LOCAL,
    "source_archive": SOURCE_ARCHIVE_SMOKE,
    "run_checks": RUN_CHECKS,
    "windows_packager": WINDOWS_PACKAGER,
    "windows_import_guard": WINDOWS_IMPORT_GUARD,
    "linux_packager": LINUX_PACKAGER,
    "provenance_stamp": PROVENANCE_STAMP,
    "provenance_verify": PROVENANCE_VERIFY,
    "builder": MACOS_BUILDER,
    "signer": MACOS_SIGNER,
    "notary": MACOS_NOTARY,
    "dmg": MACOS_DMG,
    "sdl_builder": MACOS_SDL_BUILDER,
    "unsigned_verify": MACOS_UNSIGNED_VERIFY,
    "unsigned_dmg_verify": MACOS_UNSIGNED_DMG_VERIFY,
    "mount_helper": MACOS_DMG_MOUNT_HELPER,
    "launch_probe": MACOS_LAUNCHSERVICES_PROBE,
    "bundle_verify": MACOS_BUNDLE_VERIFY,
    "asset_verify": MACOS_ASSET_VERIFY,
    "macos_provenance": MACOS_PROVENANCE,
    "sdl_config": MACOS_SDL_CONFIG,
    "entitlements": MACOS_ENTITLEMENTS,
    "info_plist": MACOS_INFO_PLIST,
    "macos_readme": MACOS_README,
    "app_pacing": APP_PACING_CHECK,
    "cmake": CMAKE_PROJECT,
    "ui_settings": UI_SETTINGS,
    "checklist": RELEASE_CHECKLIST,
    "release_notes": RELEASE_NOTES,
    "tests_readme": TESTS_README,
}


def read_sources() -> dict[str, str]:
    """Read every contract source once, under its manifest name."""
    return {
        name: path.read_text(encoding="utf-8") for name, path in SOURCES.items()
    }


def source_reader(sources: dict[str, str]):
    """A `(name, active_shell) -> text` reader over one set of sources.

    `active_shell` selects the comment-stripped view, so a pin can require that
    a command is actually executed rather than merely mentioned in a comment.
    The stripped view is computed at most once per file per validation pass.
    """
    stripped: dict[str, str] = {}

    def read(name: str, active_shell: bool) -> str:
        if not active_shell:
            return sources[name]
        if name not in stripped:
            stripped[name] = active_shell_source(sources[name])
        return stripped[name]

    return read


def pinned(group: str, sources: dict[str, str]) -> list[str]:
    """The manifest pins in `group` that these sources no longer satisfy."""
    return PIN_GROUPS[group].failures(source_reader(sources))


def check_controls(group: str, sources: dict[str, str], validate_fn) -> None:
    """Fail if any of the group's deliberate breaks slips past its gate."""
    controls = CONTROL_GROUPS[group]
    escaped = [
        control.name
        for control in controls.controls
        if not validate_fn(control.apply(sources))
    ]
    if escaped:
        raise AssertionError(controls.summary + ", ".join(escaped))


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


def validate(sources: dict[str, str]) -> list[str]:
    """The correctness workflow's pinned policy, plus its immutable action pins."""
    source = sources["correctness"]
    failures = pinned("workflow_required", sources)
    actions = PINNED_ACTION_RE.findall(source)
    if not actions:
        failures.append("workflow has no actions")
    for action, revision in actions:
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            failures.append(
                f"{action} is not pinned to an immutable 40-character SHA: "
                f"{revision!r}"
            )
    failures.extend(pinned("workflow_forbidden", sources))
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


def release_notes_version(source: str) -> str | None:
    """The version RELEASE_NOTES.md's newest section is written for."""
    match = re.search(
        r"^#\s+Golden Balloon\s+(\d+\.\d+\.\d+)\s*$", source, re.MULTILINE
    )
    return None if match is None else match.group(1)


def validate_release_notes_version(source: str) -> list[str]:
    """The build's version and the player-facing notes' title must agree.

    A version bump with no matching notes section ships an installer named for a
    release nothing describes, and notes written ahead of the bump ship a build
    that claims to be the previous release.
    """

    notes_version = release_notes_version(source)
    if notes_version is None:
        return [
            "RELEASE_NOTES.md has no '# Golden Balloon <version>' title; "
            f"CMakeLists.txt sets MDKR_VERSION {VERSION}"
        ]
    if notes_version != VERSION:
        return [
            f"version disagreement: CMakeLists.txt sets MDKR_VERSION "
            f"{VERSION}, but RELEASE_NOTES.md's newest section is titled "
            f"'Golden Balloon {notes_version}'. Bump both together."
        ]
    return []


def frame_limit_help(ui_settings: str) -> str | None:
    """kFrameLimitHelp's text, reassembled from its adjacent string literals."""
    # The literals themselves contain ';', so the statement's own terminator is
    # only recognisable as the one that follows the final closing quote.
    match = re.search(
        r'constexpr\s+const\s+char\s*\*\s*kFrameLimitHelp\s*='
        r'(?P<body>(?:\s*"(?:[^"\\]|\\.)*")+)\s*;',
        ui_settings,
    )
    if match is None:
        return None
    chunks = re.findall(r'"((?:[^"\\]|\\.)*)"', match.group("body"))
    if not chunks:
        return None
    return "".join(chunks).replace('\\"', '"').replace("\\\\", "\\")


def validate_frame_limit_help_pins(sources: dict[str, str]) -> list[str]:
    """Three files quote the frame-limit help; none may drift from the source.

    The help text lives once, in platform/app/ui_settings.cpp. CMakeLists.txt
    pins it as the app_schema CTest's PASS_REGULAR_EXPRESSION and
    macos/Scripts/verify_unsigned_release.sh pins it against the packaged app.
    Both copies were hand-maintained and both had already gone stale, and the
    CTest one failed open: CTest splits a PASS_REGULAR_EXPRESSION on unescaped
    semicolons, so the trailing fragment alone kept the test green. Regenerating
    both from the one definition is what makes them controls again.
    """

    failures: list[str] = []
    help_text = frame_limit_help(sources["ui_settings"])
    if help_text is None:
        return ["kFrameLimitHelp is missing from platform/app/ui_settings.cpp"]
    if "(" in help_text or ")" in help_text:
        failures.append(
            "kFrameLimitHelp gained a parenthesis: the CTest pin below is a "
            "regex and would need it escaped"
        )
    # One literal ';' at a time: CTest would read each side as its own regex.
    expected_regex = (
        'recommended=\\"Original \\\\(recommended\\\\)\\" '
        'group=\\"Higher refresh rates\\" '
        'caveat=\\"' + help_text.replace(";", ".") + '\\"'
    )
    if expected_regex not in sources["cmake"]:
        failures.append(
            "MDKR_FRAME_LIMIT_UI_CONTRACT_REGEX does not quote kFrameLimitHelp; "
            "expected caveat text: " + help_text.replace(";", ".")
        )
    expected_packaged = (
        '[app] frame-limit UI contract: recommended="Original (recommended)" '
        'group="Higher refresh rates" caveat="' + help_text + '"'
    )
    if expected_packaged not in sources["unsigned_verify"]:
        failures.append(
            "verify_unsigned_release.sh does not pin the current "
            "kFrameLimitHelp; expected line: " + expected_packaged
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

    failures.extend(pinned("gpu_routing", sources))
    for value in (
            "display", "display-margin", "30", "40", "60", "90", "120", "144",
            "165", "240", "uncapped"):
        if f'{{"{value}",' not in sources["ui_settings"]:
            failures.append(f"native frame-limit option {value!r} is missing")
    failures.extend(validate_frame_limit_help_pins(sources))
    ctest_route = re.search(
        r'if check\.role == "ctest":(?P<body>.*?)\n\s*cmd =',
        sources["run_checks"],
        re.DOTALL,
    )
    if ctest_route is None or '"--output-on-failure"' not in ctest_route.group("body"):
        failures.append("run_checks ROM-free CTest route is missing")
    elif '-LE' in ctest_route.group("body"):
        failures.append(
            "run_checks CTest route must not exclude labels; GPU-labelled "
            "tests run inside rom_free_units again"
        )
    # The --with-* flags survive only as accepted no-ops: a bare run covers
    # every class, because desktop safety is a window-layer property rather
    # than a refusal to execute.
    for flag in ("--with-gpu-tests", "--with-browser-tests", "--with-app-tests",
                 "--with-compiled-tests"):
        if flag not in sources["run_checks"]:
            failures.append(f"run_checks must still accept {flag}")
    if "deprecated no-op" not in sources["run_checks"]:
        failures.append(
            "run_checks must document that the --with-* flags no longer gate"
        )
    for removed in ('MDKR_DEDICATED_TEST_DESKTOP',
                    'environment["MDKR_APP_TESTS_ALLOWED"] = "1"',
                    'environment["MDKR_BROWSER_TESTS_ALLOWED"] = "1"',
                    'if app_checks and not args.with_app_tests:',
                    'command += ["-LE", "gpu|app_process|browser"]'):
        if removed in sources["run_checks"]:
            failures.append(f"run_checks must no longer gate on {removed}")
    if '-DMDKR_ENABLE_GPU_TESTS="$RUN_GPU_TESTS"' not in sources["ci_local"]:
        failures.append("local CI configure does not honor its GPU opt-in")
    if 'JOBS="${MDKR_CI_JOBS:-2}"' not in sources["ci_local"]:
        failures.append("local CI does not default to bounded build concurrency")
    if ('RUN_COMPILED_TESTS=0' not in sources["ci_local"] or
        '--with-compiled-tests' not in sources["ci_local"] or
        'if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ "$RUN_COMPILED_TESTS" -eq 1 ]; then'
            not in sources["ci_local"]):
        failures.append("local CI does not default to zero compiled-test execution")
    if 'if current_nice < 10:' not in sources["run_checks"]:
        failures.append("run_checks does not lower child CPU priority")
    if '["taskpolicy", "-b", "-p", str(os.getpid())]' not in sources["run_checks"]:
        failures.append("run_checks does not enter macOS background scheduling policy")
    # --jobs serialization policy. The workstation-safety guarantee is a
    # window-layer property (hidden surfaces, background policy, lowered
    # priority — all asserted elsewhere in this contract), NOT wholesale
    # serialization of every engine role. So the contract now pins the precise
    # policy: the roles that share ONE build tree or a fixed local port stay
    # serial, and the CPU-bound rom/native/release/asan checks pool with only
    # their render/GPU/measurement members pulled out by name. Pinning it both
    # ways catches a regression to blanket serialization (which erases the
    # speedup) AND an accidental parallelization of a GPU gate (which flakes).
    serial_roles_block = sources["run_checks"].split(
        "SERIAL_ROLES = frozenset({", 1)[1].split("})", 1)[0]
    for role in ("ctest", "instrumented", "layout",
                 "wasm", "browser", "browser_save", "browser_local"):
        if f'"{role}",' not in serial_roles_block:
            failures.append(
                f"run_checks must serialize build-tree/port-sharing role {role}"
            )
    for role in ("rom", "native", "release", "asan"):
        if f'"{role}",' in serial_roles_block:
            failures.append(
                f"run_checks must NOT serialize role {role} wholesale; its "
                "CPU-bound checks pool and only its GPU/measurement checks are "
                "named in GPU_SERIAL_NAMES/SERIAL_NAMES"
            )
    # The GPU classification must exist and be fail-closed: a marker-scan
    # backstop in validate_manifest keeps a pooled engine check that reads
    # pixels or drives a GPU surface from silently rejoining the pool.
    if "GPU_SERIAL_NAMES = frozenset({" not in sources["run_checks"]:
        failures.append(
            "run_checks must curate GPU_SERIAL_NAMES for render/GPU checks")
    for representative in ('"framed_world_views"', '"world_shadows"',
                           '"render_purity"', '"taj_character_select"'):
        if representative not in sources["run_checks"]:
            failures.append(
                f"GPU_SERIAL_NAMES must serialize render gate {representative}")
    if "GPU_VERDICT_MARKERS" not in sources["run_checks"]:
        failures.append(
            "run_checks must keep the fail-closed GPU-marker backstop that "
            "proves no pooled engine check reads rendered pixels")
    for hint in ("SDL_MAC_BACKGROUND_APP", "SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN"):
        if f'"{hint}": "1"' not in sources["run_checks"]:
            failures.append(f"run_checks omits SDL focus fail-safe {hint}=1")
        if sources["ci_local"].count(f"{hint}=1") != 2:
            failures.append(f"both local CTest lanes must set {hint}=1")
    if sources["ci_local"].count("MDKR64_HIDDEN=1 MDKR_AUDIO=0") != 2:
        failures.append(
            "both local CTest lanes must force background, silent automation"
        )
    if ("RUN_GPU_TESTS=0" not in sources["ci_local"] or
            "MDKR_CI_WITH_GPU_TESTS" in sources["ci_local"]):
        failures.append("local CI must reject an ambient GPU opt-in")
    if ("-LE 'gpu|app_process|browser'" not in sources["ci_local"] or
        'taskpolicy -b -p "$$"' not in sources["ci_local"]):
        failures.append(
            "local CTest must exclude app/browser processes and use macOS "
            "background policy by default"
        )
    if ('ENVIRONMENT "MDKR_APP_DUMP_SCHEMA=1;MDKR64_HIDDEN=1;MDKR_AUDIO=0"'
            not in sources["cmake"]):
        failures.append("app_schema must explicitly use background automation")
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


def validate_macos_release(sources: dict[str, str]) -> list[str]:
    """The macOS release workflow's signing, notarization and publication."""
    source = sources["macos_release"]
    active_source = active_shell_source(source)
    failures = pinned("macos_release", sources)
    failures.extend(pinned("macos_release_active", sources))
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
    failures.extend(pinned("macos_release_forbidden", sources))
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
    """The bundle builder, signer, notary, DMG packager and their verifiers."""
    builder = sources["builder"]
    dmg = sources["dmg"]
    entitlements = sources["entitlements"]
    failures = pinned("macos_packaging", sources)
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


def validate_windows_manifest_version(workflow: str) -> list[str]:
    """Run the Windows lane's own normalization and compare it with CMake's.

    The expectation is derived, not transcribed: the accepted version shapes
    come from the validate job's semver regex in this same workflow, the
    normalization comes from the step's own shell, and the answer it must
    produce is CMakeLists.txt's rule -- pad the dotted numeric core to four
    components with zeros. A two-part "1.0" is the case that used to yield
    "1.0..0" against an embedded "1.0.0.0".
    """
    failures: list[str] = []
    accepted = re.search(
        r"\^\[0-9\]\+\(\\\.\[0-9\]\+\)\{(?P<low>\d+),(?P<high>\d+)\}\$",
        workflow,
    )
    if accepted is None:
        return ["release workflow no longer declares its bare-semver shape"]
    normalization = re.search(
        r"^\s*IFS=\. read -r manifest_major manifest_minor manifest_patch"
        r" <<< \"[^\"]*\"\n"
        r"\s*manifest_version=\"(?P<expr>[^\"]*)\"$",
        workflow,
        re.MULTILINE,
    )
    if normalization is None:
        return ["release workflow no longer normalizes the Windows manifest "
                "version in one derivable step"]
    expression = normalization.group("expr")
    script = (
        'IFS=. read -r manifest_major manifest_minor manifest_patch <<< "$1"\n'
        f'printf %s "{expression}"\n'
    )
    for dots in range(int(accepted.group("low")), int(accepted.group("high")) + 1):
        version = ".".join(str(part) for part in range(1, dots + 2))
        expected = ".".join((version.split(".") + ["0", "0", "0", "0"])[:4])
        result = subprocess.run(
            ["bash", "-c", script, "manifest-version", version],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or result.stdout != expected:
            failures.append(
                f"Windows manifest normalization of {version!r} produced "
                f"{result.stdout!r} (exit {result.returncode}), but CMake "
                f"embeds {expected!r}"
            )
    return failures


def validate_desktop_release(sources: dict[str, str]) -> list[str]:
    """Pin portable artifacts, Linux GPU qualification, and Windows CI limits."""
    workflow = sources["desktop_release"]
    windows_packager = sources["windows_packager"]
    linux_packager = sources["linux_packager"]
    provenance_stamp = sources["provenance_stamp"]
    provenance_verify = sources["provenance_verify"]
    failures = pinned("desktop_release", sources)
    failures.extend(validate_windows_manifest_version(workflow))
    windows_manifest = re.search(
        r"expected=\"\$\(printf '%s\\n'(?P<body>.*?)\| LC_ALL=C sort\)\"",
        windows_packager,
        re.DOTALL,
    )
    expected_windows_entries = {
        "GoldenBalloon/",
        "GoldenBalloon/GoldenBalloon.exe",
        "GoldenBalloon/LICENSE",
        "GoldenBalloon/NativePhoneParty-NOTICES.txt",
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
        "Golden-Balloon.AppDir/NativePhoneParty-NOTICES.txt",
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
    failures.extend(pinned("desktop_release_forbidden", sources))
    return failures


def validate_windows_nonpublishing(sources: dict[str, str]) -> list[str]:
    """Keep hosted Windows useful without mistaking it for a GPU release gate."""
    source = sources["windows_validate"]
    active = active_shell_source(source)
    failures = pinned("windows_nonpublishing", sources)
    if "uses: actions/upload-artifact@" in active:
        failures.append(
            "Windows validation publishes an artifact without a stable GPU launcher gate"
        )
    return failures


def validate_web_demo(sources: dict[str, str]) -> list[str]:
    """The independently publishable Pages path must gate its exact payload."""
    source = sources["web_demo"]
    failures = pinned("web_demo", sources)
    # The same pins, in the order the workflow has to reach them.
    ordered = [
        source.find(pin.must_contain) for pin in PIN_GROUPS["web_demo"].pins
    ]
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


def readme_prose(source: str) -> str:
    """The README minus its fenced code blocks.

    A check named only inside a copy-pasteable command block is not documented:
    that is exactly the shape the missing thirteen had. Descriptions live in the
    prose, so the prose is what the manifest is checked against.
    """
    kept: list[str] = []
    fenced = False
    for line in source.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if not fenced:
            kept.append(line)
    return "\n".join(kept)


def validate_tests_readme(source: str) -> list[str]:
    """Every registered check must be described where the suite is documented."""
    manifest = run_checks_manifest()
    prose = readme_prose(source)
    undocumented = sorted(
        check.script for check in manifest.CHECKS
        # Entries name the module with or without its suffix; both are prose.
        if check.script and check.script[: -len(".py")] not in prose
    )
    failures = [
        f"tests/README.md does not describe registered check: {script}"
        for script in undocumented
    ]
    documented = re.findall(r"tests/(check_[a-z0-9_]+\.py)", source)
    registered = {check.script for check in manifest.CHECKS if check.script}
    for script in sorted(set(documented) - registered):
        if (TESTS / script).is_file():
            failures.append(
                f"tests/README.md documents {script}, which run_checks.py does "
                "not register"
            )
    return failures


def validate_release_checklist(sources: dict[str, str]) -> list[str]:
    """Release evidence the checklist must keep asking for."""
    source = sources["checklist"]
    failures: list[str] = []
    # The release evidence is one unrestricted run of the whole manifest. A
    # hand-listed --only recovery pattern is what let a subset masquerade as a
    # full run, so the checklist must not name individual tasks to select them.
    manifest = run_checks_manifest()
    web_roles = set(manifest.BROWSER_ROLES)
    web_tasks = sorted(
        check.name for check in manifest.CHECKS if check.role in web_roles
    )
    if not web_tasks:
        failures.append("run_checks manifest declares no web-lane tasks")
    selector = "--role " + ",".join(manifest.BROWSER_ROLES)
    if selector not in source:
        failures.append(
            f"release checklist must select the web lane by role ({selector!r}), "
            "not by naming its tasks"
        )
    named = sorted(task for task in web_tasks if f"--only {task}" in source
                   or f",{task}," in source or f"--only {task}," in source)
    if named:
        failures.append(
            "release checklist still hand-lists web tasks in an --only "
            f"selection: {', '.join(named)}"
        )
    if "--skip-wasm" in source.split("## 3. Behavioural regression suite")[0]:
        failures.append(
            "release checklist's complete-suite run must not skip the web lane"
        )
    if "python3 tools/run_checks.py --jobs 6" not in source:
        failures.append(
            "release checklist's complete-suite run must be the whole suite"
        )
    if "ctest --test-dir build-rel -L gpu --output-on-failure" not in source:
        failures.append("release checklist omits the gpu-labelled CTest lane")
    if "After the final Developer ID signatures and stapling" not in source:
        failures.append("release checklist omits signed post-sign runtime gate")
    if source.count(f"-f release_tag={RELEASE_TAG}") < 2:
        failures.append(
            "release checklist must bind both portable and macOS publication "
            f"to {RELEASE_TAG}"
        )
    if source.count(f"--ref {RELEASE_TAG}") < 2:
        failures.append(
            "release checklist must dispatch both portable and macOS publication "
            f"from the exact {RELEASE_TAG} ref"
        )
    failures.extend(pinned("release_checklist", sources))
    return failures


def main() -> int:
    sources = read_sources()
    failures = validate(sources)
    failures.extend(validate_app_source_manifest(sources["cmake"]))
    failures.extend(validate_desktop_release(sources))
    failures.extend(validate_windows_nonpublishing(sources))
    failures.extend(validate_web_demo(sources))
    failures.extend(validate_macos_release(sources))
    failures.extend(validate_gpu_test_routing(sources))
    failures.extend(validate_macos_packaging(sources))
    failures.extend(validate_output_guard(MACOS_BUILDER))
    failures.extend(validate_dmg_output_guard(MACOS_DMG))
    failures.extend(validate_sdl_path_guard(MACOS_SDL_BUILDER))
    failures.extend(validate_release_checklist(sources))
    failures.extend(validate_tests_readme(sources["tests_readme"]))
    failures.extend(validate_release_notes_version(sources["release_notes"]))
    if failures:
        raise AssertionError("CI contract drift:\n  " + "\n  ".join(failures))

    # A gate that cannot reject regressions is documentation, not a control.
    # Each control in the manifest breaks exactly one pin; the matching
    # validator has to notice.
    check_controls("ci", sources, validate)

    # The app-source manifest's controls also vary the reviewed source list the
    # validator is handed, so they stay expressed as code rather than as data.
    cmake_source = sources["cmake"]
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

    check_controls("gpu_routing", sources, validate_gpu_test_routing)
    check_controls("windows_validate", sources, validate_windows_nonpublishing)
    check_controls("desktop_release", sources, validate_desktop_release)
    check_controls("web_demo", sources, validate_web_demo)
    check_controls("macos_release", sources, validate_macos_release)
    check_controls("macos_packaging", sources, validate_macos_packaging)
    check_controls("release_checklist", sources, validate_release_checklist)
    check_controls(
        "release_notes",
        sources,
        lambda broken: validate_release_notes_version(broken["release_notes"]),
    )

    action_count = len(PINNED_ACTION_RE.findall(sources["correctness"]))
    macos_action_count = len(PINNED_ACTION_RE.findall(sources["macos_release"]))
    desktop_action_count = len(
        PINNED_ACTION_RE.findall(sources["desktop_release"])
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
