#!/usr/bin/env python3
"""Prove native object/record layout safety in both directions.

This gate closes three related LP64 defects:

* MEM-02: Object's variable native tail was laid out with N64 four-byte
  pointer/alignment assumptions.
* MEM-03: variable-length level-object records can begin at 2 mod 4, but one
  serialized view previously required four-byte alignment and record boundaries
  were trusted before typed access.
* MEM-04: HUD/weather/particle helpers were cast to larger Object prefixes, so
  renderers read fields beyond the real source object.

The fixed arms alone would only prove that today's routes happen not to trip a
diagnostic. The three legacy arms recreate each exact old access and MUST be
rejected by the corresponding sanitizer. The full arm then drives menus, all
tracks, every legal track/vehicle pair, both Adventure directions, a boss,
two-/three-/four-player split screen, and the widescreen/FOV pixel fixture under alignment
UBSan.

Usage:
    python3 tests/check_native_layout.py --rom baserom.us.v80.z64
    python3 tests/check_native_layout.py --quick
    python3 tests/check_native_layout.py --no-build
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"
SANITIZER_RE = re.compile(
    r"runtime error:|AddressSanitizer|UndefinedBehaviorSanitizer"
)


@dataclass(frozen=True)
class RuntimeCheck:
    label: str
    script: str
    timeout: int


RUNTIME_CHECKS = (
    RuntimeCheck("menu routes", "check_nav_fixtures.py", 1800),
    RuntimeCheck("attract demos", "check_attract_demo.py", 1800),
    RuntimeCheck("all tracks", "check_track_sweep.py", 3600),
    RuntimeCheck("all legal vehicles", "check_vehicle_sweep.py", 7200),
    RuntimeCheck("Adventure hub", "check_adventure_hub.py", 1800),
    RuntimeCheck("Adventure race loop", "check_adventure_race_loop.py", 2400),
    RuntimeCheck("Adventure trophy series", "check_trophy_series.py", 3600),
    RuntimeCheck("Adventure Two", "check_adventure_two.py", 3600),
    RuntimeCheck("boss/object collision", "check_collision_gridmask.py", 2400),
    RuntimeCheck("two-player split screen", "check_race_2p_split.py", 1800),
    RuntimeCheck(
        "three-/four-player split screen",
        "check_race_multiplayer.py",
        1800,
    ),
    RuntimeCheck(
        "challenge/battle modes",
        "check_challenge_modes.py",
        1800,
    ),
    RuntimeCheck(
        "Taj vehicle challenges",
        "check_taj_challenges.py",
        2400,
    ),
    RuntimeCheck(
        "widescreen/FOV proportions",
        "check_widescreen_proportions.py",
        2400,
    ),
)


def resolve_path(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


def run(
    command: list[str],
    *,
    environment: dict[str, str] | None = None,
    timeout: int = 1800,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    print("$ " + " ".join(command), flush=True)
    return subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        timeout=timeout,
        check=False,
    )


def configure_and_build(
    align_build: Path,
    asan_build: Path,
    no_build: bool,
) -> list[str]:
    failures: list[str] = []
    if no_build:
        return failures

    configure_align = [
        "cmake",
        "-S",
        str(ROOT),
        "-B",
        str(align_build),
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DMDKR_EXTRA_C_FLAGS=-fsanitize=alignment -fno-omit-frame-pointer",
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=alignment",
    ]
    proc = run(configure_align)
    if proc.returncode != 0:
        return [f"alignment CMake configure exited {proc.returncode}"]

    proc = run(
        [
            "cmake",
            "--build",
            str(align_build),
            "--target",
            "mdkr64",
            "mdkr_object_layout_test",
            "--parallel",
        ],
        timeout=3600,
    )
    if proc.returncode != 0:
        return [f"alignment build exited {proc.returncode}"]

    if not (asan_build / "CMakeCache.txt").is_file():
        proc = run(
            [
                "cmake",
                "-S",
                str(ROOT),
                "-B",
                str(asan_build),
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DCMAKE_C_FLAGS=-fsanitize=address -g -O1 "
                "-fno-omit-frame-pointer",
                "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address",
            ]
        )
        if proc.returncode != 0:
            return [f"ASan CMake configure exited {proc.returncode}"]

    proc = run(
        [
            "cmake",
            "--build",
            str(asan_build),
            "--target",
            "mdkr_object_layout_test",
            "--parallel",
        ],
        timeout=1800,
    )
    if proc.returncode != 0:
        failures.append(f"ASan object-layout test build exited {proc.returncode}")
    return failures


def imported_symbols(binary: Path) -> str:
    nm = shutil.which("nm")
    if nm is None:
        return ""
    proc = subprocess.run(
        [nm, "-u", str(binary)],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    return proc.stdout if proc.returncode == 0 else ""


def verify_instrumentation(
    align_binary: Path,
    align_unit: Path,
    asan_unit: Path,
) -> list[str]:
    failures: list[str] = []
    for label, path in (
        ("alignment game", align_binary),
        ("alignment unit", align_unit),
        ("ASan unit", asan_unit),
    ):
        if not path.is_file():
            failures.append(f"{label} is missing: {path}")

    if failures:
        return failures

    align_symbols = imported_symbols(align_binary)
    align_unit_symbols = imported_symbols(align_unit)
    asan_symbols = imported_symbols(asan_unit)
    if "ubsan_handle_type_mismatch" not in align_symbols:
        failures.append(
            "alignment game does not import a UBSan type-mismatch handler"
        )
    if "ubsan_handle_type_mismatch" not in align_unit_symbols:
        failures.append(
            "alignment unit does not import a UBSan type-mismatch handler"
        )
    if "asan_init" not in asan_symbols:
        failures.append("ASan unit does not import __asan_init")
    return failures


def sanitizer_environment(kind: str, recover: bool = False) -> dict[str, str]:
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("MDKR")
    }
    environment.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_NO_CRASH_HANDLER="1",
        UBSAN_OPTIONS=(
            "halt_on_error=0:print_stacktrace=0"
            if recover
            else "halt_on_error=1:print_stacktrace=1"
        ),
        ASAN_OPTIONS="halt_on_error=1:detect_leaks=0:symbolize=1",
    )
    if kind == "asan":
        environment.pop("UBSAN_OPTIONS", None)
    return environment


def require_clean_fixed_arm(
    binary: Path,
    environment: dict[str, str],
    label: str,
) -> list[str]:
    proc = run(
        [str(binary)],
        environment=environment,
        timeout=120,
        capture=True,
    )
    output = proc.stdout or ""
    failures: list[str] = []
    if proc.returncode != 0:
        failures.append(f"{label} fixed arm exited {proc.returncode}")
    if SANITIZER_RE.search(output):
        failures.append(f"{label} fixed arm emitted a sanitizer diagnostic")
    if "object_layout: all checks passed" not in output:
        failures.append(f"{label} fixed arm omitted its success marker")
    if failures:
        print(output, end="")
    return failures


def require_alignment_control(
    binary: Path,
    argument: str,
    type_name: str,
    finding: str,
) -> list[str]:
    proc = run(
        [str(binary), argument],
        environment=sanitizer_environment("alignment", recover=True),
        timeout=120,
        capture=True,
    )
    output = proc.stdout or ""
    failures: list[str] = []
    if "runtime error:" not in output:
        failures.append(f"{finding} legacy arm emitted no alignment diagnostic")
    if type_name not in output:
        failures.append(
            f"{finding} legacy diagnostic did not name {type_name}"
        )
    if failures:
        print(output, end="")
    else:
        count = output.count("runtime error:")
        print(f"  {finding} legacy control: {count} alignment report(s)")
    return failures


def require_asan_control(binary: Path) -> list[str]:
    proc = run(
        [str(binary), "--legacy-mem04"],
        environment=sanitizer_environment("asan"),
        timeout=120,
        capture=True,
    )
    output = proc.stdout or ""
    failures: list[str] = []
    if proc.returncode == 0:
        failures.append("MEM-04 legacy arm unexpectedly exited zero")
    if "AddressSanitizer: stack-buffer-overflow" not in output:
        failures.append("MEM-04 legacy arm emitted no ASan stack over-read")
    if "legacy_renderer_read_anim_frame" not in output:
        failures.append(
            "MEM-04 ASan diagnostic did not identify the renderer-prefix read"
        )
    if failures:
        print(output, end="")
    else:
        print("  MEM-04 legacy control: ASan rejected the renderer-prefix read")
    return failures


def native_active_source(path: Path) -> str:
    """Select NATIVE_PORT branches while conservatively retaining other #ifs."""

    output: list[str] = []
    # (parent active, exact value for the first branch; None means unrelated)
    stack: list[tuple[bool, bool | None]] = []
    active = True

    for line in path.read_text(encoding="utf-8").splitlines(keepends=True):
        directive = line.lstrip()
        selector: bool | None = None
        if directive.startswith("#ifdef NATIVE_PORT"):
            selector = True
        elif directive.startswith("#ifndef NATIVE_PORT"):
            selector = False
        elif re.match(r"#if\s+defined\s*\(\s*NATIVE_PORT\s*\)", directive):
            selector = True
        elif re.match(
            r"#if\s+!\s*defined\s*\(\s*NATIVE_PORT\s*\)", directive
        ):
            selector = False

        if directive.startswith(("#if ", "#ifdef ", "#ifndef ")):
            parent = active
            stack.append((parent, selector))
            active = parent and (selector if selector is not None else True)
            continue
        if directive.startswith("#else"):
            if not stack:
                raise ValueError(f"{path}: unmatched #else")
            parent, first_value = stack[-1]
            active = parent and (
                not first_value if first_value is not None else True
            )
            continue
        if directive.startswith("#elif"):
            if not stack:
                raise ValueError(f"{path}: unmatched #elif")
            parent, first_value = stack[-1]
            # Relevant sources use direct #ifdef/#else pairs. Conservatively
            # retain unrelated elif branches, but suppress an elif after a
            # known-true native branch.
            active = parent and (first_value is not True)
            continue
        if directive.startswith("#endif"):
            if not stack:
                raise ValueError(f"{path}: unmatched #endif")
            active, _ = stack.pop()
            continue
        if active:
            output.append(line)

    if stack:
        raise ValueError(f"{path}: unterminated preprocessor conditional")
    return "".join(output)


