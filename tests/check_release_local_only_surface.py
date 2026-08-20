#!/usr/bin/env python3
"""Lock the native and browser local-only release boundary.

Phase 3 split the old "an origin-less build shows zero party surface" rule into
the two properties it was really protecting:

  * PRESERVED (the security property): a build with no compiled MDKR_PARTY_ORIGIN
    must show NO cloud/online party surface. The cloud card only works against a
    deployed service, so without an origin it must never appear.
  * CHANGED (Phase 3 local play): the "Local play (no internet)" surface MAY
    appear regardless of origin -- it pairs phones over the LAN with no service
    at all -- gated on its own preconditions (mdkr_lan_party_can_start).

`party_surface_failures` encodes both, and `_self_test` mutation-checks it: a
cloud surface that is no longer origin-gated must trip the gate, while a
local-only surface without an origin must not.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def party_surface_failures(rom: str, phone: str) -> list[str]:
    """Return the ways the two-surface boundary is broken (empty == intact).

    `rom` is ui_rom.cpp, `phone` is ui_phone_party.cpp. Substring checks match
    this gate's existing rigor level; each one is what a realistic regression
    would have to delete to leak a cloud surface without an origin.
    """
    failures: list[str] = []
    # PRESERVED: the cloud flow (drawFull) is reached only inside the
    # compiled-origin branch, so an origin-less build draws no cloud surface.
    if not ("else if (PhoneParty_availableInBuild(serviceOrigin)) {" in phone
            and "drawFull(host, serviceOrigin);" in phone):
        failures.append(
            "cloud party surface is no longer gated behind a compiled origin "
            "(drawFull must sit inside the PhoneParty_availableInBuild branch)")
    # CHANGED: local play is offered on its own preconditions, not the origin.
    if not ('"Local play (no internet)"' in phone
            and "drawLanEntryCard(lan);" in phone):
        failures.append(
            "local-play (no internet) surface is missing")
    # The launcher admits the section for EITHER surface; the cloud arm still
    # requires the origin, the local arm only its own availability.
    if ("PhoneParty_availableInBuild(MDKR_PARTY_ORIGIN) || s.lanParty.available"
            not in rom):
        failures.append(
            "party section gate no longer separates the cloud origin from "
            "local-play availability")
    return failures


def _self_test() -> None:
    """Positive + negative control, run on every invocation (never vacuous)."""
    good_phone = (
        "    if (lan.active) {\n"
        "        drawLanActive(host, lan);\n"
        "    } else if (PhoneParty_availableInBuild(serviceOrigin)) {\n"
        "        drawFull(host, serviceOrigin);\n"
        "        if (closed) drawLanEntryCard(lan);\n"
        "    } else {\n"
        "        drawLanEntryCard(lan);\n"
        "    }\n"
        '    label = "Local play (no internet)";\n'
    )
    good_rom = (
        "  const bool partyAvailable =\n"
        "      PhoneParty_availableInBuild(MDKR_PARTY_ORIGIN) || "
        "s.lanParty.available;\n")
    # New behavior: a local-only surface reachable without an origin is fine.
    assert party_surface_failures(good_rom, good_phone) == [], (
        "control failed: the local-only surface must not trip the gate")
    # Preserved invariant: a cloud surface no longer gated on the origin leaks.
    leak_phone = good_phone.replace(
        "else if (PhoneParty_availableInBuild(serviceOrigin)) {",
        "else if (true) {  /* origin gate removed */")
    assert party_surface_failures(good_rom, leak_phone), (
        "control failed: a cloud surface without an origin must trip the gate")


def main() -> int:
    _self_test()
    try:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        launcher = (ROOT / "platform/app/ui_launcher.cpp").read_text(
            encoding="utf-8")
        rom = (ROOT / "platform/app/ui_rom.cpp").read_text(encoding="utf-8")
        phone = (ROOT / "platform/app/ui_phone_party.cpp").read_text(
            encoding="utf-8")
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
        # Phase 3 two-surface boundary: cloud stays origin-gated, local play may
        # appear without an origin.
        for failure in party_surface_failures(rom, phone):
            require(False, failure)
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
    print("check_release_local_only_surface: PASS -- cloud surface stays "
          "origin-gated, local play is origin-independent, previews compiled "
          "out and Pages cloud routes removed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
