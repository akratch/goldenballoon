#!/usr/bin/env python3
"""Internal replay-mechanism coverage for per-viewport cameras.

The snapshot unit test proves the immutable authored-camera latch and its
fail-closed controls. This gate proves game wiring that a synthetic unit cannot:
a real 2P race captures camera 1, a real 3P race captures the fourth TT spectator
camera, and a real WebGPU cinematic captures cutscene bank 4. Late, tightly
bounded PPM windows exercise the production immutable replay mechanism.

The 3P arm is especially important: gNumCameras is three even when the TT
spectator is drawn into the fourth quadrant.  A sequential ``0..gNumCameras``
snapshot walk therefore silently omits camera 3 while the rest of the game
looks healthy.

CUT COVERAGE. Capturing a camera is half the property; the other half is that
no drawn frame ever spans a camera CUT. Every arm therefore runs with the
per-tick camera journal on and this gate classifies the cuts itself, out of the
raw poses, then demands that the snapshot refused to pair across each one. The
classification deliberately does not read the module's own discontinuity flag:
a gate that asked the interpolator whether it thought this was a cut could only
ever agree with it. That independence is what found the spectate-camera cuts
(post-race and the 3P T.T. spectator), which change shot by a few hundred to a
few thousand units inside the same camera slot and so cleared none of the bars
the capture side can see for itself.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT_2P = ROOT / "tests/input_scripts/race_2p_split.txt"
SCRIPT_3P_TT = ROOT / "tests/input_scripts/race_3p_tt_camera.txt"
SCRIPT_CUTSCENE = ROOT / "tests/input_scripts/adventure_hub_drive.txt"
SCRIPT_ADVENTURE = ROOT / "tests/input_scripts/adventure_race_loop.txt"
SCRIPT_PREVIEW = ROOT / "tests/input_scripts/adventure_preview_reentry.txt"
PRESENTS = 6400
TICKS = PRESENTS // 2
DUMP_FROM = 6320
CUTSCENE_PRESENTS = 9200
CUTSCENE_DUMP_FROM = 9120
# The hub -> Dino Domain lobby -> Ancient Lake -> finish route from
# check_adventure_race_loop.py, which is the only fixture that drives real door
# transitions and a natural race finish. Budget matches that check; the
# post-race camera's first cut lands near present 14200 here.
ADVENTURE_PRESENTS = 17000
ADVENTURE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
    ":-3381,1946:-2519,1516:-1858,1099"
    ";12:E5:E0"
)
ADVENTURE_LEVELS = (0, 12, 5)   # hub, world lobby, race — each entered by door
# Issue #44 (a): between its spectate hops, the post-race finish camera DWELLS
# on one trackside point and rotates to track the racer — smooth authored
# motion that must BLEND. The Task 9 standing exclusion used to hold every
# such dwell tick discontinuous (measured on this exact route: 0 of 1,358
# dwell ticks between the first and last hop blended, camexcluded=1389),
# stepping the camera at the authored rate for the whole post-race sequence
# while OBJECT_ROOT kept blending: the racer flicker of issue #44. The hops
# themselves must STAY refused — that is classify_cuts' blended-cut check,
# unchanged — so this floor is only about the dwell ticks between them.
# Measured post-fix: 1,356 of 1,358 blended. The floor sits far below that
# on purpose: the dwell count is budget-shaped (the loss route spends its
# remaining frames in the post-race sequence), so a physics or timing change
# that legitimately shortens the window must not fail the gate — while the
# defect value is exactly zero.
POSTRACE_DWELL_BLEND_FLOOR = 80
POSTRACE_MIN_HOPS = 2           # measured 17; below 2 the window is vacuous
ADVENTURE_LOAD_FRAME_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=-?\d+ entrance=-?\d+ "
    r"vehicle=-?\d+ cutscene=(-?\d+) @frame~(\d+)")
# Issue #44 (b): the same route WON instead of lost, with the level-12 step
# list ending in a SECOND E5 — after the win the kart drives back into the
# now-cleared Ancient Lake door, and menu_adventure_track_init opens
# ADVENTURESETUP_VEHICLE with the track loaded live behind the menu
# (load_level_for_menu(mapId, -1, 1)): the flyby preview. Nothing presses a
# button after the door (see the input script), so the preview stays open
# from its load (measured @frame~16024) to the end of the budget — a
# ~1,450-tick measurement window.
PREVIEW_PRESENTS = 19000
PREVIEW_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
    ":-3381,1946:-2519,1516:-1858,1099"
    ";12:E5:E5"
)
# The preview's own load line: the ZERO_PLAYERS sentinel (-1) with the menu
# loader's cutscene=1, which no other levelId=5 load on this route carries.
PREVIEW_LOAD_RE = re.compile(
    r"level_load: levelId=5 numPlayers=-1 .*cutscene=1 @frame~(\d+)")
PREVIEW_LOAD_MAX_FRAME = 17400  # measured 16024; must leave a real window
# Ticks to skip after the preview load before the window opens: the load
# itself is a stage reset plus a fade, and the first captured tick is a
# legitimate viewport entry.
PREVIEW_SETTLE_TICKS = 30

SNAPSHOT_RE = re.compile(r"\[SNAPSHOT\] (.+)")
CAMERA_CUT_RE = re.compile(r"\[CAMERA-CUT\] (.+)")
LEVEL_LOAD_RE = re.compile(r"level_load: levelId=(\d+)")
SCHED_RE = re.compile(r"\[PRESENTSCHED-SUMMARY\] (.+)")
REPLAY_RE = re.compile(r"\[REPLAY-SUMMARY\] (.+)")
SMOOTH_RE = re.compile(
    r"^\[SMOOTH-VERDICT\] class=(\S+) (.+)$", re.MULTILINE)
CAMERA_VP_RE = re.compile(r"\[CAMERA-VP\] (.+)")
LOAD_RE = re.compile(r"level_load: levelId=5 numPlayers=(-?\d+)")
CUTSCENE_LOAD_RE = re.compile(
    r"level_load: levelId=36 numPlayers=-?\d+.*cutscene=15")
HUD_RE = re.compile(r"hud_init: hudPlayers=(\d+) numViewports=(\d+)")


#
# What counts as a camera JUMP rather than camera motion, in DKR world units
# of per-tick displacement.
#
# Calibration, measured over 16,305 adjacent same-slot capture pairs across
# every arm below: legitimate camera motion tops out at 63.6 units per tick
# (p99 = 33), and the smallest real cut in the same corpus is 669.8. The band
# between is empty. 400 sits ~6x above the fastest honest motion and ~40%
# below the cheapest cut, so it separates the two populations without pinning
# either edge.
#
# It is deliberately far under the capture side's own
# PRESENTATION_SNAPSHOT_TELEPORT_UNITS (2000). That gap is the point: cuts
# between 63 and 2000 units are exactly the ones the pose cannot self-report,
# so a gate that reused 2000 would only re-measure what capture already knows.
CUT_MOVE_UNITS = 400.0


@dataclass(frozen=True)
class Arm:
    name: str
    players_encoded: int
    script: Path
    camera_id: int
    crop: tuple[float, float, float, float]
    min_captures: int
    min_interpolations: int
    # Cut class -> witnesses this route must produce. A route that stops
    # driving its class would otherwise pass by having nothing to check.
    min_cuts: dict[str, int]
    # Game-side camera-cut NOTES this route must get consumed. Distinct from
    # min_cuts, which classifies cuts from the poses: a note is the class of
    # cut the poses cannot show. Only routes that actually drive a camera-mode
    # change or a spectate handoff can carry a floor here.
    min_cut_notes: int


ARMS = (
    # DKR's two-player layout is horizontal: camera 1 owns the lower half.
    # Two viewport entries (the 1P frontend camera and camera 1 arriving with
    # the split) and the frontend's cutscene-bank switches.
    # Measured: this route raises no camera-cut note at all — its camera-mode
    # never changes and it has no spectator — so the unconsumed-note zero below
    # is structural here and the non-vacuity floor lives on the arms that do.
    Arm("2p-camera1", 1, SCRIPT_2P, 1, (0.03, 0.97, 0.53, 0.97),
        500, 1000,
        {"viewport-entry": 4, "camera-jump": 2}, 0),   # measured 6 and 3
    # The production CRIGHT edge switches the lower-right quadrant from the
    # minimap to camera 3's TT spectator. Camera 3 both ARRIVES mid-race (a
    # viewport entry) and re-aims by snapping between spectate points.
    Arm("3p-tt-camera3", 2, SCRIPT_3P_TT, 3,
        (0.53, 0.97, 0.53, 0.97), 250, 500,
        {"viewport-entry": 6, "camera-jump": 5},   # measured 8 and 7
        1),
)


def parse_fields(output: str, pattern: re.Pattern[str], label: str) -> dict[str, int]:
    match = None
    for match in pattern.finditer(output):
        pass
    if match is None:
        raise RuntimeError(f"no [{label}] row was emitted")
    fields: dict[str, int] = {}
    for token in match.group(1).split():
        key, separator, value = token.partition("=")
        if not separator:
            continue
        try:
            fields[key] = int(value)
        except ValueError:
            continue
    return fields


def parse_smooth_verdicts(output: str) -> dict[str, dict[str, int]]:
    rows: dict[str, dict[str, int]] = {}
    for match in SMOOTH_RE.finditer(output):
        fields: dict[str, int] = {}
        for token in match.group(2).split():
            key, separator, value = token.partition("=")
            if not separator:
                continue
            try:
                fields[key] = int(value)
            except ValueError:
                continue
        rows[match.group(1)] = fields
    return rows


def parse_camera_journal(output: str) -> list[dict[str, float]]:
    """Every published camera entry, as the raw recipe the game authored."""
    rows: list[dict[str, float]] = []
    for match in CAMERA_CUT_RE.finditer(output):
        fields: dict[str, float] = {}
        for token in match.group(1).split():
            key, separator, value = token.partition("=")
            if not separator:
                continue
            try:
                fields[key] = float(value)
            except ValueError:
                continue
        rows.append(fields)
    return rows


def classify_cuts(
    rows: list[dict[str, float]],
) -> tuple[list[str], dict[str, int]]:
    """Find the cuts in a journal and report any the snapshot would blend.

    Four shapes, each a discontinuity in what the viewport is looking at
    rather than in how fast it is moving:

    * viewport-entry — this viewport has no immediately preceding captured
      tick. Level start placement, a stage reset, a viewport arriving with a
      new split, and any capture gap all land here; there is no earlier pose
      that belongs to the same shot.
    * camera-bank-switch — the viewport changed which gCameras[] entry it
      draws (the cutscene bank going on or off).
    * draw-region-change — it crossed between the presentation region and the
      authored safe aperture.
    * camera-jump — it moved further in one authoritative tick than any
      camera legitimately travels (see CUT_MOVE_UNITS).

    Rotation is deliberately not a class of its own: a look-at that swings
    hard as its subject passes the lens is honest motion, and the arc past
    which a blend would draw orientations the tick never held is the
    interpolator's documented rotation snap, not a pairing decision.
    """
    by_viewport: dict[int, list[dict[str, float]]] = {}
    for row in rows:
        by_viewport.setdefault(int(row.get("vp", -1)), []).append(row)
    blended: list[str] = []
    counts: dict[str, int] = {}
    for viewport, sequence in sorted(by_viewport.items()):
        sequence.sort(key=lambda entry: entry["tick"])
        previous: dict[str, float] | None = None
        for row in sequence:
            kinds: list[str] = []
            if previous is None or row["tick"] != previous["tick"] + 1.0:
                kinds.append("viewport-entry")
            else:
                if row["cam"] != previous["cam"]:
                    kinds.append("camera-bank-switch")
                if row["region"] != previous["region"]:
                    kinds.append("draw-region-change")
                moved = math.dist(
                    (row["x"], row["y"], row["z"]),
                    (previous["x"], previous["y"], previous["z"]))
                if moved > CUT_MOVE_UNITS:
                    kinds.append("camera-jump")
            for kind in kinds:
                counts[kind] = counts.get(kind, 0) + 1
                if row.get("blend", 0.0) != 0.0:
                    blended.append(
                        f"viewport {viewport} tick {int(row['tick'])}: "
                        f"{kind} (camera {int(row['cam'])}) was paired for "
                        "blending")
            previous = row
    return blended, counts


def check_cuts(name: str, output: str,
               minimums: dict[str, int]) -> tuple[list[str], str]:
    rows = parse_camera_journal(output)
    if not rows:
        return [f"{name}: no [CAMERA-CUT] journal was emitted"], ""
    blended, counts = classify_cuts(rows)
    failures = [f"{name}: {entry}" for entry in blended]
    for kind, wanted in sorted(minimums.items()):
        if counts.get(kind, 0) < wanted:
            failures.append(
                f"{name}: the route drove only {counts.get(kind, 0)} {kind} "
                f"cuts, want >= {wanted}")
    summary = ", ".join(f"{kind} {count}"
                        for kind, count in sorted(counts.items())) or "none"
    return failures, f"{len(rows)} captured camera entries; cuts: {summary}"


def crop_bytes(frame: tuple[int, int, bytes],
               crop: tuple[float, float, float, float]) -> bytes:
    width, height, pixels = frame
    x0 = max(0, min(width, int(width * crop[0])))
    x1 = max(x0 + 1, min(width, int(width * crop[1])))
    y0 = max(0, min(height, int(height * crop[2])))
    y1 = max(y0 + 1, min(height, int(height * crop[3])))
    rows = []
    for y in range(y0, y1):
        start = (y * width + x0) * 3
        rows.append(pixels[start:start + (x1 - x0) * 3])
    return b"".join(rows)


def intermediate_verdict(
    frames: dict[int, tuple[int, int, bytes]],
    crop: tuple[float, float, float, float],
) -> tuple[int, int]:
    """Return (candidates, intermediates distinct from both neighbours)."""
    candidates = moved = 0
    for index in sorted(frames):
        if index % 2 != 1 or index - 1 not in frames or index + 1 not in frames:
            continue
        before = crop_bytes(frames[index - 1], crop)
        current = crop_bytes(frames[index], crop)
        after = crop_bytes(frames[index + 1], crop)
        candidates += 1
        if current != before and current != after:
            moved += 1
    return candidates, moved


def held_intermediates(
    frames: dict[int, tuple[int, int, bytes]],
) -> dict[int, tuple[int, int, bytes]]:
    """Captured frames with every intermediate replaced by a real neighbour.

    The correct verdict on this capture is zero motion, and the same detector
    has to produce it from the pixels rather than from a flag. The source
    neighbour alternates so both halves of the distinctness test stay
    load-bearing: a detector that drops either comparison reports motion here.
    """
    held = dict(frames)
    intermediates = [index for index in sorted(frames) if index % 2 == 1]
    for position, index in enumerate(intermediates):
        source = index - 1 if position % 2 == 0 else index + 1
        if source in frames:
            held[index] = frames[source]
    return held


def viewport_quality(frame: tuple[int, int, bytes],
                     crop: tuple[float, float, float, float]) -> tuple[int, float]:
    pixels = crop_bytes(frame, crop)
    colours = {pixels[index:index + 3] for index in range(0, len(pixels), 3)}
    luma = [
        0.2126 * pixels[index] + 0.7152 * pixels[index + 1] +
        0.0722 * pixels[index + 2]
        for index in range(0, len(pixels), 3)
    ]
    return len(colours), statistics.pstdev(luma)


def run_arm(binary: Path, rom: Path, root: Path, arm: Arm,
            timeout: int, verbose: bool) -> tuple[str, Path]:
    run_dir = root / arm.name
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="original",
        MDKR_SYNTH_FIELDS="2",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_RENDERER="gl",
        MDKR_PRESENT_RATE="60",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_PRESENT_SNAPSHOT="1",
        MDKR_TEST_CAMERA_VP_ENDPOINTS="1",
        MDKR_INTERNAL_TEST_TOKEN="mdkr64-presentation-replay-v1",
        MDKR_TEST_CAMERA_CUT_TRACE="1",
        MDKR_DUMP_FROM=str(DUMP_FROM),
        MDKR_SAVE_DIR=str(save_dir),
        # Isolate the video config with the save (see check_door_blocks.py).
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    command = [
        str(binary), "--headless-frames", str(PRESENTS),
        "--window-size", "320x240", "--input-script", str(arm.script),
        "--dump-frames", str(frame_dir), "--rom", str(rom),
    ]
    if verbose:
        print(f"$ ({arm.name}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{arm.name}: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(f"{arm.name}: fatal marker {marker}")
    return output, frame_dir


def check_arm(output: str, frame_dir: Path, arm: Arm) -> tuple[list[str], str]:
    failures: list[str] = []
    try:
        snapshot = parse_fields(output, SNAPSHOT_RE, "SNAPSHOT")
        sched = parse_fields(output, SCHED_RE, "PRESENTSCHED-SUMMARY")
        camera_vp = parse_fields(output, CAMERA_VP_RE, "CAMERA-VP")
    except RuntimeError as error:
        return [f"{arm.name}: {error}"], ""

    if (str(arm.players_encoded) not in LOAD_RE.findall(output) or
            (str(arm.players_encoded), str(arm.players_encoded + 1))
            not in HUD_RE.findall(output)):
        failures.append(
            f"{arm.name}: level 5 never loaded with the expected "
            f"{arm.players_encoded + 1}-player HUD")

    for key, expected in (("ticks", TICKS), ("presents", PRESENTS),
                          ("tickfields", 2), ("presentrate", 60)):
        if sched.get(key) != expected:
            failures.append(
                f"{arm.name}: scheduler {key}={sched.get(key)}, expected {expected}")
    if sched.get("interp", 0) < TICKS - 32:
        failures.append(
            f"{arm.name}: only {sched.get('interp', 0)} interpolated presents")
    if sched.get("interpviews", 0) <= 0:
        failures.append(f"{arm.name}: no interpolated view-projection was consumed")
    if snapshot.get("overflows") != 0:
        failures.append(
            f"{arm.name}: snapshot overflows={snapshot.get('overflows')}")

    # Camera-cut notes must be filed and consumed in ONE id space.
    #
    # The note sites (racer.c's camera-mode change and spectate handoff,
    # tracks.c's 3P TT spectate switch) all name a player index, which is the
    # viewport; the capture records the gCameras[] slot, which is that index
    # plus four whenever the viewport's cutscene camera is active. Keyed on the
    # slot, notes raised on those ticks missed and were then destroyed at
    # commit, and the camera blended across a hard cut.
    #
    # No pose-based classifier below can witness that: a cut invisible from the
    # pose is invisible to a classifier built from poses, which is the entire
    # reason these notes exist. The counters are the witness. Both arms drive a
    # cutscene-bank switch AND a spectate/mode change, so `camcutconsumed` is
    # the non-vacuity proof that notes were raised at all, and
    # `camcutunconsumed` is nonzero exactly when the two id spaces disagree.
    if snapshot.get("camcutunconsumed", -1) != 0:
        failures.append(
            f"{arm.name}: {snapshot.get('camcutunconsumed')} camera-cut notes "
            "survived the publish of the viewport they name — the note and "
            "the capture are keyed in different id spaces, and those cuts "
            "were blended across")
    if snapshot.get("camcutconsumed", 0) < arm.min_cut_notes:
        failures.append(
            f"{arm.name}: only {snapshot.get('camcutconsumed')} camera-cut "
            f"notes were consumed (raised={snapshot.get('camcutnote')}), want "
            f">= {arm.min_cut_notes}; without one the zero above is vacuous")
    if snapshot.get("identityinsertfail", -1) != 0:
        failures.append(
            f"{arm.name}: {snapshot.get('identityinsertfail')} spawns could "
            "not be registered in the identity table")
    if (camera_vp.get("alpha0checks", 0) <= 0 or
            camera_vp.get("alpha0mismatch", -1) != 0 or
            camera_vp.get("alpha0expected", 0) == 0 or
            camera_vp.get("alpha0expected") != camera_vp.get("alpha0actual")):
        failures.append(
            f"{arm.name}: alpha-zero VP is not byte-exact to the retained "
            "task's authored camera")
    if (camera_vp.get("nextchecks", 0) <= 0 or
            camera_vp.get("nextmismatch", -1) != 0 or
            camera_vp.get("nextexpected", 0) == 0 or
            camera_vp.get("nextexpected") != camera_vp.get("nextactual")):
        failures.append(
            f"{arm.name}: the prior alpha-one target did not become the next "
            "task's exact authored VP")
    if (camera_vp.get("midpointchecks", 0) <= 0 or
            camera_vp.get("midpointmoved", 0) <= 0 or
            camera_vp.get("midpointhash", 0) == 0):
        failures.append(
            f"{arm.name}: no semantic midpoint VP movement was observed")
    if camera_vp.get("mutationreject") != camera_vp.get("alpha0checks"):
        failures.append(
            f"{arm.name}: exact VP mutation control rejected "
            f"{camera_vp.get('mutationreject')}/"
            f"{camera_vp.get('alpha0checks')} one-bit mutations")

    camera_key = f"cam{arm.camera_id}"
    interp_key = f"caminterp{arm.camera_id}"
    captures = snapshot.get(camera_key, 0)
    interpolations = snapshot.get(interp_key, 0)
    if (snapshot.get("camera_mask", 0) & (1 << arm.camera_id)) == 0:
        failures.append(
            f"{arm.name}: camera_mask={snapshot.get('camera_mask', 0)} omits "
            f"camera {arm.camera_id}")
    if captures < arm.min_captures:
        failures.append(
            f"{arm.name}: camera {arm.camera_id} captured {captures} ticks, "
            f"want >= {arm.min_captures}")
    if interpolations < arm.min_interpolations:
        failures.append(
            f"{arm.name}: camera {arm.camera_id} resolved only "
            f"{interpolations} interpolated poses, want >= "
            f"{arm.min_interpolations}")

    frames: dict[int, tuple[int, int, bytes]] = {}
    try:
        for path in sorted(frame_dir.glob("frame_*.ppm")):
            frames[int(path.stem.split("_")[1])] = read_ppm(path)
    except (OSError, RuntimeError, ValueError) as error:
        failures.append(f"{arm.name}: could not read PPM witness: {error}")
        return failures, ""

    candidates, moved = intermediate_verdict(frames, arm.crop)
    control_candidates, control_moved = intermediate_verdict(
        held_intermediates(frames), arm.crop)
    if candidates < 30:
        failures.append(
            f"{arm.name}: only {candidates} bounded intermediate-frame pairs")
    if moved < 5:
        failures.append(
            f"{arm.name}: target viewport moved on only {moved}/{candidates} "
            "intermediate frames")
    if control_candidates != candidates or control_moved != 0:
        failures.append(
            f"{arm.name}: frozen-intermediate detector control was not "
            f"rejected ({control_moved}/{control_candidates} accepted)")

    colours = 0
    sigma = 0.0
    if frames:
        colours, sigma = viewport_quality(frames[max(frames)], arm.crop)
        if colours < 200 or not math.isfinite(sigma) or sigma < 8.0:
            failures.append(
                f"{arm.name}: target viewport looks blank: colours={colours}, "
                f"luma sigma={sigma:.1f}")

    cut_failures, cut_note = check_cuts(arm.name, output, arm.min_cuts)
    failures.extend(cut_failures)

    note = (
        f"{arm.name}: camera {arm.camera_id} captured {captures} ticks and "
        f"resolved {interpolations} interpolated poses; target crop moved on "
        f"{moved}/{candidates} intermediate presents "
        f"({colours} colours, luma sigma {sigma:.1f}); held control "
        f"{control_moved}/{control_candidates}; "
        f"{camera_vp.get('alpha0checks', 0)} exact "
        f"alpha-zero and {camera_vp.get('nextchecks', 0)} chained alpha-one "
        f"endpoint checks; cut notes "
        f"{snapshot.get('camcutconsumed')}/{snapshot.get('camcutnote')} "
        f"consumed, {snapshot.get('camcutunconsumed')} unconsumed; "
        f"{cut_note}")
    return failures, note


def run_cutscene_arm(binary: Path, rom: Path, root: Path, timeout: int,
                     verbose: bool) -> tuple[str, Path]:
    run_dir = root / "cutscene-camera4"
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="original",
        MDKR_SYNTH_FIELDS="2",
        MDKR_TRACE="1",
        MDKR_RENDERER="webgpu",
        MDKR_PRESENT_RATE="60",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_PRESENT_SNAPSHOT="1",
        MDKR_TEST_CAMERA_VP_ENDPOINTS="1",
        MDKR_INTERNAL_TEST_TOKEN="mdkr64-presentation-replay-v1",
        MDKR_TEST_CAMERA_CUT_TRACE="1",
        MDKR_DUMP_FROM=str(CUTSCENE_DUMP_FROM),
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    command = [
        str(binary), "--headless-frames", str(CUTSCENE_PRESENTS),
        "--window-size", "320x240", "--input-script", str(SCRIPT_CUTSCENE),
        "--dump-frames", str(frame_dir), "--rom", str(rom),
    ]
    if verbose:
        print(f"$ (cutscene-camera4) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"cutscene-camera4: exit {process.returncode}\n{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(f"cutscene-camera4: fatal marker {marker}")
    return output, frame_dir


def check_cutscene_arm(output: str, frame_dir: Path) -> tuple[list[str], str]:
    failures: list[str] = []
    try:
        snapshot = parse_fields(output, SNAPSHOT_RE, "SNAPSHOT")
        sched = parse_fields(output, SCHED_RE, "PRESENTSCHED-SUMMARY")
        camera_vp = parse_fields(output, CAMERA_VP_RE, "CAMERA-VP")
    except RuntimeError as error:
        return [f"cutscene-camera4: {error}"], ""
    if CUTSCENE_LOAD_RE.search(output) is None:
        failures.append("cutscene-camera4: adventure intro cutscene never loaded")
    if sched.get("presents") != CUTSCENE_PRESENTS:
        failures.append(
            f"cutscene-camera4: presents={sched.get('presents')}, expected "
            f"{CUTSCENE_PRESENTS}")
    captures = snapshot.get("cam4", 0)
    interpolations = snapshot.get("caminterp4", 0)
    if (snapshot.get("camera_mask", 0) & (1 << 4)) == 0:
        failures.append(
            f"cutscene-camera4: camera_mask={snapshot.get('camera_mask')} "
            "omits the authored cutscene bank")
    # Bank 4 is precisely where the note/capture id spaces used to diverge:
    # the note names viewport 0, the capture records slot 0+4. Nothing may be
    # left pending after this arm publishes that viewport.
    if snapshot.get("camcutunconsumed", -1) != 0:
        failures.append(
            f"cutscene-camera4: {snapshot.get('camcutunconsumed')} camera-cut "
            "notes survived the publish of the viewport they name while the "
            "cutscene bank was active")
    if captures < 500 or interpolations < 500:
        failures.append(
            "cutscene-camera4: insufficient authored bank-4 activity "
            f"(captures={captures}, interpolations={interpolations})")
    if (camera_vp.get("alpha0checks", 0) <= 0 or
            camera_vp.get("alpha0mismatch", -1) != 0 or
            camera_vp.get("alpha0expected", 0) == 0 or
            camera_vp.get("alpha0expected") != camera_vp.get("alpha0actual")):
        failures.append(
            "cutscene-camera4: bank-4 alpha-zero VP is not byte-exact")
    if (camera_vp.get("nextchecks", 0) <= 0 or
            camera_vp.get("nextmismatch", -1) != 0 or
            camera_vp.get("nextexpected", 0) == 0 or
            camera_vp.get("nextexpected") != camera_vp.get("nextactual")):
        failures.append(
            "cutscene-camera4: bank-4 alpha-one endpoint chain is not exact")
    if (camera_vp.get("midpointmoved", 0) <= 0 or
            camera_vp.get("mutationreject") !=
            camera_vp.get("alpha0checks")):
        failures.append(
            "cutscene-camera4: midpoint or one-bit semantic control is absent")
    frames: dict[int, tuple[int, int, bytes]] = {}
    try:
        for path in sorted(frame_dir.glob("frame_*.ppm")):
            frames[int(path.stem.split("_")[1])] = read_ppm(path)
    except (OSError, RuntimeError, ValueError) as error:
        failures.append(f"cutscene-camera4: could not read PPM witness: {error}")
    candidates, moved = intermediate_verdict(frames, (0.0, 1.0, 0.0, 1.0))
    if candidates < 30 or moved < 5:
        failures.append(
            f"cutscene-camera4: bank-4 midpoint witness moved on "
            f"{moved}/{candidates} frames")
    # Bank 4 arrives and leaves across the cinematic's own stage boundaries, so
    # its switches present as viewport entries; measured 7 entries and 3 jumps.
    cut_failures, cut_note = check_cuts(
        "cutscene-camera4", output,
        {"viewport-entry": 5, "camera-jump": 2})
    failures.extend(cut_failures)
    return failures, (
        f"cutscene-camera4: camera 4 captured {captures} ticks and resolved "
        f"{interpolations} interpolated poses; full frame moved on "
        f"{moved}/{candidates} midpoint presents; "
        f"{camera_vp.get('alpha0checks', 0)} exact alpha-zero and "
        f"{camera_vp.get('nextchecks', 0)} chained alpha-one endpoint checks; "
        f"{cut_note}")


def run_adventure_arm(binary: Path, rom: Path, root: Path, timeout: int,
                      verbose: bool) -> str:
    """Drive the hub -> lobby -> race -> finish loop.

    The frontend routes above never leave a level through a door and never
    finish a race, so two whole cut classes live only here: the door
    transitions the drive hook steers into, and the post-race camera, which
    hands the shot to trackside spectate cameras and then keeps re-handing it
    as the racer passes them. No PPM witness — the pixel evidence for this
    route belongs to check_adventure_race_loop.py; what is new here is the
    journal.
    """
    run_dir = root / "adventure-doors-postrace"
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="original",
        MDKR_SYNTH_FIELDS="2",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_DRIVE_ROUTE=ADVENTURE_ROUTE,
        # The instrumented racer naturally wins here; the loss control keeps
        # the production finishing-order path in play for the finish camera.
        MDKR_ADVENTURE_LOSS="1",
        MDKR_RENDERER="gl",
        MDKR_PRESENT_RATE="60",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_PRESENT_SNAPSHOT="1",
        MDKR_SMOOTH_VERDICT="1",
        MDKR_INTERNAL_TEST_TOKEN="mdkr64-presentation-replay-v1",
        MDKR_TEST_CAMERA_CUT_TRACE="1",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    command = [
        str(binary), "--headless-frames", str(ADVENTURE_PRESENTS),
        "--window-size", "320x240", "--input-script", str(SCRIPT_ADVENTURE),
        "--rom", str(rom),
    ]
    if verbose:
        print(f"$ (adventure-doors-postrace) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"adventure-doors-postrace: exit {process.returncode}\n"
            f"{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(
                f"adventure-doors-postrace: fatal marker {marker}")
    return output


def postrace_dwell_blend(output: str) -> tuple[int, int, int] | None:
    """(hops, dwell candidates, dwell ticks blended) in the post-race window.

    The window opens at the levelId=5 race load and runs to the end of the
    trace: this arm's loss route spends its whole remaining budget in the
    race level's post-race sequence (the return to the lobby is the race-loop
    check's business, not this gate's). A hop is a consecutive same-slot,
    same-camera, same-region capture pair that moved further than
    CUT_MOVE_UNITS — during the race itself that never happens (legitimate
    camera motion tops out at 63.6 units/tick; see CUT_MOVE_UNITS'
    calibration), so every hop counted here is the finish camera changing
    spectate point. The converse does not hold: a spectate hop that also
    crosses a camera bank or draw region is skipped by the same-cam/region
    requirement, which can only SHRINK the first-to-last-hop window —
    conservative in the safe direction. Dwell candidates are the
    consecutive, same-camera, same-region, sub-threshold rows between the
    first and last counted hop, i.e. exactly the rows no cut class claims.
    """
    race_frame = None
    for match in ADVENTURE_LOAD_FRAME_RE.finditer(output):
        if int(match.group(1)) == 5:
            race_frame = int(match.group(3))
            break
    if race_frame is None:
        return None
    rows = [row for row in parse_camera_journal(output)
            if int(row.get("vp", -1)) == 0 and
            race_frame / 2 < row["tick"]]
    rows.sort(key=lambda row: row["tick"])
    hops: list[float] = []
    dwell: list[dict[str, float]] = []
    previous: dict[str, float] | None = None
    for row in rows:
        if (previous is not None and row["tick"] == previous["tick"] + 1.0 and
                row["cam"] == previous["cam"] and
                row["region"] == previous["region"]):
            moved = math.dist(
                (row["x"], row["y"], row["z"]),
                (previous["x"], previous["y"], previous["z"]))
            if moved > CUT_MOVE_UNITS:
                hops.append(row["tick"])
            else:
                dwell.append(row)
        previous = row
    if len(hops) < 2:
        return len(hops), 0, 0
    window = [row for row in dwell if hops[0] <= row["tick"] <= hops[-1]]
    blended = sum(1 for row in window if row.get("blend", 0.0) != 0.0)
    return len(hops), len(window), blended


def check_adventure_arm(output: str) -> tuple[list[str], str]:
    name = "adventure-doors-postrace"
    failures: list[str] = []
    loaded = {int(level) for level in LEVEL_LOAD_RE.findall(output)}
    missing = [level for level in ADVENTURE_LEVELS if level not in loaded]
    if missing:
        failures.append(
            f"{name}: the route never drove through the doors into levels "
            f"{missing}")
    try:
        snapshot = parse_fields(output, SNAPSHOT_RE, "SNAPSHOT")
        replay = parse_fields(output, REPLAY_RE, "REPLAY-SUMMARY")
    except RuntimeError as error:
        return failures + [f"{name}: {error}"], ""
    if snapshot.get("overflows") != 0:
        failures.append(f"{name}: snapshot overflows={snapshot.get('overflows')}")
    #
    # The camera-cut note witness lives here because this is the only fixture
    # that drives BOTH halves of the id space in one run: a cutscene-bank
    # switch (so a viewport records slot p+4) and the post-race camera changing
    # spectate point and mode (so racer.c raises notes naming viewport p).
    # Keyed on the slot, those notes missed by four bits and were destroyed at
    # the same commit; the camera then blended across the cut, which no
    # pose-based classifier below can see, because a cut the pose does not
    # carry is exactly what a note is for.
    #
    if snapshot.get("camcutunconsumed", -1) != 0:
        failures.append(
            f"{name}: {snapshot.get('camcutunconsumed')} camera-cut notes "
            "survived the publish of the viewport they name — the note sites "
            "and the capture are keyed in different id spaces")
    if snapshot.get("camcutconsumed", 0) <= 0:
        failures.append(
            f"{name}: no camera-cut note was consumed "
            f"(raised={snapshot.get('camcutnote')}); the zero above is vacuous "
            "and this route is the one that drives post-race spectate cuts")
    if snapshot.get("identityinsertfail", -1) != 0:
        failures.append(
            f"{name}: {snapshot.get('identityinsertfail')} spawns could not "
            "be registered in the identity table")

    # This one long route crosses frontend, hub, lobby, race, finish camera and
    # three level lifetimes. It therefore owns the breadth assertion that the
    # shorter race-focused stage gates cannot make: an interpolation refusal
    # must come from a classified discontinuity/correspondence rule, never from
    # reading an external pointer the retained task failed to capture.
    for field in ("uncapturedext", "uncapturedrefusals"):
        if replay.get(field) != 0:
            failures.append(
                f"{name}: replay {field}={replay.get(field)}, expected zero "
                "across the full frontend/adventure/race/finish route")

    smooth = parse_smooth_verdicts(output)
    # Calibration and qualification on this exact route measured 228/49/34-36.
    # These ceilings leave meaningful route/content margin while still failing
    # if a class quietly falls back to holding most of its in-between frames.
    held_ceilings = {
        "WORLD_SCROLL": 350,
        "WATER_WAVE": 100,
        "OBJECT_ROOT": 100,
    }
    for surface_class, ceiling in held_ceilings.items():
        row = smooth.get(surface_class)
        if row is None:
            failures.append(
                f"{name}: no SMOOTH-VERDICT row for {surface_class}")
            continue
        blend = row.get("blend", 0)
        snap = row.get("snap", 0)
        reported = row.get("heldpermille", -1)
        expected = snap * 1000 // (blend + snap) if blend + snap else -1
        if blend <= 0:
            failures.append(
                f"{name}: {surface_class} blended no frame; its held ratio "
                "is therefore not a smoothing witness")
        if reported != expected:
            failures.append(
                f"{name}: {surface_class} heldpermille={reported}, but raw "
                f"blend={blend}/snap={snap} recomputes to {expected}")
        if reported > ceiling:
            failures.append(
                f"{name}: {surface_class} heldpermille={reported} exceeds "
                f"the route ceiling {ceiling}")
    # Measured on this route: 9 viewport entries (three of them the door
    # transitions), one in-place cutscene-bank switch, and 20 jumps, all but
    # three of which are the post-race camera changing spectate point. The
    # floors keep margin under each.
    cut_failures, cut_note = check_cuts(
        name, output,
        {"viewport-entry": 6, "camera-bank-switch": 1, "camera-jump": 12})
    failures.extend(cut_failures)
    # Issue #44 (a): the other half of the post-race property. The hops must
    # refuse to pair (asserted just above, via the classified-cut check),
    # AND the dwell ticks between them must blend — a camera that holds its
    # authored pose on every dwell tick steps at 30 Hz while the racer it is
    # re-centering interpolates past it, which is the reported flicker.
    dwell = postrace_dwell_blend(output)
    if dwell is None:
        failures.append(
            f"{name}: could not bound the post-race window (no levelId=5 "
            "race load in the trace)")
        dwell_note = "post-race window unbounded"
    else:
        hops, candidates, blended = dwell
        dwell_note = (f"post-race hops {hops}, dwell blended "
                      f"{blended}/{candidates}")
        if hops < POSTRACE_MIN_HOPS:
            failures.append(
                f"{name}: only {hops} post-race spectate hops were observed "
                f"(want >= {POSTRACE_MIN_HOPS}); the dwell floor below is "
                "vacuous without a bounded hop window")
        elif blended < POSTRACE_DWELL_BLEND_FLOOR:
            failures.append(
                f"{name}: only {blended}/{candidates} post-race dwell ticks "
                f"blended (want >= {POSTRACE_DWELL_BLEND_FLOOR}) — the "
                "finish camera is stepping at the authored rate between "
                "spectate hops while the racer keeps interpolating "
                "(issue #44 a)")
    return failures, (
        f"{name}: levels {sorted(loaded)} entered; cut notes "
        f"{snapshot.get('camcutconsumed')}/{snapshot.get('camcutnote')} "
        f"consumed, {snapshot.get('camcutunconsumed')} unconsumed; replay "
        f"externals/refusals {replay.get('uncapturedext')}/"
        f"{replay.get('uncapturedrefusals')}; held permille "
        + ", ".join(
            f"{surface}={smooth.get(surface, {}).get('heldpermille')}"
            for surface in held_ceilings)
        + f"; {dwell_note}; {cut_note}")


def run_preview_arm(binary: Path, rom: Path, root: Path, timeout: int,
                    verbose: bool) -> str:
    """Win the race, then drive back into the cleared door (issue #44 b).

    The only fixture that reaches a CLEARED-track preview: a live level scene
    rendered under the menu shell while menu_camera_centre borrows the active
    camera for its overlay pass every tick. Before the borrow scope existed,
    that second latch conflicted the tick's authored-camera set — zero
    cameras published for the whole preview (camconflict=1781 on this exact
    route, one per menu tick with a live scene; the [CAMERA-CUT] journal
    emitted NOTHING after the preview load), so the flyby stepped at the
    authored rate. No PPM witness: the journal plus the census are the
    assertion, exactly as in the adventure arm.
    """
    run_dir = root / "adventure-preview-reentry"
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="original",
        MDKR_SYNTH_FIELDS="2",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_DRIVE_ROUTE=PREVIEW_ROUTE,
        # A WIN is load-bearing here: only a win sets RACE_CLEARED, and only
        # a cleared track re-opens as the live vehicle-select preview.
        MDKR_ADVENTURE_WIN="1",
        # Deterministic post-race selection, same as
        # check_adventure_race_loop.py (a first-place finish normally skips
        # the menu entirely; this pins the path if that ever changes).
        MDKR_TEST_POSTRACE_OPTION="1",
        MDKR_RENDERER="gl",
        MDKR_PRESENT_RATE="60",
        MDKR_PRESENT_SMOOTHING="interpolate",
        MDKR_PRESENT_SCHED_TRACE="1",
        MDKR_PRESENT_SNAPSHOT="1",
        MDKR_INTERNAL_TEST_TOKEN="mdkr64-presentation-replay-v1",
        MDKR_TEST_CAMERA_CUT_TRACE="1",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    command = [
        str(binary), "--headless-frames", str(PREVIEW_PRESENTS),
        "--window-size", "320x240", "--input-script", str(SCRIPT_PREVIEW),
        "--rom", str(rom),
    ]
    if verbose:
        print(f"$ (adventure-preview-reentry) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"adventure-preview-reentry: exit {process.returncode}\n"
            f"{output[-3000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:"):
        if marker in output:
            raise RuntimeError(
                f"adventure-preview-reentry: fatal marker {marker}")
    return output


def check_preview_arm(output: str) -> tuple[list[str], str]:
    name = "adventure-preview-reentry"
    failures: list[str] = []
    load = None
    for match in PREVIEW_LOAD_RE.finditer(output):
        load = int(match.group(1))
    if load is None:
        return [f"{name}: the cleared-track preview never loaded (no "
                "levelId=5 numPlayers=-1 cutscene=1 load) — the route did "
                "not re-enter the door after the win"], ""
    if load > PREVIEW_LOAD_MAX_FRAME:
        failures.append(
            f"{name}: preview loaded at frame {load} (measured 16024, limit "
            f"{PREVIEW_LOAD_MAX_FRAME}) — not enough window left to measure")
    try:
        snapshot = parse_fields(output, SNAPSHOT_RE, "SNAPSHOT")
    except RuntimeError as error:
        return failures + [f"{name}: {error}"], ""
    if snapshot.get("overflows") != 0:
        failures.append(
            f"{name}: snapshot overflows={snapshot.get('overflows')}")
    # THE defect signal. One conflict per preview tick was completely silent
    # before this counter existed; zero is what "the scene is the sole
    # author" means. Absent counts as failure, not as zero.
    if snapshot.get("camconflict", -1) != 0:
        failures.append(
            f"{name}: camconflict={snapshot.get('camconflict')} — an "
            "authored-camera recipe was refused as CONFLICTING, so every "
            "such tick published zero cameras and rendered fail-closed at "
            "the authored rate")
    # Non-vacuity for the fix itself: the borrow scope must actually have
    # armed on this route (menu_camera_centre runs every preview tick).
    if snapshot.get("camborrowskips", 0) <= 0:
        failures.append(
            f"{name}: camborrowskips={snapshot.get('camborrowskips')} — the "
            "menu overlay's borrowed-lens latch never declared itself, so "
            "either the bracket is gone or the route missed the preview")
    # The flyby must actually be captured and blended INSIDE the preview
    # window, not somewhere earlier on the route (the opening cinematic also
    # drives bank 4). Floors sit well under the measured 1457/1457 so a
    # timing drift up to PREVIEW_LOAD_MAX_FRAME still passes.
    window_start = load // 2 + PREVIEW_SETTLE_TICKS
    rows = [row for row in parse_camera_journal(output)
            if row.get("tick", 0) > window_start]
    flyby = [row for row in rows if row.get("cam") == 4]
    blended = sum(1 for row in flyby if row.get("blend", 0.0) != 0.0)
    if len(flyby) < 600:
        failures.append(
            f"{name}: only {len(flyby)} bank-4 camera entries were published "
            f"in the preview window after tick {window_start} (measured "
            "1457, want >= 600) — zero here is exactly the conflict defect")
    if blended < 500:
        failures.append(
            f"{name}: only {blended}/{len(flyby)} preview-window camera "
            "entries were pairable for blending (measured 1457, want >= "
            "500) — the flyby is captured but still refuses to interpolate")
    return failures, (
        f"{name}: preview loaded @frame~{load}; window rows {len(flyby)} "
        f"(blend {blended}); camconflict={snapshot.get('camconflict')} "
        f"camborrowskips={snapshot.get('camborrowskips')}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, SCRIPT_2P, SCRIPT_3P_TT, SCRIPT_CUTSCENE,
                 SCRIPT_ADVENTURE, SCRIPT_PREVIEW):
        if not path.is_file():
            print(f"check_camera_snapshot_coverage: FAIL\n  - missing {path}")
            return 1

    failures: list[str] = []
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-camera-snapshot-") as tmp:
        root = Path(tmp)
        for arm in ARMS:
            try:
                output, frame_dir = run_arm(
                    binary, rom, root, arm, args.timeout, args.verbose)
            except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
                failures.append(str(error))
                continue
            arm_failures, note = check_arm(output, frame_dir, arm)
            failures.extend(arm_failures)
            if note:
                notes.append(note)

        try:
            output, frame_dir = run_cutscene_arm(
                binary, rom, root, args.timeout, args.verbose)
            arm_failures, note = check_cutscene_arm(output, frame_dir)
            failures.extend(arm_failures)
            if note:
                notes.append(note)
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
            failures.append(str(error))

        try:
            output = run_adventure_arm(
                binary, rom, root, args.timeout, args.verbose)
            arm_failures, note = check_adventure_arm(output)
            failures.extend(arm_failures)
            if note:
                notes.append(note)
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
            failures.append(str(error))

        try:
            output = run_preview_arm(
                binary, rom, root, args.timeout, args.verbose)
            arm_failures, note = check_preview_arm(output)
            failures.extend(arm_failures)
            if note:
                notes.append(note)
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
            failures.append(str(error))

    if failures:
        print("check_camera_snapshot_coverage: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        for note in notes:
            print(f"  - observed: {note}")
        return 1

    print("check_camera_snapshot_coverage: PASS")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
