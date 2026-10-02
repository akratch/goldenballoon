#!/usr/bin/env python3
"""A content pack replaces the generated Taj, Wizpig and Terry portraits.

Why this exists
---------------
The three bonus racers have no retail portrait, so the port draws theirs from
code: `taj_portrait_texture()`, `wizpig_portrait_texture()` and
`terry_portrait_texture()` in game/src/menu.c compose 40x40 RGBA cards out of
rectangles and ellipses without bundling extracted portraits. A player can
replace that first-party fallback artwork with a content pack.

That route already works, and this gate is the thing that says so out loud and
keeps it true. A generated portrait is an ORDINARY texture bind: menu.c hands
its texels to the display list like any material, `dkr_bind_tile()` keys them
the way it keys a ROM tile, and the override layer in front of the ROM path
(platform/fast3d/gfx_pc_dkr.c) neither knows nor cares where the pixels came
from. So the published digest contract in platform/mod_texture_key.h covers
these cards with no second scheme, no version bump and no producer-side seam:
the digest is taken over the generated RGBA bytes plus the same header fields
it takes over a ROM texture's, because they arrive at the same function.

What is easy to lose, and what this file is really guarding
-----------------------------------------------------------
A ROM texture's digest is frozen for all time -- the ROM does not change. A
GENERATED portrait's digest is a function of OUR code. Move one ellipse in
menu.c and the name changes, and every pack in the wild that addressed the old
name goes quietly inert: no crash, no log line, no failing test. The player
sees the stock portrait and has no way to tell a broken pack from a pack they
installed wrong.

So the three current digests below are pinned deliberately. Retouching the art
must update docs/MODDING.md and preserve published pack names as aliases. The
blue-elephant Taj revision retains its old name, with separate GL/WebGPU arms
below proving that an unmodified pre-1.7 pack still draws the replacement.

The route
---------
`tests/input_scripts/race_full_3lap_tt.txt` to frame 7700 -- the same real
post-race flow, the same capture frame and the same result-card anchor
`check_bonus_results_portraits.py` already uses, so the region sampled here is
a region another gate independently proves holds a portrait. Each character is
put in player 0 by its own bootstrap variable (`MDKR_TAJ_TEST_PLAYER` and its
two siblings). Nine runs, each in its own throwaway working directory, which
is also where that arm's `mods/` and dumped corpus live:

    baseline          Taj, no `mods/` at all, MDKR_MOD_TEXTURE_DUMP on
    taj               Taj, one pack overriding the pinned Taj digest, dump ON
    wizpig            Wizpig, the same at the pinned Wizpig digest
    terry             Terry, the same at the pinned Terry digest
    disabled          the identical pack with `enabled = 0` in its pack.ini
    webgpu-baseline   the baseline arm again on the shipped default backend
    webgpu-taj        the taj arm again on the shipped default backend

The last two are not decoration. WebGPU is what ships, and a digest measured
only on GL would be a name published from a renderer most players never run.
The digest excludes everything renderer-chosen BY CONSTRUCTION -- the allocation
address, row addressing, mips, the font atlas -- so the three names should be
backend-independent; "should be" is what these arms convert into an observation.
They were also measured by hand across both backends before being written: all
three digests are byte-identical on GL and WebGPU, which is why docs/MODDING.md
publishes one table and not two.

Frames are NOT compared across backends anywhere here. Two rasterizers need not
produce the same bytes and nothing in this file claims they do; the byte-identity
assertion (5) stays within GL.

The pack image is `quadrant_png()` imported from check_mod_texture_override.py
-- the same synthetic magenta-corner-on-green PNG that gate authors for itself,
at 64x64 here. Deliberately NOT a solid colour, and deliberately larger than
the 40x40 card. A solid replacement cannot witness texture ADDRESSING: every
sub-window of solid magenta is solid magenta, which is exactly how the
corner-sampling defect behind issue #34 stayed invisible to that gate's earlier
form. The quadrant makes the two outcomes unmistakable on a portrait too --
correct logical addressing shows the whole card the whole image, green three to
one; corner addressing shows it magenta.

No ROM bytes and no game art reach the repository: the pack pixels are two
constants this file's neighbour names, every run works in a temporary
directory, and the dumped corpus is read for filenames only.

What is asserted
----------------
 1. REACHABILITY, AND NOTHING VACUOUS. Every run exits 0 with no runtime
    diagnostic, and player 0 owns THIS character's result card
    (`bonus_results_portrait: player=0 identity=<n>`) composed at 40x40. The
    ownership line is what makes the arm character-specific: the results screen
    composes all three cards on every run, so "a card was composed" is true in
    every arm and would assert nothing. The three pack arms report their pack
    active; the disabled arm reports it skipped WITH the pack.ini reason; the
    baseline arm reports no pack at all.

 2. THE DUMP PUBLISHES THE PORTRAIT DIGEST. The baseline arm's
    MDKR_MOD_TEXTURE_DUMP corpus contains `<TAJ_DIGEST>.png` and a sidecar
    saying 40x40 -- so a pack author following the "Finding a digest" recipe in
    docs/MODDING.md finds these cards there like any other texture, rather than
    having to be told a constant. When it is absent the failure NAMES every
    40x40 digest the route did dump, because the overwhelmingly likely cause is
    that the procedural art changed and this is the new name.

 3. A PACK DRAWS THE PORTRAIT. In each pack arm the whole card region is the
    pack's own two colours -- coverage at least 99.5% -- and green-dominant
    between 60% and 85% of it, which is the quadrant seen whole. Green-family
    is the witness rather than magenta because Taj's own purple turban reads
    magenta-family (measured: 23% of the stock card), while green-family is
    exactly 0 in all three stock cards including Terry's, whose teal has too
    much blue to qualify.

 4. AND ONLY THE PORTRAIT. A region of the same results screen beside the card
    carries zero pack pixels in every arm. Without this, "the card is the pack"
    would also be satisfied by a build that replaced the entire frame.

 5. TURNING THE PACK OFF RESTORES THE GENERATED CARD, EXACTLY. The disabled
    arm's frame is BYTE-IDENTICAL to the no-pack baseline's. Byte-identical is
    the whole assertion: a merely "different" frame would also be produced by a
    pack that half-applied. This is the no-pack guarantee -- an install with no
    pack renders what it rendered before packs existed.

 6. THE DUMP RECORDS AN OVERRIDE WHOLE, AND AT ITS OWN SIZE. The `taj` arm
    runs with a pack installed AND the dump on -- the one combination that
    reaches the encode path, and one no other arm and no other gate produces.
    The written PNG must be 64x64 and pixel-identical to the pack's own image,
    and its sidecar must agree. This is a regression pin: the renderer used to
    pass the pack's 64x64 buffer with the tile's LOGICAL 40x40 size, because
    the two share `uw`/`uh` by that point and the second had been reset for
    texcoord normalisation. stb then encoded 40x40 at a 160-byte stride from a
    256-byte-stride image -- in bounds, so no crash and no sanitizer report,
    just a silently sheared picture handed to the author who asked what the
    game drew. This arm runs on GL only: the call site it pins is in
    gfx_pc_dkr.c, above the backend interface and shared by both renderers, so
    a second WebGPU copy of it would cost a run to observe the same code.

 7. AND ALL OF IT HOLDS ON THE RENDERER THAT SHIPS. On WebGPU the corpus
    publishes the SAME Taj digest (assertion 2 again, so the published name is
    not a GL artifact) and a pack at that name draws the card (assertion 3
    again). Every arm additionally proves it got the backend it asked for, so
    an adapter that quietly fell back to GL cannot pass as WebGPU evidence.

Self-validation -- this check is proven to be able to fail
-----------------------------------------------------------
Both positive controls below were run by hand against this build, not by this
file, and both are the SAME defect seen from its two ends: the pinned name and
the picture it names having drifted apart.

CONTROL 1, the pin is wrong. Change the last digit of TAJ_DIGEST below and
re-run. Assertion 2 fires first -- before any pack arm, because the baseline
arm runs first -- and it self-diagnoses, naming the digest the art actually
produces:

    check_bonus_portrait_pack: FAIL -- baseline: MDKR_MOD_TEXTURE_DUMP did not
    publish 7757ffb6d3f809fbde246ca559d51eb5. The generated portrait art has
    almost certainly changed, which renames it and silently breaks every pack
    that addressed the old name. 40x40 digests this route did dump:
    ['7757ffb6d3f809fbde246ca559d51eb4']

CONTROL 2, the art is what moved -- the failure this gate exists for. Change
ONE channel of ONE pixel of Taj's card in game/src/menu.c (measured on
`taj_portrait_pixel(16, 4, ...)`, 96 -> 97 in the blue channel: a change no
player could see) and rebuild. The card's digest becomes
8a107fd16aef96b359e2469454aa23cb, every pack addressing the old name goes
inert, and assertion 2 fails with that new name in hand. Nothing else in the
suite notices: the art still draws, the portrait gates still pass, and without
this assertion the break would surface as a modder's bug report.

Always muted and headless (`MDKR_AUDIO=0`, `--headless-frames`), per
tests/README.md. `MDKR_AUDIO=off` would be a silent no-op -- only the digit `0`
disables. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

from check_mod_texture_override import (PACK_RGBA, PACK_RGBA_ALT,
                                        quadrant_png)
from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
CAPTURE_FRAME = 7700
FRAMES = CAPTURE_FRAME + 20

# The published names of the three generated portraits, under the digest
# contract in platform/mod_texture_key.h. Pinned on purpose: see the header.
# Reproduce them with
#   MDKR_MOD_TEXTURE_DUMP=<dir> MDKR_<NAME>_TEST_PLAYER=0 ... --headless-frames
# and look for the 40x40 RGBA sidecar, which is what assertion 2 does.
TAJ_DIGEST = "dcd45f4f32c9e1da4abeb3c1c1f8011b"
TAJ_LEGACY_DIGEST = "7757ffb6d3f809fbde246ca559d51eb4"
WIZPIG_DIGEST = "813ff52ed6575a1f29c8fa2fc47b2464"
TERRY_DIGEST = "fd222569d7bd95580075402b7315e178"

# character -> (bootstrap variable, published digest, ModRacerIdentity value).
# The identity is what makes assertion 1 character-SPECIFIC: the results screen
# composes all three cards on every run, so "a card was composed" is true in
# every arm and would assert nothing. Only one of them is the player's, and
# only the player's is bound -- which is also why one arm's corpus holds one
# 40x40 digest and not three.
PORTRAITS = {
    "taj": ("MDKR_TAJ_TEST_PLAYER", TAJ_DIGEST, 1),
    "wizpig": ("MDKR_WIZPIG_TEST_PLAYER", WIZPIG_DIGEST, 2),
    "terry": ("MDKR_TERRY_TEST_PLAYER", TERRY_DIGEST, 3),
}

# CARD_BOUNDS is the retail stopwatch portrait anchor on the single-player
# race-times card, copied from check_bonus_results_portraits.py -- so the
# region sampled here is one another gate independently proves holds a
# portrait, rather than a rectangle this file chose for itself. BESIDE_BOUNDS
# is an equal-sized strip of the same results screen clear of the card, and
# exists only to give assertion 4 somewhere pack pixels must NOT appear.
CARD_BOUNDS = (0.105, 0.72, 0.215, 0.88)
BESIDE_BOUNDS = (0.300, 0.72, 0.410, 0.88)

PACK_SIZE = 64
PACK_INI = b"[pack]\nname=Portrait Test\npriority=100\n"
PACK_INI_OFF = b"[pack]\nname=Portrait Test\npriority=100\nenabled=0\n"

# Assertion 3's floors. Coverage is all but exact; the band around the
# quadrant's own 1:3 split is wide enough for the card's edge texels and
# narrow enough to separate "the whole image" from "one corner of it", which
# is the whole reason the image is not a solid colour.
MIN_PACK_COVERAGE = 0.995
MIN_GREEN_SHARE = 0.60
MAX_GREEN_SHARE = 0.85

BAD_MARKERS = ("[FATAL]", "[CRASH]", "AddressSanitizer",
               "UndefinedBehaviorSanitizer", "runtime error:")


class CheckError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckError(message)


def install_pack(run_dir: Path, digest: str, enabled: bool) -> None:
    """Writes `mods/PortraitPack/textures/<digest>.png` and its pack.ini."""
    pack = run_dir / "mods" / "PortraitPack"
    (pack / "textures").mkdir(parents=True)
    (pack / "pack.ini").write_bytes(PACK_INI if enabled else PACK_INI_OFF)
    (pack / "textures" / f"{digest}.png").write_bytes(quadrant_png(PACK_SIZE))


def decode_png(blob: bytes) -> tuple[int, int, bytes]:
    """(width, height, RGBA8 bytes) from an 8-bit RGBA non-interlaced PNG.

    Standard library only, and deliberately a real decoder rather than a
    header peek: assertion 6 has to compare the dumped PIXELS, not just the
    dimensions in the IHDR. A sheared image carries the right header.

    Its own non-vacuity is checked by round-tripping quadrant_png() through it
    before it is trusted with the game's output -- but that fixture is written
    with row filter 0 on every row, so the self-check exercises the unfiltered
    path ONLY. stb_image_write picks filters per row heuristically and the
    game's dumps were measured using types 1, 2 and 4, so those branches are
    covered only by the pixel comparison itself: if one of them is wrong, the
    comparison fails and this docstring is the first place to look. A fixture
    that exercised all five would have to be hand-encoded, which is a decoder
    of its own to get wrong.
    """
    require(blob[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG")
    offset = 8
    header: tuple[int, ...] | None = None
    idat = bytearray()
    while offset + 8 <= len(blob):
        length = int.from_bytes(blob[offset:offset + 4], "big")
        tag = blob[offset + 4:offset + 8]
        body = blob[offset + 8:offset + 8 + length]
        offset += 12 + length
        if tag == b"IHDR":
            header = (
                int.from_bytes(body[0:4], "big"),
                int.from_bytes(body[4:8], "big"),
                body[8], body[9], body[12],
            )
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
    require(header is not None, "PNG has no IHDR")
    width, height, depth, colour, interlace = header
    require(depth == 8 and colour == 6 and interlace == 0,
            f"unexpected PNG encoding: depth={depth} colour={colour} "
            f"interlace={interlace}")

    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    out = bytearray()
    previous = bytearray(stride)
    position = 0
    for _ in range(height):
        kind = raw[position]
        position += 1
        line = bytearray(raw[position:position + stride])
        position += stride
        if kind == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                left = line[i - 4] if i >= 4 else 0
                up = previous[i]
                corner = previous[i - 4] if i >= 4 else 0
                estimate = left + up - corner
                da, db, dc = (abs(estimate - left), abs(estimate - up),
                              abs(estimate - corner))
                if da <= db and da <= dc:
                    predictor = left
                elif db <= dc:
                    predictor = up
                else:
                    predictor = corner
                line[i] = (line[i] + predictor) & 0xFF
        else:
            require(kind == 0, f"unsupported PNG row filter {kind}")
        out += line
        previous = line
    return width, height, bytes(out)


def classify(path: Path, bounds: tuple[float, float, float, float]) -> tuple[int, int, int]:
    """(magenta-family, green-family, total) inside a fractional region.

    Classified by hue relation rather than exact value because the combiner
    multiplies texels by shade and prim colour: scaling preserves which
    channels dominate, exact values it does not. The margins keep the game's
    own greys, fades and text out of both families -- and, measured on the
    stock cards, keep Terry's teal out of the green family, which is what
    lets green be the pack's witness for all three characters.
    """
    width, height, pixels = read_ppm(path)
    x0, y0, x1, y1 = bounds
    magenta = green = total = 0
    for y in range(int(height * y0), int(height * y1)):
        for x in range(int(width * x0), int(width * x1)):
            offset = (y * width + x) * 3
            red, grn, blue = pixels[offset], pixels[offset + 1], pixels[offset + 2]
            total += 1
            if grn > 64 and grn > (red + blue):
                green += 1
            elif red > 64 and blue > 64 and red > 2 * grn and blue > 2 * grn:
                magenta += 1
    return magenta, green, total


def run_arm(binary: Path, rom: Path, root: Path, label: str, character: str,
            pack: str | None, enabled: bool, corpus: bool,
            timeout: int, verbose: bool,
            renderer: str = "gl") -> tuple[Path, str, Path | None]:
    """One headless run. Returns (frame, stdout, corpus dir or None)."""
    run_dir = root / label
    save = run_dir / "save"
    frames = run_dir / "frames"
    save.mkdir(parents=True)
    frames.mkdir()
    dump = None
    if corpus:
        dump = run_dir / "corpus"
        dump.mkdir()
    if pack is not None:
        install_pack(run_dir, pack, enabled)

    variable = PORTRAITS[character][0]
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update({
        "LC_ALL": "C",
        "MDKR_AUDIO": "0",
        # A repo-root mdkr64.ini (e.g. FrameLimit=uncapped) must not govern a
        # frame-budgeted headless run.
        "MDKR_VIDEO_CONFIG_PATH": os.devnull,
        "MDKR_AUTOPILOT": "1",
        "MDKR_TRACE": "1",
        "MDKR_LOAD_TRACK": "5:0",
        variable: "0",
        "MDKR_SAVE_DIR": str(save),
        # This arm's own pack folder, whatever a portable marker would say.
        "MDKR_MODS_DIR": str(run_dir / "mods"),
        "MDKR_RENDERER": renderer,
        "MDKR64_HIDDEN": "1",
        "MDKR_DUMP_FROM": str(CAPTURE_FRAME),
        "MDKR_DUMP_EVERY": "10000",
    })
    if dump is not None:
        env["MDKR_MOD_TEXTURE_DUMP"] = str(dump)

    command = [
        str(binary), "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT), "--dump-frames", str(frames),
        "--rom", str(rom),
    ]
    if verbose:
        print(f"$ (cd {run_dir} && " + " ".join(command) + ")", flush=True)
    # cwd is this arm's own directory: it is where `mods/` is read from, so
    # one arm's pack cannot be seen by another.
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False,
    )
    output = process.stdout or ""
    (run_dir / "run.log").write_text(output, encoding="utf-8")
    require(process.returncode == 0,
            f"{label}: runner exited {process.returncode}")
    for marker in BAD_MARKERS:
        require(marker not in output, f"{label}: emitted {marker}")

    # Without this the WebGPU arms below could silently be a second pair of GL
    # arms -- an unavailable adapter falling back would assert nothing about
    # the backend the game actually ships on, which is the whole point of them.
    require(f"renderer backend: {renderer}" in output,
            f"{label}: asked for the {renderer} backend and did not get it")

    frame = frames / f"frame_{CAPTURE_FRAME}.ppm"
    require(frame.is_file(), f"{label}: no capture at frame {CAPTURE_FRAME}")
    return frame, output, dump


def assert_reached_card(label: str, character: str, output: str) -> None:
    """Assertion 1: player 0 really reached this character's own card."""
    identity = PORTRAITS[character][2]
    owned = (f"bonus_results_portrait: player=0 identity={identity} "
             "source=native-card")
    require(owned in output,
            f"{label}: player 0 did not reach {character}'s result card "
            f"({owned!r} absent)")
    sized = ("taj_portrait: source=native-taj-card size=40x40 retail=40x40"
             if character == "taj" else
             f"bonus_portrait: identity={identity} source=native-card "
             "size=40x40 retail=40x40")
    require(sized in output,
            f"{label}: the card was not composed at retail size "
            f"({sized!r} absent)")


