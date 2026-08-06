#!/usr/bin/env python3
"""Prove that the native ImGui overlay pauses and resumes a real race.

This enters through the production app-owned WebGPU window, navigates the
ordinary title-to-Time-Trial route, and lets DKR's own AI drive.  The scripted
overlay opens only after the race clock and racer are active.  While it is open,
both the user-facing race state and the complete v3 authoritative-state hash
must remain byte-for-byte fixed; after it closes, racing must resume.
"""

from __future__ import annotations

import argparse
from array import array
import math
import os
import re
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

from harness_utils import resolve_binary


TICKS = 3900
OPEN_FRAME = 3400
CLOSE_FRAME = 3600
SCRIPT = Path(__file__).resolve().parent / "input_scripts" / "nav_to_time_trial_race.txt"

FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Segmentation fault|Abort trap"
)
BOUNDARY_RE = re.compile(
    r"^\[overlay-test\] simulation (paused|resumed) at tick (\d+) rate=(\d+)$",
    re.MULTILINE,
)
PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+) "
    r"clock=(\d+) cp=(-?\d+) lap=(-?\d+)"
)
HASH_RE = re.compile(
    r"^\[SIMHASH\] tick=(\d+) objs=(\d+) h=([0-9a-f]+)$", re.MULTILINE
)
AUDIO_PAUSE_RE = re.compile(
    r"^\[overlay-audio\] paused sample=(\d+) prior=(\d+) authored=(\d+) "
    r"overlay=(\d+) authored-groups=(\d+),(\d+),(\d+),(\d+) "
    r"effective-groups=(\d+),(\d+),(\d+),(\d+)$", re.MULTILINE
)
AUDIO_RESUME_RE = re.compile(
    r"^\[overlay-audio\] resumed sample=(\d+) prior=(\d+) authored=(\d+) "
    r"overlay=(\d+) authored-groups=(\d+),(\d+),(\d+),(\d+) "
    r"effective-groups=(\d+),(\d+),(\d+),(\d+)$", re.MULTILINE
)
CAPTURE_RE = re.compile(
    r"^\[overlay-input\] game input (captured by the overlay|released to the "
    r"game) at tick (\d+)$", re.MULTILINE
)

# The handoff arm below. Opening travels a real SDL key event, exactly as the
# pad's menu button does; closing runs from the render callback, exactly as the
# on-screen Resume button does under a mouse click, Enter, or ImGui gamepad nav.
HANDOFF_TICKS = 900
HANDOFF_OPEN_FRAME = 400
HANDOFF_CLOSE_FRAME = 600


def clean_environment(**updates: str) -> dict[str, str]:
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(updates)
    return env


def fail(message: str, output: str) -> int:
    print(f"FAIL overlay pause: {message}", file=sys.stderr)
    print(output[-5000:], file=sys.stderr)
    return 1


def pcm_rms(samples: array, channels: int, start: int, end: int) -> float:
    region = samples[start * channels:end * channels]
    if not region:
        return 0.0
    return math.sqrt(sum(value * value for value in region) / len(region))


