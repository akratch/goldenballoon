#!/usr/bin/env python3
"""Validate third-party notice coverage.

Deliberately narrow: it checks the specific vendored components whose licence
and provenance posture matters for this repository, and cross-references each
against THIRD_PARTY.md. A licence that only appears in THIRD_PARTY.md does not
satisfy a source-redistribution condition, so where the notice has to travel
with the file the check is made against the file itself.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check mdkr64 third-party notice coverage.")
    parser.add_argument("--repo-root", default=".", help="repository root")
    return parser.parse_args()


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    args = parse_args()
    root = Path(args.repo_root).resolve()
    problems: list[str] = []

    # Required files and the notice text each must contain. Each entry is a
    # source-of-truth check: the file that actually carries the license, not
    # just a mention of it elsewhere.
    required_files = {
        "lib/glad/LICENSE": (
            "The glad source code",
            "Copyright (c) 2013-2021 David Herberth",
        ),
        "lib/glad/README.md": ("OpenGL function loader", "LICENSE"),
        "lib/sdl_gamecontrollerdb/LICENSE.txt": (
            "Sam Lantinga",
            "Permission is granted to anyone to use this software",
        ),
        "platform/fast3d/PROVENANCE.md": (
            "Copyright (c) 2020 Emill, MaikelChan",
            "modified BSD-2-Clause",
            "binary contains no assets you do not have the right to distribute",
        ),
    }

    for rel_path, needles in required_files.items():
        path = root / rel_path
        if not path.is_file():
            problems.append(f"missing third-party notice file: {rel_path}")
            continue
        text = read_text(path)
        for needle in needles:
            if needle not in text:
                problems.append(f"{rel_path} is missing expected notice text: {needle!r}")

    # mixer.c/.h must credit the perfect_dark origin in the file itself, not
    # just in THIRD_PARTY.md -- the source-level attribution is what a
    # downstream consumer sees if they only grab the file.
    for rel_path in ("platform/mixer.c", "platform/mixer.h"):
        path = root / rel_path
        if not path.is_file():
            problems.append(f"missing mixer source: {rel_path}")
            continue
        text = read_text(path)
        if "fgsfdsfgs/perfect_dark" not in text:
            problems.append(f"{rel_path} is missing its fgsfdsfgs/perfect_dark attribution")

    # Every n64-fast3d-engine-derived backend file must carry its own
    # attribution comment -- the license's redistribution condition is a
    # source-redistribution one, so the notice has to travel with the file.
    fast3d_attribution_files = {
        "platform/fast3d/gfx_opengl.c": "Emill/n64-fast3d-engine",
        "platform/fast3d/gfx_cc.c": "Emill/n64-fast3d-engine",
        "platform/fast3d/gfx_rendering_api.h": "Emill/n64-fast3d-engine",
    }
    for rel_path, needle in fast3d_attribution_files.items():
        path = root / rel_path
        if not path.is_file():
            problems.append(f"missing fast3d backend source: {rel_path}")
            continue
        text = read_text(path)
        if needle not in text:
            problems.append(f"{rel_path} is missing expected attribution text: {needle!r}")

    third_party = root / "THIRD_PARTY.md"
    third_party_text = ""
    if third_party.is_file():
        third_party_text = read_text(third_party)
        for needle in (
            "lib/glad/LICENSE",
            "lib/sdl_gamecontrollerdb",
            "fgsfdsfgs/perfect_dark",
            "Emill/n64-fast3d-engine",
            "mgb64",
            "SMAA",
            "github.com/iryoku/smaa",
            "wgpu-native",
            "MSYS2",
            "brand/appicon-source.png",
            "game/include/PR/",
            "docs/ref/",
        ):
            if needle not in third_party_text:
                problems.append(f"THIRD_PARTY.md is missing expected text: {needle!r}")
    else:
        problems.append("missing THIRD_PARTY.md")

    notice = root / "NOTICE.md"
    if notice.is_file():
        notice_text = read_text(notice)
        for needle in (
            "SMAA",
            "THIRD_PARTY.md",
            "game/include/PR/",
        ):
            if needle not in notice_text:
                problems.append(f"NOTICE.md is missing expected text: {needle!r}")
    else:
        problems.append("missing NOTICE.md")

    # The SMAA lookup tables are carried as generated output; their generator
    # is not vendored here. Each header must therefore keep naming its upstream
    # so the attribution travels with the data, and must not point at a local
    # generator path that does not exist. This is a presence check on the
    # upstream reference, not an absence check on the generator.
    for rel_path in (
        "platform/fast3d/smaa_area_tex.h",
        "platform/fast3d/smaa_search_tex.h",
    ):
        path = root / rel_path
        if not path.is_file():
            problems.append(f"missing SMAA LUT header: {rel_path}")
            continue
        text = read_text(path)
        if "iryoku/smaa" not in text:
            problems.append(f"{rel_path} is missing its SMAA upstream attribution")
        if "tools/smaa" in text:
            problems.append(
                f"{rel_path} points at tools/smaa, which this repository does "
                "not contain -- name the upstream generator instead"
            )

    # None of these build tools is vendored here; THIRD_PARTY.md documents each
    # as living upstream rather than in this tree. If one is ever vendored in,
    # the THIRD_PARTY.md row describing it must land in the SAME change -- fail
    # loudly instead of letting the guard go silently stale.
    not_vendored_dirs = (
        "tools/smaa",
        "tools/mktex",
        "tools/asm-processor",
        "tools/gzipsrc",
        "tools/ido5.3_recomp",
    )
    for rel_dir in not_vendored_dirs:
        if (root / rel_dir).exists() and rel_dir not in third_party_text:
            problems.append(
                f"{rel_dir} now exists but THIRD_PARTY.md has no row describing "
                "it as vendored in this repository"
            )

    if problems:
        print("FAIL: third-party notice guard found issue(s):", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print("PASS: third-party notice guard passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