def verify_renderer_source_contract() -> list[str]:
    files = (
        "game/src/game_ui.c",
        "game/src/menu.c",
        "game/src/objects.c",
        "game/src/particles.c",
        "game/src/weather.c",
    )
    source = "\n".join(native_active_source(ROOT / name) for name in files)
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)
    forbidden = (
        (
            "Object cast passed to billboard renderer",
            r"render_sprite_billboard\s*\([^;]{0,1200}?"
            r"\(\s*Object\s*\*\s*\)",
        ),
        (
            "ObjectSegment cast passed to ortho renderer",
            r"render_ortho_triangle_image\s*\([^;]{0,1200}?"
            r"\(\s*ObjectSegment\s*\*\s*\)",
        ),
        (
            "Object cast passed to bubble renderer",
            r"render_bubble_trap\s*\([^;]{0,1200}?"
            r"\(\s*Object\s*\*\s*\)",
        ),
        (
            "foreign prefix cast passed to matrix renderer",
            r"mtx_cam_push\s*\([^;]{0,1200}?"
            r"\(\s*ObjectTransform\s*\*\s*\)",
        ),
    )
    failures = [
        label
        for label, pattern in forbidden
        if re.search(pattern, source, flags=re.DOTALL)
    ]
    if not failures:
        print("  MEM-04 source contract: native renderer calls use real fields")
    return failures