def assert_corpus_publishes(label: str, dump: Path, digest: str) -> None:
    """Assertion 2: the author dump names the portrait like any texture."""
    sidecar = dump / f"{digest}.txt"
    if not sidecar.is_file():
        found = sorted(
            path.stem for path in dump.glob("*.txt")
            if "width=40\nheight=40\n" in
            path.read_text(encoding="utf-8", errors="replace"))
        raise CheckError(
            f"{label}: MDKR_MOD_TEXTURE_DUMP did not publish {digest}. "
            "The generated portrait art has changed: update the current name "
            "and prove the previously published pack name remains an alias. "
            f"40x40 digests this route did dump: {found or 'none'}")
    require((dump / f"{digest}.png").is_file(),
            f"{label}: {digest} has a sidecar but no PNG")
    text = sidecar.read_text(encoding="utf-8", errors="replace")
    # Anchored, like the search above: unanchored, "width=40" is also a
    # substring of width=400 and "height=40" of height=408, so a differently
    # sized texture that happened to collide with this digest would read as
    # the card.
    require("width=40\nheight=40\n" in text,
            f"{label}: {digest} is not the 40x40 card: {text!r}")


def assert_override_dump(label: str, dump: Path, digest: str) -> None:
    """Assertion 6: the dump wrote the OVERRIDE, whole and at its own size.

    The regression this pins: the renderer used to hand the dump the pack's
    64x64 buffer together with the tile's LOGICAL 40x40 size, because the two
    live in the same `uw`/`uh` pair by that point and the second one had been
    reset for texcoord normalisation (issue #34). stb encoded 40x40 at a
    160-byte stride out of a 256-byte-stride image: in bounds, so no crash and
    no sanitizer report, and only reachable with a pack installed AND the dump
    on -- neither the corpus arms (no pack) nor the pack arms (no dump) could
    see it, which is exactly how it survived being written.

    Dimensions alone would not catch a re-introduction cleanly, so the pixels
    are compared: the dumped image must equal the pack's own PNG, decoded.
    """
    expected_w, expected_h, expected = decode_png(quadrant_png(PACK_SIZE))
    # The decoder is checked before it is trusted: a decoder that returned the
    # same wrong answer for both files would make the comparison below vacuous.
    require((expected_w, expected_h) == (PACK_SIZE, PACK_SIZE) and
            len(expected) == PACK_SIZE * PACK_SIZE * 4,
            f"{label}: this file's own PNG reader is broken "
            f"({expected_w}x{expected_h}, {len(expected)} bytes)")
    corner = expected[0:4]
    far = expected[((PACK_SIZE - 1) * PACK_SIZE + PACK_SIZE - 1) * 4:][:4]
    require(bytes(corner) == bytes(PACK_RGBA) and
            bytes(far) == bytes(PACK_RGBA_ALT),
            f"{label}: this file's own PNG reader does not round-trip the "
            f"quadrant (corner={tuple(corner)}, far={tuple(far)})")

    written = dump / f"{digest}.png"
    require(written.is_file(), f"{label}: the dump wrote no {digest}.png")
    width, height, actual = decode_png(written.read_bytes())
    require((width, height) == (PACK_SIZE, PACK_SIZE),
            f"{label}: the dump wrote the override at {width}x{height}, not "
            f"the replacement's own {PACK_SIZE}x{PACK_SIZE}; the logical tile "
            "size reached the encoder and the image is sheared")
    require(actual == expected,
            f"{label}: the dumped override is not the pack's own image "
            f"({sum(a != b for a, b in zip(actual, expected))} of "
            f"{len(expected)} bytes differ)")

    sidecar = (dump / f"{digest}.txt").read_text(encoding="utf-8",
                                                 errors="replace")
    require(f"width={PACK_SIZE}\nheight={PACK_SIZE}\n" in sidecar,
            f"{label}: the sidecar disagrees with the PNG it describes: "
            f"{sidecar!r}")


