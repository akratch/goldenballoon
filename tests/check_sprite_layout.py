#!/usr/bin/env python3
"""Independently prove the sprite-builder allocation contract against the ROM.

The runtime stages each serialized SpriteAsset in a 512-byte buffer and builds
one display list per frame. Historically the allocation used ``4*T + F`` Gfx
commands (T textures, F frames), although the writer actually emits::

    2*F + 2*T + sum(ceil(frame_tile_count / 5))

Empty frames make those formulae diverge. This check decodes the ROM directly,
validates every flexible frame-offset tail, and keeps the two affected US/PAL
v80 assets as positive controls. It deliberately shares no C layout code with
the runtime, so agreement is independent evidence rather than a tautology.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SPRITE_SOURCE = ROOT / "game" / "src" / "textures_sprites.c"
ROM_LAYOUTS = {
    (0xE402430D, 0xD2FCFC9D): ("us.v80", 0x000ED0E0, 0x000ED1B0),
    (0x596E145B, 0xF7D9879F): ("pal.v80", 0x000ED170, 0x000ED240),
}
ASSET_SPRITES = 12
ASSET_SPRITES_TABLE = 13
SPRITE_HEADER_SIZE = 12
MAX_STAGED_SIZE = 512
EXPECTED_UNDERRUNS = {
    162: (27, 29, (0, 0, 1, 2, 3, 4, 5, 5)),
    177: (66, 67, (0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13)),
}

# The complete G_VTX_APPEND blast radius of the supported ROMs: the only sprite
# frames that emit more than one appended vertex run, as
# {sprite_id: ((frame, tile_count, run_count), ...)}.
#
# Both revision-1 ROMs carry identical asset payloads, so this holds for PAL as
# well.  Of 600 frames across 193 sprites, only these two exceed five tiles:
#
#   sprite 108 frame 0 -- a one-frame, six-band RGBA16 billboard (base texture
#     446, anchor (80,122); bands 60x26@y9, 92x21@y34, 120x17@y54, 108x18@y70,
#     96x21@y87, 52x15@y107).  These are the intro shrubs of GitHub issue #11;
#     the sixth band is the bottom one that the pre-e14f6ee append base dropped.
#     Reached everywhere -- it loads on every one of 27 swept route runs.
#   sprite 178 frame 6 -- one frame of an eleven-frame RGBA32 burst animation
#     (base texture 745, anchor (32,18)) that breaks into six fragments before
#     fading.  Referenced by exactly one object header; not observed on any
#     swept route.
EXPECTED_MULTIRUN = {
    108: ((0, 6, 2),),
    178: ((6, 6, 2),),
}

REQUIRED_SOURCE_COUNTS = {
    "load_sprite_asset_checked(": 5,  # definition plus four readers
    "asset_load(ASSET_SPRITES": 1,
    "sprite_build_layout(": 1,
    "sprite_init_frame(spriteAsset, sprite, i)": 1,
    "tex_free(sprite->textures[i]);": 1,
    "mempool_free(sprite);": 3,
}
FORBIDDEN_LEGACY_LAYOUT = (
    "allocSize += numTextures * 4 * sizeof(Vertex);",
    "gSpriteDLists + numTextures * 0x20",
)


def source_contract(source: str) -> list[str]:
    failures: list[str] = []
    for fragment, expected in REQUIRED_SOURCE_COUNTS.items():
        actual = source.count(fragment)
        if actual != expected:
            failures.append(
                f"{fragment!r}: found {actual}, expected {expected}"
            )
    for fragment in FORBIDDEN_LEGACY_LAYOUT:
        if fragment in source:
            failures.append(f"legacy allocation fragment returned: {fragment!r}")
    return failures


def prove_source_contract(source: str) -> list[str]:
    """Prove each census assertion has an executable failure direction."""
    failures = source_contract(source)
    if failures:
        return failures
    for fragment in REQUIRED_SOURCE_COUNTS:
        mutated = source.replace(fragment, "", 1)
        if not source_contract(mutated):
            failures.append(
                f"source assertion is inert after removing {fragment!r}"
            )
    for fragment in FORBIDDEN_LEGACY_LAYOUT:
        if not source_contract(source + "\n" + fragment):
            failures.append(
                f"legacy-layout rejection is inert for {fragment!r}"
            )
    return failures


def normalize_rom(raw: bytes) -> bytes:
    if len(raw) < 0x40:
        raise ValueError("ROM is shorter than its N64 header")
    magic = raw[:4]
    if magic == b"\x80\x37\x12\x40":
        return raw
    if magic == b"\x37\x80\x40\x12":
        if len(raw) % 2:
            raise ValueError("byte-swapped ROM length is not even")
        out = bytearray(raw)
        for offset in range(0, len(out), 2):
            out[offset], out[offset + 1] = out[offset + 1], out[offset]
        return bytes(out)
    if magic == b"\x40\x12\x37\x80":
        if len(raw) % 4:
            raise ValueError("word-swapped ROM length is not a multiple of four")
        out = bytearray(raw)
        for offset in range(0, len(out), 4):
            out[offset:offset + 4] = reversed(out[offset:offset + 4])
        return bytes(out)
    raise ValueError(f"unrecognized N64 byte order ({magic.hex()})")


def be32(blob: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(blob):
        raise ValueError(f"u32 read at 0x{offset:x} exceeds ROM")
    return struct.unpack_from(">I", blob, offset)[0]


def section(rom: bytes, lut: int, base: int, index: int) -> bytes:
    start = be32(rom, lut + 4 * (index + 1))
    end = be32(rom, lut + 4 * (index + 2))
    if end < start or base + end > len(rom):
        raise ValueError(
            f"asset section {index} has invalid extent 0x{start:x}..0x{end:x}"
        )
    return rom[base + start:base + end]


def signed_table(raw: bytes) -> list[int]:
    if len(raw) % 4:
        raise ValueError("sprite offset table length is not word-aligned")
    values = list(struct.unpack(f">{len(raw) // 4}i", raw))
    try:
        sentinel = values.index(-1)
    except ValueError as exc:
        raise ValueError("sprite offset table has no -1 sentinel") from exc
    # The runtime's count excludes the terminal end offset immediately before
    # the sentinel. A table [start_0, ..., end_N, -1] therefore has N assets.
    if sentinel < 2:
        raise ValueError("sprite offset table contains no assets")
    return values[:sentinel]


def multirun_frames(offsets: tuple[int, ...]) -> tuple[tuple[int, int, int], ...]:
    """Frames that emit MORE THAN ONE appended vertex run.

    sprite_init_frame() issues one gSPVertexDKR(..., G_VTX_APPEND) per group of
    at most five tiles and restarts its triangle indices at vertex 1 for each
    group.  That is only correct because G_VTX_APPEND is a fixed base (the
    length of the last flag-0 load) and not a running cursor -- see
    game/include/f3ddkr.h and tests/check_intro_shrub_sprite.py.  A frame with
    six or more tiles is therefore the ONLY shape that can expose an append-base
    regression, so the set of such frames is the exact blast radius and is
    pinned here.

    Returns (frame_index, tile_count, run_count) for each affected frame.
    """
    return tuple(
        (frame, end - start, (end - start + 4) // 5)
        for frame, (start, end) in enumerate(zip(offsets, offsets[1:]))
        if end - start > 5
    )


def prove_multirun_classifier() -> list[str]:
    """The multi-run classifier must have an executable failure direction."""
    failures: list[str] = []
    # Five tiles is exactly one run; six is the first shape that appends twice.
    if multirun_frames((0, 5)) != ():
        failures.append("a five-tile frame was reported as multi-run")
    if multirun_frames((0, 6)) != ((0, 6, 2),):
        failures.append("a six-tile frame was not reported as multi-run")
    if multirun_frames((0, 1, 12)) != ((1, 11, 3),):
        failures.append("an eleven-tile frame did not report three runs")
    if multirun_frames((0,)) != ():
        failures.append("a frameless sprite produced a multi-run frame")
    return failures


def command_count(offsets: tuple[int, ...]) -> int:
    return sum(
        2 + 2 * (end - start) + ((end - start) + 4) // 5
        for start, end in zip(offsets, offsets[1:])
    )


def census(rom_path: Path) -> tuple[
    str, int, int,
    dict[int, tuple[int, int, tuple[int, ...]]],
    dict[int, tuple[tuple[int, int, int], ...]],
]:
    rom = normalize_rom(rom_path.read_bytes())
    identity = (be32(rom, 0x10), be32(rom, 0x14))
    try:
        build, lut, base = ROM_LAYOUTS[identity]
    except KeyError as exc:
        raise ValueError(
            "unsupported ROM identity "
            f"CRC1/CRC2={identity[0]:08X}/{identity[1]:08X}"
        ) from exc

    sprite_data = section(rom, lut, base, ASSET_SPRITES)
    offsets = signed_table(section(rom, lut, base, ASSET_SPRITES_TABLE))
    failures: list[str] = []
    underruns: dict[int, tuple[int, int, tuple[int, ...]]] = {}
    multirun: dict[int, tuple[tuple[int, int, int], ...]] = {}
    max_size = 0

    for sprite_id, (start, end) in enumerate(zip(offsets, offsets[1:])):
        size = end - start
        max_size = max(max_size, size)
        if start < 0 or end < start or end > len(sprite_data):
            failures.append(
                f"sprite {sprite_id}: invalid extent {start}..{end} "
                f"within {len(sprite_data)} bytes"
            )
            continue
        if size > MAX_STAGED_SIZE:
            failures.append(
                f"sprite {sprite_id}: {size} bytes exceeds "
                f"{MAX_STAGED_SIZE}-byte staging buffer"
            )
            continue
        blob = sprite_data[start:end]
        if len(blob) < SPRITE_HEADER_SIZE + 2:
            failures.append(f"sprite {sprite_id}: truncated header/flexible tail")
            continue
        frames = struct.unpack_from(">h", blob, 2)[0]
        if frames <= 0:
            failures.append(f"sprite {sprite_id}: invalid frame count {frames}")
            continue
        tail_end = SPRITE_HEADER_SIZE + frames + 1
        if tail_end > len(blob):
            failures.append(
                f"sprite {sprite_id}: {frames + 1}-byte frame table exceeds "
                f"{len(blob)}-byte asset"
            )
            continue
        frame_offsets = tuple(blob[SPRITE_HEADER_SIZE:tail_end])
        if frame_offsets[0] != 0:
            failures.append(
                f"sprite {sprite_id}: first cumulative texture offset is "
                f"{frame_offsets[0]}, expected zero"
            )
            continue
        if any(b < a for a, b in zip(frame_offsets, frame_offsets[1:])):
            failures.append(f"sprite {sprite_id}: descending frame offsets")
            continue

        affected = multirun_frames(frame_offsets)
        if affected:
            multirun[sprite_id] = affected

        textures = frame_offsets[-1]
        old_count = 4 * textures + frames
        exact_count = command_count(frame_offsets)
        if exact_count > old_count:
            underruns[sprite_id] = (old_count, exact_count, frame_offsets)

    if failures:
        raise ValueError("; ".join(failures))
    return build, len(offsets) - 1, max_size, underruns, multirun


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--build",
        help="accepted for tools/run_checks.py's common native invocation",
    )
    args = parser.parse_args()

    try:
        source_failures = prove_source_contract(SPRITE_SOURCE.read_text())
        if source_failures:
            raise ValueError(
                "sprite reader/builder source contract failed: "
                + "; ".join(source_failures)
            )
        build, count, max_size, underruns, multirun = census(Path(args.rom))
    except (OSError, ValueError, struct.error) as exc:
        print(f"check_sprite_layout: FAIL — {exc}", file=sys.stderr)
        return 1

    if underruns != EXPECTED_UNDERRUNS:
        print(
            "check_sprite_layout: FAIL — old-layout positive controls changed\n"
            f"  expected: {EXPECTED_UNDERRUNS}\n"
            f"  actual:   {underruns}",
            file=sys.stderr,
        )
        return 1

    classifier_failures = prove_multirun_classifier()
    if classifier_failures:
        print(
            "check_sprite_layout: FAIL — multi-run classifier is inert: "
            + "; ".join(classifier_failures),
            file=sys.stderr,
        )
        return 1

    if multirun != EXPECTED_MULTIRUN:
        print(
            "check_sprite_layout: FAIL — the G_VTX_APPEND blast radius moved\n"
            f"  expected: {EXPECTED_MULTIRUN}\n"
            f"  actual:   {multirun}\n"
            "  Every listed frame emits several appended vertex runs against\n"
            "  one fixed base. If this set grows, the new content is covered by\n"
            "  no pixel gate — extend tests/check_intro_shrub_sprite.py.",
            file=sys.stderr,
        )
        return 1

    rendered = ", ".join(
        f"sprite {sprite_id} {old}->{exact}"
        for sprite_id, (old, exact, _offsets) in underruns.items()
    )
    multirendered = ", ".join(
        f"sprite {sprite_id} frame {frame} {tiles} tiles/{runs} runs"
        for sprite_id, frames in EXPECTED_MULTIRUN.items()
        for frame, tiles, runs in frames
    )
    print(
        f"check_sprite_layout: PASS — {build}, {count} sprites, "
        f"max asset {max_size} bytes; four readers share one checked boundary; "
        f"positive controls: {rendered}; "
        f"multi-run append frames: {multirendered}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
