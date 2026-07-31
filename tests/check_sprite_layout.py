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


def command_count(offsets: tuple[int, ...]) -> int:
    return sum(
        2 + 2 * (end - start) + ((end - start) + 4) // 5
        for start, end in zip(offsets, offsets[1:])
    )


def census(rom_path: Path) -> tuple[str, int, int, dict[int, tuple[int, int, tuple[int, ...]]]]:
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

        textures = frame_offsets[-1]
        old_count = 4 * textures + frames
        exact_count = command_count(frame_offsets)
        if exact_count > old_count:
            underruns[sprite_id] = (old_count, exact_count, frame_offsets)

    if failures:
        raise ValueError("; ".join(failures))
    return build, len(offsets) - 1, max_size, underruns


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
        build, count, max_size, underruns = census(Path(args.rom))
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

    rendered = ", ".join(
        f"sprite {sprite_id} {old}->{exact}"
        for sprite_id, (old, exact, _offsets) in underruns.items()
    )
    print(
        f"check_sprite_layout: PASS — {build}, {count} sprites, "
        f"max asset {max_size} bytes; four readers share one checked boundary; "
        f"positive controls: {rendered}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
