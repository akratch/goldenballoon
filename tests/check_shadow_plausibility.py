#!/usr/bin/env python3
"""Every rendered world shadow must map to a caster that is really there.

Why this exists
---------------
The world-shadow feed's failure mode is not "no shadow". It is a shadow with
nothing behind it — the v0.4 playthrough's "random shadows from random objects
projecting into the world that don't seem to make sense in places". Three
separate waves have now closed an instance of it (a cascade planner that
pancaked cached casters onto the light near plane, a level's static geometry
leaking into the next level's cache, the void curtain frozen as a permanent
wall, and at R2 an inverted cascade depth axis that shadowed every surface
standing on the ground while nothing ever cast onto the ground itself). Each
was found by looking at pixels, because no gate asserted the property all four
violated.

This is that gate. Two independent halves, because the defect class has two
independent halves:

  1. CASTER PROVENANCE (`[WORLD-FX]`). Every triangle in the stage cache must
     still describe the geometry at the address it was admitted under, must lie
     inside the world envelope, and must not have been dropped by an allocation
     failure. `staleCasters` is the tenancy counter: the cache dedups by raw
     arena `Triangle` address, so an address that is freed and reused — or a
     buffer the game rewrites in place — otherwise keeps casting whatever it
     held first, for the rest of the stage, from wherever that was.

  2. SHADOW ATTRIBUTION (pixels). Shadows must land where objects are. A
     shadow-on/shadow-off frame pair is differenced, and the darkened pixels
     must (a) exist at all, (b) never brighten the image, and (c) not swallow
     the frame — an inverted or pancaked map darkens most of what is visible,
     which is exactly how the R2 defect looked before it was measured.

Both halves run on several worlds and at every shadow budget tier the engine
plans (1P gets 2048px/2 cascades, 2P 1024px/2, 3-4P 1024px/1 — the budget is
keyed on the split-screen view count, so the tiers are reached by running the
split-screen fixtures rather than by a setting).

Broken direction
----------------
`MDKR_TEST_SHADOW_BOGUS_CASTER=<world units>` displaces the first admission of
every static caster along +Y, so the depth map holds geometry that is not where
the object is — the injected bogus caster this gate must be able to see. That
arm must FAIL the provenance half. It is run last, and its failure is this
check's evidence that the passing arms mean something.

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md. Exit 0 = pass.

Usage:
    tests/check_shadow_plausibility.py --build build --rom baserom.us.v80.z64
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, fatal_re, read_ppm, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "tests" / "input_scripts"

WORLD_FX_RE = re.compile(r"\[WORLD-FX\] (.*)")
PLAN_RE = re.compile(r"\[SHADOW-PLAN\] (.*)")
FATAL_RE = fatal_re(ignore_case=True)

# Bogus-caster displacement, in world units. Large enough that no float wobble
# could produce it and that the resulting phantom is visible, small enough to
# stay far inside the engine's own implausible-vertex limit (250k) so the arm
# exercises the tenancy check rather than the coordinate clamp.
BOGUS_OFFSET = 900.0


@dataclass(frozen=True)
class Scene:
    name: str
    track: int
    script: str
    views: int
    resolution: int
    cascades: int


# One scene per shadow budget tier, and three differently authored worlds at the
# 1P tier: an open sunlit valley, an interior ice canyon, and an over-water
# flight track. A tier is a (resolution, cascades) pair the planner derives from
# the view count, so the split-screen fixtures are how the lower tiers are
# reached.
SCENES = (
    Scene("ancient-lake-1p", 5, "nav_to_time_trial_race.txt", 1, 2048, 2),
    Scene("everfrost-1p", 13, "nav_to_time_trial_race.txt", 1, 2048, 2),
    Scene("greenwood-1p", 19, "nav_to_time_trial_race.txt", 1, 2048, 2),
    Scene("ancient-lake-2p", 5, "race_2p_split.txt", 2, 1024, 2),
    Scene("ancient-lake-4p", 5, "race_4p_split.txt", 4, 1024, 1),
)


def kv(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


@dataclass
class Arm:
    label: str
    returncode: int
    output: str
    world_fx: dict[str, str]
    plan: dict[str, str]
    frame_dir: Path


def run_arm(
    binary: Path,
    rom: Path,
    scene: Scene,
    label: str,
    shadows: bool,
    root: Path,
    frames: int,
    dump_at: int,
    renderer: str,
    bogus: float,
    timeout: int,
    verbose: bool,
    traced: bool = True,
) -> Arm:
    run_dir = root / f"{scene.name}-{label}"
    frame_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frame_dir.mkdir(parents=True)
    save_dir.mkdir(parents=True)
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_WORLD_FX_TRACE="1",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_RENDERER=renderer,
        MDKR_LOAD_TRACK=str(scene.track),
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_DUMP_FROM=str(dump_at),
        MDKR_DUMP_EVERY="999999",
        MDKR_WORLD_SHADOW="full" if shadows else "off",
    )
    # MDKR_TRACE arms engine behaviour of its own, not just printing
    # (mdkr_resource_trace_enabled(); wave "shadowdeep" R1 in
    # docs/open-items/renderer.md is a static-caster-cache reset that only ever
    # ran because every shadow gate exported it). The provenance half of this
    # check reads [WORLD-FX]/[SHADOW-PLAN], which have their own variable, so
    # dropping MDKR_TRACE costs this gate nothing -- and the shipping arm in
    # main() proves the traced frames are the frames players get.
    if traced:
        env["MDKR_TRACE"] = "1"
    if bogus:
        env["MDKR_TEST_SHADOW_BOGUS_CASTER"] = repr(bogus)
    command = [
        str(binary),
        "--headless-frames", str(frames),
        "--input-script", str(SCRIPTS / scene.script),
        "--rom", str(rom),
        "--window-size", "320x240",
        "--remastered",
        "--dump-frames", str(frame_dir),
    ]
    if verbose:
        print(f"$ ({scene.name}-{label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = process.stdout or ""
    (run_dir / "log.txt").write_text(output)
    world_fx = WORLD_FX_RE.findall(output)
    plan = PLAN_RE.findall(output)
    return Arm(
        label=f"{scene.name}-{label}",
        returncode=process.returncode,
        output=output,
        world_fx=kv(world_fx[-1]) if world_fx else {},
        plan=kv(plan[-1]) if plan else {},
        frame_dir=frame_dir,
    )


def provenance_failures(arm: Arm, scene: Scene) -> list[str]:
    """Half 1: is every caster a caster that is really there?"""
    failures: list[str] = []
    if arm.returncode != 0:
        failures.append(f"{arm.label}: exited {arm.returncode}")
    fatal = FATAL_RE.search(arm.output)
    if fatal:
        failures.append(f"{arm.label}: fatal marker {fatal.group(0)!r}")
    if not arm.world_fx:
        failures.append(f"{arm.label}: no [WORLD-FX] census in output")
        return failures

    def count(field: str) -> int:
        return int(arm.world_fx.get(field, "-1"))

    if count("staleCasters") != 0:
        failures.append(
            f"{arm.label}: {count('staleCasters')} stale caster admissions "
            f"(worst displacement {arm.world_fx.get('staleWorst')} world "
            "units) — the stage cache is casting geometry that is no longer "
            "at the address it was keyed on")
    if count("implausible") != 0:
        failures.append(
            f"{arm.label}: {count('implausible')} casters outside the world "
            "envelope")
    if count("allocFails") != 0:
        failures.append(
            f"{arm.label}: {count('allocFails')} capture allocation failures — "
            "the committed caster set is incomplete, so a passing plausibility "
            "result would be measuring a truncated scene")
    if count("committed") <= 0:
        failures.append(f"{arm.label}: committed no caster frames")
    if count("static") <= 0 and count("dynamic") <= 0:
        failures.append(f"{arm.label}: terminal frame carried no casters")

    if not arm.plan:
        failures.append(f"{arm.label}: no [SHADOW-PLAN] line")
        return failures
    if arm.plan.get("valid") != "1":
        failures.append(f"{arm.label}: terminal cascade plan invalid")
    else:
        views = int(arm.plan.get("views", "-1"))
        resolution = int(arm.plan.get("res", "-1"))
        maps = int(arm.plan.get("maps", "-1"))
        if views != scene.views:
            failures.append(
                f"{arm.label}: planned {views} views, fixture drives "
                f"{scene.views}")
        elif (resolution, maps) != (scene.resolution,
                                    scene.views * scene.cascades):
            failures.append(
                f"{arm.label}: tier mismatch res={resolution} maps={maps}, "
                f"expected res={scene.resolution} "
                f"maps={scene.views * scene.cascades}")
    if int(arm.plan.get("invalidWorldRecv", "0")) != 0:
        failures.append(
            f"{arm.label}: "
            f"{arm.plan.get('invalidWorldRecv')} receivers sampled without a "
            "valid world position")
    return failures


def attribution(on: Arm, off: Arm) -> tuple[list[str], str]:
    """Half 2: do the shadows land on the scene rather than over it?"""
    failures: list[str] = []
    on_frames = sorted(on.frame_dir.glob("frame_*.ppm"))
    off_frames = sorted(off.frame_dir.glob("frame_*.ppm"))
    if not on_frames or not off_frames:
        return ([f"{on.label}: no frame captures to compare"], "")
    width, height, lit = read_ppm(off_frames[-1])
    on_width, on_height, shaded = read_ppm(on_frames[-1])
    if (width, height) != (on_width, on_height):
        return ([f"{on.label}: frame dimensions differ between arms"], "")

    pixels = width * height
    darker = brighter = 0
    for index in range(0, len(lit), 3):
        before = lit[index:index + 3]
        after = shaded[index:index + 3]
        if before == after:
            continue
        if any(right > left for left, right in zip(before, after)):
            brighter += 1
        if any(right < left for left, right in zip(before, after)):
            darker += 1

    # A shadow pass that darkens nothing is not being exercised, and one that
    # darkens most of the visible frame is not attributable to the objects in
    # it: that is what an inverted or pancaked depth axis produces, and it is
    # the shape this half exists to reject. The upper bound is deliberately
    # generous (a third of the frame) so a legitimately overcast interior still
    # passes; the pre-R2 inverted axis measured well past it.
    if darker < pixels // 400:
        failures.append(
            f"{on.label}: shadows darkened only {darker}/{pixels} pixels — "
            "the receiver is not being exercised on this route")
    if darker > pixels // 3:
        failures.append(
            f"{on.label}: shadows darkened {darker}/{pixels} pixels; a "
            "footprint this large cannot be attributed to the casters in "
            "frame")
    # Enabling shadows may only remove light. Anything brighter is the decal
    # handoff releasing a blob, which is bounded and one-sided in the other
    # gate; here it must not become a second, opposite effect.
    if brighter > darker // 4:
        failures.append(
            f"{on.label}: shadows brightened {brighter}/{pixels} pixels "
            f"against {darker} darkened")
    return (failures, f"{darker}/{pixels} darkened, {brighter} brightened")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", default="gl", choices=("gl", "webgpu"))
    parser.add_argument("--frames", type=int, default=3500)
    parser.add_argument("--dump-at", type=int, default=3499)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument(
        "--scenes", default="",
        help="comma-separated scene subset (diagnostic; release runs use all)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    wanted = {name for name in args.scenes.split(",") if name}
    scenes = [s for s in SCENES if not wanted or s.name in wanted]
    if not scenes:
        print(f"FAIL: no scene matched {args.scenes!r}", file=sys.stderr)
        return 1

    failures: list[str] = []
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-shadow-plausibility-") as tmp:
        root = Path(tmp)
        for scene in scenes:
            try:
                off = run_arm(binary, rom, scene, "off", False, root,
                              args.frames, args.dump_at, args.renderer, 0.0,
                              args.timeout, args.verbose)
                on = run_arm(binary, rom, scene, "on", True, root,
                             args.frames, args.dump_at, args.renderer, 0.0,
                             args.timeout, args.verbose)
            except subprocess.TimeoutExpired:
                failures.append(f"{scene.name}: timed out after "
                                f"{args.timeout}s")
                continue
            # The caster census and cascade plan are produced by the capture
            # path, not by the shadow draw, so both arms owe the same
            # provenance. A tenancy defect that only shows with the effect
            # disabled is still a caster that is not where its object is.
            scene_failures = provenance_failures(off, scene)
            scene_failures += provenance_failures(on, scene)
            attribution_failures, note = attribution(on, off)
            scene_failures += attribution_failures
            failures += scene_failures
            notes.append(
                f"{scene.name} (res={on.plan.get('res')} "
                f"maps={on.plan.get('maps')}): "
                f"static={on.world_fx.get('static')} "
                f"dynamic={on.world_fx.get('dynamic')} "
                f"stale={on.world_fx.get('staleCasters')} "
                f"implausible={on.world_fx.get('implausible')}"
                + (f"; {note}" if note else ""))

        # Shipping configuration: the first scene's production arm again with
        # MDKR_TRACE absent, which is what the shipped launchers and the web
        # shell actually run. It carries the full provenance half (that reads
        # MDKR_WORLD_FX_TRACE, a different variable) and adds the assertion the
        # "shadowdeep" R1 leak needed: the frame a player sees must be the frame
        # this gate measured.
        ship_scene = scenes[0]
        try:
            shipping = run_arm(binary, rom, ship_scene, "on-shipping", True,
                               root, args.frames, args.dump_at, args.renderer,
                               0.0, args.timeout, args.verbose, traced=False)
        except subprocess.TimeoutExpired:
            failures.append(f"shipping arm timed out after {args.timeout}s")
        else:
            failures += provenance_failures(shipping, ship_scene)
            if "[PACE]" in shipping.output:
                failures.append(
                    "shipping arm emitted [PACE] rows -- MDKR_TRACE reached it, "
                    "so it is not in shipping configuration")
            traced_frames = sorted(
                (root / f"{ship_scene.name}-on" / "frames").glob("*.ppm"))
            ship_frames = sorted(shipping.frame_dir.glob("*.ppm"))
            if len(traced_frames) != len(ship_frames) or not ship_frames:
                failures.append(
                    f"shipping arm dumped {len(ship_frames)} frames against "
                    f"{len(traced_frames)} traced")
            else:
                differing = [
                    a.name for a, b in zip(traced_frames, ship_frames)
                    if a.read_bytes() != b.read_bytes()]
                if differing:
                    failures.append(
                        f"MDKR_TRACE changes the rendered image on "
                        f"{ship_scene.name}: {len(differing)} of "
                        f"{len(traced_frames)} frames differ from the same "
                        f"route in shipping configuration ({differing[0]}). "
                        f"Every pixel and caster verdict above is then about a "
                        f"program nobody ships -- find what the trace variable "
                        f"arms besides printing")
                else:
                    notes.append(
                        f"shipping configuration ({ship_scene.name}, "
                        f"MDKR_TRACE unset): {len(ship_frames)} frame(s) "
                        f"byte-identical to the traced arm, "
                        f"static={shipping.world_fx.get('static')} "
                        f"stale={shipping.world_fx.get('staleCasters')}")

        # Broken direction, last: with a bogus caster injected the provenance
        # half must report the phantom. A gate that cannot fail here is not
        # measuring anything.
        control_scene = scenes[0]
        try:
            control = run_arm(binary, rom, control_scene, "bogus", True, root,
                              args.frames, args.dump_at, args.renderer,
                              BOGUS_OFFSET, args.timeout, args.verbose)
        except subprocess.TimeoutExpired:
            failures.append(f"positive control timed out after {args.timeout}s")
        else:
            control_failures = provenance_failures(control, control_scene)
            stale = int(control.world_fx.get("staleCasters", "0")) if \
                control.world_fx else 0
            if not control_failures or stale <= 0:
                failures.append(
                    "positive control failed: with "
                    f"MDKR_TEST_SHADOW_BOGUS_CASTER={BOGUS_OFFSET} the caster "
                    f"feed reported staleCasters={stale} and raised "
                    f"{len(control_failures)} findings — this check cannot "
                    "detect the defect it exists for")
            else:
                notes.append(
                    f"positive control: bogus caster injected at "
                    f"+{BOGUS_OFFSET} units reported staleCasters={stale} "
                    f"(worst {control.world_fx.get('staleWorst')}) and failed "
                    f"with {len(control_failures)} findings, as required")

    if failures:
        print("check_shadow_plausibility: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        for note in notes:
            print(f"  . {note}")
        return 1
    print("check_shadow_plausibility: PASS")
    for note in notes:
        print(f"  {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
