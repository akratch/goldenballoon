#!/usr/bin/env python3
"""Exercise replacement-model draws and GPU failure with generated model art.

Usage: python3 tests/check_sprite_models_gpu.py --binary build/mdkr64 --rom ROM
The ROM stays local; every pack and save is synthetic and temporary.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from check_sprite_models import pack
from harness_utils import resolve_binary

FAULTS = (
    "modern.module", "modern.bind-group-layout", "modern.pipeline-layout",
    "modern.pipeline", "modern.vertex-buffer", "modern.index-buffer",
    "modern.texture", "modern.view", "modern.uniform", "modern.sampler",
    "modern.bind-group",
)


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
    with tempfile.TemporaryDirectory(prefix="mdkr-model-gpu-") as tmp:
        root = Path(tmp)
        pack(root / "mods/base", 100, (255, 0, 0))
        # Native engine-owned devices recover once at a complete-frame boundary.
        # Persistent failures must then exit cleanly instead of retrying forever.
        cases = (None, "disabled") if args.smoke_only else (None, *FAULTS, *(f"{point}@all" for point in FAULTS), None, "disabled")
        baseline_hashes = None
        for index, fault in enumerate(cases):
            disabled = fault == "disabled"
            if disabled:
                fault = None
            case = root / str(index)
            (case / "save").mkdir(parents=True)
            env = {k: v for k, v in os.environ.items() if not k.startswith(("MDKR", "GE007_"))}
            env.update(MDKR_RENDERER="webgpu", MDKR_AUDIO="0", MDKR64_HIDDEN="1",
                       MDKR_APP_PREFS_DIR=str(root), MDKR_SAVE_DIR=str(case / "save"), MDKR_VIDEO_CONFIG_PATH=str(case / "video.ini"),
                       MDKR_TEST_SCRIPT_ONLY_INPUT="1", MDKR_PRESENT_RATE="30",
                       MDKR_LOAD_TRACK="8", MDKR_AUTOPILOT="1")
            env.update(MDKR_STATE_HASH="3", MDKR_STATE_HASH_FILE=str(case / "hashes.txt"),
                       MDKR_SPRITE_MODELS="0" if disabled else "1")
            if fault:
                env["MDKR_WEBGPU_FAULT"] = fault
            process = subprocess.run(
                [str(binary), "--rom", str(rom), "--headless-frames", "3400", "--window-size", "320x240",
                 "--input-script", str(route)], cwd=root, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            output = process.stdout
            persistent = fault is not None and fault.endswith("@all")
            if process.returncode != (1 if persistent else 0):
                raise AssertionError(f"{fault}: unexpected exit {process.returncode}\n{output[-4000:]}")
            if fault:
                point = fault.removesuffix("@all")
                assert f"[webgpu-fault] injected {point}@1" in output, output[-4000:]
                assert output.count("attempting one native device reinitialization") == 1, output[-4000:]
                if not persistent:
                    assert "native device recovery succeeded; gameplay continues" in output, output[-4000:]
            else:
                draws = {int(sprite): int(near) + int(far) for sprite, near, far in
                         re.findall(r"\[MODELS\] sprite=(\d+) near=(\d+) far=(\d+)", output)}
                if disabled:
                    assert not draws, "disabled replacement path emitted model draws"
                else:
                    assert all(draws.get(sprite, 0) > 0 for sprite in (114, 115, 116)), output[-4000:]
                    stats = re.findall(r"\[WGPU-MODELS\] textureUploads=(\d+) textureReuses=(\d+) textureUploadBytes=(\d+) geometryBytes=(\d+) entries=(\d+)", output)
                    assert len(stats) == 1, f"expected one cache lifetime: {stats}"
                    uploads, reuses, texture_bytes, geometry_bytes, entries = map(int, stats[0])
                    assert uploads == 1 and texture_bytes == 4, stats
                    assert entries >= 3 and reuses == entries - 1, stats
                    assert geometry_bytes == entries * (3 * 36 + 3 * 4), stats
            # Recovery must finish the same simulation, not merely print a
            # successful device bring-up and exit early. Persistent failures
            # intentionally terminate and cannot satisfy this completion gate.
            if not persistent:
                hashes = (case / "hashes.txt").read_text().splitlines()
                assert len(hashes) == 3400, f"incomplete simulation stream: {len(hashes)}"
                assert all(re.fullmatch(r"\[SIMHASH\] tick=\d+ objs=-?\d+ h=[0-9a-f]{16}", row) for row in hashes)
                ticks = [int(re.search(r"tick=(\d+)", row)[1]) for row in hashes]
                assert ticks == list(range(ticks[0], ticks[0] + 3400)), "missing or duplicate simulation ticks"
                assert len({row.rsplit("h=", 1)[1] for row in hashes}) > 1, "constant simulation hash stream"
                if baseline_hashes is None:
                    baseline_hashes = hashes
                else:
                    assert hashes == baseline_hashes, f"{fault or 'normal/disabled'} changed simulation hashes"
            for forbidden in ("Validation Error", "validation error", "AddressSanitizer", "SIGABRT", "SIGSEGV",
                              "switched to OpenGL", "[webgpu] device error"):
                assert forbidden not in output, output[-4000:]
            print(f"PASS {fault or ('disabled models: simulation parity' if disabled else 'model draws and shutdown')}", flush=True)


if __name__ == "__main__":
    main()
