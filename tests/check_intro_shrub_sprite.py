#!/usr/bin/env python3
"""Multi-batch billboard sprites must place every tile in its own quad.

The bug this locks down
-----------------------
`sprite_init_frame()` (game/src/textures_sprites.c) builds one quad per sprite
tile, but the RSP only holds twenty vertices at a time, so a frame wider than
five tiles is emitted as SEVERAL runs: each run issues its own
`gSPVertexDKR(..., G_VTX_APPEND)` and then restarts its triangle indices at
vertex 1 (`curVertIndex = 0`).  That is only correct because G_VTX_APPEND is not
a running cursor -- the F3DDKR RSP keeps ONE count, the length of the last
flag-0 load, and every appended load writes immediately after THAT
(game/include/f3ddkr.h).  Two consecutive appends therefore share a base and the
second overwrites the first.

The HLE advanced the base on every append instead, so run 2 landed at slot 21
while its triangles still named slots 1..4.  Every sprite with a sixth tile then
drew that tile's texture over the FIRST tile's rectangle and never drew it in
its own place.  Both halves of GitHub issue #11 are that one fault: the
new-game intro's shrubs are six-tile sprites, so each grew a duplicate piece
stacked on top of its crown AND lost its bottom band, which left it hanging
above the sand.  Verified against real hardware with the ares oracle
(docs/ORACLE.md): the console draws neither the duplicate nor the gap.

Why the check works this way
----------------------------
The failure mode is wrong pixels and no reference image may be committed (no
ROM-derived bytes -- tools/check_no_rom.sh), so the check renders the authored
intro cutscene and scores three fixed regions of one deterministic frame
(byte-identical across runs and across both native backends).  A pixel counts
as foliage when green clearly dominates both other channels.

  * HALO   -- the water/cliff band directly above the foreground shrubs, where
              the duplicated tile was painted.  Measured 5058 foliage pixels
              broken, 1621..1622 fixed (the real crowns reach into it).
  * STEM   -- the bottom band of the isolated mid-ground shrub, the tile the
              broken build never drew at all.  Measured 0 broken, 84..91 fixed.
              This is the "not seated on the ground" half.
  * BODY   -- the same shrub's leaf mass, which BOTH builds draw (4151..4223).
              It is the vacuity guard: the halo and stem bounds could also be
              met by a scene that renders no shrubs at all.

HALO and STEM separate by 1.7x and by presence/absence respectively, so the
thresholds sit clear of both sets.  Verified in both directions: with the append
base advanced again (the pre-fix HLE) HALO rises past its ceiling and STEM falls
to zero.

Usage:
    tests/check_intro_shrub_sprite.py [--build build] [--rom baserom.us.v80.z64]
                                      [--renderer both|gl|webgpu]
                                      [--keep-frames DIR] [-v]

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one assertion failed (each
printed with the measured value).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary

# The one fixture that reaches MENU_NEWGAME_CINEMATIC and level 36, the authored
# opening sequence (see the header of the script itself).
SCRIPT = "tests/input_scripts/adventure_hub_drive.txt"
FRAMES = 2880
DUMP_FRAME = 2875      # mid-cutscene, camera facing the shrubs by the water

# Regions as fractions of the frame, so they survive a change of dump resolution.
HALO_ROI = (0.4648, 0.7396, 0.7500, 0.7708)
STEM_ROI = (0.8188, 0.8479, 0.8531, 0.8667)
BODY_ROI = (0.7969, 0.7812, 0.8828, 0.8333)

# --- thresholds (measurements in the module docstring) -----------------------
MAX_HALO = 3000        # fixed 1621..1622, broken 5058
MIN_STEM = 40          # fixed 84..91, broken 0
MIN_BODY = 3000        # fixed 4151..4212, broken 4223 -- vacuity guard only

# Green clearly dominant in both other channels, and bright enough not to catch
# the shaded water. Deliberately coarse: the artifact is opaque foliage.
CHANNEL_MARGIN = 25
MIN_GREEN = 80


def foliage_pixels(w: int, h: int, px: bytes,
                   roi: tuple[float, float, float, float]) -> int:
    x0 = int(roi[0] * w)
    y0 = int(roi[1] * h)
    x1 = int(roi[2] * w)
    y1 = int(roi[3] * h)
    count = 0
    for y in range(y0, y1):
        row = y * w
        for x in range(x0, x1):
            o = (row + x) * 3
            red, green, blue = px[o], px[o + 1], px[o + 2]
            if (green >= MIN_GREEN and green > red + CHANNEL_MARGIN
                    and green > blue + CHANNEL_MARGIN):
                count += 1
    return count


def run(binary: str, rom: str, script: str, renderer: str,
        run_dir: str, frame_dir: str, verbose: bool) -> tuple[str, int]:
    os.makedirs(frame_dir, exist_ok=True)
    save_dir = os.path.join(run_dir, "save")
    os.makedirs(save_dir, exist_ok=True)
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        MDKR_AUDIO="0",     # belt-and-braces; --headless-frames is the guarantee
        MDKR_RENDERER=renderer,
        MDKR_SAVE_DIR=save_dir,
        # A developer's saved video policy must not renumber the authored frame
        # this check scores.
        MDKR_VIDEO_CONFIG_PATH=os.path.join(run_dir, "video.ini"),
        MDKR_DUMP_FROM=str(DUMP_FRAME),
        MDKR_DUMP_EVERY=str(FRAMES),   # exactly one frame
        MDKR64_HIDDEN="1",
        LC_ALL="C",
    )
    cmd = [binary, "--headless-frames", str(FRAMES), "--input-script", script,
           "--dump-frames", frame_dir, "--rom", rom, "--pure"]
    if verbose:
        print("$ MDKR_RENDERER=" + renderer + " " + " ".join(cmd))
    proc = subprocess.run(
        cmd, capture_output=True, text=True, env=env, cwd=run_dir
    )
    return proc.stdout + proc.stderr, proc.returncode


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument(
        "--renderer", choices=("both", "gl", "webgpu"), default="both",
        help="renderer coverage (default: both shipped native backends)",
    )
    ap.add_argument("--keep-frames", default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = os.path.abspath(resolve_binary(args.build))
    rom = os.path.abspath(args.rom)
    script = os.path.abspath(SCRIPT)
    for path in (binary, rom, script):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    root = args.keep_frames or tempfile.mkdtemp(prefix="mdkr_intro_shrub_")
    renderers = (
        ("webgpu", "gl") if args.renderer == "both" else (args.renderer,)
    )
    failures: list[str] = []

    for renderer in renderers:
        run_dir = os.path.join(root, renderer)
        frame_dir = os.path.join(run_dir, "frames")
        os.makedirs(run_dir, exist_ok=True)
        output, code = run(binary, rom, script, renderer, run_dir, frame_dir,
                           args.verbose)
        if code != 0:
            failures.append(f"{renderer}: binary exited {code}")
            print(output[-2000:], file=sys.stderr)
            continue
        frame = os.path.join(frame_dir, f"frame_{DUMP_FRAME}.ppm")
        if not os.path.exists(frame):
            failures.append(
                f"{renderer}: the route never dumped frame {DUMP_FRAME}")
            continue
        w, h, px = read_ppm(frame)
        halo = foliage_pixels(w, h, px, HALO_ROI)
        stem = foliage_pixels(w, h, px, STEM_ROI)
        body = foliage_pixels(w, h, px, BODY_ROI)
        print(f"{renderer:7s} halo={halo:6d} (max {MAX_HALO})  "
              f"stem={stem:5d} (min {MIN_STEM})  "
              f"body={body:6d} (min {MIN_BODY})")
        if body < MIN_BODY:
            failures.append(
                f"{renderer}: shrub body {body} < {MIN_BODY} -- the scene did "
                f"not render, so the other bounds prove nothing")
        if halo > MAX_HALO:
            failures.append(
                f"{renderer}: {halo} foliage pixels above the shrubs "
                f"(max {MAX_HALO}) -- a duplicated sprite tile is stacked on "
                f"the crowns")
        if stem < MIN_STEM:
            failures.append(
                f"{renderer}: shrub base {stem} < {MIN_STEM} -- the sprite's "
                f"bottom tile is missing, so the shrub floats")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("PASS: intro shrub sprites place every tile in its own quad")
    return 0


if __name__ == "__main__":
    sys.exit(main())
