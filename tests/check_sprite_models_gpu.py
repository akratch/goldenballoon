#!/usr/bin/env python3
"""Draw every replacement-model family on WebGPU and survive its GPU failures.

Usage: python3 tests/check_sprite_models_gpu.py --binary build/mdkr64 --rom ROM
The ROM stays local; every pack, save and preference is synthetic and temporary.

One race on a track that carries every replaced sprite a race can show (palms
with their canopies, beach/blueberry/rubber trees, five weapon-balloon colours
and the banana; the two collectible balloons appear only in Adventure, so the
sprite_models CTest covers them loading and sharing their mesh) runs in these
arms, each in its own save, video and preference directory:

* models: every one of those sprites draws; both canopy sizes attach; the
  banana's spin moves through its phases; fading models never write depth;
  and the five balloon colours and both canopy sizes share their GPU geometry.
  (Retiring renderer references before model storage is freed is the
  sprite_models CTest's: at engine exit the device teardown retires the GPU
  cache first whatever the pack shutdown does, so no log here can show it.)
* high-rate: presented at 120 Hz with interpolation over the same simulation,
  so retained model recipes replay at interpolated poses (the branch runs; its
  pose is checked by the sprite_model_replay CTest).
* translucent-atlas: the same art with every texel at 60% alpha. A cutout
  texel only decides coverage, so the frames must equal the models arm's.
* disabled: MDKR_SPRITE_MODELS=0 draws the original sprites. Its frames must
  differ from the models arm's, or the frame comparisons prove nothing.
* faults: every modern-mesh allocation failing once, and failing every time.
  The optional model path switches itself off and the original sprites draw
  (frames equal the disabled arm's); the renderer never reinitializes and the
  game never exits.

Every arm must complete the same 3400-tick simulation with a byte-identical
state-hash stream.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from check_sprite_models import family_pack
from harness_utils import (find_fatal, resolve_binary, ABORT_MARKERS, DEVICE_MARKERS,
                           VALIDATION_MARKERS)

FAULTS = (
    "modern.module", "modern.bind-group-layout", "modern.pipeline-layout",
    "modern.pipeline", "modern.vertex-buffer", "modern.index-buffer",
    "modern.texture", "modern.view", "modern.uniform", "modern.sampler",
    "modern.bind-group",
)
FRAMES = 3400
# Late frames, when every raced sprite has been on screen many times over.
DUMP_FROM, DUMP_EVERY = 3100, 100
TRACK = "8"
# Every replacement slot a race shows; 154/155 (collectible balloons) are
# Adventure-only and covered by the sprite_models CTest.
CANOPIES = (114, 116)
BALLOONS = (147, 148, 149, 150, 151)
BANANA = 156
FAMILY_SPRITES = (107, 108, 113, 114, 115, 116, *BALLOONS, BANANA)


class GateFailure(Exception):
    pass


def require(condition, message, output=""):
    if not condition:
        raise GateFailure(message + (f"\n{output[-4000:]}" if output else ""))


def run_arm(binary, rom, route, root, name, *, rate="30", fault=None, disabled=False,
            alpha=255, smoothing=None):
    case = root / name
    (case / "save").mkdir(parents=True)
    (case / "prefs").mkdir()
    (case / "frames").mkdir()
    # Non-packaged builds read mods/ beside the working directory.
    family_pack(case / "mods/base", 100, (255, 0, 0), alpha=alpha)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MDKR", "GE007_"))}
    env.update(MDKR_RENDERER="webgpu", MDKR_AUDIO="0", MDKR64_HIDDEN="1",
               MDKR_APP_PREFS_DIR=str(case / "prefs"), MDKR_SAVE_DIR=str(case / "save"),
               MDKR_VIDEO_CONFIG_PATH=str(case / "video.ini"),
               MDKR_TEST_SCRIPT_ONLY_INPUT="1", MDKR_PRESENT_RATE=rate,
               MDKR_LOAD_TRACK=TRACK, MDKR_AUTOPILOT="1",
               MDKR_STATE_HASH="3", MDKR_STATE_HASH_FILE=str(case / "hashes.txt"),
               MDKR_SPRITE_MODELS="0" if disabled else "1",
               MDKR_DUMP_FROM=str(DUMP_FROM), MDKR_DUMP_EVERY=str(DUMP_EVERY))
    if fault:
        env["MDKR_WEBGPU_FAULT"] = fault
    if smoothing:
        env["MDKR_PRESENT_SMOOTHING"] = smoothing
    process = subprocess.run(
        [str(binary), "--rom", str(rom), "--headless-ticks", str(FRAMES),
         "--window-size", "320x240", "--input-script", str(route),
         "--dump-frames", str(case / "frames")],
        cwd=case, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=300)
    output = process.stdout
    require(process.returncode == 0, f"{name}: exit {process.returncode} (the optional model "
            "path must never end the game)", output)
    marker = find_fatal(output, *VALIDATION_MARKERS, *DEVICE_MARKERS, *ABORT_MARKERS,
                        "switched to OpenGL",
                        r"\[webgpu\] device error", "attempting one native device reinitialization")
    require(marker is None, f"{name}: runtime emitted {marker!r}", output)
    hashes = (case / "hashes.txt").read_text().splitlines()
    require(len(hashes) == FRAMES, f"{name}: incomplete simulation stream ({len(hashes)} rows)")
    require(all(re.fullmatch(r"\[SIMHASH\] tick=\d+ objs=-?\d+ h=[0-9a-f]{16}", row) for row in hashes),
            f"{name}: malformed state-hash rows")
    ticks = [int(re.search(r"tick=(\d+)", row)[1]) for row in hashes]
    require(ticks == list(range(ticks[0], ticks[0] + FRAMES)), f"{name}: missing or duplicate ticks")
    require(len({row.rsplit("h=", 1)[1] for row in hashes}) > 1, f"{name}: constant state hash")
    frames = {path.name: path.read_bytes() for path in sorted((case / "frames").glob("frame_*.ppm"))}
    require(len(frames) >= 3, f"{name}: expected late frame dumps, found {len(frames)}")
    return output, hashes, frames


def model_stats(output):
    draws = {int(s): (int(n), int(f)) for s, n, f in
             re.findall(r"^\[MODELS\] sprite=(\d+) near=(\d+) far=(\d+)$", output, re.M)}
    poses = {int(s): dict(authored=int(a), replayed=int(r), attached=int(t), phases=int(p))
             for s, a, r, t, p in re.findall(
                 r"^\[MODELS\] poses sprite=(\d+) authored=(\d+) replayed=(\d+) attached=(\d+) "
                 r"phases=(\d+)$", output, re.M)}
    material = re.findall(r"^\[MODELS\] material fading=(\d+) fadingDepthWrites=(\d+) envTinted=(\d+)$",
                          output, re.M)
    gpu = re.findall(r"^\[WGPU-MODELS\] textureUploads=(\d+) textureReuses=(\d+) textureUploadBytes=(\d+) "
                     r"geometryBytes=(\d+) geometryReuses=(\d+) entries=(\d+)$", output, re.M)
    return draws, poses, material, gpu


def check_models_arm(output):
    draws, poses, material, gpu = model_stats(output)
    for sprite in FAMILY_SPRITES:
        require(sum(draws.get(sprite, (0, 0))) > 0, f"models: sprite {sprite} never drew a model", output)
        require(poses.get(sprite, {}).get("authored", 0) > 0, f"models: sprite {sprite} has no posed draws")
    for sprite in CANOPIES:
        pose = poses[sprite]
        require(pose["attached"] == pose["authored"],
                f"models: canopy {sprite} attached {pose['attached']} of {pose['authored']} poses")
    require(poses[BANANA]["phases"] >= 8,
            f"models: the banana's spin used {poses[BANANA]['phases']} phases; it must turn")
    require(all(p["replayed"] == 0 for p in poses.values()),
            "models: 30 Hz presentation of the 30 Hz simulation replayed a recipe")
    require(len(material) == 1, f"models: expected one material census line, got {material}", output)
    fading, fading_depth, _ = map(int, material[0])
    require(fading > 0, "models: no fading model drew, so the depth-write check is vacuous")
    require(fading_depth == 0, f"models: {fading_depth} of {fading} fading model draws wrote depth "
            "(the billboard they replace does not)")
    require(len(gpu) == 1, f"models: expected one GPU cache lifetime, got {gpu}", output)
    uploads, texture_reuses, texture_bytes, geometry_bytes, geometry_reuses, entries = map(int, gpu[0])
    require(texture_bytes == uploads * 4, f"models: each 1x1 atlas uploads once ({gpu[0]})")
    require(texture_reuses > 0 and geometry_reuses > 0,
            f"models: shared atlases and shared meshes must reuse GPU resources ({gpu[0]})")
    require(geometry_bytes == (entries - geometry_reuses) * (3 * 36 + 3 * 4),
            f"models: one geometry upload per distinct mesh ({gpu[0]})")
    return draws


def check_high_rate_arm(output):
    # How many interpolated frames headless synthetic pacing presents depends
    # on host timing (about 5,900 replayed model poses in this arm as measured,
    # but a few dozen in the same run without frame dumps), so this asks only
    # that the replay branch ran in the shipped binary. The replayed pose
    # itself is sprite_model_replay's to check.
    _, poses, _, _ = model_stats(output)
    replayed = sum(pose["replayed"] for pose in poses.values())
    require(replayed > 0, "high-rate: no model recipe replayed at an interpolated pose", output)
    return replayed


def check_fault_arm(name, point, output, disabled_frames, frames):
    require(f"[webgpu-fault] injected {point}@1" in output, f"{name}: the fault never fired", output)
    require(output.count("[webgpu] modern meshes unavailable") == 1,
            f"{name}: the model path must switch itself off exactly once", output)
    draws, _, _, _ = model_stats(output)
    require(not any(sum(pair) for pair in draws.values()), f"{name}: models drew after the failure",
            output)
    require(frames == disabled_frames, f"{name}: late frames differ from the original sprites")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--smoke-only", action="store_true")
    args = parser.parse_args()
    if args.binary is not None and args.build is not None:
        parser.error("specify --binary or --build, not both")
    binary = Path(resolve_binary(args.binary or args.build or "build")).resolve()
    rom = args.rom.resolve()
    route = Path(__file__).resolve().parent / "input_scripts/nav_to_time_trial_race.txt"
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-model-gpu-") as tmp:
            root = Path(tmp)
            output, baseline, model_frames = run_arm(binary, rom, route, root, "models")
            check_models_arm(output)
            print("PASS models: every raced sprite, canopy attachment, banana spin, no fading "
                  "depth writes, shared GPU geometry", flush=True)
            output, hashes, disabled_frames = run_arm(binary, rom, route, root, "disabled", disabled=True)
            require(hashes == baseline, "disabled: models changed the simulation")
            require(not model_stats(output)[0], "disabled: the original sprites path drew models")
            require(disabled_frames != model_frames,
                    "disabled: frames equal the models arm, so no frame comparison can fail")
            print("PASS disabled models: simulation parity, sprites drawn", flush=True)
            if args.smoke_only:
                return 0
            output, hashes, _ = run_arm(binary, rom, route, root, "high-rate", rate="120",
                                       smoothing="interpolate")
            require(hashes == baseline, "high-rate: presentation rate changed the simulation")
            replayed = check_high_rate_arm(output)
            print(f"PASS high-rate replay: {replayed} interpolated model poses", flush=True)
            output, hashes, frames = run_arm(binary, rom, route, root, "translucent-atlas", alpha=153)
            require(hashes == baseline, "translucent-atlas: atlas alpha changed the simulation")
            require(frames == model_frames,
                    "translucent-atlas: a kept cutout texel blended by its own alpha")
            print("PASS cutout coverage: texel alpha decides coverage only", flush=True)
            for index, (point, persistent) in enumerate((p, s) for s in (False, True) for p in FAULTS):
                fault = f"{point}@all" if persistent else point
                name = f"fault-{index:02d}"
                output, hashes, frames = run_arm(binary, rom, route, root, name, fault=fault)
                require(hashes == baseline, f"{name} ({fault}): the failure changed the simulation")
                check_fault_arm(f"{name} ({fault})", point, output, disabled_frames, frames)
                print(f"PASS {fault}: models off, sprites drawn, game continues", flush=True)
    except GateFailure as failure:
        print(f"FAIL {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
