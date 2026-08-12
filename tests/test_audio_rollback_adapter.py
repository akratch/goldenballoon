#!/usr/bin/env python3
"""Guard the production SFX rollback seam, not just its pure state machine."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def function(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index(next_signature, start + len(signature))]


def validate(
    audio: str,
    sfx: str,
    runtime: str,
    authority: str,
    main_pc: str,
    particles: str,
) -> None:
    table = function(audio, "void sound_play(", "void sound_play_spatial(")
    spatial = function(audio, "void sound_play_spatial(", "void sound_play_direct(")
    direct = function(audio, "void sound_play_direct(", "void sound_volume_set_relative(")
    assert table.index("GAMEPLAY_EVENT_TRACE(") < table.index(
        "rollback_sound_request("
    ) < table.index("sound_play_table_raw(")
    assert "MDKR_ROLLBACK_AUDIO_TABLE" in table
    assert spatial.index("GAMEPLAY_EVENT_TRACE(") < spatial.index(
        "rollback_sound_request("
    ) < spatial.index("sound_play_table_raw(")
    assert "MDKR_ROLLBACK_AUDIO_SPATIAL" in spatial
    assert "sound_position_hash(x, y, z)" in spatial
    assert direct.index("GAMEPLAY_EVENT_TRACE(") < direct.index(
        "rollback_sound_request("
    ) < direct.index("sound_play_direct_raw(")
    assert "MDKR_ROLLBACK_AUDIO_DIRECT" in direct

    apply = function(
        sfx,
        "static void apply_rollback_audio_command(",
        "static bool defer_rollback_audio_command(",
    )
    assert apply.index("handle->rollbackGeneration != command->generation") < apply.index(
        "sndp_stop_raw(handle)"
    )
    assert "ROLLBACK_AUDIO_COMMAND_PARAM_BASE" in apply
    for signature, raw_call in (
        ("void sndp_set_priority(", "sndp_set_priority_raw("),
        ("void sndp_stop(", "sndp_stop_raw("),
        ("void sndp_set_param(", "sndp_set_param_raw("),
    ):
        body = sfx[sfx.index(signature) :]
        assert body.index("defer_rollback_audio_command(") < body.index(raw_call)
    assert "state->rollbackGeneration = sRollbackSoundGeneration" in sfx
    assert "sndp_stop_rollback_voice(" in sfx

    # A persistent launcher reuses process globals across engine arenas. The
    # audio-player roots must be retired before the first allocation in the
    # replacement arena; merely rebuilding the list would dereference freed
    # links while setting group volume during the second epoch.
    init_player = function(sfx, "void sndp_init_player(", "ALMicroTime sndp_voice_handler(")
    first_allocation = init_player.index("alHeapAlloc(")
    for reset in (
        "gSoundStateLists.allocHead = NULL;",
        "gSoundStateLists.allocTail = NULL;",
        "gSoundStateLists.freeHead = NULL;",
        "gSoundGroupVolume = NULL;",
        "*gSoundPlayerPtr = (SoundPlayer){0};",
    ):
        assert init_player.index(reset) < first_allocation

    end_rewrite = function(
        runtime,
        "static void end_effect_rewrite(",
        "static void confirm_effects_through(",
    )
    assert end_rewrite.index("mdkr_rollback_events_end_rewrite(") < end_rewrite.index(
        "mdkr_rollback_audio_flush_commands("
    )
    assert "audspat_rollback_view(&audio_spatial)" in authority
    assert "TAG_ALLOC_AUDIO_POINT_POOL" in authority
    assert "MDKR_AUDIO_SPATIAL_ROLLBACK_STATE_SPAN_COUNT" in authority

    shutdown = main_pc[main_pc.index("shutdown:") :]
    particle_reset = shutdown.index("reset_particles_with_assets();")
    assert particle_reset < shutdown.index("dkr_arena_shutdown();")
    particle_reset_body = function(
        particles, "void reset_particles_with_assets(", "void free_particle_vertices_triangles("
    )
    for release in (
        "free_particle_buffers();",
        "free_particle_vertices_triangles();",
        "free_particle_assets();",
        "particle_free_dummy();",
    ):
        assert release in particle_reset_body


def main() -> None:
    paths = (
        ROOT / "game/src/audio.c",
        ROOT / "game/src/audiosfx.c",
        ROOT / "platform/rollback/rollback_game_runtime.c",
        ROOT / "platform/rollback/rollback_game_authority.c",
        ROOT / "platform/main_pc.c",
        ROOT / "game/src/particles.c",
    )
    sources = [path.read_text(encoding="utf-8") for path in paths]
    validate(*sources)
    mutations = (
        (
            0,
            "MDKR_ROLLBACK_AUDIO_SPATIAL, soundID, handlePtr, x, y, z",
            "MDKR_ROLLBACK_AUDIO_TABLE, soundID, handlePtr, x, y, z",
        ),
        (1, "handle->rollbackGeneration != command->generation", "false"),
        (2, "mdkr_rollback_audio_flush_commands(&runtime->audio);", ""),
        (3, "audspat_rollback_view(&audio_spatial)", "true"),
        (1, "gSoundStateLists.allocHead = NULL;", ""),
        (4, "reset_particles_with_assets();", "free_particle_assets();"),
    )
    for source_index, old, new in mutations:
        broken = list(sources)
        assert old in broken[source_index]
        broken[source_index] = broken[source_index].replace(old, new, 1)
        try:
            validate(*broken)
        except (AssertionError, ValueError):
            continue
        raise AssertionError(f"audio rollback positive control survived mutation: {old}")
    print("test_audio_rollback_adapter: PASS")


if __name__ == "__main__":
    main()
