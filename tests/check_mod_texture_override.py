#!/usr/bin/env python3
"""A pack's textures replace the ROM's, and the settings switch applies while you play.

Why this exists
---------------
`Content.PacksEnabled` is declared `MDKR_VIDEO_SCOPE_LIVE`, which is a promise
to the player: tick the "Custom content" box and the picture changes, no
relaunch. For a long time nothing kept that promise. The key was read exactly
once, by `platform_content_packs_init()` at boot, and no publisher ever carried
it anywhere afterwards -- so the settings checkbox took effect on the NEXT
launch while `Tab`, which pulls the identical lever
(`mdkr_mod_texture_set_enabled`), changed the picture instantly. Two controls,
one lever, two different answers, and the slow one was the one that looked
authoritative.

`mdkr_video_config_publish()` now carries the key to that lever, and this gate
is what makes the promise checkable rather than argued. It is also the only
thing standing between a future refactor and a checkbox that silently does
nothing again, which is a defect no crash, no log line and no unit test would
ever surface.

WHY BOTH DIRECTIONS ARE LIVE, WHICH WAS NOT OBVIOUS. If the pack scan were
skipped when the setting starts at 0, off->on would have nothing to switch on
and would have had to stay restart-required. It is not skipped:
`platform_content_packs_init()` scans `mods/`, applies `Content.PackDisabled`,
and binds the decoded-texture store unconditionally -- the setting only decides
whether the store ANSWERS. Arm 3 below is the observation that proves it, not
an inference from reading the source.

WHAT THIS CANNOT DRIVE, STATED PLAINLY. `Tab` is an SDL keyboard event, and a
headless run has no keyboard; the input-script fixture format carries
controller tokens only. So there is no `Tab` arm here. Tab and the setting call
the same one-line lever, and the rule between them -- an explicit settings
change wins and sets the lever to whatever the setting says, ending any
momentary comparison -- is a property of the publication point, documented
there and asserted by the ROM-free `video_config_runtime` unit test in both
directions. What is NOT claimed anywhere is that this file tests the keypress.

The route
---------
Boot with no input script, 380 frames, 320x240, GL. Textures first bind at
frame 9 and the title screen's set at frame 90, so the whole measurement sits
inside the cheapest part of the game to reach. Four runs:

    corpus     no pack, MDKR_MOD_TEXTURE_DUMP -- writes <digest>.png for every
               texture the route binds. This is the pack author's own workflow
               (tools/mod_texture_dump.py, docs/MODDING.md) and the pack below
               is built from its output, so the digest contract is exercised
               end to end rather than pinned to a constant that could rot.
    baseline   no pack at all
    pack       every dumped digest overridden with solid magenta
    live       the same pack, plus two runtime settings changes

Nothing ROM-derived is written into the repository: every run has its own
throwaway working directory, which is also where `mods/` and the dumped corpus
live.

The pack overrides EVERY digest the corpus run found, with one 4x4 magenta PNG
this file encodes itself. Overriding everything rather than one hand-picked
texture is what lets the frames below be compared as whole-image hashes: the
question "did any pack pixel survive" then has a yes/no answer at every frame,
instead of depending on whether one chosen texture happened to be on screen.
The size mismatch is deliberate too -- UVs are normalised against the uploaded
size, so a 4x4 override standing in for a 64x32 ROM tile is the same code path
a real pack takes at a higher resolution.

The live arm's schedule, and why the frames are where they are
--------------------------------------------------------------
    MDKR_TEST_SETTINGS_TOGGLE=Content.PacksEnabled=0@150,Content.PacksEnabled=1@320

That is the existing headless seam for the live-settings path
(platform_sdl_min.c), and it fires `mdkr_video_config_runtime_set()` -- the
exact call the settings panel makes. Not a rewritten ini and a relaunch, which
would prove only that the boot path still reads the file.

Frames are sampled at 120, 240 and 360 (MDKR_DUMP_FROM=120, MDKR_DUMP_EVERY=120)
-- one in each of the three states, each at least 30 frames clear of a
transition. Measured on the reference run (US v80), the change lands on the
frame the toggle names, with no smearing in either direction: frames 91..149
carry pack pixels, 150..319 are byte-identical to the no-pack baseline, and
320 onward carry pack pixels again.

What is asserted
----------------
 1. REACHABILITY, AND NOTHING VACUOUS. Every run exits 0 with no runtime
    diagnostic; the corpus run dumps textures; the pack and live runs report
    the pack active and the baseline run reports no pack at all; both scheduled
    settings changes are logged as applied (a key pinned by the environment
    would resolve LOCKED here and every assertion below would be measuring
    nothing).

 2. A PACK CHANGES THE PICTURE. At all three sampled frames the pack run's
    image differs from the baseline's. This is what makes assertion 3 mean
    something: "identical to the baseline" is only evidence if the pack arm
    is not.

 3. THE SETTINGS CHANGE APPLIES LIVE, AND OFF MEANS THE ROM'S OWN TEXELS.
    Frame 120, before the first change, is byte-identical to the pack run.
    Frame 240, after Content.PacksEnabled=0 was set THROUGH THE SETTINGS
    ROUTE, is byte-identical to the NO-PACK BASELINE.

    Byte-identical is the whole assertion. A merely "different" frame would
    also be produced by a store that stopped answering while the texture cache
    kept serving the pack pixels it had already uploaded; only an exact match
    with the baseline proves the cache retired those entries and re-uploaded
    the ROM's texels, which is what `override_generation` exists to do.

 4. AND IT COMES BACK. Frame 360, after Content.PacksEnabled=1 was set at
    runtime, is byte-identical to the pack run again -- so the registry really
    was scanned despite the switch having been off, and the flip costs a cache
    generation rather than a rescan. This is the arm that would have had to
    become "the panel says restart-required" had the scan been conditional.

 5. IT IS PRESENTATION ONLY. The `[SIMHASH]` v3 stream is byte-identical across
    all three arms, all 380 rows. Content.PacksEnabled is content class
    (`mdkr_video_key_is_content`, folded into `mdkr_video_key_is_player_comfort`)
    and toggling it mid-run must not move one bit of authoritative state.

Self-validation -- this check is proven to be able to fail
-----------------------------------------------------------
POSITIVE CONTROL (run by hand, not by this file): comment out the two receiver
calls in `mdkr_video_config_publish()` (platform/video_config_runtime.c), which
restores the exact defect this gate exists for -- the key is still declared
LIVE, the setter still returns LIVE, the log still says applied=1, and nothing
happens. Rebuild and re-run. The check exits 1 at assertion 3:

    check_mod_texture_override: FAIL - frame 240: Content.PacksEnabled=0 was
    applied at runtime, but the frame is not the no-pack baseline (live
    <hash> vs baseline <hash>); it is still the pack's image

Assertion 4 fails on the same build for the mirrored reason. Assertions 1, 2
and 5 keep passing, honestly: the pack is still installed and still applied, and
a setting that does nothing moves no state either.

Always muted + headless (`MDKR_AUDIO=0` and `--headless-frames`), per
tests/README.md. `MDKR_AUDIO=off` would be a silent no-op -- only the digit `0`
disables. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

# ---- the route -------------------------------------------------------------
FRAMES = 380
WINDOW = "320x240"

# Sampled frames, one per state. MDKR_DUMP_FROM/MDKR_DUMP_EVERY can only express
# an arithmetic progression, so the three are evenly spaced and the toggle ticks
# below are placed between them rather than the other way round.
DUMP_FROM = 120
DUMP_EVERY = 120
FRAME_PACK_ON = 120       # before the first change
FRAME_PACK_OFF = 240      # after Content.PacksEnabled=0
FRAME_PACK_BACK = 360     # after Content.PacksEnabled=1
SAMPLED = (FRAME_PACK_ON, FRAME_PACK_OFF, FRAME_PACK_BACK)

# Toggle ticks. Each sits at least 30 frames clear of a sampled frame in both
# directions, so a tick/frame skew this route does not currently have would
# still not move which state a sample belongs to.
TICK_OFF = 150
TICK_ON = 320

PACK_INI = b"[pack]\nname=Texture Test\npriority=100\n"
PACK_RGBA = (255, 0, 255, 255)     # magenta: nothing the ROM draws looks like it
PACK_SIZE = 4

BAD_MARKERS = ("[FATAL]", "[CRASH]", "AddressSanitizer",
               "UndefinedBehaviorSanitizer", "runtime error:")


class CheckError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckError(message)


# ==========================================================================
# the pack this check authors for itself
# ==========================================================================
def solid_png(rgba: tuple[int, int, int, int], size: int) -> bytes:
    """A solid RGBA8 PNG, encoded here from the standard library alone.

    No ROM bytes reach this function. The override pixels are a constant this
    file names, which is the only reason "the frame is showing the pack" can be
    distinguished from "the frame is showing something else that also changed".
    """
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + bytes(rgba) * size for _ in range(size))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def install_pack(run_dir: Path, digests: list[str]) -> None:
    """Writes `mods/TexturePack/textures/<digest>.png` for every digest given."""
    textures = run_dir / "mods" / "TexturePack" / "textures"
    textures.mkdir(parents=True)
    (run_dir / "mods" / "TexturePack" / "pack.ini").write_bytes(PACK_INI)
    blob = solid_png(PACK_RGBA, PACK_SIZE)
    for digest in digests:
        (textures / f"{digest}.png").write_bytes(blob)


# ==========================================================================
# capture
# ==========================================================================
class Arm:
    def __init__(self, label: str, packed: bool, toggles: str = "",
                 corpus: bool = False):
        self.label = label
        self.packed = packed
        self.toggles = toggles
        self.corpus = corpus
        self.frames: dict[int, str] = {}
        self.rows: list[str] = []
        self.mods: list[str] = []
        self.applied: list[tuple[str, str, int]] = []


def run_arm(binary: Path, rom: Path, root: Path, arm: Arm, digests: list[str],
            timeout: int, verbose: bool) -> None:
    """One headless run. NEVER without --headless-frames; MDKR_AUDIO=0 on top."""
    run_dir = root / arm.label
    (run_dir / "save").mkdir(parents=True)
    frame_dir = run_dir / "frames"
    frame_dir.mkdir()
    corpus_dir = run_dir / "corpus"
    if arm.packed:
        install_pack(run_dir, digests)

    # A clean environment. In particular no MDKR_CONTENT_PACKS: that is
    # Content.PacksEnabled's env name, and an inherited one would pin the key
    # above RUNTIME rank, so the live arm's changes would resolve LOCKED and
    # this whole file would pass by measuring nothing.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_RENDERER="gl",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(run_dir / "save"),
        # The runtime setter PERSISTS. Without a private path the live arm would
        # rewrite the developer's real settings file.
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "mdkr64.ini"),
        MDKR_STATE_HASH="3",
        MDKR_DUMP_FROM=str(DUMP_FROM),
        MDKR_DUMP_EVERY=str(DUMP_EVERY),
    )
    if arm.corpus:
        corpus_dir.mkdir()
        env["MDKR_MOD_TEXTURE_DUMP"] = str(corpus_dir)
    if arm.toggles:
        env["MDKR_TEST_SETTINGS_TOGGLE"] = arm.toggles

    command = [str(binary), "--headless-frames", str(FRAMES),
               "--rom", str(rom), "--window-size", WINDOW,
               "--dump-frames", str(frame_dir)]
    if verbose:
        print(f"$ ({arm.label}) MDKR_TEST_SETTINGS_TOGGLE={arm.toggles} "
              + " ".join(command), flush=True)
    process = subprocess.run(
        command, cwd=str(run_dir), env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False)
    output = process.stdout
    require(process.returncode == 0,
            f"{arm.label}: engine exited {process.returncode}\n{output[-4000:]}")
    for marker in BAD_MARKERS:
        require(marker not in output,
                f"{arm.label}: runtime diagnostic {marker!r}\n{output[-4000:]}")

    for line in output.splitlines():
        if line.startswith("[SIMHASH]"):
            arm.rows.append(line)
        elif "[MODS]" in line:
            arm.mods.append(line)
        elif line.startswith("[SETTINGS-TOGGLE]"):
            fields = dict(part.split("=", 1) for part in line.split()
                          if "=" in part)
            arm.applied.append((fields.get("key", "?"),
                                fields.get("value", "?"),
                                int(fields.get("applied", "0"))))

    for frame in SAMPLED:
        path = frame_dir / f"frame_{frame:04d}.ppm"
        require(path.is_file(),
                f"{arm.label}: the route never dumped frame {frame}")
        arm.frames[frame] = hashlib.sha256(path.read_bytes()).hexdigest()

    if arm.corpus:
        digests.extend(sorted(entry.stem
                              for entry in corpus_dir.glob("*.png")))


# ==========================================================================
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR,
                        help="native build directory or mdkr64 executable")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    require(binary.is_file(), f"build executable not found: {binary}")
    require(rom.is_file(), f"ROM not found: {rom}")

    toggles = (f"Content.PacksEnabled=0@{TICK_OFF},"
               f"Content.PacksEnabled=1@{TICK_ON}")
    corpus = Arm("corpus", packed=False, corpus=True)
    baseline = Arm("baseline", packed=False)
    pack = Arm("pack", packed=True)
    live = Arm("live", packed=True, toggles=toggles)
    digests: list[str] = []

    print("1. four headless runs; the pack is built from the corpus the first "
          "one dumps")
    with tempfile.TemporaryDirectory(prefix="mdkr_mod_texture_") as tmp:
        root = Path(tmp)
        run_arm(binary, rom, root, corpus, digests, args.timeout, args.verbose)
        require(digests,
                "the corpus run dumped no textures, so there is nothing to "
                "override and every assertion below would be vacuous")
        for arm in (baseline, pack, live):
            run_arm(binary, rom, root, arm, digests, args.timeout,
                    args.verbose)

    require(not baseline.mods,
            "the baseline run found content packs; its mods/ is not empty\n"
            + "\n".join(baseline.mods))
    for arm in (pack, live):
        require(any("1 pack(s) active" in line for line in arm.mods),
                f"{arm.label}: the pack was not discovered as active\n"
                + "\n".join(arm.mods))
    require(not pack.applied,
            "the pack run changed a setting at runtime; it is supposed to be "
            "the unchanging comparison")
    expected = [("Content.PacksEnabled", "0", 1), ("Content.PacksEnabled", "1", 1)]
    require(live.applied == expected,
            "the live run's scheduled settings changes did not both apply "
            f"(a locked key applies nothing): {live.applied}")
    print(f"   ok  {len(digests)} texture(s) overridden; pack active in 2 runs; "
          "both settings changes applied")

    print("2. a pack changes the picture at every sampled frame")
    for frame in SAMPLED:
        require(pack.frames[frame] != baseline.frames[frame],
                f"frame {frame}: the pack run is byte-identical to the no-pack "
                "baseline, so this frame draws none of the overridden textures "
                "and proves nothing below")
    print("   ok  frames " + ", ".join(str(f) for f in SAMPLED)
          + " all differ from the baseline")

    print("3. Content.PacksEnabled=0 at runtime restores the ROM's own texels")
    require(live.frames[FRAME_PACK_ON] == pack.frames[FRAME_PACK_ON],
            f"frame {FRAME_PACK_ON}: before any settings change the live run "
            "should be showing the pack, but it does not match the pack run "
            f"(live {live.frames[FRAME_PACK_ON][:16]} vs pack "
            f"{pack.frames[FRAME_PACK_ON][:16]})")
    require(live.frames[FRAME_PACK_OFF] == baseline.frames[FRAME_PACK_OFF],
            f"frame {FRAME_PACK_OFF}: Content.PacksEnabled=0 was applied at "
            "runtime, but the frame is not the no-pack baseline (live "
            f"{live.frames[FRAME_PACK_OFF][:16]} vs baseline "
            f"{baseline.frames[FRAME_PACK_OFF][:16]}); it is still the pack's "
            "image")
    print(f"   ok  frame {FRAME_PACK_ON} == the pack run; frame "
          f"{FRAME_PACK_OFF} == the no-pack baseline, byte for byte")

    print("4. and Content.PacksEnabled=1 brings it back without a relaunch")
    require(live.frames[FRAME_PACK_BACK] == pack.frames[FRAME_PACK_BACK],
            f"frame {FRAME_PACK_BACK}: Content.PacksEnabled=1 was applied at "
            "runtime, but the pack's textures did not come back (live "
            f"{live.frames[FRAME_PACK_BACK][:16]} vs pack "
            f"{pack.frames[FRAME_PACK_BACK][:16]})")
    print(f"   ok  frame {FRAME_PACK_BACK} == the pack run again; the registry "
          "survived the switch being off")

    print("5. presentation only: no authoritative state moved")
    require(len(baseline.rows) == FRAMES,
            f"expected {FRAMES} [SIMHASH] rows, got {len(baseline.rows)} - the "
            "v3 stream is not being emitted")
    for arm in (pack, live):
        require(arm.rows == baseline.rows,
                f"{arm.label}: the [SIMHASH] v3 stream differs from the "
                "baseline's - content packs are NOT presentation-only")
    print(f"   ok  {len(baseline.rows)} [SIMHASH] rows identical across all "
          "three arms")

    print("check_mod_texture_override: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckError, OSError, subprocess.TimeoutExpired) as error:
        print(f"check_mod_texture_override: FAIL - {error}", file=sys.stderr)
        raise SystemExit(1)
