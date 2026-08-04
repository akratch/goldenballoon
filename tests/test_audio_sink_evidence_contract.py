#!/usr/bin/env python3
"""Keep accepted-SDL evidence on the correct side of the sink boundary."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "platform" / "audi_port_dkr.c").read_text(encoding="utf-8")
SFX_SOURCE = (ROOT / "game" / "src" / "audiosfx.c").read_text(encoding="utf-8")
AUDIO_SOURCE = (ROOT / "game" / "src" / "audio.c").read_text(encoding="utf-8")
COMPAT_SOURCE = (ROOT / "platform" / "audio_compat.c").read_text(encoding="utf-8")
GAME_LOOP_SOURCE = (ROOT / "game" / "src" / "thread3_main.c").read_text(encoding="utf-8")
HOST_TICK_SOURCE = (ROOT / "platform" / "stubs_dkr.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)


def main() -> int:
    require('getenv("MDKR_AUDIO_SINK_DUMP")' in SOURCE,
            "accepted-sink capture must have its own opt-in environment variable")
    require('getenv("MDKR_AUDIO_DUMP")' in SOURCE,
            "deterministic synth capture must remain available")

    start = SOURCE.index("s32 osAiSetNextBuffer")
    end = SOURCE.index("/* ---- music tempo trace", start)
    sink = SOURCE[start:end]
    require(sink.index("audio_measure_and_dump") < sink.index("SDL_QueueAudio"),
            "existing deterministic capture must stay before the host sink")

    success = re.search(
        r"if \(SDL_QueueAudio\(s_dev, buf, size\) == 0\) \{(?P<body>.*?)\n\s*\} else \{",
        sink, re.DOTALL)
    require(success is not None, "native SDL success/failure branch is required")
    require("mdkr_audio_sink_evidence_accepted" in success.group("body"),
            "only SDL-success branch may capture accepted PCM")
    require("mdkr_audio_reconnect_note_accepted_stereo" in success.group("body"),
            "accepted evidence must follow the same repaired block bookkeeping")
    require("repairPending" in success.group("body"),
            "accepted evidence must label reconnect-repaired blocks")

    failure_tail = sink[success.end():]
    require("mdkr_audio_sink_evidence_rejected" in failure_tail,
            "SDL rejection must be visible to evidence telemetry")
    require("mdkr_audio_sink_evidence_dropped" in sink,
            "emergency queue drops must be visible to evidence telemetry")

    service_start = SOURCE.index("static void audio_sync_player_volume")
    service_end = SOURCE.index("void dkr_audio_service_summary", service_start)
    service = SOURCE[service_start:service_end]
    require(service.index("music_volume_config_set") <
            service.index("sndp_set_global_volume") <
            service.index("music_jingle_volume_set") <
            service.index("mdkr_audio_gain_ramp_target"),
            "a coherent live volume generation must update music, SFX/jingles, then master")
    require(service.index("audio_sync_player_volume();") <
            service.index("amAudioSynthFrame") <
            service.index("mdkr_audio_gain_ramp_apply_s16") <
            service.index("osAiSetNextBuffer"),
            "music/SFX buses and the master ramp must reach PCM before submission")

    require("if (!gSoundPlayerReady)" in SFX_SOURCE and
            "gSoundPlayerReady = TRUE;" in SFX_SOURCE,
            "pre-init SFX updates must retain their scalar without posting to an unready queue")
    require("case AL_SNDP_GLOBAL_VOL_EVT:" in SFX_SOURCE and
            "alSynSetVol(\n                        sndp->drvr, &soundState->voice, volume, 1000);" in SFX_SOURCE,
            "live SFX gain refresh must use the authored nonzero ramp")
    require("if (seqp == NULL) {\n        return;\n    }" in COMPAT_SOURCE and
            "void alCSPSetVol(ALCSPlayer *seqp, s16 vol)" in COMPAT_SOURCE,
            "music/jingle bus synchronization must tolerate an absent sequence player")

    pause_start = GAME_LOOP_SOURCE.index("const s32 overlayPaused = platformOverlayWantsInput()")
    pause_rate = GAME_LOOP_SOURCE.index(
        "logicUpdateRate = 0;", pause_start,
        GAME_LOOP_SOURCE.index("if (get_lockup_status())", pause_start))
    audio_queue = GAME_LOOP_SOURCE.index(
        "sound_update_queue(logicUpdateRate);", pause_rate)
    require(pause_start < pause_rate < audio_queue,
            "overlay pause must freeze game-timed audio work with simulation")
    host_tick_start = HOST_TICK_SOURCE.index("g_simTickCounter++")
    host_tick_end = HOST_TICK_SOURCE.index("const bool exit_requested", host_tick_start)
    host_tick = HOST_TICK_SOURCE[host_tick_start:host_tick_end]
    require(host_tick.index("dkr_audio_advance_fields") <
            host_tick.index("dkr_audio_service_tick"),
            "already-playing audio must remain independently serviced while overlay holds simulation")
    require("mdkr64_engine_overlay_audio_pause" in HOST_TICK_SOURCE and
            "sound_overlay_pause_set(TRUE);" in HOST_TICK_SOURCE and
            "mdkr64_engine_overlay_audio_resume" in HOST_TICK_SOURCE,
            "F1 overlay must apply an independent pause mix")
    require("sndp_set_overlay_pause(next);" in AUDIO_SOURCE and
            "sound_volume_change((s32)mode);" in HOST_TICK_SOURCE,
            "an authored mode change must compose beneath the overlay layer")
    for function in ("void music_volume_set", "void music_volume_config_set"):
        start = AUDIO_SOURCE.index(function)
        end = AUDIO_SOURCE.index("\n}", start)
        require("sOverlayPauseMix" in AUDIO_SOURCE[start:end],
                f"{function} must preserve the pause duck during live updates")
    jingle_start = AUDIO_SOURCE.index("void music_jingle_volume_set")
    jingle_end = AUDIO_SOURCE.index("\n}", jingle_start)
    require("sOverlayPauseMix" in
            AUDIO_SOURCE[jingle_start:jingle_end],
            "effects preview must not unmute jingles during overlay pause")
    print("audio sink evidence contract: pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
