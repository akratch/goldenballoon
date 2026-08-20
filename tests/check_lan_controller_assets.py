#!/usr/bin/env python3
"""Pin the local-play controller asset set across code and packaging.

The trimmed controller assets that make local-only (no internet) Phone Party
work are named in three kinds of place that must never disagree:

  1. platform/party/lan_party_launch.cpp -- the C++ manifest the embedded LAN
     server serves to a phone (the kControllerAssets table).
  2. tools/lan_controller_assets.txt -- the source of truth the packaging
     scripts read to stage exactly those files into each artifact.
  3. dist/web -- the tracked web tree the files are copied from.

If the C++ table and the txt drift, a packaged build stages the wrong files and
the controller page loads broken in the shipped artifact (the failure the Task 5
review flagged). This check makes that impossible: the two lists must be equal,
and every listed file must exist under dist/web.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ASSET_LIST = ROOT / "tools" / "lan_controller_assets.txt"
MANIFEST_CPP = ROOT / "platform" / "party" / "lan_party_launch.cpp"
WEB_ROOT = ROOT / "dist" / "web"

# Each kControllerAssets row is {"<relative>", "<served>"}; we compare the
# relative (source) paths, deduped (index.html appears twice: "/controller/" and
# "/controller/index.html" both serve it).
_ROW = re.compile(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}')


def txt_relatives() -> set[str]:
    out: set[str] = set()
    for line in ASSET_LIST.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        out.add(line)
    return out


def cpp_relatives() -> set[str]:
    text = MANIFEST_CPP.read_text(encoding="utf-8")
    start = text.index("kControllerAssets")
    end = text.index("};", start)
    return {m.group(1) for m in _ROW.finditer(text[start:end])}


def main() -> int:
    failures: list[str] = []
    txt = txt_relatives()
    cpp = cpp_relatives()
    if not txt:
        failures.append(f"{ASSET_LIST} lists no assets")
    if txt != cpp:
        only_txt = sorted(txt - cpp)
        only_cpp = sorted(cpp - txt)
        if only_txt:
            failures.append(
                f"in tools/lan_controller_assets.txt but not the C++ "
                f"manifest table: {only_txt}")
        if only_cpp:
            failures.append(
                f"in lan_party_launch.cpp kControllerAssets but not the "
                f"packaging list: {only_cpp}")
    for relative in sorted(txt):
        if not (WEB_ROOT / relative).is_file():
            failures.append(f"listed asset missing from dist/web: {relative}")

    if failures:
        print("check_lan_controller_assets: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"check_lan_controller_assets: PASS ({len(txt)} assets, code and "
          "packaging list agree, all present in dist/web)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
