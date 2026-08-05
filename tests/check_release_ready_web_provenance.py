#!/usr/bin/env python3
"""Deterministic fixtures for candidate staged-web provenance rejection."""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CHECKER = ROOT / "tools" / "ci" / "check_web_build_provenance.py"
HEAD = "0123456789abcdef0123456789abcdef01234567"
OTHER = "89abcdef0123456789abcdef0123456789abcdef"


def candidate_version() -> str:
    """Read the one definition of the release version, as the caller does.

    check_release_ready.sh resolves MDKR_VERSION out of CMakeLists.txt and
    hands it to the checker, so these fixtures must be built from that same
    value. Transcribing a literal here would go stale at every version bump
    and stop exercising the shape the release path actually passes.
    """
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r'set\(MDKR_VERSION "([^"]+)"', text)
    if match is None:
        raise AssertionError("CMakeLists.txt does not set MDKR_VERSION")
    return match.group(1)


VERSION = candidate_version()
# Guaranteed to differ from VERSION whatever the bump, so the rejection case
# cannot degrade into a self-comparison that passes vacuously.
WRONG_VERSION = f"{VERSION}-not-the-candidate"


def run_case(name: str, record: dict[str, object], expected_code: int,
             expected_text: str) -> None:
    with tempfile.TemporaryDirectory(prefix="mdkr-web-provenance-") as temporary:
        web_dir = Path(temporary) / "web"
        web_dir.mkdir()
        (web_dir / "build-info.json").write_text(
            json.dumps(record), encoding="utf-8")
        result = subprocess.run(
            [sys.executable, str(CHECKER), "--web-dir", str(web_dir),
             "--commit", HEAD, "--version", VERSION],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False)
    output = result.stdout + result.stderr
    if result.returncode != expected_code or expected_text not in output:
        raise AssertionError(
            f"{name}: expected exit {expected_code} with {expected_text!r}; "
            f"got exit {result.returncode}: {output}")


def main() -> int:
    run_case("clean exact candidate",
             {"version": VERSION, "source_commit": HEAD, "source_dirty": False}, 0,
             "web build provenance: PASS")
    run_case("dirty staged build",
             {"version": VERSION, "source_commit": HEAD, "source_dirty": True}, 1,
             "source_dirty=True")
    run_case("stale staged build",
             {"version": VERSION, "source_commit": OTHER, "source_dirty": False}, 1,
             "does not equal HEAD")
    run_case("missing staged version",
             {"source_commit": HEAD, "source_dirty": False}, 1,
             "does not equal candidate version")
    run_case("wrong staged version",
             {"version": WRONG_VERSION, "source_commit": HEAD,
              "source_dirty": False}, 1,
             "does not equal candidate version")
    print("check_release_ready_web_provenance: PASS -- clean, dirty, stale, and version fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
