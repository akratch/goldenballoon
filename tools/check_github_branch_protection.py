#!/usr/bin/env python3
"""Validate GitHub branch-protection JSON for mdkr64 launch readiness."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def nested_enabled(node: dict[str, object], keys: tuple[str, ...]) -> bool:
    value: object = node
    for key in keys:
        if not isinstance(value, dict) or key not in value:
            return False
        value = value[key]
    return bool(value) if isinstance(value, bool) else False


def emit(findings: list[tuple[str, str]], level: str, message: str) -> None:
    findings.append((level, message))


def check_branch_protection(
    protection: dict[str, object],
    required_checks: set[str],
) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    status_checks = protection.get("required_status_checks")

    if required_checks:
        if not isinstance(status_checks, dict):
            emit(findings, "fail", "main branch protection does not require status checks")
        else:
            emit(findings, "ok", "main branch protection requires status checks")
            if status_checks.get("strict") is True:
                emit(findings, "ok", "required status checks must be up to date before merge")
            else:
                emit(findings, "fail", "required status checks are not set to strict/up-to-date")

            contexts = set()
            for context in status_checks.get("contexts") or []:
                if isinstance(context, str):
                    contexts.add(context)
            for check in status_checks.get("checks") or []:
                if isinstance(check, dict) and isinstance(check.get("context"), str):
                    contexts.add(str(check["context"]))

            missing = sorted(required_checks - contexts)
            if missing:
                emit(
                    findings,
                    "fail",
                    "main branch protection is missing required status check(s): "
                    + ", ".join(missing),
                )
            else:
                emit(findings, "ok", "main branch protection requires configured status checks")
    elif isinstance(status_checks, dict):
        contexts = set()
        for context in status_checks.get("contexts") or []:
            if isinstance(context, str):
                contexts.add(context)
        for check in status_checks.get("checks") or []:
            if isinstance(check, dict) and isinstance(check.get("context"), str):
                contexts.add(str(check["context"]))
        if contexts:
            emit(
                findings,
                "fail",
                "main branch protection requires hosted status check(s), but launch policy is local-CI only: "
                + ", ".join(sorted(contexts)),
            )
        else:
            emit(findings, "ok", "main branch protection has no required hosted status checks")
        if status_checks.get("strict") is True:
            emit(findings, "warn", "strict status-check mode is enabled without required hosted checks")
    else:
        emit(findings, "ok", "main branch protection has no required hosted status checks")

    pr_reviews = protection.get("required_pull_request_reviews")
    if isinstance(pr_reviews, dict):
        count = int(pr_reviews.get("required_approving_review_count") or 0)
        if count >= 1:
            emit(findings, "ok", f"pull request review requirement is enabled ({count} approval(s))")
        else:
            emit(findings, "warn", "pull request reviews are enabled but no approving review is required")
    else:
        emit(findings, "warn", "pull request reviews are not required for main")

    if nested_enabled(protection, ("enforce_admins", "enabled")):
        emit(findings, "ok", "branch protection applies to administrators")
    else:
        emit(findings, "warn", "branch protection does not apply to administrators")

    if nested_enabled(protection, ("allow_force_pushes", "enabled")):
        emit(findings, "fail", "force pushes are allowed on main")
    else:
        emit(findings, "ok", "force pushes are disabled on main")

    if nested_enabled(protection, ("allow_deletions", "enabled")):
        emit(findings, "fail", "branch deletion is allowed on main")
    else:
        emit(findings, "ok", "branch deletion is disabled on main")

    if nested_enabled(protection, ("required_conversation_resolution", "enabled")):
        emit(findings, "ok", "conversation resolution is required before merge")
    else:
        emit(findings, "warn", "conversation resolution is not required before merge")

    return findings


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("json", help="branch protection JSON returned by GitHub")
    parser.add_argument(
        "--required-check",
        action="append",
        dest="required_checks",
        default=[],
        help=(
            "required status-check context; may be repeated. Defaults to none "
            "because public launch uses local preflight evidence instead of hosted CI."
        ),
    )
    parser.add_argument(
        "--format",
        choices=("text", "tabs"),
        default="text",
        help="output format; tabs emits LEVEL<TAB>MESSAGE for shell callers",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    required_checks = set(args.required_checks)
    try:
        with Path(args.json).open("r", encoding="utf-8") as handle:
            protection = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: could not read branch protection JSON: {exc}", file=sys.stderr)
        return 2

    if not isinstance(protection, dict):
        print("ERROR: branch protection JSON root is not an object", file=sys.stderr)
        return 2

    findings = check_branch_protection(protection, required_checks)
    for level, message in findings:
        if args.format == "tabs":
            print(f"{level}\t{message}")
        else:
            print(f"{level.upper()}: {message}")

    return 1 if any(level == "fail" for level, _ in findings) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
