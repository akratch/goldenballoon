#!/usr/bin/env python3
"""Keep vanished predicted rumble wired to a host-only motor stop."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / "platform/rollback/rollback_game_runtime.c"
STUBS = ROOT / "platform/stubs_dkr.c"


def validate(runtime: str, stubs: str) -> None:
    assert "cancel_rollback_effect, &sRollbackGameRuntime" in runtime, (
        "the production journal must install its reconciliation callback"
    )
    cancel_start = runtime.index("static void cancel_rollback_effect(")
    cancel_end = runtime.index("static void begin_effect_tick(", cancel_start)
    cancel = runtime[cancel_start:cancel_end]
    assert "event->id.kind == GAMEPLAY_EVENT_RUMBLE" in cancel
    assert "mdkr_rollback_rumble_cancel_preview(" in cancel

    adapter_start = stubs.index(
        "void mdkr_rollback_rumble_cancel_preview(unsigned controller_index)"
    )
    adapter_end = stubs.index(
        "/* ======================================================================== *",
        adapter_start,
    )
    adapter = stubs[adapter_start:adapter_end]
    assert adapter.index("controller_index < MAXCONTROLLERS") < adapter.index(
        "platform_pad_rumble((int)controller_index, 0)"
    ), "the motor stop must be bounds-checked and must never start vibration"


def main() -> None:
    runtime = RUNTIME.read_text(encoding="utf-8")
    stubs = STUBS.read_text(encoding="utf-8")
    validate(runtime, stubs)
    for broken_runtime, broken_stubs in (
        (
            runtime.replace(
                "cancel_rollback_effect, &sRollbackGameRuntime",
                "NULL, &sRollbackGameRuntime",
                1,
            ),
            stubs,
        ),
        (
            runtime,
            stubs.replace(
                "platform_pad_rumble((int)controller_index, 0)",
                "platform_pad_rumble((int)controller_index, 1)",
                1,
            ),
        ),
    ):
        try:
            validate(broken_runtime, broken_stubs)
        except (AssertionError, ValueError):
            continue
        raise AssertionError("rumble adapter positive control did not fail")
    print("test_rumble_rollback_adapter: PASS")


if __name__ == "__main__":
    main()
