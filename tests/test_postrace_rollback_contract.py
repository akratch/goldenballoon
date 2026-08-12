#!/usr/bin/env python3
"""Keep post-race authored ticks replayable and their admission latches owned."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
AUTHORITY = ROOT / "platform/rollback/rollback_game_authority.c"
THREAD = ROOT / "game/src/thread3_main.c"


def validate(authority: str, thread: str) -> None:
    for scalar, tag in (
        ("gPostRaceViewPort", "TAG_POST_RACE_VIEWPORT"),
        ("D_801234F8", "TAG_SCENE_LOAD_PENDING"),
        ("D_801234FC", "TAG_SCENE_LOAD_MODE"),
    ):
        assert f"ADD_SCALAR(ranges, &count, {scalar}, {tag})" in authority
    start = thread.index("s32 mdkr_game_resimulate_tick(")
    end = thread.index("/**\n * Reset dialogue", start)
    replay = thread[start:end]
    assert "gPostRaceViewPort ||" not in replay
    assert "!gPostRaceViewPort" not in replay
    for required in (
        "sRollbackResimulating ||",
        "gIsPaused ||",
        "D_801234FC != 0 ||",
        "D_801234F8 ||",
        "gLevelLoadTimer != 0 ||",
        "textbox_visible() != 0",
    ):
        assert required in replay


def main() -> None:
    authority = AUTHORITY.read_text(encoding="utf-8")
    thread = THREAD.read_text(encoding="utf-8")
    validate(authority, thread)
    mutations = (
        (authority.replace(
            "ADD_SCALAR(ranges, &count, gPostRaceViewPort, TAG_POST_RACE_VIEWPORT)",
            "true", 1), thread),
        (authority, thread.replace(
            "gGameMode != GAMEMODE_INGAME || gIsPaused ||",
            "gGameMode != GAMEMODE_INGAME || gIsPaused || gPostRaceViewPort ||",
            1)),
        (authority, thread.replace("D_801234F8 ||", "false ||", 1)),
    )
    for broken_authority, broken_thread in mutations:
        try:
            validate(broken_authority, broken_thread)
        except (AssertionError, ValueError):
            continue
        raise AssertionError("post-race rollback positive control did not fail")
    print("test_postrace_rollback_contract: PASS")


if __name__ == "__main__":
    main()
