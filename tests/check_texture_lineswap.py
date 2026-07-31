#!/usr/bin/env python3
"""Pre-swizzled ("interlaced") textures must be un-swizzled before decode.

The bug this locks down
----------------------
Real RDP TMEM stores every ODD row of a texture with the two 32-bit halves of
each 64-bit word exchanged, and the texture-fetch unit un-exchanges them on the
way out.  `LOADBLOCK` normally performs that exchange itself, at a rate given by
its `dxt` field, so DRAM holds a plain linear image and the two exchanges cancel.

`dxt` is a 1.11 reciprocal (`CALC_DXT` = ceil(2048 / words_per_line)), so it can
only express a row length exactly when the row is a power-of-two number of 64-bit
words.  For every other width libultra offers the `...S` macro family — "the S at
the end means odd lines are already word Swapped" (game/include/PR/gbi.h:2699) —
which passes dxt = 0 (no load-time exchange) and relies on the ASSET being
pre-swizzled to cancel the fetch-time exchange instead.  DKR's asset tool does
exactly that, records it as `TextureHeader.flags` bit 0x04 (RENDER_LINE_SWAP,
documented as "Interlaced texture"), and `material_init()` then picks the S
macros for those textures (game/src/textures_sprites.c:1631).

We have no TMEM: `dkr_upload_tile_texture()` reads DRAM rows straight through, so
the fetch-time exchange never happens for us.  A pre-swizzled asset therefore
decoded with every odd row's texel groups transposed — "mostly right, but
scrambled".  It was reported as the golden-balloon HUD glyph in the browser
build; it reproduced identically in the native build on BOTH backends
(MDKR_RENDERER=webgpu and =gl), because the fault is in the shared F3DDKR HLE and
not in any backend.

It was never a small bug.  Measured over three routes, **dxt == 0 loads are
~30 % of all texture uploads** (hub 306/947, race 286/962, menus 105/284) and
they include the in-race and hub MINIMAPS, every palm frond and plant on Timber's
Island, the plane's propeller disc, the collectable-balloon sprites and the
particle/smoke atlases.  Nothing crashed and nothing was ever asserted on, which
is why it survived 107 commits — the same silence as the ASSET_MISC_8 denormal.

Why the check works this way
----------------------------
The failure mode is *wrong pixels*, and no reference image may be committed (no
ROM-derived bytes — tools/check_no_rom.sh).  So the check renders the route
TWICE from the same binary and compares the two arms:

    arm ON  : normal run                 (odd rows un-swizzled)
    arm OFF : MDKR_LINESWAP=off          (pre-fix decode, reproduced verbatim)

`MDKR_LINESWAP=off` was verified to reproduce the pre-fix binary's frames
byte-for-byte (4/4 sampled frames), so arm OFF is a faithful positive control
that is re-rendered on every run.  That is deliberate: this check cannot pass
vacuously, because it has to *see* the broken image and then measure that the
normal arm is better. Both arms explicitly use `--pure`, so mipmaps,
anisotropy and Remastered-only effects cannot dilute this restoration gate.

The metric is the artifact itself.  The swizzle displaces odd texel rows
horizontally, so neighbouring texel rows stop agreeing while same-parity rows
still do.  Measure, at texel-row spacing `step` (= frame_width / 320, the HiDPI
scale; the HUD is authored in 320x240 space):

    parity = mean |I(x, y) - I(x, y+step)|  /  mean |I(x, y) - I(x, y+2*step)|

A clean image is smoother between adjacent rows than between rows two apart, so
parity < 1.  A swizzled one is rougher between adjacent rows, so parity > 1.

Two regions are scored:

  * CHANGED-PIXEL MASK — every pixel the two arms disagree on.  Self-targeting:
    no hand-picked rectangle, so it follows whatever textures the route happens
    to load. Because that mask spans minified 3D geometry whose screen-row
    spacing is not necessarily one authored texel row, it asserts the clean-arm
    ceiling and improvement ratio, not an absolute broken-arm parity. Measured
    after screen-linear shade restoration: OFF 1.01..1.58, ON 0.61..0.73
    (ratio 1.52..2.60).
  * MINIMAP ROI — the in-race minimap corner, the largest single visible
    casualty (a field of disconnected dashes before, a continuous track outline
    after).  Measured: OFF 1.45..2.43, ON 0.60..0.69 (ratio >= 2.1).

Thresholds sit well clear of both sets.  Verified in both directions: with the
un-swizzle reverted the two arms render identically, the changed-pixel count
drops to 0 and both parity assertions fail (see docs/OPEN_ITEMS.md).

The reported symptom itself — the golden balloon in the top-left of the ADVENTURE
HUD — lives on the hub route, which is 2x the frames for a weaker ROI signal: the
balloon is small and mostly flat gold, so its absolute parity only reaches
OFF 0.97..0.98 against ON 0.56..0.64 (still a clean 1.70..1.76x separation, but
below MIN_PARITY_OFF, which is calibrated for the minimap).  Lower the bound to
run that route, and note its changed-mask numbers are as strong as the default
route's (OFF 1.25..1.31, ON 0.61..0.64, 1.99..2.08x):

    tests/check_texture_lineswap.py --script tests/input_scripts/adventure_hub_drive.txt \
        --frames 7300 --dump-from 7000 --dump-every 100 \
        --roi 0.04,0.04,0.15,0.18 --min-parity-off 0.90

Usage:
    tests/check_texture_lineswap.py [--build build] [--rom baserom.us.v80.z64]
                                    [--keep-frames DIR] [-v]

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one assertion failed (each
printed with the measured value).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

from harness_utils import resolve_binary

SCRIPT = "tests/input_scripts/race_drive_long.txt"
FRAMES = 3900          # race clock starts ~3120; this samples the settled HUD
DUMP_FROM = 3400
DUMP_EVERY = 150

# The in-race minimap corner, as fractions of the frame so the ROI survives a
# change of dump resolution.
MINIMAP_ROI = (0.781, 0.562, 0.953, 0.917)

# --- thresholds (measurements in the module docstring) -----------------------
MASK_DELTA = 8            # luma difference that counts a pixel as "changed"
MIN_CHANGED_PIXELS = 500  # sampled at stride 2; observed 4647..17746, broken-arm-only 0
MIN_UPLOADS = 50          # observed 286 on this route; 0 would mean a dead route
# A clean image scores < 1 (adjacent rows agree better than rows 2 apart).
MAX_PARITY_ON = 1.00      # observed 0.60..0.72 (mask) / 0.60..0.69 (minimap)
MIN_PARITY_OFF = 1.05     # observed 1.07..1.81 (mask) / 1.45..2.43 (minimap)
MIN_PARITY_RATIO = 1.40   # observed >= 1.79 (mask) / >= 2.10 (minimap)

TEX_RE = re.compile(r"\[TEX\] lineSwappedUploads=(\d+)")


def read_ppm(path: str) -> tuple[int, int, bytes]:
    """Minimal P6 reader (same shape as check_race_drive.py's)."""
    data = open(path, "rb").read()
    idx, fields = 0, []
    while len(fields) < 4:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] not in (b"\n", b""):
                idx += 1
            continue
        j = idx
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[idx:j])
        idx = j
    idx += 1                      # single whitespace byte before the raster
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[idx:idx + w * h * 3]


