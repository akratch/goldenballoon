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
    require_contains(
        "README.md",
        "The complete start-to-credits campaign is not automated or claimed complete.",
    )
    require_contains(
        "README.md",
        "Linux does not yet have a native **Choose ROM File** dialog.",
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
        "**No native file dialog on Linux.**",
    )
    require_contains(
        "docs/APP_SHELL.md",
        "Drag-and-drop and the typed path are the documented\npaths on Linux",
    )
    print("product claim boundaries passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
