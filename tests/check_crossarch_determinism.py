#!/usr/bin/env python3
"""Cross-architecture determinism: arm64 and x86_64 must agree bit for bit.

Why this exists
---------------
The online compatibility identity (platform/online/compatibility_identity.h) is
derived from version, source commit, source-dirty and ROM revision. It does NOT
carry the CPU architecture. Matchmaking will therefore happily seat an ARM64
macOS player against an x86_64 Windows player in the same lockstep session.

Every determinism instrument this project already owns runs one binary on one
host: check_determinism.py re-runs the same executable, check_state_hash.py
re-runs the same executable, and the rollback convergence gates replay inside a
single process. All of them hold the architecture fixed, so none of them can
observe an architecture-dependent simulation. If the simulation is not
bit-identical across architectures, that is a live desync in shipped online
play, and no gate in the tree would notice.

The measurement is available on one Apple Silicon workstation: build a thin
x86_64 executable next to the native thin arm64 one, drive both through the
same ROM and the same input script, and compare the authoritative [SIMHASH]
streams tick by tick.

What it asserts
---------------
For each route:

1. **Cross-architecture identity.** The x86_64 stream and the arm64 stream are
   byte-identical for every tick. The report names the FIRST divergent tick, its
   two rows, and whether the population (``objs=``) or only the state digest
   moved -- a boolean would leave the next person bisecting from nothing.

2. **The two binaries really are different architectures.** Both are read with
   ``lipo -archs`` and must be thin, must disagree, and must together cover
   arm64 and x86_64. Two arm64 binaries compare identical for a reason that has
   nothing to do with the claim, and that pass would be silent.

3. **The two binaries were configured the same way.** Build type and every
   MDKR_* cache entry must match across the two CMakeCache.txt files. A Release
   arm64 against a Debug x86_64 measures optimisation level, not architecture.

4. **Neither binary is stale.** Both must be newer than the newest tracked
   source file, or the run is comparing two different games and calling the
   difference architectural.

5. **Positive control.** One extra arm64 arm runs with ``MDKR_RNGSEED=legacy``
   and must DIVERGE from the arm64 base stream. Without it, an empty or
   constant stream, a mis-set ``MDKR_STATE_HASH``, or a comparator that always
   returns "identical" passes this gate while proving nothing.

What it does NOT prove
----------------------
- **Not an OS gate.** Both binaries here run on macOS against the same kernel,
  the same dyld, and the same libSystem/libm images (the x86_64 slice of them,
  under Rosetta 2). It says nothing about Windows-vs-macOS, glibc-vs-msvcrt,
  or the wasm lane. A Windows x86_64 peer can still desync against a macOS
  x86_64 peer for reasons this gate cannot see.
- **Not a compiler gate.** Both binaries come from the same AppleClang. A
  divergence caused by GCC or MinGW codegen on the Windows lane is out of
  scope.
- **Not a proof about untravelled code.** It covers exactly the routes it
  drives. Cross-architecture divergence has every reason to be level-shaped:
  the mechanisms that make a simulation host-dependent (uninitialised reads,
  recycled pool bytes, pointer-derived values) are the same ones that made
  levels 41, 37 and 11 fail their own self-consistency arm in
  check_state_hash.py, and each of those needed a level that reaches it. The
  default route set includes them for that reason.
- **Not a claim about Rosetta 2 fidelity.** Rosetta translates x86_64 SSE to
  NEON. Apple documents IEEE-correct scalar semantics, but a divergence found
  here is a divergence between "this arm64 binary" and "this x86_64 binary as
  Rosetta executes it", which is what a Rosetta player would actually run and
  is NOT necessarily what an Intel Mac or a Windows PC would compute. Confirm a
  positive finding on real x86_64 hardware before calling it the whole story.

Usage
-----
    tests/check_crossarch_determinism.py [--build build] \
        [--build-x86 build-x86_64] [--rom baserom.us.v80.z64] \
        [--level 5 --level 37 --level 11] [--frames 3600] [-v]

``--build``/``--build-x86`` take a build directory or the executable inside it,
exactly like every other check. See docs/architecture/cross-architecture-
determinism.md for how to produce the x86_64 tree. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary
# The stream capture, parsing, comparison, report and configuration guards are
# shared with check_previous_release_parity.py; they are re-exported here
# because tests/test_crossarch_determinism.py unit-tests them through this gate.
from simhash_streams import (  # noqa: F401
    HASH_VERSION, SHARED_CACHE_KEYS, SIMHASH_RE, SOURCE_SUFFIXES, SOURCE_TREES,
    SimHashRow, configuration_problems, control_problem, divergence_report,
    first_divergence, newest_source_mtime, parse_cmake_cache,
    parse_simhash_rows, read_cmake_cache, run_hashed, staleness_problems,
    stream_problems)

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"

# The same route and tick budget check_state_hash.py anchors on, so a
# divergence found here is expressed in the same coordinates as every other
# authoritative-state finding in the tree. The field set is simhash_streams.py's
# HASH_VERSION.
DEFAULT_FRAMES = 3600

# Levels whose authoritative state is known to depend on memory the engine did
# not write. They are the measured host-dependence hotspots from
# check_state_hash.py's process-nondeterminism arms, and an architecture change
# moves stack frames, allocation sizes and pool recycling order -- which is
# exactly what those defects read. Level 5 is the anchor route.
DEFAULT_LEVELS = (5, 37, 11)

WINDOW = "320x240"

# --------------------------------------------------------------------------- #
#  Pure helpers (unit-tested by tests/test_crossarch_determinism.py)
# --------------------------------------------------------------------------- #


def parse_lipo_archs(text: str) -> tuple[str, ...]:
    """The architecture slices ``lipo -archs`` named, in the order printed.

    ``lipo -archs`` prints one whitespace-separated line: ``arm64``,
    ``x86_64``, or ``x86_64 arm64`` for a universal binary. Returning a tuple
    rather than a bool keeps the caller able to say WHICH slices it found,
    which is the whole content of the "these are not two architectures"
    failure.
    """

    return tuple(text.split())


# Mach-O slice names to the architecture FAMILY they belong to. Apple ships
# sub-variants (arm64e for pointer-authenticated system binaries, x86_64h for
# Haswell-and-later) that are the same architecture for this gate's purposes:
# the claim being made is "these two binaries are different architectures", and
# arm64e-versus-arm64 does not satisfy it while arm64e-versus-x86_64 does.
# Mapping rather than string-matching also keeps an unexpected slice (i386, a
# future name) failing closed instead of quietly passing a prefix test.
ARCH_FAMILIES = {
    "arm64": "arm64",
    "arm64e": "arm64",
    "arm64_32": "arm64",
    "x86_64": "x86_64",
    "x86_64h": "x86_64",
}


def architecture_family(slice_name: str) -> str | None:
    """The family ``slice_name`` belongs to, or ``None`` when unrecognised."""

    return ARCH_FAMILIES.get(slice_name)


def architecture_problems(arm_archs: tuple[str, ...],
                          x86_archs: tuple[str, ...]) -> list[str]:
    """Reasons the pair cannot support a cross-architecture claim.

    Fail closed on three separate ways this gate could pass vacuously:

    - a universal (fat) binary, because the loader picks the native slice and
      an "x86_64" build would silently run as arm64;
    - a binary that is not the architecture its role requires; and
    - two binaries of the same architecture, which is a self-comparison wearing
      this check's name.
    """

    problems: list[str] = []
    families: list[str | None] = []
    for label, archs, want in (("--build", arm_archs, "arm64"),
                               ("--build-x86", x86_archs, "x86_64")):
        family = None
        if not archs:
            problems.append(
                f"{label}: could not read any architecture from the binary")
        elif len(archs) > 1:
            problems.append(
                f"{label}: universal binary ({' '.join(archs)}). The loader "
                "runs the native slice, so an x86_64 claim here would silently "
                "measure arm64. Configure each tree with a single "
                "CMAKE_OSX_ARCHITECTURES value so both executables are thin")
        else:
            family = architecture_family(archs[0])
            if family is None:
                problems.append(
                    f"{label}: unrecognised architecture {archs[0]!r}; this "
                    "gate only knows how to reason about arm64 and x86_64")
            elif family != want:
                problems.append(
                    f"{label}: binary is {archs[0]} ({family}), expected "
                    f"{want}")
        families.append(family)
    if not problems and families[0] == families[1]:
        problems.append(
            f"both binaries are {families[0]}; this gate would compare a "
            "build against itself and pass without testing anything")
    return problems


# --------------------------------------------------------------------------- #
#  Process-facing helpers
# --------------------------------------------------------------------------- #


def binary_architectures(path: Path) -> tuple[str, ...]:
    """``lipo -archs`` on ``path``; empty when the tool or the file says nothing.

    ``lipo`` ships with the command line tools on every macOS host that can
    build this project, so its absence is reported as an empty result and the
    caller fails closed rather than skipping the assertion.
    """

    lipo = shutil.which("lipo")
    if lipo is None:
        return ()
    proc = subprocess.run([lipo, "-archs", str(path)], text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          check=False)
    if proc.returncode != 0:
        return ()
    return parse_lipo_archs(proc.stdout)


def run_arm(binary: Path, rom: Path, label: str, root: Path, level: int,
            frames: int, extra_env: dict[str, str], timeout: int,
            verbose: bool, script: Path = SCRIPT) -> list[SimHashRow]:
    """One headless run on ``level``; returns its [SIMHASH] stream.

    simhash_streams.run_hashed() owns the hermetic environment and the file
    sink; this gate adds only the retargeted level.
    """

    env = {"MDKR_LOAD_TRACK": str(level)}
    env.update(extra_env)
    return run_hashed(
        binary, rom, root / label, label, frames, script, env, timeout,
        verbose, window=WINDOW,
        timeout_hint="The x86_64 arm runs under Rosetta 2 and is materially "
                     "slower than the native one, so raise --timeout rather "
                     "than reading this as a failure").rows


def rosetta_available() -> bool:
    """Whether this host can execute an x86_64 binary at all.

    A missing Rosetta 2 makes every x86_64 arm die with a bare exec failure.
    Naming that up front beats reporting it once per route as a mystery exit
    code.
    """

    if sys.platform != "darwin":
        return True
    arch = shutil.which("arch")
    if arch is None:
        return True
    proc = subprocess.run([arch, "-x86_64", "/usr/bin/true"],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL, check=False)
    return proc.returncode == 0


# --------------------------------------------------------------------------- #
#  Entry point
# --------------------------------------------------------------------------- #


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR,
                        help="the native arm64 build directory or executable")
    parser.add_argument("--build-x86", default="build-x86_64",
                        help="the thin x86_64 build directory or executable")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--level", action="append", type=int, dest="levels",
                        help="MDKR_LOAD_TRACK level to race (repeatable)")
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    parser.add_argument(
        "--script", default=str(SCRIPT),
        help="input script to drive both architectures through. The default "
             "route is check_state_hash.py's anchor; point this at an "
             "Adventure-party route to cover adventure_party_spawn.c's "
             "authoritative sinf/cosf spawn positions, which the default "
             "route never reaches")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--allow-stale", action="store_true",
                        help="skip the binary-newer-than-source guard")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    arm_binary = Path(os.path.abspath(resolve_binary(args.build)))
    x86_binary = Path(os.path.abspath(resolve_binary(args.build_x86)))
    rom = Path(os.path.abspath(args.rom))
    script = Path(os.path.abspath(args.script))
    levels = args.levels or list(DEFAULT_LEVELS)

    if not arm_binary.exists():
        print(f"check_crossarch_determinism: FAIL\n  - no arm64 build at "
              f"{arm_binary}")
        return 1
    if not x86_binary.exists():
        print(
            "check_crossarch_determinism: FAIL\n"
            f"  - no x86_64 build at {x86_binary}. Produce one with\n"
            "      macos/Scripts/build_crossarch_x86_64.sh\n"
            "    and see docs/architecture/cross-architecture-determinism.md "
            "for the toolchain requirements. This gate fails rather than skips "
            "because a skipped cross-architecture gate reads as a passing one "
            "in the suite summary")
        return 1
    for path in (rom, script):
        if not path.exists():
            print(f"check_crossarch_determinism: FAIL\n  - missing {path}")
            return 1

    problems: list[str] = []
    problems += architecture_problems(binary_architectures(arm_binary),
                                      binary_architectures(x86_binary))
    problems += configuration_problems(read_cmake_cache(arm_binary),
                                       read_cmake_cache(x86_binary))
    if not args.allow_stale:
        newest, newest_path = newest_source_mtime(ROOT)
        problems += staleness_problems(
            {"the arm64 binary": arm_binary, "the x86_64 binary": x86_binary},
            newest, newest_path)
    if not rosetta_available():
        problems.append(
            "this host cannot execute x86_64 binaries; install Rosetta 2 with "
            "`softwareupdate --install-rosetta`")
    if problems:
        print("check_crossarch_determinism: FAIL")
        for problem in problems:
            print(f"  - {problem}")
        return 1

    print(f"check_crossarch_determinism: v{HASH_VERSION} field set, "
          f"{args.frames} ticks, levels {','.join(str(n) for n in levels)}")
    print(f"  arm64  {arm_binary}")
    print(f"  x86_64 {x86_binary}")

    failures: list[str] = []
    compared = 0
    with tempfile.TemporaryDirectory(prefix="mdkr-crossarch-") as tmp:
        root = Path(tmp)
        for level in levels:
            route = f"level {level}"
            try:
                arm = run_arm(arm_binary, rom, f"lvl{level}-arm64", root,
                              level, args.frames, {}, args.timeout,
                              args.verbose, script)
                x86 = run_arm(x86_binary, rom, f"lvl{level}-x86_64", root,
                              level, args.frames, {}, args.timeout,
                              args.verbose, script)
            except RuntimeError as error:
                failures.append(f"{route}: {error}")
                continue
            route_problems = (stream_problems(arm, args.frames,
                                              f"{route} arm64") +
                              stream_problems(x86, args.frames,
                                              f"{route} x86_64"))
            if route_problems:
                failures.extend(route_problems)
                continue
            index = first_divergence(arm, x86)
            if index is not None:
                failures.extend(divergence_report(route, arm, x86, index))
                continue
            compared += len(arm)
            print(f"  {route}: {len(arm)} ticks byte-identical across "
                  "arm64 and x86_64")

        # The control runs last and on one level only: it costs one more pass
        # and it is what stops a broken capture from passing this gate quietly.
        try:
            base = run_arm(arm_binary, rom, "control-base", root, levels[0],
                           args.frames, {}, args.timeout, args.verbose,
                           script)
            legacy = run_arm(arm_binary, rom, "control-legacy", root,
                             levels[0], args.frames,
                             {"MDKR_RNGSEED": "legacy"}, args.timeout,
                             args.verbose, script)
        except RuntimeError as error:
            failures.append(f"positive control: {error}")
        else:
            failures.extend(stream_problems(base, args.frames,
                                            "control base"))
            failures.extend(stream_problems(legacy, args.frames,
                                            "control legacy-RNG"))
            problem = control_problem(base, legacy)
            if problem is not None:
                failures.append(problem)

    if failures:
        print("check_crossarch_determinism: FAIL")
        for failure in failures:
            print(f"  - {failure}" if not failure.startswith("    ")
                  else failure)
        return 1
    print(f"check_crossarch_determinism: PASS — {compared} authoritative ticks "
          f"byte-identical between a thin arm64 and a thin x86_64 build across "
          f"{len(levels)} route(s); the legacy-RNG control diverges, so the "
          "comparison is live")
    return 0


if __name__ == "__main__":
    sys.exit(main())
