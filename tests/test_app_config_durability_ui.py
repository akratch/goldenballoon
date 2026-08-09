#!/usr/bin/env python3
"""Keep launcher preference durability outcomes honest at every UI boundary."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / "platform" / "app" / "app_config.h").read_text(encoding="utf-8")
SETTINGS = (ROOT / "platform" / "app" / "ui_settings.cpp").read_text(encoding="utf-8")
ROM = (ROOT / "platform" / "app" / "ui_rom.cpp").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    require("enum class PersistResult" in HEADER,
            "AppConfig persistence must expose a typed tri-state result")
    for outcome in ("Failed", "Durable", "DurabilityUnconfirmed"):
        require(re.search(rf"\b{outcome}\b", HEADER) is not None,
                f"AppConfig result is missing {outcome}")
    require("persistResultApplied" in HEADER,
            "visible-but-unconfirmed writes need one shared applied predicate")

    # Both the initial UI-scale commit and its Retry action must retire their
    # dirty/error state after a visible replacement, while presenting a warning
    # instead of claiming a durable save.
    require(SETTINGS.count("AppConfig::persistResultApplied(persist)") == 2,
            "both UI-scale save paths must accept visible unconfirmed writes")
    require(SETTINGS.count("PersistResult::DurabilityUnconfirmed") >= 2,
            "both UI-scale save paths must distinguish durability uncertainty")
    require("UI scale applied, but was not confirmed written to disk" in SETTINGS,
            "UI-scale warning must say the committed scale was applied")

    # A freshly validated replacement and Forget both act on a path only once
    # it was applied. Retry deliberately re-enters that same validation and
    # replacement transaction, so it cannot promote stale cached identity.
    # An unconfirmed replacement must remain active with a natural restart
    # warning; it must never use the old failure path claiming nothing changed.
    require(ROM.count("AppConfig::persistResultApplied(persist)") == 2,
            "validated ROM replacement and Forget must share applied-result "
            "handling")
    require("BrandPrimaryButton(\"Retry Save\"" in ROM and
            "retryCandidatePersistence" in ROM and
            "const std::string candidatePath = s.romCandidatePath;" in ROM and
            "clearCandidateFeedback(s);" in ROM and
            "requestValidation(s, ValidationPurpose::Selection, candidatePath);" in ROM,
            "a retained replacement must be revalidated before Retry can "
            "persist or promote it")
    require("ROM path applied, but was not confirmed written to disk" in ROM,
            "ROM replacement must surface durability uncertainty after activation")
    require("ROM path forgotten for this session, but storage durability was not " in ROM,
            "Forget must acknowledge its visible result when durability is uncertain")
    require(not re.search(r"if\s*\(!\s*AppConfig::setAndSave", ROM + SETTINGS),
            "no UI path may collapse AppConfig's tri-state result into bool")
    print("app config durability UI contract passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
