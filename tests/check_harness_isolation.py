#!/usr/bin/env python3
"""Guard: every check that isolates its save directory must isolate its
video config the same way.

The game resolves mdkr64.ini from the working directory unless
MDKR_VIDEO_CONFIG_PATH is set. Checks run with cwd = repo root, so a
launcher ini left there by an interactive session reaches any engine a
check spawns; with display-paced smoothing configured, --headless-frames
budgets count presents instead of authored ticks and drives fall short
(the taj_theme release-gate failure of 2026-08-19; the mechanism is
documented in check_door_blocks.py). This scanner fails the moment a
check gains an MDKR_SAVE_DIR assignment with no config pin, so the class
of contaminated-host false failures cannot grow back.

Usage: check_harness_isolation.py [--self-test]
"""

import re
import sys
import tempfile
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent

# An actual env assignment, not a prose mention: "MDKR_SAVE_DIR": ...,
# MDKR_SAVE_DIR=..., or env["MDKR_SAVE_DIR"] = ...
SAVE_DIR_ASSIGNMENT = re.compile(r"""MDKR_SAVE_DIR["']?\s*[:=]""")
PIN_MARKERS = ("MDKR_VIDEO_CONFIG_PATH", "save_env")

# Files with a save-dir mention but, by design, no pin of their own.
# Every entry carries the reason it is exempt; a new entry needs one too.
EXEMPT = {
    # Spawns its children through check_ghost_matrix.run_child(), which
    # pins via harness_utils.save_env(); has no env site of its own.
    "check_ghost_bank_capacity.py",
    # Never sets MDKR_SAVE_DIR (docstring mention only); isolates app
    # prefs via MDKR_APP_PREFS_DIR, and mdkr64_app.ini prefs persistence
    # is its subject, distinct from the video config.
    "check_shell_dropfile.py",
}


def scan(directory):
    offenders = []
    for path in sorted(directory.glob("check_*.py")):
        if path.name in EXEMPT or path.name == Path(__file__).name:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if SAVE_DIR_ASSIGNMENT.search(text) and not any(
            marker in text for marker in PIN_MARKERS
        ):
            offenders.append(path.name)
    return offenders


def self_test():
    with tempfile.TemporaryDirectory(prefix="mdkr-isolation-") as raw:
        fixture = Path(raw)
        bad = fixture / "check_fixture_unpinned.py"
        bad.write_text('env = {"MDKR_SAVE_DIR": str(save)}\n')
        good = fixture / "check_fixture_pinned.py"
        good.write_text(
            'env = {"MDKR_SAVE_DIR": s, "MDKR_VIDEO_CONFIG_PATH": v}\n'
        )
        prose = fixture / "check_fixture_prose.py"
        prose.write_text('"""Mentions MDKR_SAVE_DIR in prose only."""\n')
        found = scan(fixture)
        if found != ["check_fixture_unpinned.py"]:
            print(
                f"check_harness_isolation: FAIL -- self-test scanner is "
                f"broken: flagged {found}",
                file=sys.stderr,
            )
            return 1
    print("check_harness_isolation: self-test PASS -- scanner is not vacuous")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()
    for name in sorted(EXEMPT):
        if not (TESTS_DIR / name).is_file():
            print(
                f"check_harness_isolation: FAIL -- exempt file {name} no "
                "longer exists; prune the exemption",
                file=sys.stderr,
            )
            return 1
    offenders = scan(TESTS_DIR)
    if offenders:
        print(
            "check_harness_isolation: FAIL -- these checks set "
            "MDKR_SAVE_DIR without pinning MDKR_VIDEO_CONFIG_PATH (or "
            "using save_env); a repo-root mdkr64.ini reaches their "
            "engines:\n  " + "\n  ".join(offenders),
            file=sys.stderr,
        )
        return 1
    print(
        "check_harness_isolation: PASS -- every save-isolated check pins "
        f"its video config ({len(EXEMPT)} documented exemptions)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