def assert_pack_drew_card(label: str, frame: Path) -> None:
    """Assertions 3 and 4: the card is the pack image, and nothing else is."""
    magenta, green, total = classify(frame, CARD_BOUNDS)
    coverage = (magenta + green) / total
    require(coverage >= MIN_PACK_COVERAGE,
            f"{label}: the pack did not draw the card (pack-colour coverage "
            f"{coverage:.1%}, need {MIN_PACK_COVERAGE:.1%})")
    share = green / (magenta + green)
    require(MIN_GREEN_SHARE <= share <= MAX_GREEN_SHARE,
            f"{label}: the card shows part of the pack image rather than all "
            f"of it (green share {share:.1%}, expected between "
            f"{MIN_GREEN_SHARE:.0%} and {MAX_GREEN_SHARE:.0%}); a replacement "
            "is the unpadded picture at any scale, so the whole 64x64 quadrant "
            "belongs on the whole 40x40 card")
    assert_no_pack_beside(label, frame)


def assert_no_pack_beside(label: str, frame: Path) -> None:
    magenta, green, _ = classify(frame, BESIDE_BOUNDS)
    require(magenta == 0 and green == 0,
            f"{label}: pack colours reached the screen beside the card "
            f"(magenta={magenta}, green={green}); this arm proves the card "
            "specifically, not the whole frame")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--evidence-dir")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom, SCRIPT):
        if not path.is_file():
            print(f"check_bonus_portrait_pack: FAIL -- missing {path}",
                  file=sys.stderr)
            return 1

    temporary = args.evidence_dir is None
    root = (Path(tempfile.mkdtemp(prefix="mdkr-portrait-pack-"))
            if temporary else Path(args.evidence_dir))
    root.mkdir(parents=True, exist_ok=True)
    try:
        # Baseline first: it is the no-pack reference assertion 5 compares
        # against, and the corpus assertion 2 reads.
        baseline, output, corpus = run_arm(
            binary, rom, root, "baseline", "taj", None, True, True,
            args.timeout, args.verbose)
        assert_reached_card("baseline", "taj", output)
        require("[MODS]" not in output,
                "baseline: a pack was found in an arm that installs none")
        assert_corpus_publishes("baseline", corpus, TAJ_DIGEST)
        assert_no_pack_beside("baseline", baseline)
        magenta, green, total = classify(baseline, CARD_BOUNDS)
        require(green == 0,
                f"baseline: the generated Taj card already reads green-family "
                f"({green}/{total}); green is this gate's pack witness and "
                "must be absent from the stock cards")
        # Not an assertion, and deliberately so: magenta-family is NOT a
        # witness here. Taj's own purple turban reads magenta-family on about a
        # quarter of the stock card, which is exactly why the pack arms measure
        # green share and total coverage instead of magenta. Recorded so a
        # later reader does not "tighten" this into a magenta floor that Taj
        # would satisfy with no pack installed at all.
        require(magenta > 0,
                "baseline: the generated Taj card no longer reads any "
                "magenta-family pixel, so the note above about why magenta "
                "cannot be a witness has gone stale and the pack arms should "
                "be revisited")
        baseline_bytes = baseline.read_bytes()

        for character, (_, digest, _identity) in PORTRAITS.items():
            # The Taj arm carries the dump as well, so the override-dump
            # assertion costs no extra run: a pack installed AND the dump on is
            # the one combination that reaches the sheared-encode path, and it
            # is a combination no other arm and no other gate produces.
            wants_corpus = character == "taj"
            frame, output, dump = run_arm(
                binary, rom, root, character, character, digest, True,
                wants_corpus, args.timeout, args.verbose)
            assert_reached_card(character, character, output)
            require("[MODS]   active: Portrait Test (priority 100)" in output,
                    f"{character}: the pack did not install")
            assert_pack_drew_card(character, frame)
            if wants_corpus:
                assert_override_dump(character, dump, digest)

        frame, output, dump = run_arm(
            binary, rom, root, "taj-legacy", "taj", TAJ_LEGACY_DIGEST, True,
            True, args.timeout, args.verbose)
        assert_reached_card("taj-legacy", "taj", output)
        assert_pack_drew_card("taj-legacy", frame)
        # The author dump names current source content even when the installed
        # replacement was discovered through a historical-name alias.
        assert_override_dump("taj-legacy", dump, TAJ_DIGEST)

        # The positive control: the identical pack, switched off by its own
        # pack.ini, must give back the generated card exactly.
        frame, output, _ = run_arm(
            binary, rom, root, "disabled", "taj", TAJ_DIGEST, False, False,
            args.timeout, args.verbose)
        assert_reached_card("disabled", "taj", output)
        require("[MODS]   skipped: Portrait Test - its pack.ini sets "
                "enabled = 0" in output,
                "disabled: the pack was not reported skipped for its own "
                "pack.ini; this arm would then be measuring nothing")
        _, green, _ = classify(frame, CARD_BOUNDS)
        require(green == 0,
                f"disabled: pack pixels survived a pack.ini that switched the "
                f"pack off (green-family {green})")
        require(frame.read_bytes() == baseline_bytes,
                "disabled: switching the pack off did not restore the "
                "generated card exactly (frame "
                f"{hashlib.sha256(frame.read_bytes()).hexdigest()[:16]} vs "
                f"baseline {hashlib.sha256(baseline_bytes).hexdigest()[:16]})")

        # WebGPU is the shipped default renderer, so the two claims this file
        # makes have to hold there and not only on the GL arms above. They stay
        # two runs rather than one folded arm: a dump taken with a pack
        # installed correctly records the REPLACEMENT (assertion 6 is what
        # says so), so it cannot also show what an author dumping a fresh
        # corpus sees, which is the whole of what webgpu-baseline claims.
        #
        # Frames are NOT compared across renderers: two backends need not
        # rasterize the same bytes, and nothing here claims they do. What is
        # claimed is the digest -- which is a function of the generated texels
        # and the tile header, both decided before either backend sees them.
        _, output, corpus = run_arm(
            binary, rom, root, "webgpu-baseline", "taj", None, True, True,
            args.timeout, args.verbose, renderer="webgpu")
        assert_reached_card("webgpu-baseline", "taj", output)
        assert_corpus_publishes("webgpu-baseline", corpus, TAJ_DIGEST)

        frame, output, _ = run_arm(
            binary, rom, root, "webgpu-taj", "taj", TAJ_DIGEST, True, False,
            args.timeout, args.verbose, renderer="webgpu")
        assert_reached_card("webgpu-taj", "taj", output)
        require("[MODS]   active: Portrait Test (priority 100)" in output,
                "webgpu-taj: the pack did not install")
        assert_pack_drew_card("webgpu-taj", frame)
        frame, output, _ = run_arm(
            binary, rom, root, "webgpu-taj-legacy", "taj", TAJ_LEGACY_DIGEST,
            True, False, args.timeout, args.verbose, renderer="webgpu")
        assert_reached_card("webgpu-taj-legacy", "taj", output)
        assert_pack_drew_card("webgpu-taj-legacy", frame)
    except (OSError, ValueError, subprocess.TimeoutExpired,
            CheckError) as error:
        print(f"check_bonus_portrait_pack: FAIL -- {error}", file=sys.stderr)
        print(f"evidence: {root}", file=sys.stderr)
        return 1

    print("check_bonus_portrait_pack: PASS -- a pack redrew the generated Taj, "
          "Wizpig and Terry cards at their published digests on GL and on the "
          "shipped WebGPU default, the author dump publishes those digests, "
          "pre-1.7 Taj packs remain compatible on both backends, "
          "and switching the pack off restored the generated cards "
          "byte-for-byte")
    if args.evidence_dir:
        print(f"evidence: {root}")
    if temporary:
        shutil.rmtree(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
