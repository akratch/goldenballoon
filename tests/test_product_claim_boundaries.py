#!/usr/bin/env python3
"""Keep release-facing qualification and accessibility claims intentionally narrow."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent


def require_contains(path: str, text: str) -> None:
    """Assert a document still makes a claim, however the prose is wrapped.

    These needles are sentences, not lines. Comparing raw text made the gate
    fail on a markdown reflow that moved one word of the screen-reader
    disclaimer onto the next line -- the claim was intact and the gate said it
    had been dropped. Collapsing whitespace on both sides keeps the assertion
    about what the document states and not about where it wraps; a deleted or
    reworded claim still fails, which the suite proves by mutation.
    """
    source = (ROOT / path).read_text(encoding="utf-8")
    if " ".join(text.split()) not in " ".join(source.split()):
        raise AssertionError(f"{path} no longer states: {text!r}")


def current_release_notes() -> str:
    source = (ROOT / "RELEASE_NOTES.md").read_text(encoding="utf-8")
    section = re.search(
        r"(?ms)^# Golden Balloon [^\n]+\n.*?(?=^# Golden Balloon |\Z)",
        source,
    )
    if section is None:
        raise AssertionError("RELEASE_NOTES.md has no release section")
    return section.group(0)


def main() -> int:
    require_contains(
        "README.md",
        "WebGPU with the Restored presentation is the qualified visual path;",
    )
    require_contains(
        "README.md",
        "not advertised as screen-reader compatible",
    )
    # The 2026-09-01 README pass reworded the platform limits. Linux still has
    # no native file picker (platform/app/file_dialog_stub.cpp), and the README
    # now says so where the player looks for it -- in the getting-started steps
    # -- rather than in a limitations bullet. The former "campaign is not
    # automated or claimed complete" disclaimer is gone on purpose: the campaign
    # has been gated since 2026-08-07 (docs/open-items/README.md), so the
    # disclaimer had become the false claim.
    require_contains(
        "README.md",
        "on Linux, drag the ROM onto the launcher or paste its",
    )
    require_contains(
        "docs/APP_SHELL.md",
        "WebGPU with Restored presentation is the qualified native visual path;",
    )
    require_contains(
        "docs/APP_SHELL.md",
        "not advertised as screen-reader compatible",
    )
    require_contains(
        "RELEASE_NOTES.md",
        "WebGPU with Restored presentation remains the qualified native and browser",
    )
    require_contains(
        "RELEASE_NOTES.md",
        "does not claim a\n  VoiceOver, UI Automation, or other screen-reader semantic tree.",
    )
    release_notes = current_release_notes()
    release_notes_words = " ".join(release_notes.split())
    if "does not increase unique visual FPS" in release_notes:
        raise AssertionError(
            "the current release notes must not describe working interpolation as inert"
        )
    if "Interpolated draws presentation-only in-between images" not in release_notes_words:
        raise AssertionError(
            "the current release notes must distinguish interpolated images from authored holds"
        )
    # There is no contrast control anywhere in the product -- not in
    # MdkrVideoKey, not in AppConfig, not in AppTheme. It was listed as a
    # supported accessibility capability in three places for several releases
    # before anyone checked. Scoped to the CURRENT release section and the
    # README rather than the whole file: the older sections record what was
    # claimed at the time, and rewriting them would erase that this happened.
    #
    # The needle is the list form, not the bare word: the shell's visual design
    # genuinely is high-contrast, and docs/APP_SHELL.md says so accurately.
    # What may not return is contrast sitting in a list of things a player can
    # adjust, beside scaling and reduced motion, which really are settings.
    for label, haystack in (("the current release notes", release_notes_words),
                            ("README.md", " ".join(
                                (ROOT / "README.md").read_text(
                                    encoding="utf-8").split()))):
        if "scaling, contrast, and reduced motion" in haystack:
            raise AssertionError(
                f"{label} lists a contrast setting among the accessibility "
                "capabilities; the product has never had one"
            )
    require_contains(
        "dist/web/index.html",
        "Qualified browser path:</strong>\n                WebGPU with Restored presentation.",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "Graphics backend: WebGPU (recommended)",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "Restored is the recommended default: widescreen and sharper ",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "Motion smoothing, and Simulation cadence keep the values selected ",
    )
    settings = (ROOT / "platform/app/ui_settings.cpp").read_text(encoding="utf-8")
    if "Pure protects the original presentation and gameplay settings" in settings:
        raise AssertionError(
            "Pure must not call retained non-original timing values original"
        )
    require_contains(
        "platform/video_config.c",
        "interpolate draws presentation-only in-between images from adjacent ",
    )
    require_contains(
        "tests/check_presentation_matrix.py",
        "Every\n  midpoint must differ from BOTH of its neighbours",
    )
    require_contains(
        "docs/APP_SHELL.md",
        "**Linux in-window file selection.**",
    )
    require_contains(
        "docs/APP_SHELL.md",
        "`isAvailable()` stays false on Linux",
    )
    require_contains(
        "docs/APP_SHELL.md",
        "ROM drag-and-drop and typed paths remain.",
    )
    # Online boundaries: the same six facts pinned in both places a player
    # reads them (RELEASE_NOTES.md's current release section, README.md's
    # online section), so a future rewrite cannot drop one from either.
    # D-WIRE: envelope v3, the MPF2 report and control protocol 2 all changed
    # in 1.6.0, nothing negotiates and nothing downgrades, so a mixed pair is
    # refused at room join. One sentence of release copy makes that refusal
    # read as expected rather than as a bug -- and it belongs in the CURRENT
    # release section, not merely somewhere in the file's history.
    if "Both players need the same version of the game." not in release_notes_words:
        raise AssertionError(
            "the current release notes must say both players need the same version"
        )
    require_contains("README.md", "Both players need the same version of the game.")
    native_only_claim = (
        "Online Room ships in native desktop packages. The published browser "
        "build remains local-only."
    )
    if native_only_claim not in release_notes_words:
        raise AssertionError(
            "the current release notes must identify Online Room as native-only"
        )
    require_contains("README.md", native_only_claim)
    for known_camera_claim in (
        "Keep the camera out of walls",
        "remains an optional, experimental camera mode.",
        "the default camera, is unaffected.",
        "check also fails in 1.6.0 on brief correction re-engagements",
        "class is not new to 1.7.0",
    ):
        if known_camera_claim not in release_notes_words:
            raise AssertionError(
                "the current release notes must disclose the optional-camera "
                f"motion residual: {known_camera_claim!r}"
            )
    for path in ("RELEASE_NOTES.md", "README.md"):
        require_contains(path, "Both players must be on the same platform.")
        require_contains(path, "A race is two players, not more.")
        require_contains(path, "You can't join a race after it starts.")
        require_contains(path, "If the host leaves, the race ends.")
        require_contains(
            path,
            "Some networks can't connect two players directly. There is no "
            "relay yet, so those pairs can't race online for now.",
        )
        require_contains(
            path, "The service that pairs you can see that you're both connected."
        )
    # Custom characters are presentation identities. Keep the Workshop's save
    # review precise: ghosts/network retain a retail character ID, but course
    # records and adventure saves do not become donor-owned data, and peer
    # package negotiation is still future work.
    require_contains(
        "platform/app/ui_settings.cpp",
        "Course records and adventure saves remain ordinary game data; they ",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "do not embed the custom package.",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "Package negotiation for online peers ",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "is not implemented, so the donor is always the safe authoritative ",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "Ghost and network/rollback character ID",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "Acceleration curve by vehicle",
    )
    if "records, ghosts, saves, and ordinary online authority remain" in settings:
        raise AssertionError(
            "the Workshop must not describe ordinary record/save data as "
            "donor-owned"
        )
    # Package selection is a review, not an install side effect. Both native
    # portable and source-only compiler routes bind the bytes and installed
    # base that were actually shown before the final action is enabled.
    for claim in (
        "Validate and review",
        "Install reviewed character",
        "Install reviewed update",
        "I confirm I have the right to use this package locally",
        "the importer cannot verify copyright, trademark, attribution, or redistribution rights",
        "The package or installed character may have changed; validate and review it again.",
        "License (SPDX)",
        "Creator / attribution",
        "Unavailable (legacy cache)",
        "These declarations and the exact LICENSE.txt bytes are authenticated by the active source digest.",
    ):
        require_contains("platform/app/ui_settings.cpp", claim)
    require_contains(
        "platform/modern_character_install.c",
        "the package file changed after review; validate the new bytes before installing",
    )
    require_contains(
        "tools/character_package_manager.py",
        '"the installed character changed after review; review the "',
    )
    require_contains(
        "docs/MODDING.md",
        "Drag-and-drop stages the same review instead of bypassing it.",
    )
    require_contains(
        "docs/architecture/custom-character-pipeline.md",
        "describing those as “owned by the donor” would be incorrect.",
    )
    # Disable and delete have intentionally different recoverability. A later
    # Workshop-authored revision may exist only in the managed directory, so a
    # generic "remove cache" confirmation would materially understate loss.
    require_contains(
        "platform/app/ui_settings.cpp",
        "Disable is reversible and retains every Workshop revision, fit setting, review, and player assignment.",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "A revision created only inside the Workshop may have no other copy.",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "The external .mdkrchar file you originally chose is not touched.",
    )
    require_contains(
        "docs/MODDING.md",
        "Importing an update or saving a Portrait, Rig, or Profile Studio revision preserves that state.",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "It will be fully revalidated and compiled before becoming current. The present source stays retained",
    )
    require_contains(
        "platform/app/ui_settings.cpp",
        "Writes the exact authenticated mdkrchar source and refuses to overwrite an existing file.",
    )
    registry = (ROOT / "platform/modern_character_registry.c").read_text(
        encoding="utf-8"
    )
    if "registry_init(registry, directory, 0)" not in registry or \
            "registry_init(registry, directory, 1)" not in registry:
        raise AssertionError(
            "runtime discovery and Workshop inventory must retain distinct "
            "disabled-cache policies"
        )
    print("product claim boundaries passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
