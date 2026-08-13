#!/usr/bin/env python3
"""Walk every native Online Room gallery view and require spoken controls."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import resolve_binary


COPY_PREFIX = "online-gallery-copy-v2|"
SPEAK_RE = re.compile(r"^\[SPEAK\] cat=(\S+) pri=(\S+) text=(.*)$",
                      re.MULTILINE)
TRACE_RE = re.compile(r"\[online-gallery\] rendered slug=([a-z0-9-]+)")
SPEC_RE = re.compile(
    r"^online-gallery-v1 slug=([a-z0-9-]+).* admission=([01]) "
    r"timeout-visible=[01]$"
)
INPUT_TOKEN = "mdkr64-app-ui-input-v1"


@dataclass(frozen=True)
class CopyCase:
    slug: str
    title: str
    actions: tuple[str, ...]
    verification_phrase: str
    requires_admission: bool


class A11yError(RuntimeError):
    pass


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def run(binary: str, environment: dict[str, str], label: str,
        timeout: int) -> str:
    completed = subprocess.run(
        [binary], env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise A11yError(
            f"{label} exited {completed.returncode}\n{completed.stdout[-4000:]}"
        )
    return completed.stdout


def discover(binary: str, root: Path, timeout: int) -> list[CopyCase]:
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_DUMP_ONLINE_GALLERY="1",
        MDKR_APP_PREFS_DIR=str(root / "inventory-prefs"),
        MDKR_VIDEO_CONFIG_PATH=str(root / "inventory-video.ini"),
        MDKR_SAVE_DIR=str(root / "inventory-saves"),
        MDKR_AUDIO="0",
    )
    (root / "inventory-prefs").mkdir()
    (root / "inventory-saves").mkdir()
    output = run(binary, environment, "gallery copy inventory", timeout)
    admission = {
        match.group(1): match.group(2) == "1"
        for match in map(SPEC_RE.match, output.splitlines()) if match
    }
    rows: list[CopyCase] = []
    for line in output.splitlines():
        if not line.startswith(COPY_PREFIX):
            continue
        fields = line.split("|")
        if len(fields) != 8:
            raise A11yError(f"malformed copy inventory row: {line}")
        (_version, slug, title, primary, secondary, cancel, timeout_action,
         verification_phrase) = fields
        actions = tuple(
            dict.fromkeys(label for label in
                          (primary, secondary, cancel, timeout_action) if label)
        )
        if not slug or not title or not actions:
            raise A11yError(f"incomplete copy inventory row: {line}")
        if slug not in admission:
            raise A11yError(f"copy row has no semantic inventory row: {slug}")
        rows.append(CopyCase(slug, title, actions, verification_phrase,
                             admission[slug]))
    header = re.search(r"^online-gallery-v1 count=(\d+)$", output, re.MULTILINE)
    if header is None or len(rows) != int(header.group(1)):
        raise A11yError(
            f"copy inventory count mismatch: rows={len(rows)}, "
            f"header={header.group(1) if header else 'missing'}"
        )
    phrase_rows = [case for case in rows if case.verification_phrase]
    if (len(phrase_rows) != 1 or
            phrase_rows[0].actions[:2] != ("Words Match", "Words Differ")):
        raise A11yError(
            "gallery must expose exactly one view with both phrase decisions"
        )
    return rows


def walk(binary: str, root: Path, case: CopyCase, timeout: int) -> None:
    case_root = root / case.slug
    prefs = case_root / "prefs"
    saves = case_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    config = case_root / "video.ini"
    config.write_text("[Accessibility]\nSpeech=1\n", encoding="utf-8")
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_PREFS_DIR=str(prefs),
        MDKR_VIDEO_CONFIG_PATH=str(config),
        MDKR_SAVE_DIR=str(saves),
        MDKR_AUDIO="0",
        MDKR_A11Y_TRACE="1",
        MDKR64_HIDDEN="1",
        MDKR_APP_PANEL="Online Room",
        MDKR_APP_ONLINE_FAKE="1",
        MDKR_APP_ONLINE_GALLERY=case.slug,
        # Four-action timeout recovery plus the launcher rail needs more than
        # one short focus cycle at 960x720. Keep one fixed budget for every
        # case so a layout change cannot selectively starve the hard cases.
        MDKR_APP_SMOKE_FRAMES="72",
        MDKR_APP_SMOKE_A11Y_WALK="1",
        MDKR_APP_SMOKE_INPUT="keyboard",
        MDKR_APP_SMOKE_INPUT_TOKEN=INPUT_TOKEN,
    )
    if case.requires_admission:
        environment["MDKR_APP_ONLINE_FAKE_ALLOW_START"] = "1"
    output = run(binary, environment, case.slug, timeout)
    traces = TRACE_RE.findall(output)
    if traces != [case.slug]:
        raise A11yError(
            f"{case.slug}: expected one matching semantic render, got {traces}"
        )
    spoken = SPEAK_RE.findall(output)
    status = [(priority, text) for category, priority, text in spoken
              if category == "status"]
    focus = [text for category, _priority, text in spoken
             if category == "focus"]
    if not any(text.startswith(case.title + ".") for _priority, text in status):
        raise A11yError(
            f"{case.slug}: title/status was not announced; got {status[:5]}"
        )
    if case.slug.startswith("failure-") and not any(
            priority == "critical" for priority, _text in status):
        raise A11yError(f"{case.slug}: recovery announcement was not assertive")
    if case.verification_phrase and not any(
            case.verification_phrase in text and
            "Do not continue if even 1 word differs." in text
            for _priority, text in status):
        raise A11yError(
            f"{case.slug}: verification phrase or mismatch warning was not "
            f"announced; got {status[:5]}"
        )
    missing = [label for label in case.actions
               if not any(text.startswith(label + ",") for text in focus)]
    if missing:
        raise A11yError(
            f"{case.slug}: keyboard focus never voiced {missing}; "
            f"heard={focus[:12]}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-online-a11y-") as temporary:
            root = Path(temporary)
            cases = discover(binary, root, args.timeout)
            for case in cases:
                walk(binary, root, case, args.timeout)
    except (A11yError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL online room accessibility: {error}")
        return 1
    print(
        "PASS online room accessibility: "
        f"cases={len(cases)} titles={len(cases)} keyboard-action-sets={len(cases)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