def luma(path: str) -> tuple[int, int, list[int]]:
    w, h, px = read_ppm(path)
    out = [0] * (w * h)
    for i in range(w * h):
        o = i * 3
        out[i] = (px[o] * 299 + px[o + 1] * 587 + px[o + 2] * 114) // 1000
    return w, h, out


def parity(px: list[int], w: int, h: int, pts: list[tuple[int, int]], step: int) -> float:
    """mean |I(y)-I(y+step)| / mean |I(y)-I(y+2*step)| over `pts`.

    < 1 for a clean image (neighbouring texel rows agree); > 1 when odd rows have
    been displaced horizontally, which is exactly the swizzle artifact.
    """
    s1 = s2 = n = 0
    for x, y in pts:
        if y + 2 * step >= h:
            continue
        base = px[y * w + x]
        s1 += abs(base - px[(y + step) * w + x])
        s2 += abs(base - px[(y + 2 * step) * w + x])
        n += 1
    if n == 0 or s2 == 0:
        return 0.0
    return (s1 / n) / (s2 / n)


def run(binary: str, rom: str, script: str, frames: int, dump_from: int,
        dump_every: int, frame_dir: str, lineswap_off: bool,
        verbose: bool) -> tuple[str, int]:
    os.makedirs(frame_dir, exist_ok=True)
    env = dict(os.environ,
               MDKR_AUDIO="0",     # belt-and-braces; --headless-frames is the guarantee
               MDKR_DUMP_FROM=str(dump_from),
               MDKR_DUMP_EVERY=str(dump_every))
    if lineswap_off:
        env["MDKR_LINESWAP"] = "off"
    else:
        env.pop("MDKR_LINESWAP", None)
    cmd = [binary, "--headless-frames", str(frames), "--input-script", script,
           "--dump-frames", frame_dir, "--rom", rom, "--pure"]
    if verbose:
        print("$ " + ("MDKR_LINESWAP=off " if lineswap_off else "") + " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return proc.stdout + proc.stderr, proc.returncode


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--script", default=SCRIPT)
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--dump-from", type=int, default=DUMP_FROM)
    ap.add_argument("--dump-every", type=int, default=DUMP_EVERY)
    ap.add_argument("--roi", default=None,
                    help="x0,y0,x1,y1 as fractions of the frame (default: the "
                         "in-race minimap corner)")
    ap.add_argument("--min-parity-off", type=float, default=MIN_PARITY_OFF,
                    help="lower bound on the OFF arm's parity, i.e. how combed the "
                         "positive control must look. Calibrated for the default "
                         "minimap ROI; a smaller/flatter ROI needs a smaller value.")
    ap.add_argument("--keep-frames", default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, args.script):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1
    roi = MINIMAP_ROI
    if args.roi:
        roi = tuple(float(v) for v in args.roi.split(","))
        if len(roi) != 4:
            print("FAIL: --roi wants x0,y0,x1,y1", file=sys.stderr)
            return 1

    root = args.keep_frames or tempfile.mkdtemp(prefix="mdkr_lineswap_")
    dirs = {"on": os.path.join(root, "on"), "off": os.path.join(root, "off")}
    failures: list[str] = []
    uploads = {}
    for arm in ("on", "off"):
        out, rc = run(binary, args.rom, args.script, args.frames, args.dump_from,
                      args.dump_every, dirs[arm], arm == "off", args.verbose)
        if rc != 0:
            failures.append(f"arm {arm}: exit code {rc}")
        for marker in ("[CRASH]", "[FATAL]"):
            if marker in out:
                line = next((l for l in out.splitlines() if marker in l), marker)
                failures.append(f"arm {arm}: {marker} in output: {line.strip()}")
        m = TEX_RE.search(out)
        if not m:
            failures.append(f"arm {arm}: no '[TEX] lineSwappedUploads=' line — the "
                            f"decoder no longer reports the path this check tests")
            uploads[arm] = -1
        else:
            uploads[arm] = int(m.group(1))
            if uploads[arm] < MIN_UPLOADS:
                failures.append(f"arm {arm}: only {uploads[arm]} dxt==0 texture "
                                f"uploads (want >= {MIN_UPLOADS}) — this route no "
                                f"longer loads pre-swizzled textures, so the check "
                                f"would be vacuous")

    frames_on = sorted(f for f in os.listdir(dirs["on"]) if f.endswith(".ppm"))
    frames_off = sorted(f for f in os.listdir(dirs["off"]) if f.endswith(".ppm"))
    common = [f for f in frames_on if f in frames_off]
    if len(common) < 3:
        failures.append(f"only {len(common)} frame(s) dumped by both arms (want >= 3)")

    rows = []
    for name in common:
        w, h, a = luma(os.path.join(dirs["off"], name))   # OFF = pre-fix decode
        w2, h2, b = luma(os.path.join(dirs["on"], name))  # ON  = un-swizzled
        if (w, h) != (w2, h2):
            failures.append(f"{name}: arms differ in size {w}x{h} vs {w2}x{h2}")
            continue
        # Texel-row spacing: the HUD is authored in 320x240 and the dump is a
        # whole multiple of that (1280x960 => 4).
        step = max(1, w // 320)

        changed = [(x, y) for y in range(0, h, 2) for x in range(0, w, 2)
                   if abs(a[y * w + x] - b[y * w + x]) > MASK_DELTA]
        roi_pts = [(x, y)
                   for y in range(int(h * roi[1]), int(h * roi[3]), 2)
                   for x in range(int(w * roi[0]), int(w * roi[2]), 2)]

        mask_off = parity(a, w, h, changed, step)
        mask_on = parity(b, w, h, changed, step)
        roi_off = parity(a, w, h, roi_pts, step)
        roi_on = parity(b, w, h, roi_pts, step)
        rows.append((name, len(changed), mask_off, mask_on, roi_off, roi_on))

        # The fixed minimap ROI has known authored texel-row spacing, so its OFF
        # arm must cross the absolute "combed" threshold. The dynamic changed
        # mask also includes minified 3D surfaces where one framebuffer row is
        # not one texel row; there the non-vacuous controls are changed-pixel
        # count, clean-arm ceiling and OFF/ON improvement ratio.
        scored = [("minimap-roi", roi_off, roi_on, True)]
        if len(changed) < MIN_CHANGED_PIXELS:
            # An empty mask makes the mask parity numbers meaningless, so skip
            # those — but the fixed ROI is still scored, so a reverted build fails
            # on TWO independent grounds rather than only on "nothing changed".
            failures.append(f"{name}: only {len(changed)} pixel(s) differ between the "
                            f"arms (want >= {MIN_CHANGED_PIXELS}) — the un-swizzle is "
                            f"not changing the image, so it is not running")
        else:
            scored.insert(0, ("changed-mask", mask_off, mask_on, False))

        for tag, off, on, require_absolute_off in scored:
            if on > MAX_PARITY_ON:
                failures.append(f"{name} {tag}: un-swizzled arm still looks combed "
                                f"(parity {on:.3f}, want <= {MAX_PARITY_ON})")
            if require_absolute_off and off < args.min_parity_off:
                failures.append(f"{name} {tag}: MDKR_LINESWAP=off no longer reproduces "
                                f"the artifact (parity {off:.3f}, want >= "
                                f"{args.min_parity_off}) — the positive control is gone")
            ratio = off / on if on > 0 else 0.0
            if ratio < MIN_PARITY_RATIO:
                failures.append(f"{name} {tag}: parity only improved {ratio:.2f}x "
                                f"(off {off:.3f} -> on {on:.3f}, want >= "
                                f"{MIN_PARITY_RATIO}x)")

    if args.verbose or failures:
        print(f"  dxt==0 uploads: on={uploads.get('on')} off={uploads.get('off')}")
        for name, nch, mo, mn, ro, rn in rows:
            print(f"  {name}: changed={nch:6d}  mask off={mo:.3f} on={mn:.3f} "
                  f"({mo / mn if mn else 0:.2f}x)  minimap off={ro:.3f} on={rn:.3f} "
                  f"({ro / rn if rn else 0:.2f}x)")

    if args.keep_frames is None:
        shutil.rmtree(root, ignore_errors=True)

    if failures:
        print("check_texture_lineswap: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_texture_lineswap: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
