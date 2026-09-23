#!/usr/bin/env python3
"""Keyboard/gamepad reachability for the closed LAN phone panel.

Asset-free: the existing party trace relaxes ROM readiness, not transport
availability. Does not open a room or certify ready-ROM / connected-room UI.
"""
import argparse
from pathlib import Path
import tempfile

from check_a11y_shell import session, run, utterances
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", help="accepted for suite compatibility; unused")
    parser.add_argument("--size", default="640x480")
    parser.add_argument("--input", choices=("both", "keyboard", "gamepad"), default="both")
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="mdkr-phone-panel-focus-") as tmp:
        for mode in ("keyboard", "gamepad"):
            if args.input not in ("both", mode):
                continue
            _, env = session(Path(tmp), mode)
            env.update(MDKR_APP_PANEL="Play", MDKR_APP_PARTY_TRACE="1",
                       MDKR_APP_SMOKE_WINDOW_SIZE=args.size,
                       MDKR_APP_SMOKE_FRAMES="200", MDKR_APP_SMOKE_A11Y_WALK="1",
                       MDKR_APP_SMOKE_INPUT=mode,
                       MDKR_APP_SMOKE_INPUT_TOKEN="mdkr64-app-ui-input-v1")
            output = run(Path(resolve_binary(args.build)).resolve(), env, mode, 180)
            focused = [text for category, _, text in utterances(output) if category == "focus"]
            if "[app] party:" not in output or "local=1" not in output:
                failures.append(f"{mode}: phone surface unavailable")
            elif not any(text.startswith("Start Local Play.") for text in focused):
                failures.append(f"{mode}: Start Local Play not reached; focused={sorted(set(focused))}")
            else:
                print(f"PASS: {mode} reaches Start Local Play at {args.size}", flush=True)
    if failures:
        raise SystemExit("\n".join(failures))


if __name__ == "__main__":
    main()
