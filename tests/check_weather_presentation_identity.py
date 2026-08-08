#!/usr/bin/env python3
"""Precipitation must carry a presentation identity, not be replayed verbatim.

THE DEFECT CLASS. A presentation owner is only minted where
``presentation_snapshot_identity_generation(transform)`` succeeds, i.e. for a
real spawned Object in ``gObjPtrList``. Weather is not spawned: snow flakes come
out of ``gSnowVertexData``, the rain sheet out of ``gRainVertices``, and the
splashes out of the module-static ``gRainSplashSegments`` array. None of them
was ever registered, so an interpolated present re-walked their tick-T bytes
unchanged -- the precipitation froze for two to four presents and then jumped
while everything around it glided. Precipitation has the largest per-tick screen
displacement in the scene, so that reads as strobing rather than as falling.

Nothing failed when that was true. There was no gate on this class at all: the
authoritative hashes do not move (nothing about registration is authoritative),
every pixel control passes (a frozen flake is a coherent image), and the
aggregate packet counters were already large from particles and model
deformation. This gate is that missing witness, and it is structural on purpose
-- the counters name the content, where a screenshot difference would only say
that *some* pixel moved.

Three assertions, on two routes:

* **REGISTERED.** ``renderervertexreg`` is nonzero on both routes. This is the
  non-vacuity witness: it says the content was given an identity at all, which
  is the entire defect. Zero here is the pre-fix tree.
* **SUBSTITUTED.** ``renderervertexoverride`` is nonzero. Registration alone
  proves nothing -- a batch can be registered, resolve a pair, and have the two
  endpoints be identical, which substitutes nothing and moves no pixel. An
  override is counted only when {T} and {T+1} actually differ, so this is the
  assertion that the interpolated present shows a DIFFERENT flake position from
  the authored one.
* **GUARDED, AND THE GUARD IS NOT VACUOUS.** ``renderervertexjumphold`` is
  nonzero on the snow route. ``snow_vertices()`` places a flake at
  ``((physics - camera) & radius_mask)``, so a flake leaving the volume
  reappears on the opposite face in the same physics slot: same identity, same
  batch shape, and a jump of the full volume width that a blend would draw as a
  streak across the screen. ``GfxPresentationMatrixOwner::max_vertex_delta``
  refuses those pairs. A zero here would mean either that the wrap never happens
  (it does -- it is how the effect is authored) or that the threshold is so wide
  it can never fire, and both make the guard a comment rather than a check.

  The rain sheet is deliberately NOT asserted to hold: it is an ortho quad
  rotated about a fixed origin and has nothing to wrap.

Determinism is asserted alongside, on the same runs: registering an identity is
a render-side act and must not perturb the authoritative stream. The state hash
of each smoothing arm must equal the same route's hash with smoothing off.

Always muted + headless. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, parse_last, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
HASH_VERSION = "3"

# 120 Hz against the 30 Hz authoritative tick: four presents per tick, three of
# them reconstructions. This is the same alpha grid every other presentation
# gate in the tree measures on.
PRESENT_RATE = "120"

# Level 13 is the snow route and level 37 the rain route -- 37 is also
# check_weather_rng_order.py's route, for the same reason: it is the normal
# level that actually exercises rain.
#
# The tick counts are the point at which each route is under power with weather
# on screen. Weather does not draw in the menus, so a short run measures a
# scene with no precipitation in it and reports zero for reasons that have
# nothing to do with the code under test -- which is why the REGISTERED
# assertion is worth making.
SNOW_TRACK = "13"
SNOW_TICKS = 3600
RAIN_TRACK = "37"
RAIN_TICKS = 4200

STATE_RE = re.compile(r"\[SIMHASH\]")


def run(binary: str, rom: str, root: Path, label: str, track: str, ticks: int,
        smoothing: bool, timeout: int, verbose: bool) -> tuple[dict, list[str]]:
    """One headless arm. Returns its last [PRESENT-PACKET] row and hash stream."""
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MDKR")}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK=track,
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_PRESENT_SCHED_TRACE="1",
    )
    if smoothing:
        env["MDKR_PRESENT_RATE"] = PRESENT_RATE
        env["MDKR_PRESENT_SMOOTHING"] = "interpolate"
    command = [binary, "--headless-ticks", str(ticks),
               "--input-script", str(SCRIPT), "--rom", rom,
               "--window-size", "320x240"]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(command, cwd=run_dir, env=env, text=True,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT,
                             timeout=timeout, check=False)
    output = process.stdout
    if process.returncode != 0:
        raise RuntimeError(f"{label}: exit code {process.returncode}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in output:
            line = next((l for l in output.splitlines() if marker in l), marker)
            raise RuntimeError(f"{label}: {line.strip()}")
    packet = parse_last(output, "PRESENT-PACKET")
    if not packet:
        raise RuntimeError(f"{label}: no [PRESENT-PACKET] row in the output")
    hashes = [line for line in output.splitlines() if STATE_RE.search(line)]
    if not hashes:
        raise RuntimeError(f"{label}: no [SIMHASH] rows in the output")
    return packet, hashes


def field(packet: dict, name: str, label: str) -> int:
    if name not in packet:
        raise RuntimeError(
            f"{label}: [PRESENT-PACKET] carries no {name} field — the counter "
            f"this gate reads was renamed or removed")
    return int(packet[name])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=str(DEFAULT_BUILD_DIR))
    parser.add_argument("--rom", required=True)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    binary = str(Path(resolve_binary(args.build)).resolve())
    errors: list[str] = []
    notes: list[str] = []

    with tempfile.TemporaryDirectory(prefix="mdkr-weather-identity-") as tmp:
        root = Path(tmp)
        try:
            rom = str(Path(args.rom).resolve())
            snow, snow_hashes = run(binary, rom, root, "snow-smooth",
                                    SNOW_TRACK, SNOW_TICKS, True,
                                    args.timeout, args.verbose)
            snow_off, snow_off_hashes = run(binary, rom, root, "snow-off",
                                            SNOW_TRACK, SNOW_TICKS, False,
                                            args.timeout, args.verbose)
            rain, rain_hashes = run(binary, rom, root, "rain-smooth",
                                    RAIN_TRACK, RAIN_TICKS, True,
                                    args.timeout, args.verbose)
            rain_off, rain_off_hashes = run(binary, rom, root, "rain-off",
                                            RAIN_TRACK, RAIN_TICKS, False,
                                            args.timeout, args.verbose)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            print(f"check_weather_presentation_identity: FAIL\n  - {error}")
            return 1

        for label, packet in (("snow", snow), ("rain", rain)):
            registrations = field(packet, "renderervertexreg", label)
            hits = field(packet, "renderervertexhit", label)
            overrides = field(packet, "renderervertexoverride", label)
            holds = field(packet, "renderervertexjumphold", label)
            if registrations == 0:
                errors.append(
                    f"{label}: renderervertexreg=0 — the weather vertex "
                    f"batches carry no presentation identity, so an "
                    f"interpolated present replays their tick-T bytes verbatim")
            if overrides == 0:
                errors.append(
                    f"{label}: renderervertexoverride=0 — the batches are "
                    f"registered but no interpolated present ever substituted "
                    f"a moved vertex, so the registration changes nothing "
                    f"({registrations} registrations, {hits} pair hits)")
            notes.append(
                f"{label}: {registrations} registered batches, {hits} resolved "
                f"pairs, {overrides} moved substitutions, {holds} guard holds")

        snow_holds = field(snow, "renderervertexjumphold", "snow")
        if snow_holds == 0:
            errors.append(
                "snow: renderervertexjumphold=0 — the volume-wrap guard never "
                "fired over a whole snow route. Either the wrap stopped "
                "happening or max_vertex_delta is too wide to ever refuse a "
                "pair; a guard that cannot fire is not a guard")

        for label, smooth, plain in (("snow", snow_hashes, snow_off_hashes),
                                     ("rain", rain_hashes, rain_off_hashes)):
            if smooth != plain:
                first = next((index for index, (a, b)
                              in enumerate(zip(smooth, plain)) if a != b),
                             min(len(smooth), len(plain)))
                errors.append(
                    f"{label}: the smoothing arm's authoritative state stream "
                    f"diverges from the same route with smoothing off at row "
                    f"{first} — registering a presentation identity perturbed "
                    f"the simulation")
            else:
                notes.append(
                    f"{label}: {len(smooth)} authoritative rows byte-identical "
                    f"with and without smoothing")

    if errors:
        print("check_weather_presentation_identity: FAIL")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("check_weather_presentation_identity: PASS")
    for note in notes:
        print(f"  - {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