def run_runtime_matrix(
    align_build: Path,
    rom: Path,
    quick: bool,
) -> list[str]:
    failures: list[str] = []
    checks = (
        RUNTIME_CHECKS[-1:]
        if quick
        else RUNTIME_CHECKS
    )
    environment = sanitizer_environment("alignment")
    for index, check in enumerate(checks, 1):
        print(
            f"\n[{index}/{len(checks)}] alignment runtime: {check.label}",
            flush=True,
        )
        proc = run(
            [
                sys.executable,
                str(TESTS / check.script),
                "--build",
                str(align_build),
                "--rom",
                str(rom),
            ],
            environment=environment,
            timeout=check.timeout,
        )
        if proc.returncode != 0:
            failures.append(
                f"alignment runtime {check.label} exited {proc.returncode}"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--build", default="build-align")
    parser.add_argument("--asan-build", default="build-asan")
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="reuse already-configured alignment and ASan artifacts",
    )
    parser.add_argument(
        "--quick",
        action="store_true",
        help="run controls plus the widescreen/FOV fixture, not the full matrix",
    )
    args = parser.parse_args()

    rom = resolve_path(args.rom)
    align_build = resolve_path(args.build)
    asan_build = resolve_path(args.asan_build)
    if not rom.is_file():
        print(f"check_native_layout: FAIL - missing ROM: {rom}", file=sys.stderr)
        return 2

    failures = configure_and_build(
        align_build, asan_build, args.no_build
    )
    align_binary = align_build / "mdkr64"
    align_unit = align_build / "mdkr_object_layout_test"
    asan_unit = asan_build / "mdkr_object_layout_test"
    failures += verify_instrumentation(
        align_binary, align_unit, asan_unit
    )
    if failures:
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("check_native_layout: FAIL", file=sys.stderr)
        return 1

    print("\nSanitizer fixed/legacy controls", flush=True)
    failures += require_clean_fixed_arm(
        align_unit,
        sanitizer_environment("alignment"),
        "alignment",
    )
    failures += require_clean_fixed_arm(
        asan_unit,
        sanitizer_environment("asan"),
        "ASan",
    )
    failures += require_alignment_control(
        align_unit,
        "--legacy-mem02",
        "LegacyShadowData",
        "MEM-02",
    )
    failures += require_alignment_control(
        align_unit,
        "--legacy-mem03",
        "LegacyLevelObjectEntryHud",
        "MEM-03",
    )
    failures += require_asan_control(asan_unit)
    try:
        failures += verify_renderer_source_contract()
    except ValueError as exc:
        failures.append(str(exc))

    if not failures:
        failures += run_runtime_matrix(align_build, rom, args.quick)

    if failures:
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(
            f"check_native_layout: FAIL - {len(failures)} problem(s)",
            file=sys.stderr,
        )
        return 1

    mode = "quick" if args.quick else "full"
    print(
        "check_native_layout: PASS - fixed arms clean, all legacy controls "
        f"rejected, {mode} alignment runtime matrix clean"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
