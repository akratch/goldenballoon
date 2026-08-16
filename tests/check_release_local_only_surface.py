#!/usr/bin/env python3
"""Lock the native and browser local-only release boundary."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    try:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        launcher = (ROOT / "platform/app/ui_launcher.cpp").read_text(
            encoding="utf-8")
        rom = (ROOT / "platform/app/ui_rom.cpp").read_text(encoding="utf-8")
        release = (ROOT / ".github/workflows/release.yml").read_text(
            encoding="utf-8")
        web = (ROOT / ".github/workflows/web-demo.yml").read_text(
            encoding="utf-8")
        mac = (ROOT / "macos/Scripts/build_app_bundle.sh").read_text(
            encoding="utf-8")
        prepare = (ROOT / "tools/web/prepare_local_only_release.py").read_text(
            encoding="utf-8")

        require("option(MDKR_ENABLE_ONLINE_ROOM_PREVIEW" in cmake and
                "MDKR_ONLINE_ROOM_PREVIEW_DEFAULT OFF" in cmake and
                "MDKR_ENABLE_ONLINE_ROOM_PREVIEW=$<BOOL:" in cmake,
                "native deferred preview lacks an opt-in compile boundary")
        require("#if MDKR_ENABLE_ONLINE_ROOM_PREVIEW" in launcher and
                "#else\n    return false;\n#endif" in launcher,
                "release launcher can expose Online Room through its environment")
        require("partyAvailable && (ready || partyTraceRequested)" in rom,
                "party trace can manufacture a surface without a compiled origin")
        require(release.count("-DMDKR_ENABLE_ONLINE_ROOM_PREVIEW=OFF") == 2,
                "Linux and Windows release builds must compile out Online Room")
        require(release.count("MDKR_ONLINE_ROOM_PREVIEW=1") == 2 and
                release.count('"Online Room" Play') == 2 and
                release.count("active-panel=$expected_panel") == 2,
                "built and packaged launchers do not prove the preview stays absent")
        require("-DMDKR_ENABLE_ONLINE_ROOM_PREVIEW=OFF" in mac and
                "unexpectedly includes the deferred Online Room preview" in mac,
                "macOS release bundle does not compile and verify the preview out")

        require("prepare_local_only_release.py" in web and
                "check_browser_local_only_release.py" in web and
                "with: { path: publish-web }" in web and
                "with: { path: dist/web }" not in web,
                "Pages workflow does not publish the verified local-only payload")
        for route in ("controller", "online", "party", "room"):
            require(f'"{route}"' in prepare,
                    f"browser packager does not remove {route}/")
        for runtime in ("mdkr-online-tools.js", "mdkr-online-tools.wasm"):
            require(runtime in prepare,
                    f"browser packager does not remove {runtime}")
    except (OSError, AssertionError) as error:
        print(f"check_release_local_only_surface: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print("check_release_local_only_surface: PASS -- native previews compiled "
          "out and Pages cloud routes removed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