def pcm_step_peak(samples: array, channels: int, start: int, end: int) -> int:
    """Largest same-channel adjacent-sample edge in a frame interval."""
    start = max(1, start)
    end = min(end, len(samples) // channels)
    peak = 0
    for frame in range(start, end):
        for channel in range(channels):
            here = samples[frame * channels + channel]
            before = samples[(frame - 1) * channels + channel]
            peak = max(peak, abs(here - before))
    return peak


def transition_edge_limit(samples: array, channels: int, rate: int,
                          boundary: int) -> tuple[int, int]:
    """Return transition peak and a local-program-material-derived limit."""
    scan = rate // 8       # covers the next synthesis quantum/event ramp
    reference = rate // 2
    transition_peak = pcm_step_peak(
        samples, channels, boundary, boundary + scan
    )
    reference_peak = max(
        pcm_step_peak(samples, channels, boundary - reference, boundary),
        pcm_step_peak(
            samples, channels, boundary + scan, boundary + reference
        ),
    )
    # Permit ordinary transients in the race mix, but reject a new edge whose
    # magnitude is materially outside the immediately surrounding programme.
    return transition_peak, max(8192, int(reference_peak * 1.5))


def check_input_handoff(binary: Path, rom: Path, timeout: int) -> int:
    """Issue #20: the pad must come back however the menu was closed.

    Every close path that worked closed the overlay from inside its SDL event
    handler -- F1, Escape, the pad's menu button, B/Circle -- so the pump saw
    input capture change across one event and released the game's sources. The
    on-screen Resume button closes it from the RENDER callback instead, where
    there is no event to observe, and the suppression latch stayed set for the
    rest of the session: the game never saw another button.

    This drives exactly that shape. There is no synthetic pad here, so the
    proof is the handoff itself: capture is claimed on the dispatched open and
    given back on the render-callback close.
    """
    with tempfile.TemporaryDirectory(prefix="mdkr64_overlay_handoff_") as temp:
        root = Path(temp)
        prefs = root / "prefs"
        prefs.mkdir()
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(HANDOFF_TICKS),
            MDKR_APP_PREFS_DIR=str(prefs),
            MDKR_AUDIO="0",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "save"),
            MDKR_TEST_OVERLAY_ESCAPE_OPEN_FRAME=str(HANDOFF_OPEN_FRAME),
            MDKR_TEST_OVERLAY_CLOSE_FRAME=str(HANDOFF_CLOSE_FRAME),
            MDKR_VIDEO_CONFIG_PATH=str(root / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
    output = process.stdout or ""
    if process.returncode != 0:
        return fail(f"handoff run exited {process.returncode}", output)
    fatal = FATAL_RE.search(output)
    if fatal:
        return fail(f"handoff run emitted {fatal.group(0)!r}", output)
    if "[overlay-test] Escape handled; overlay=open" not in output:
        return fail("the handoff run never opened the overlay", output)
    if "[overlay-test] closed at frame" not in output:
        return fail("the handoff run never closed the overlay", output)

    transitions = [(state, int(tick)) for state, tick in CAPTURE_RE.findall(output)]
    if [state for state, _tick in transitions] != [
            "captured by the overlay", "released to the game"]:
        return fail(
            "expected exactly one capture/release pair around the overlay, "
            f"got {transitions}", output)
    captured, released = transitions[0][1], transitions[1][1]
    if released <= captured:
        return fail(
            f"capture was released at tick {released}, before or at the tick "
            f"it was claimed ({captured})", output)
    print(
        "PASS overlay input handoff: the overlay claimed game input at tick "
        f"{captured} on a dispatched open and gave it back at tick {released} "
        "on a close performed from the render callback (issue #20)"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-rel")
    parser.add_argument("--rom", default="baserom.us.v80.z64", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.expanduser().resolve()
    for path in (binary, rom, SCRIPT):
        if not path.is_file():
            parser.error(f"missing required file: {path}")

    handoff = check_input_handoff(binary, rom, args.timeout)
    if handoff != 0:
        return handoff

    with tempfile.TemporaryDirectory(prefix="mdkr64_overlay_pause_") as temp:
        root = Path(temp)
        wav = root / "overlay-pause.wav"
        prefs = root / "prefs"
        prefs.mkdir()
        # Exercise the actual F1/Escape overlay path in the smallest supported
        # window with the largest supported player scale. The run still opens
        # and closes through real SDL events while the engine is live.
        (prefs / "mdkr64_app.ini").write_text(
            "# compact overlay qualification\nui_scale=2.00\n",
            encoding="utf-8",
        )
        env = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_AUTOPLAY_TICKS=str(TICKS),
            MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SCRIPT),
            MDKR_APP_PREFS_DIR=str(prefs),
            MDKR_APP_SMOKE_WINDOW_SIZE="640x480",
            MDKR_APP_UI_TRACE="1",
            MDKR_AUDIO="0",
            MDKR_AUDIO_DUMP=str(wav),
            MDKR_AUDIO_RMS="1",
            MDKR_TEST_OVERLAY_AUDIO_PENDING_MODE="2",
            MDKR_AUTOPILOT="1",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(root / "save"),
            MDKR_STATE_HASH="3",
            MDKR_TEST_OVERLAY_ESCAPE_OPEN_FRAME=str(OPEN_FRAME),
            MDKR_TEST_OVERLAY_ESCAPE_CLOSE_FRAME=str(CLOSE_FRAME),
            MDKR_TRACE="1",
            MDKR_VIDEO_CONFIG_PATH=str(root / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        if args.verbose:
            print(f"$ {binary}", flush=True)
        process = subprocess.run(
            [str(binary)], cwd=root, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=args.timeout, check=False,
        )
        output = process.stdout or ""
        capture_data = None
        if process.returncode == 0 and wav.is_file():
            with wave.open(str(wav), "rb") as capture:
                rate = capture.getframerate()
                channels = capture.getnchannels()
                width = capture.getsampwidth()
                frames = capture.getnframes()
                pcm = array("h")
                pcm.frombytes(capture.readframes(frames))
            capture_data = (rate, channels, width, frames, pcm)

    if process.returncode != 0:
        return fail(f"process exited {process.returncode}", output)
    fatal = FATAL_RE.search(output)
    if fatal:
        return fail(f"emitted {fatal.group(0)!r}", output)
    if capture_data is None:
        return fail("audio capture was not finalized", output)
    for marker in (
        "[app] host: WebGPU",
        "[app-ui-test] window size=640x480",
        "[app-ui] ui-scale loaded=2.00",
        "[SDL] adopted the app shell's WebGPU window",
        "[overlay-test] queued Escape-open",
        "[overlay-test] Escape handled; overlay=open",
        "[overlay-test] queued Escape-close",
        "[overlay-test] Escape handled; overlay=closed",
        f"[SDL] headless: reached {TICKS} simulation ticks, exiting cleanly.",
    ):
        if marker not in output:
            return fail(f"missing {marker!r}", output)

    boundaries = BOUNDARY_RE.findall(output)
    if len(boundaries) != 2 or [row[0] for row in boundaries] != ["paused", "resumed"]:
        return fail(f"expected one pause/resume boundary pair, got {boundaries}", output)
    pause_tick, resume_tick = int(boundaries[0][1]), int(boundaries[1][1])
    if pause_tick < 3200:
        return fail(f"overlay paused before live racing (tick {pause_tick})", output)
    if resume_tick - pause_tick < 150:
        return fail(
            f"pause interval was only {resume_tick - pause_tick} ticks", output
        )
    if any(int(row[2]) <= 0 for row in boundaries):
        return fail(f"pause did not replace a live nonzero rate: {boundaries}", output)

    hashes = {int(tick): digest for tick, _objects, digest in HASH_RE.findall(output)}
    held_hashes = [hashes.get(tick) for tick in range(pause_tick, resume_tick)]
    if None in held_hashes:
        return fail("authoritative hash stream has a gap while paused", output)
    if len(set(held_hashes)) != 1:
        return fail("authoritative state changed while the overlay was open", output)
    # Resumption is only readable from ticks that were actually published: a
    # stream that stops at the overlay must fail here rather than satisfy the
    # comparison with absent digests.
    resumed_ticks = range(resume_tick, min(TICKS, resume_tick + 20))
    if not resumed_ticks or any(tick not in hashes for tick in resumed_ticks):
        return fail("authoritative hash stream has a gap after resuming", output)
    if all(hashes[tick] == held_hashes[0] for tick in resumed_ticks):
        return fail("authoritative state did not resume after closing the overlay", output)

    pace = {
        int(match.group(1)): match.groups()[1:]
        for match in PACE_RE.finditer(output)
    }
    held_pace = [pace.get(frame) for frame in range(pause_tick + 1, resume_tick + 1)]
    if None in held_pace:
        return fail("racer telemetry has a gap while paused", output)
    if len(set(held_pace)) != 1:
        return fail("kart position, clock, checkpoint, or lap changed while paused", output)
    post = next((pace[frame] for frame in range(resume_tick + 1, TICKS)
                 if frame in pace), None)
    if post is None or post == held_pace[0] or int(post[3]) <= int(held_pace[0][3]):
        return fail("kart and race clock did not advance after resume", output)

    paused_audio = AUDIO_PAUSE_RE.findall(output)
    resumed_audio = AUDIO_RESUME_RE.findall(output)
    if len(paused_audio) != 1 or len(resumed_audio) != 1:
        return fail(
            f"expected one audio pause/resume pair, got "
            f"{paused_audio}/{resumed_audio}", output
        )
    pause_values = list(map(int, paused_audio[0]))
    resume_values = list(map(int, resumed_audio[0]))
    pause_sample, prior_mode, pause_authored, pause_overlay = pause_values[:4]
    pause_authored_groups = pause_values[4:8]
    pause_effective_groups = pause_values[8:12]
    resume_sample, resume_prior, resume_authored, resume_overlay = \
        resume_values[:4]
    resume_authored_groups = resume_values[4:8]
    resume_effective_groups = resume_values[8:12]
    if (prior_mode, pause_authored, pause_overlay) != (0, 2, 1):
        return fail(
            "overlay did not retain a pending authored ambient mode: "
            f"{paused_audio[0]}", output
        )
    if pause_authored_groups != [0, 32767, 32767, 32767] or \
            pause_effective_groups != [0, 32767, 0, 0]:
        return fail(
            "pending authored groups escaped the independent overlay layer: "
            f"{paused_audio[0]}", output
        )
    if (resume_prior, resume_authored, resume_overlay) != (2, 2, 0) or \
            resume_authored_groups != [0, 32767, 32767, 32767] or \
            resume_effective_groups != resume_authored_groups:
        return fail(
            "overlay close did not reveal the latest authored audio policy: "
            f"{resumed_audio[0]}", output
        )

    rate, channels, width, frames, pcm = capture_data
    if sys.byteorder != "little":
        pcm.byteswap()
    if (rate, channels, width) != (22050, 2, 2):
        return fail(
            f"unexpected PCM format {rate}Hz/{channels}ch/{width * 8}-bit",
            output,
        )
    guard = rate // 2
    window = rate * 2
    if pause_sample < window or resume_sample + window > frames or \
            resume_sample - pause_sample <= guard * 2:
        return fail("audio pause window is too short for comparison", output)
    before_rms = pcm_rms(
        pcm, channels, pause_sample - window, pause_sample - guard
    )
    paused_rms = pcm_rms(
        pcm, channels, pause_sample + guard, resume_sample - guard
    )
    after_rms = pcm_rms(
        pcm, channels, resume_sample + guard, resume_sample + window
    )
    if paused_rms < 64.0:
        return fail("pause silenced music instead of keeping it reduced", output)
    if paused_rms >= min(before_rms, after_rms) * 0.6:
        return fail(
            f"pause mix did not suppress dominant race audio: "
            f"before={before_rms:.1f} paused={paused_rms:.1f} "
            f"after={after_rms:.1f}", output
        )
    edge_results: list[tuple[str, int, int]] = []
    for boundary, label in ((pause_sample, "pause"),
                            (resume_sample, "resume")):
        peak, limit = transition_edge_limit(
            pcm, channels, rate, boundary
        )
        edge_results.append((label, peak, limit))
        if peak > limit:
            return fail(
                f"{label} introduced a PCM edge within the following audio "
                f"quantum: peak={peak} local-limit={limit}", output
            )

    # Sensitivity control: the same window must reject a deliberately injected
    # one-sample polarity edge. This prevents a generous local threshold from
    # turning the pop-free assertion into a permanent pass.
    broken = array("h", pcm)
    injected_frame = pause_sample + rate // 100
    injected_index = injected_frame * channels
    broken[injected_index] = (
        -32768 if broken[injected_index - channels] >= 0 else 32767
    )
    broken_peak, broken_limit = transition_edge_limit(
        broken, channels, rate, pause_sample
    )
    if broken_peak <= broken_limit:
        return fail(
            "PCM edge sensitivity control did not reject an injected pop: "
            f"peak={broken_peak} local-limit={broken_limit}", output
        )

    print(
        "PASS overlay pause: production WebGPU app held complete authoritative "
        f"state and live race telemetry for {resume_tick - pause_tick} ticks; "
        f"authored audio mix reduced PCM RMS {before_rms:.0f}->{paused_rms:.0f} "
        f"and restored it to {after_rms:.0f}; transition edges "
        + ", ".join(f"{name}={peak}/{limit}" for name, peak, limit in edge_results)
        + f", injected={broken_peak}/{broken_limit} rejected; pending ambient "
        "policy remained composed; then resumed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
