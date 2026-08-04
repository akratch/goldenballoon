#!/usr/bin/env python3
"""Keep complete-ROM hashing and revalidation off the ImGui thread."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parent.parent
UI_ROM = (ROOT / "platform/app/ui_rom.cpp").read_text(encoding="utf-8")
UI_LAUNCHER = (ROOT / "platform/app/ui_launcher.cpp").read_text(encoding="utf-8")
VALIDATOR = (ROOT / "platform/app/rom_validate.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "platform/app/main_app.cpp").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    require("mdkr_validate_rom(" not in UI_ROM,
            "ROM panel must not hash a complete image on the UI thread")
    require("mdkr_validate_rom(" not in UI_LAUNCHER,
            "Play must use the asynchronous final-ROM check")
    for fragment in (
        "std::thread", "std::condition_variable", "latest_.fetch_add",
        "mdkr_validate_rom_progress", "info.cancelled", "Cancel Check",
        "ImGui::ProgressBar", "ValidationPurpose::Play",
    ):
        require(fragment in UI_ROM,
                f"asynchronous ROM workflow is missing {fragment!r}")
    require("kReadChunk = 64u * 1024u" in VALIDATOR,
            "ROM reader must retain bounded cancellation/progress points")
    require("ROM validation cancelled." in VALIDATOR,
            "cancelled checks need a distinct non-error result")
    stale_check = UI_ROM.index("static int progress")
    stale_publish = UI_ROM.index("progressBytes_.store", stale_check)
    stale_generation_check = UI_ROM.index("latest_.load", stale_check)
    require(stale_generation_check < stale_publish,
            "a cancelled/stale worker must not publish progress into a newer check")
    require("static void cancelValidation" in UI_ROM and
            "cancelValidation(s, /*clearUnusableSelection=*/false);" in UI_ROM,
            "Cancel Change and Forget Saved Path must invalidate pending work")
    require("romPlayValidationPassed" in UI_LAUNCHER and
            "action.type = LauncherActionType::Play" in UI_LAUNCHER,
            "Play may transition only after the final worker result publishes")
    require("async ROM check serviceFrames=" in MAIN and
            "validationDeadline" in MAIN,
            "live drop smoke must wait responsively for worker publication")
    retry_start = UI_ROM.index("static void retryCandidatePersistence")
    retry_end = UI_ROM.index("static void requestValidation", retry_start)
    retry_body = UI_ROM[retry_start:retry_end]
    require("const std::string candidatePath = s.romCandidatePath;" in retry_body and
            "clearCandidateFeedback(s);" in retry_body and
            "requestValidation(s, ValidationPurpose::Selection, candidatePath);" in retry_body and
            "AppConfig::setAndSave" not in retry_body and
            "s.romPath =" not in retry_body,
            "Retry must copy the candidate path, clear stale feedback, and "
            "revalidate before it can persist or replace the active ROM")
    print("async ROM validation contract passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
