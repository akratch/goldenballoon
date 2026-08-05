"""Shared, side-effect-free helpers for the headless regression checks."""

from __future__ import annotations

import contextlib
import errno
import fcntl
import hashlib
import os
import shutil
import tempfile
from collections.abc import Mapping
from pathlib import Path

# Everything eeprom_store()/eeprom_load() (platform/stubs_dkr.c) can leave in a
# save directory. A check that clears EEPROM state must clear all of it, and a
# check that borrows a real save directory must put all of it back.
EEPROM_ARTIFACTS = (
    "eeprom.bin",
    "eeprom.bin.tmp",
    "eeprom.bin.bad",
    "eeprom.bin.previous",
    "eeprom.bin.importing",
    "eeprom.bin.autosave.tmp",
    "eeprom.bin.autosave.1",
    "eeprom.bin.autosave.2",
    "eeprom.bin.autosave.3",
)


def resolve_binary(build: str | os.PathLike[str]) -> str:
    """Resolve ``--build`` consistently from either a directory or executable.

    Every behavioural check accepts the same contract:

    - ``--build build-rel`` resolves to ``build-rel/mdkr64``;
    - ``--build build-rel/mdkr64`` remains that executable.

    Existence is intentionally checked by each caller so its existing failure
    message and any additional input validation remain intact.
    """

    path = Path(build).expanduser()
    if path.is_dir():
        path = path / "mdkr64"
    return str(path)


def completed_tick_conservation(summary: Mapping[str, int], expected_ticks: int,
                                label: str) -> str | None:
    """Validate a finite run after its final pass, before its next ticket.

    The scheduler records the pass that reached the headless budget and only
    then prevents dispatch of a *next* pass. Consequently one already-earned
    host ticket remains pending at clean termination. This is distinct from a
    live-frame debt assertion: callers keep their own intermediate pacing
    checks, while this helper is only for the terminal summary.
    """
    ticks = summary.get("ticks", -1)
    simticks = summary.get("simticks", -1)
    issued = summary.get("issued", -1)
    pending = summary.get("pending", -1)
    updates = summary.get("updates", -1)
    lead = summary.get("lead", -1)
    max_pending = summary.get("maxpending", -1)
    if ticks != expected_ticks or simticks != expected_ticks:
        return (f"{label}: completed ticks/simticks={ticks}/{simticks}, "
                f"expected {expected_ticks}/{expected_ticks}")
    if issued + pending != ticks:
        return (f"{label}: clock conservation failed: ticks={ticks}, "
                f"issued={issued}, pending={pending}")
    if simticks != issued + 1:
        return (f"{label}: bootstrap completion failed: simticks={simticks}, "
                f"issued={issued}")
    if updates + 1 != simticks:
        return (f"{label}: game-update completion failed: updates={updates}, "
                f"simticks={simticks}")
    if pending != 1:
        return (f"{label}: clean completion must retain exactly one next "
                f"ticket, pending={pending}")
    # present_sched_trace_entry samples clock-minus-issued before taking the
    # next ticket, so its high-water lead is exactly the driver's pending-debt
    # high water. A clean steady run therefore ends with lead=maxpending=1;
    # grouped catch-up may raise both, but they must never disagree.
    if lead != max_pending:
        return (f"{label}: scheduler lead/maxpending={lead}/{max_pending}, "
                "expected exact agreement")
    return None


def test_save_dir() -> str:
    """The save directory the engine and the check must agree on.

    ``tools/run_checks.py`` points this at a run-scoped temporary directory and
    exports the same path as ``MDKR_SAVE_DIR``, so no suite run writes the
    repository's playable ``save/``. A standalone run keeps that historical
    default, which is why every caller also wraps its run in
    :func:`preserved_eeprom`.
    """

    return os.environ.get("MDKR_TEST_SAVE_DIR", "save")


def save_env(env: dict[str, str], save_dir: str) -> dict[str, str]:
    """Point the engine at ``save_dir``; the caller reads results from there."""

    env["MDKR_SAVE_DIR"] = os.path.abspath(save_dir)
    return env


@contextlib.contextmanager
def exclusive_build_dir(build_dir: str | os.PathLike[str]):
    """Hold an exclusive lock on a shared instrumented build directory.

    These checks configure and rebuild a fixed directory. The suite runs one
    task at a time, but two suites (or a suite and a standalone run) would
    otherwise interleave a reconfigure with a compile and produce a failure
    that has nothing to do with the code under test.
    """

    path = Path(build_dir).resolve()
    # The lock lives outside the repository: .gitignore's build*/ pattern only
    # covers directories, so a sibling lock file would show up as untracked and
    # trip the clean-tree guards.
    digest = hashlib.sha256(str(path).encode("utf-8")).hexdigest()[:16]
    lock_path = Path(tempfile.gettempdir()) / f"mdkr64-build-{digest}.lock"
    handle = open(lock_path, "w")
    try:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            if exc.errno not in (errno.EACCES, errno.EAGAIN):
                raise
            raise RuntimeError(
                f"{path} is locked by another run ({lock_path}); the suite runs "
                "one task at a time — wait for it to finish rather than sharing "
                "the instrumented build directory"
            ) from exc
        yield str(path)
    finally:
        handle.close()


@contextlib.contextmanager
def preserved_eeprom(save_dir: str):
    """Restore whatever EEPROM state ``save_dir`` held before the check ran.

    These checks assert on a specific starting state, so they delete and
    rewrite EEPROM images. When the directory is a developer's real ``save/``
    that would destroy a playable file; the prior bytes go back on the way out
    however the check exits.
    """

    directory = Path(save_dir)
    directory.mkdir(parents=True, exist_ok=True)
    saved: dict[str, bytes | None] = {}
    for name in EEPROM_ARTIFACTS:
        candidate = directory / name
        saved[name] = candidate.read_bytes() if candidate.is_file() else None
    try:
        yield str(directory)
    finally:
        for name, payload in saved.items():
            candidate = directory / name
            if payload is None:
                if candidate.is_dir():
                    shutil.rmtree(candidate, ignore_errors=True)
                elif candidate.exists():
                    candidate.unlink()
            else:
                candidate.write_bytes(payload)
