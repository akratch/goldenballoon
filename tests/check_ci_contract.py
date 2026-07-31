#!/usr/bin/env python3
"""Fail closed if the repository correctness workflow loses required coverage."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "correctness.yml"
PINNED_ACTION_RE = re.compile(r"^\s*-\s+uses:\s+([^@\s]+)@([^\s#]+)", re.MULTILINE)


def validate(source: str) -> list[str]:
    required = {
        "push trigger": "\n  push:",
        "pull-request trigger": "\n  pull_request:",
        "manual trigger": "\n  workflow_dispatch:",
        "read-only permissions": "\npermissions:\n  contents: read",
        "full-history clean-room checkout": "fetch-depth: 0",
        "clean-room history gate": "tools/check_clean_room.sh",
        "native SDK surface gate": "tools/check_native_sdk_surface.py",
        "third-party notice gate": "tools/check_third_party_notices.py",
        "image metadata gate": "tools/check_image_metadata.py",
        "address-domain gate": "tests/check_address_domains.py",
        "Linux WebGPU cell": "label: Linux WebGPU",
        "Linux OpenGL cell": "label: Linux OpenGL-only",
        "explicit Linux GCC compiler": "compiler: gcc",
        "macOS WebGPU cell": "label: macOS WebGPU warnings",
        "matrix compiler selection": "CC: ${{ matrix.compiler }}",
        "warning register": "-Wsometimes-uninitialized",
        "ROM-free CTest": "ctest --test-dir build-ci --output-on-failure",
        "ASan+UBSan": "-fsanitize=address,undefined",
        "linked wasm build": "tools/web/build_web.sh",
        "linked wasm layout": "tests/check_wave_visible_table.py",
        "browser save custody": "tests/check_browser_save_ui.py",
        "artifact ROM guard": "tools/check_no_rom.sh dist/web",
    }
    failures = [
        f"missing {label}: {needle!r}"
        for label, needle in required.items()
        if needle not in source
    ]
    actions = PINNED_ACTION_RE.findall(source)
    if not actions:
        failures.append("workflow has no actions")
    for action, revision in actions:
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            failures.append(
                f"{action} is not pinned to an immutable 40-character SHA: "
                f"{revision!r}"
            )
    forbidden = {
        "write-all permissions": "write-all",
        "repository write permission": "contents: write",
        "ROM secret/input": "MDKR_ROM",
        "untrusted pull-request target": "pull_request_target",
    }
    failures.extend(
        f"forbidden {label}: {needle!r}"
        for label, needle in forbidden.items()
        if needle in source
    )
    return failures


def main() -> int:
    source = WORKFLOW.read_text(encoding="utf-8")
    failures = validate(source)
    if failures:
        raise AssertionError("CI contract drift:\n  " + "\n  ".join(failures))

    # A gate that cannot reject regressions is documentation, not a control.
    controls = {
        "trigger": source.replace("\n  pull_request:", "\n  removed:", 1),
        "matrix": source.replace("label: Linux WebGPU", "label: removed", 1),
        "compiler": source.replace("compiler: gcc", "compiler: removed"),
        "action pin": re.sub(
            r"(@)[0-9a-f]{40}", r"\1v4", source, count=1
        ),
        "ROM guard": source.replace("tools/check_no_rom.sh dist/web", "true", 1),
        "native SDK surface guard": source.replace(
            "tools/check_native_sdk_surface.py", "true", 1
        ),
        "third-party notice guard": source.replace(
            "tools/check_third_party_notices.py", "true", 1
        ),
        "image metadata guard": source.replace(
            "tools/check_image_metadata.py", "true", 1
        ),
    }
    escaped = [name for name, broken in controls.items() if not validate(broken)]
    if escaped:
        raise AssertionError(
            "CI positive controls unexpectedly passed: " + ", ".join(escaped)
        )

    action_count = len(PINNED_ACTION_RE.findall(source))
    print(
        "check_ci_contract: PASS — push/PR/manual policy, "
        "3-cell native matrix, sanitizer lane, linked wasm/save custody, "
        f"ROM guards, and {action_count} immutable action pins"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
