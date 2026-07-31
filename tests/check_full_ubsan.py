#!/usr/bin/env python3
"""Optimized, fail-closed UBSan coverage over broad production routes.

The focused array-bounds sweep intentionally enables only three sanitizer
families and allow-lists documented decomp idioms. This gate answers the wider
question: does an optimized build encounter *any* C undefined behavior while
traversing the broad content census, all 47 legal track/vehicle combinations,
3P/4P completion, stateful challenge results, first-save filename UI, and a
long race?

It builds RelWithDebInfo with the selected compiler's full ``undefined`` group
plus an explicit float-cast check (GCC omits that class from the group), proves
the two classes that motivated this gate can actually abort, verifies the
linked handlers, and runs every route with ``halt_on_error=1``. There is no
diagnostic allow-list: one report is a failure.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = ROOT / "build-ubsan-full"
# GCC's ``undefined`` group does not include float-to-integer overflow, while
# Clang's does. Name it explicitly so the gate has the same coverage on both
# compiler families.
SANITIZER_FLAGS = (
    "-fsanitize=undefined,float-cast-overflow -fno-omit-frame-pointer"
)
REQUIRED_HANDLERS = (
    "__ubsan_handle_float_cast_overflow",
    "__ubsan_handle_out_of_bounds",
    "__ubsan_handle_pointer_overflow",
    "__ubsan_handle_shift_out_of_bounds",
    "__ubsan_handle_type_mismatch_v1",
)
BAD_RE = re.compile(
    r"runtime error:|SUMMARY: UndefinedBehaviorSanitizer|"
    r"UndefinedBehaviorSanitizer:DEADLYSIGNAL"
)


def run(
    command: list[str],
    *,
    env: dict[str, str] | None = None,
    timeout: int = 900,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        check=False,
    )


def configure_and_build(
    build: Path,
    *,
    compiler: str | None,
    optimization: str | None,
    gl_only: bool,
) -> bool:
    sanitizer_flags = SANITIZER_FLAGS
    if optimization is not None:
        sanitizer_flags = f"-{optimization} {sanitizer_flags}"
    configure_env = dict(os.environ)
    if compiler is not None:
        configure_env["CC"] = compiler
    configure = run(
        [
            "cmake",
            "-S",
            str(ROOT),
            "-B",
            str(build),
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            f"-DMDKR_EXTRA_C_FLAGS={sanitizer_flags}",
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined,float-cast-overflow",
            f"-DMDKR_WEBGPU_BACKEND={'OFF' if gl_only else 'ON'}",
        ],
        env=configure_env,
    )
    if configure.returncode != 0:
        print("check_full_ubsan: configure failed")
        print(configure.stdout[-8000:])
        return False
    build_result = run(
        ["cmake", "--build", str(build), f"-j{os.cpu_count() or 4}"],
        timeout=1200,
    )
    if build_result.returncode != 0:
        print("check_full_ubsan: build failed")
        print(build_result.stdout[-8000:])
        return False
    return True


def compiler_from_cache(build: Path) -> str | None:
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("CMAKE_C_COMPILER:FILEPATH="):
            return line.split("=", 1)[1]
    return None


def handlers_are_linked(binary: Path) -> bool:
    symbols = run(["nm", "-u", str(binary)])
    missing = [name for name in REQUIRED_HANDLERS if name not in symbols.stdout]
    if missing:
        print("check_full_ubsan: missing linked handlers: " + ", ".join(missing))
        return False
    return True


def positive_controls(build: Path, optimization: str | None) -> bool:
    compiler = compiler_from_cache(build)
    if compiler is None:
        print("check_full_ubsan: cannot resolve C compiler from CMakeCache.txt")
        return False
    source = ROOT / "tests/fixtures/full_ubsan_positive_control.c"
    with tempfile.TemporaryDirectory(prefix="mdkr_full_ubsan_control_") as temp:
        binary = Path(temp) / "control"
        compiled = run(
            [
                compiler,
                str(source),
                f"-{optimization or 'O2'}",
                "-g",
                "-fsanitize=undefined,float-cast-overflow",
                "-fno-sanitize-recover=all",
                "-o",
                str(binary),
            ]
        )
        if compiled.returncode != 0:
            print("check_full_ubsan: positive-control compile failed")
            print(compiled.stdout)
            return False
        expected = {
            "shift": "left shift of negative value",
            "float-cast": "outside the range of representable values",
        }
        for arm, needle in expected.items():
            result = run(
                [str(binary), arm],
                env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1"),
            )
            if result.returncode == 0 or needle not in result.stdout:
                print(
                    f"check_full_ubsan: {arm} positive control did not abort "
                    f"with {needle!r}"
                )
                print(result.stdout)
                return False
    return True


def checked_route(
    label: str,
    command: list[str],
    environment: dict[str, str],
    *,
    timeout: int = 900,
) -> bool:
    try:
        result = run(command, env=environment, timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f"check_full_ubsan: {label} timed out")
        return False
    diagnostic = BAD_RE.search(result.stdout)
    if result.returncode != 0 or diagnostic:
        reason = (
            f"exit {result.returncode}"
            if result.returncode != 0
            else f"diagnostic {diagnostic.group(0)!r}"
        )
        print(f"check_full_ubsan: {label} failed ({reason})")
        print(result.stdout[-12000:])
        return False
    print(f"  {label}: PASS")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--build-dir",
        default=str(DEFAULT_BUILD),
        help="instrumented build directory (default: build-ubsan-full)",
    )
    parser.add_argument(
        "--compiler",
        help="C compiler used when configuring a new build (for example gcc-16)",
    )
    parser.add_argument(
        "--optimization",
        choices=("O0", "O1", "O2", "O3", "Os"),
        help="explicit optimization appended after CMake's build-type flags",
    )
    parser.add_argument(
        "--gl-only",
        action="store_true",
        help="disable WebGPU and omit its content-census route",
    )
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).expanduser().resolve()
    rom = Path(args.rom).expanduser().resolve()
    if not rom.is_file():
        print(f"check_full_ubsan: missing ROM: {rom}")
        return 2
    if not args.no_build and not configure_and_build(
        build_dir,
        compiler=args.compiler,
        optimization=args.optimization,
        gl_only=args.gl_only,
    ):
        return 1
    binary = build_dir / "mdkr64"
    if not binary.is_file():
        print(f"check_full_ubsan: missing instrumented binary: {binary}")
        return 2
    if not handlers_are_linked(binary) or not positive_controls(
        build_dir, args.optimization
    ):
        return 1

    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("MDKR_")
    }
    environment.update(
        MDKR_AUDIO="0",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
        PYTHONUNBUFFERED="1",
    )
    python = sys.executable
    routes = [
        (
            "challenge win/loss and result lifecycle",
            [
                python,
                str(ROOT / "tests/check_challenge_modes.py"),
                "--build",
                str(binary),
                "--rom",
                str(rom),
            ],
            900,
        ),
        (
            "47 legal track/vehicle combinations",
            [
                python,
                str(ROOT / "tests/check_vehicle_sweep.py"),
                "--build",
                str(binary),
                "--rom",
                str(rom),
            ],
            1200,
        ),
        (
            "3P/4P race and results lifecycle",
            [
                python,
                str(ROOT / "tests/check_race_multiplayer.py"),
                "--build",
                str(binary),
                "--rom",
                str(rom),
            ],
            600,
        ),
        (
            "first-save filename entry",
            [
                python,
                str(ROOT / "tests/check_filename_entry.py"),
                "--build",
                str(binary),
                "--rom",
                str(rom),
            ],
            300,
        ),
    ]
    if not args.gl_only:
        routes.insert(
            0,
            (
                "46-route WebGPU content census",
                [
                    python,
                    str(ROOT / "tests/check_webgpu_content_census.py"),
                    "--build",
                    str(binary),
                    "--rom",
                    str(rom),
                ],
                900,
            ),
        )
    for label, command, timeout in routes:
        if not checked_route(label, command, environment, timeout=timeout):
            return 1

    with tempfile.TemporaryDirectory(prefix="mdkr_full_ubsan_race_") as save_dir:
        race_env = dict(
            environment,
            MDKR_SAVE_DIR=save_dir,
            MDKR_RENDERER="gl",
            MDKR_AUTOPILOT="1",
        )
        if not checked_route(
            "7,500-frame race/result soak",
            [
                str(binary),
                "--headless-frames",
                "7500",
                "--input-script",
                str(ROOT / "tests/input_scripts/nav_to_time_trial_race.txt"),
                "--rom",
                str(rom),
            ],
            race_env,
            timeout=600,
        ):
            return 1

    census = "46-route census, " if not args.gl_only else ""
    print(
        "check_full_ubsan: PASS — optimized full-undefined build, positive "
        f"controls, {census}47 legal vehicles, 3P/4P, stateful challenges, "
        "filename UI, and 7,500-frame race"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
