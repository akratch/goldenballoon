#!/usr/bin/env python3
"""Guard the launcher-to-game deterministic disconnect-takeover seam."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def between(source: str, start: str, end: str) -> str:
    first = source.index(start)
    return source[first : source.index(end, first + len(start))]


def validate(transport: str, runtime: str, racer: str, app: str) -> None:
    schedule = between(
        transport,
        "MdkrMatchTakeoverResult mdkr_match_transport_schedule_ai_takeover(",
        "bool mdkr_match_transport_ai_takeover_mask_for_tick(",
    )
    assert "match_epoch != transport->match_epoch" in schedule
    assert "mdkr_net_tick_after(activation_tick," in schedule
    assert "MDKR_MATCH_TAKEOVER_DUPLICATE" in schedule
    assert "MDKR_MATCH_TAKEOVER_CONFLICT" in schedule
    assert "cell->status[slot] == MDKR_NET_INPUT_RECEIVED" in schedule

    receive = between(
        transport,
        "MdkrMatchTransportIngressResult mdkr_match_transport_receive(",
        "bool mdkr_match_transport_drain_tick(",
    )
    assert "MDKR_MATCH_INGRESS_TAKEN_OVER" in receive
    assert "takeover_ignored_inputs++" in receive
    drain = between(
        transport,
        "bool mdkr_match_transport_drain_tick(",
        "bool mdkr_match_transport_take_dirty(",
    )
    assert "next.ai_takeover_started_mask |= bit" in drain
    assert "next.stats.takeover_started++" in drain
    assert "(MdkrPadSample){0u, 0, 0, 1u}" in drain

    prepare = between(
        runtime,
        "bool mdkr_rollback_game_runtime_prepare_tick(",
        "static bool record_bootstrap_input_boundary(",
    )
    assert prepare.index("mdkr_match_input_runtime_drain(") < prepare.index(
        "mdkr_match_input_runtime_begin_tick(tick)"
    ) < prepare.index("input_rollback_apply_from_previous(")
    replay = between(
        runtime,
        "static bool reconcile_network_inputs(",
        "bool mdkr_rollback_game_runtime_prepare_tick(",
    )
    assert replay.index("mdkr_match_input_runtime_begin_tick(tick)") < replay.index(
        "resimulate_timed("
    )
    timed = between(
        runtime,
        "static bool resimulate_timed(",
        "static void log_pool_mutation(",
    )
    assert "mdkr_game_resimulate_tick(update_rate, input)" in timed

    update = between(racer, "void update_player_racer(", "void racer_enter_door(")
    assert "mdkr_match_input_runtime_slot_ai_controlled(" in update
    takeover_dispatch = between(
        update,
        "const s16 savedPlayerIndex = tempRacer->playerIndex;",
        "/* Observation-only opponent stuck-recovery witness",
    )
    save = takeover_dispatch.index(
        "const s16 savedPlayerIndex = tempRacer->playerIndex;"
    )
    cpu = takeover_dispatch.index(
        "tempRacer->playerIndex = PLAYER_COMPUTER;", save
    )
    ai = takeover_dispatch.index("update_AI_racer(", cpu)
    restore = takeover_dispatch.index(
        "tempRacer->playerIndex = savedPlayerIndex;", ai
    )
    assert save < cpu < ai < restore

    assert "[NET-TAKEOVER]" in app
    assert "policy=ai-no-handback" in app
    assert "takeoverStarted=%u takeoverIgnored=%u" in app


def main() -> None:
    paths = (
        ROOT / "platform/net/match_transport.c",
        ROOT / "platform/rollback/rollback_game_runtime.c",
        ROOT / "game/src/racer.c",
        ROOT / "platform/app/main_app.cpp",
    )
    sources = [path.read_text(encoding="utf-8") for path in paths]
    validate(*sources)
    mutations = (
        (0, "MDKR_MATCH_INGRESS_TAKEN_OVER", "MDKR_MATCH_INGRESS_ACCEPTED"),
        (0, "next.stats.takeover_started++;", ""),
        (1, "mdkr_match_input_runtime_begin_tick(tick) ||", "true ||"),
        (2, "if (networkAiTakeover) tempRacer->playerIndex = savedPlayerIndex;", ""),
        (3, "policy=ai-no-handback", "policy=handback"),
    )
    for index, old, new in mutations:
        broken = list(sources)
        assert old in broken[index]
        broken[index] = broken[index].replace(old, new, 1)
        try:
            validate(*broken)
        except (AssertionError, ValueError):
            continue
        raise AssertionError(f"AI takeover mutation survived: {old}")
    print("test_ai_takeover_adapter: PASS")


if __name__ == "__main__":
    main()
