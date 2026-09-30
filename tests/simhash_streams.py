"""Capture and compare authoritative [SIMHASH] streams between two binaries.

For the gates that run two different executables through the same route and
require the same simulation, tick for tick. tests/check_crossarch_determinism.py
runs a thin arm64 build against a thin x86_64 build of the same source.

The pure helpers are unit-tested by tests/test_crossarch_determinism.py. Their
default wording is the cross-architecture gate's, so its reports read exactly as
they always have; another gate passes its own labels.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path
from typing import NamedTuple

# The same field set check_state_hash.py anchors on, so a divergence found by
# either gate is expressed in the same coordinates as every other
# authoritative-state finding in the tree.
HASH_VERSION = "3"

# sim_hash.c formats the digest with %016llx, so a well-formed row carries
# exactly 16 hex characters. Pinning the WIDTH is what makes a torn final line
# -- the tail a killed or disk-full run leaves behind -- fail to match instead
# of parsing into a short digest that then "differs" from its peer for an I/O
# reason rather than a simulation one.
SIMHASH_RE = re.compile(
    r"^\[SIMHASH\] tick=(\d+) objs=(-?\d+) h=([0-9a-f]{16})\s*$")

# Cache entries that must agree between the two build trees. Anything that can
# change generated code changes what a float computes, so a mismatch here is
# reported as a configuration failure rather than as a finding.
SHARED_CACHE_KEYS = (
    "CMAKE_BUILD_TYPE",
    "CMAKE_C_FLAGS",
    "CMAKE_CXX_FLAGS",
    "CMAKE_C_FLAGS_RELEASE",
    "CMAKE_CXX_FLAGS_RELEASE",
    "CMAKE_INTERPROCEDURAL_OPTIMIZATION",
)

# Trees whose contents compile into the executable. A binary older than the
# newest file here was built from different source than the one on disk.
SOURCE_TREES = ("game", "platform", "include", "src", "lib")
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".m", ".mm", ".inc")


class SimHashRow(NamedTuple):
    """One authoritative tick as sim_hash.c published it."""

    tick: int
    objs: int
    digest: str
    raw: str


def parse_cmake_cache(text: str) -> dict[str, str]:
    """``NAME:TYPE=value`` entries from a CMakeCache.txt, as ``{NAME: value}``.

    Comment and blank lines are skipped, and so are the internal ``NAME-ADVANCED``
    bookkeeping rows, which carry no configuration meaning and would otherwise
    double the size of every mismatch report.
    """

    values: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(("#", "//")):
            continue
        name, separator, value = line.partition("=")
        if not separator or ":" not in name:
            continue
        key = name.split(":", 1)[0]
        if key.endswith("-ADVANCED"):
            continue
        values[key] = value
    return values


def configuration_problems(first_cache: dict[str, str],
                           second_cache: dict[str, str],
                           labels: tuple[str, str] = ("arm64", "x86_64"),
                           consequence: str = (
                               "The two trees must be configured identically "
                               "apart from the architecture, or this gate "
                               "measures the configuration difference"),
                           ignore: tuple[str, ...] = ()) -> list[str]:
    """Configuration differences that would confound the comparison.

    Compares the shared keys above plus every ``MDKR_*`` project option, so a
    new option is covered the day it is added rather than the day someone
    remembers to list it here. Keys that are supposed to differ (the
    architecture selection) are simply never compared; ``ignore`` names any
    project option a caller has a documented reason to let differ.
    """

    problems: list[str] = []
    keys = set(SHARED_CACHE_KEYS)
    keys.update(k for k in first_cache if k.startswith("MDKR_"))
    keys.update(k for k in second_cache if k.startswith("MDKR_"))
    for key in sorted(keys - set(ignore)):
        first = first_cache.get(key)
        second = second_cache.get(key)
        if first == second:
            continue
        problems.append(
            f"{key} differs: {labels[0]}={first!r} {labels[1]}={second!r}. "
            f"{consequence}")
    return problems


def newest_source_mtime(root: Path) -> tuple[float, Path | None]:
    """The most recently modified compiled source under ``root``.

    Returns ``(mtime, path)``; ``(0.0, None)`` when nothing matched, which a
    caller treats as "cannot judge staleness" rather than "fresh".
    """

    newest = 0.0
    newest_path: Path | None = None
    for tree in SOURCE_TREES:
        base = root / tree
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            mtime = path.stat().st_mtime
            if mtime > newest:
                newest, newest_path = mtime, path
    return newest, newest_path


def staleness_problems(binaries: dict[str, Path], newest: float,
                       newest_path: Path | None,
                       consequence: str = (
                           "Rebuild both trees from the same source before "
                           "comparing them; a stale binary makes a source "
                           "difference look architectural")) -> list[str]:
    """Binaries older than the newest source file they are supposed to contain.

    A stale executable is the most likely way a two-binary gate reports a
    spectacular divergence that is really "you edited the simulation and only
    rebuilt one tree".
    """

    if newest_path is None:
        return []
    problems: list[str] = []
    for label, path in binaries.items():
        if path.stat().st_mtime < newest:
            problems.append(
                f"{label} ({path}) is older than {newest_path}. {consequence}")
    return problems


def parse_simhash_rows(text: str) -> list[SimHashRow]:
    """Every ``[SIMHASH]`` row in ``text``, oldest first.

    Matching the whole line rather than a prefix is deliberate: a truncated
    final line (a killed run, a full disk) must be dropped rather than parsed
    into a short digest that then "differs" from its peer for an I/O reason.
    """

    rows: list[SimHashRow] = []
    for line in text.splitlines():
        match = SIMHASH_RE.match(line)
        if match is None:
            continue
        rows.append(SimHashRow(int(match.group(1)), int(match.group(2)),
                               match.group(3), line.strip()))
    return rows


def stream_problems(rows: list[SimHashRow], expected: int,
                    label: str) -> list[str]:
    """Whether a captured stream is evidence at all.

    An empty stream, a short stream, or one whose ticks are not 0..N-1 cannot
    support an identity claim -- two empty streams are equal, and equality over
    nothing is the failure mode this project has been bitten by before. So the
    contract is checked before the comparison, not instead of it.
    """

    problems: list[str] = []
    if not rows:
        problems.append(
            f"{label}: no [SIMHASH] rows. The instrument did not arm "
            f"(MDKR_STATE_HASH={HASH_VERSION} must reach the process), so "
            "there is nothing to compare and an identity claim would be "
            "vacuous")
        return problems
    if len(rows) != expected:
        problems.append(
            f"{label}: {len(rows)} [SIMHASH] rows, expected {expected}")
    ticks = [row.tick for row in rows]
    if ticks != list(range(len(rows))):
        first_bad = next(i for i, tick in enumerate(ticks) if tick != i)
        problems.append(
            f"{label}: tick numbering is not contiguous from 0; row "
            f"{first_bad} carries tick={ticks[first_bad]}")
    return problems


def first_divergence(first: list[SimHashRow],
                     second: list[SimHashRow]) -> int | None:
    """Index of the first tick whose published row differs, or ``None``.

    Compares the published fields rather than the raw text so a future
    formatting change to the row cannot turn into a false divergence.
    """

    for index, (a, b) in enumerate(zip(first, second)):
        if (a.tick, a.objs, a.digest) != (b.tick, b.objs, b.digest):
            return index
    if len(first) != len(second):
        return min(len(first), len(second))
    return None


def divergence_report(route: str, first: list[SimHashRow],
                      second: list[SimHashRow], index: int,
                      labels: tuple[str, str] = ("arm64", "x86_64"),
                      kind: str = "architecture",
                      parties: str = "the two architectures",
                      value_suspects: str = (
                          "Float arithmetic, an uninitialised read, or a "
                          "pointer-derived value are the candidates")) -> list[str]:
    """The triage a maintainer needs from a red run, without a second run.

    Names the first divergent tick, both rows, whether the POPULATION or only
    the state digest moved, and how the streams behave afterwards. The
    population distinction matters: a differing ``objs=`` means the two
    binaries disagree about which objects exist, which is a spawn/despawn or
    allocation-order defect, while an identical population with a different
    digest is a value defect -- typically a float or a decision. They have
    disjoint suspect lists.
    """

    width = max(len(labels[0]), len(labels[1]))
    lines = [f"{route}: {kind} divergence at tick {index}"]
    if index >= len(first) or index >= len(second):
        lines.append(
            f"    the streams have different lengths ({labels[0]} {len(first)} "
            f"rows, {labels[1]} {len(second)} rows); one run ended early")
        return lines
    a, b = first[index], second[index]
    lines.append(f"    {labels[0]:<{width}} {a.raw}")
    lines.append(f"    {labels[1]:<{width}} {b.raw}")
    if a.objs != b.objs:
        lines.append(
            f"    the authoritative POPULATION differs ({a.objs} vs {b.objs}): "
            f"{parties} disagree about which objects exist, so look "
            "at spawn/despawn conditions and allocation order before float "
            "arithmetic")
    else:
        lines.append(
            f"    the population agrees ({a.objs} objects) and only the state "
            f"digest moved: a field VALUE differs. {value_suspects}")
    tail = list(zip(first[index:], second[index:]))
    differing = sum(1 for x, y in tail
                    if (x.tick, x.objs, x.digest) != (y.tick, y.objs, y.digest))
    lines.append(
        f"    {index} ticks were identical; {differing} of the {len(tail)} "
        "remaining ticks differ")
    lines.append(
        f"    bisect with: MDKR_STATE_HASH={HASH_VERSION} "
        f"MDKR_HASH_DUMP_TICK={max(index - 1, 0)} MDKR_HASH_DUMP_UNTIL={index} "
        "MDKR_HASH_DUMP_IDS=1 on both binaries, then diff the [HASHOBJ] and "
        "[HASHOBJID] rows to name the object and field that moved")
    return lines


def control_problem(base: list[SimHashRow],
                    control: list[SimHashRow]) -> str | None:
    """The positive control: a deliberately different simulation must differ.

    ``MDKR_RNGSEED=legacy`` restores the pre-fix boot seeds
    (platform/math_util_native.c), so its stream provably is not the base
    stream. If this comparator reports them identical, then the identical
    result it reported for the real arms means nothing: the capture, the parse
    or the comparison is broken.
    """

    if first_divergence(base, control) is None:
        return ("positive control failed: the MDKR_RNGSEED=legacy arm produced "
                "a stream identical to the base arm. The capture or the "
                "comparison is broken, so every 'identical' verdict above is "
                "unsupported")
    return None


def read_cmake_cache(binary: Path) -> dict[str, str]:
    """The CMakeCache.txt beside ``binary``, parsed; empty when absent."""

    cache = binary.parent / "CMakeCache.txt"
    if not cache.is_file():
        return {}
    return parse_cmake_cache(cache.read_text(encoding="utf-8",
                                             errors="replace"))


class HashedRun(NamedTuple):
    """One headless run: its [SIMHASH] stream and everything it printed."""

    rows: list[SimHashRow]
    output: str


def run_hashed(binary: Path, rom: Path, run_dir: Path, label: str, frames: int,
               script: Path, env_overrides: dict[str, str], timeout: int,
               verbose: bool, window: str = "320x240",
               timeout_hint: str = "Raise --timeout rather than reading this "
                                   "as a failure") -> HashedRun:
    """One headless run in its own directory; returns its [SIMHASH] stream.

    The environment is scrubbed of MDKR*/GE007_* first and rebuilt from
    literals, exactly as check_state_hash.py does, so a maintainer's exported
    renderer, pacing or diagnostic preference cannot reach one arm and not the
    other -- which would look like a difference between the two binaries.
    LC_ALL and TZ are pinned for the same reason at the libc layer. The save
    directory and video config are the run directory's own, and the run
    directory is the working directory, so relative content roots are empty.

    The stream is read from MDKR_STATE_HASH_FILE rather than stdout. The file
    sink is flushed per tick, so a run killed by the timeout still leaves a
    line-aligned prefix that names how far the two arms agreed, and a stdout
    pipe cannot lose or interleave rows behind the engine's other logging.
    """

    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    stream_path = run_dir / "simhash.txt"
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        LANG="C",
        TZ="UTC",
        MDKR_AUDIO="0",
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_STATE_HASH_FILE=str(stream_path),
        MDKR_AUTOPILOT="1",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    env.update(env_overrides)
    command = [
        str(binary), "--headless-frames", str(frames),
        "--input-script", str(script), "--rom", str(rom),
        "--window-size", window,
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        process = subprocess.run(
            command, cwd=run_dir, env=env, text=True, errors="replace",
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False)
    except subprocess.TimeoutExpired as expired:
        captured = len(parse_simhash_rows(
            stream_path.read_text(encoding="utf-8", errors="replace")
            if stream_path.is_file() else ""))
        raise RuntimeError(
            f"{label}: timed out after {timeout}s with {captured} of {frames} "
            f"ticks captured. {timeout_hint}") from expired
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n"
            f"{(process.stdout or '')[-3000:]}")
    if not stream_path.is_file():
        raise RuntimeError(
            f"{label}: MDKR_STATE_HASH_FILE was never created. The build "
            "predates the sim_hash.c file sink, or the run never reached a "
            "hashing tick")
    return HashedRun(
        parse_simhash_rows(stream_path.read_text(encoding="utf-8",
                                                 errors="replace")),
        process.stdout or "")
