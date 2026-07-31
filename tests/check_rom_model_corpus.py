#!/usr/bin/env python3
"""Walk every retail model asset without relying on a reachable game route.

DKR's ROM does not contain raw F3DDKR display lists. Object and level models
contain batches; the game turns those batches into display-list commands at
runtime. The live WebGPU census therefore answers "what did our routes emit",
while this gate answers the complementary question: "did those routes leave an
unparsed model variant in the supported ROM?"

For both supported revision-1 ROMs (whose asset payloads are identical), this
check inflates and validates all 390 object models and all 55 level models. It
checks every renderer-consumed geometry/visibility region, batch sentinel,
texture reference, vertex/triangle span, and triangle-local vertex index, then
reports the complete authored render-state inventory. Parser positive controls
corrupt real records and must be rejected.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import struct
import sys
import zlib


ASSET_LEVEL_MODELS_TABLE = 26
ASSET_LEVEL_MODELS = 27
ASSET_OBJECT_MODELS_TABLE = 28
ASSET_OBJECT_MODELS = 29
ASSET_TEXTURES_3D = 2
ASSET_TEXTURES_3D_TABLE = 3
ASSET_SECTION_COUNT = 50

OBJECT_MODEL_COUNT = 390
LEVEL_MODEL_COUNT = 55

OBJECT_HEADER_SIZE = 0x58
LEVEL_HEADER_SIZE = 0x4C
LEVEL_SEGMENT_SIZE = 0x44
TEXTURE_INFO_SIZE = 8
VERTEX_SIZE = 10
TRIANGLE_SIZE = 16
BATCH_SIZE = 12
DKR_VERTEX_CAPACITY = 32
DKR_TRIANGLE_COMMAND_CAPACITY = 16

SUPPORTED = {
    (0xE402430D, 0xD2FCFC9D): ("us.v80", 0x000ED0E0, 0x000ED1B0),
    (0x596E145B, 0xF7D9879F): ("pal.v80", 0x000ED170, 0x000ED240),
}


class CorpusError(ValueError):
    pass


def bes16(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from(">h", data, offset)[0]


def be32(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def normalize_rom(data: bytearray) -> str:
    if len(data) < 0x40:
        raise CorpusError("ROM is shorter than its header")
    magic = bytes(data[:4])
    if magic == bytes((0x80, 0x37, 0x12, 0x40)):
        return "z64"
    if magic == bytes((0x37, 0x80, 0x40, 0x12)):
        for offset in range(0, len(data) - 1, 2):
            data[offset], data[offset + 1] = data[offset + 1], data[offset]
        return "v64"
    if magic == bytes((0x40, 0x12, 0x37, 0x80)):
        for offset in range(0, len(data) - 3, 4):
            data[offset:offset + 4] = reversed(data[offset:offset + 4])
        return "n64"
    raise CorpusError(f"unsupported ROM byte order/magic {magic.hex()}")


def checked_region(
    data: bytes | bytearray,
    offset: int,
    count: int,
    stride: int,
    label: str,
) -> None:
    if offset < 0 or count < 0 or stride <= 0:
        raise CorpusError(f"{label}: negative offset/count or invalid stride")
    size = count * stride
    if count and offset == 0:
        raise CorpusError(f"{label}: non-empty region has a null offset")
    if offset > len(data) or size > len(data) - offset:
        raise CorpusError(
            f"{label}: region {offset:#x}+{size:#x} exceeds {len(data):#x}"
        )


def read_offset_table(raw: bytes, label: str) -> list[int]:
    if len(raw) % 4:
        raise CorpusError(f"{label}: table length is not word-aligned")
    values = list(struct.unpack(f">{len(raw) // 4}i", raw))
    try:
        terminal = values.index(-1)
    except ValueError as exc:
        raise CorpusError(f"{label}: missing -1 terminator") from exc
    offsets = values[:terminal]
    if len(offsets) < 2:
        raise CorpusError(f"{label}: table contains no records")
    if offsets[0] != 0:
        raise CorpusError(f"{label}: first record does not start at zero")
    if any(value < 0 for value in offsets):
        raise CorpusError(f"{label}: negative pre-terminal offset")
    if any(right <= left for left, right in zip(offsets, offsets[1:])):
        raise CorpusError(f"{label}: offsets are not strictly increasing")
    return offsets


def inflate_record(compressed: bytes, label: str) -> bytes:
    if len(compressed) < 6:
        raise CorpusError(f"{label}: compressed record is too short")
    expected = struct.unpack_from("<I", compressed, 0)[0]
    inflater = zlib.decompressobj(wbits=-15)
    try:
        output = inflater.decompress(compressed[5:]) + inflater.flush()
    except zlib.error as exc:
        raise CorpusError(f"{label}: invalid raw-deflate payload: {exc}") from exc
    if not inflater.eof:
        raise CorpusError(f"{label}: truncated raw-deflate payload")
    if len(output) != expected:
        raise CorpusError(
            f"{label}: header says {expected} inflated bytes, got {len(output)}"
        )
    return output


@dataclass
class Census:
    records: int = 0
    segments: int = 0
    textures: int = 0
    vertices: int = 0
    triangles: int = 0
    batches: int = 0
    max_batch_vertices: int = 0
    max_batch_triangles: int = 0
    # R12 instrument: BACKFACE_DRAW (0x40) prevalence. The shadow depth pass
    # front-face-culls single-sided geometry, so the fraction of triangles
    # WITHOUT this flag measures how much authored content is an open shell
    # the shadow pass can only see from one side.
    backface_triangles: int = 0
    flags: set[int] = field(default_factory=set)
    # kind, render flags, textured, TextureInfo format, surface type
    render_states: set[tuple[str, int, bool, int, int]] = field(default_factory=set)


def validate_batches(
    data: bytes | bytearray,
    *,
    label: str,
    kind: str,
    texture_offset: int,
    texture_count: int,
    vertex_offset: int,
    vertex_count: int,
    triangle_offset: int,
    triangle_count: int,
    batch_offset: int,
    batch_count: int,
    texture_asset_count: int,
    census: Census,
) -> None:
    checked_region(data, texture_offset, texture_count, TEXTURE_INFO_SIZE, f"{label} textures")
    checked_region(data, vertex_offset, vertex_count, VERTEX_SIZE, f"{label} vertices")
    checked_region(data, triangle_offset, triangle_count, TRIANGLE_SIZE, f"{label} triangles")
    checked_region(data, batch_offset, batch_count + 1, BATCH_SIZE, f"{label} batches")

    previous_vertex = -1
    previous_face = -1
    for index in range(batch_count + 1):
        base = batch_offset + index * BATCH_SIZE
        vertices = bes16(data, base + 2)
        faces = bes16(data, base + 4)
        if not 0 <= vertices <= vertex_count:
            raise CorpusError(
                f"{label}: batch {index} vertex sentinel {vertices} "
                f"outside 0..{vertex_count}"
            )
        if not 0 <= faces <= triangle_count:
            raise CorpusError(
                f"{label}: batch {index} face sentinel {faces} "
                f"outside 0..{triangle_count}"
            )
        if vertices < previous_vertex or faces < previous_face:
            raise CorpusError(f"{label}: batch {index} sentinels move backwards")
        previous_vertex = vertices
        previous_face = faces

    if bes16(data, batch_offset + batch_count * BATCH_SIZE + 2) != vertex_count:
        raise CorpusError(f"{label}: terminal batch does not consume all vertices")
    if bes16(data, batch_offset + batch_count * BATCH_SIZE + 4) != triangle_count:
        raise CorpusError(f"{label}: terminal batch does not consume all triangles")
    if bes16(data, batch_offset + 2) != 0 or bes16(data, batch_offset + 4) != 0:
        raise CorpusError(f"{label}: first batch does not start at vertex/face zero")

    for index in range(batch_count):
        base = batch_offset + index * BATCH_SIZE
        next_base = base + BATCH_SIZE
        texture_index = data[base]
        first_vertex = bes16(data, base + 2)
        first_face = bes16(data, base + 4)
        next_vertex = bes16(data, next_base + 2)
        next_face = bes16(data, next_base + 4)
        batch_vertices = next_vertex - first_vertex
        batch_triangles = next_face - first_face
        flags = be32(data, base + 8)

        if texture_index != 0xFF and texture_index >= texture_count:
            raise CorpusError(
                f"{label}: batch {index} texture {texture_index} "
                f"outside 0..{texture_count - 1}"
            )
        if batch_triangles and batch_vertices == 0:
            raise CorpusError(f"{label}: batch {index} has faces but no vertices")
        if kind == "object":
            vertex_override = struct.unpack_from(">b", data, base + 1)[0]
            if not 0 <= vertex_override <= batch_vertices:
                raise CorpusError(
                    f"{label}: batch {index} vertex override {vertex_override} "
                    f"outside 0..{batch_vertices}"
                )
        for triangle in range(first_face, next_face):
            tri = triangle_offset + triangle * TRIANGLE_SIZE
            indices = data[tri + 1:tri + 4]
            if any(vertex >= batch_vertices for vertex in indices):
                raise CorpusError(
                    f"{label}: batch {index} triangle {triangle} has local "
                    f"indices {tuple(indices)} for {batch_vertices} vertices"
                )
            if data[tri] & 0x40:  # BACKFACE_DRAW
                census.backface_triangles += 1

        textured = texture_index != 0xFF
        texture_format = -1
        surface_type = -1
        if textured:
            tex = texture_offset + texture_index * TEXTURE_INFO_SIZE
            texture_asset = struct.unpack_from(">i", data, tex)[0]
            if not 0 <= texture_asset < texture_asset_count:
                raise CorpusError(
                    f"{label}: batch {index} references 3D texture asset "
                    f"{texture_asset}, table contains {texture_asset_count}"
                )
            texture_format = data[tex + 6]
            surface_type = data[tex + 7]
        census.flags.add(flags)
        census.render_states.add(
            (kind, flags, textured, texture_format, surface_type)
        )
        census.max_batch_vertices = max(census.max_batch_vertices, batch_vertices)
        census.max_batch_triangles = max(census.max_batch_triangles, batch_triangles)

    census.textures += texture_count
    census.vertices += vertex_count
    census.triangles += triangle_count
    census.batches += batch_count


def parse_object_model(
    data: bytes | bytearray,
    model_id: int,
    census: Census,
    texture_asset_count: int,
) -> None:
    label = f"object model {model_id}"
    if len(data) < OBJECT_HEADER_SIZE:
        raise CorpusError(f"{label}: shorter than the {OBJECT_HEADER_SIZE:#x}-byte header")
    if be32(data, 0x2C) != len(data):
        raise CorpusError(
            f"{label}: fileSize={be32(data, 0x2C)}, inflated={len(data)}"
        )
    texture_count = bes16(data, 0x22)
    vertex_count = bes16(data, 0x24)
    triangle_count = bes16(data, 0x26)
    batch_count = bes16(data, 0x28)
    if min(texture_count, vertex_count, triangle_count, batch_count) < 0:
        raise CorpusError(f"{label}: negative nested-array count")
    validate_batches(
        data,
        label=label,
        kind="object",
        texture_offset=be32(data, 0x00),
        texture_count=texture_count,
        vertex_offset=be32(data, 0x04),
        vertex_count=vertex_count,
        triangle_offset=be32(data, 0x08),
        triangle_count=triangle_count,
        batch_offset=be32(data, 0x38),
        batch_count=batch_count,
        texture_asset_count=texture_asset_count,
        census=census,
    )
    attach_count = bes16(data, 0x18)
    sphere_count = bes16(data, 0x20)
    if attach_count < 0 or sphere_count < 0:
        raise CorpusError(f"{label}: negative attachment/collision-sphere count")
    checked_region(data, be32(data, 0x14), attach_count, 2, f"{label} attach points")
    checked_region(data, be32(data, 0x1C), sphere_count, 2, f"{label} collision spheres")
    animation_indices = be32(data, 0x4C)
    if animation_indices:
        checked_region(
            data, animation_indices, vertex_count, 2, f"{label} animation indices"
        )
    census.records += 1


def parse_level_model(
    data: bytes | bytearray,
    model_id: int,
    census: Census,
    texture_asset_count: int,
) -> None:
    label = f"level model {model_id}"
    if len(data) < LEVEL_HEADER_SIZE:
        raise CorpusError(f"{label}: shorter than the {LEVEL_HEADER_SIZE:#x}-byte header")
    if be32(data, 0x48) != len(data):
        raise CorpusError(
            f"{label}: modelSize={be32(data, 0x48)}, inflated={len(data)}"
        )
    texture_offset = be32(data, 0x00)
    segment_offset = be32(data, 0x04)
    texture_count = bes16(data, 0x18)
    segment_count = bes16(data, 0x1A)
    if texture_count < 0 or segment_count <= 0:
        raise CorpusError(
            f"{label}: invalid texture/segment counts {texture_count}/{segment_count}"
        )
    checked_region(
        data, texture_offset, texture_count, TEXTURE_INFO_SIZE, f"{label} textures"
    )
    checked_region(
        data, segment_offset, segment_count, LEVEL_SEGMENT_SIZE, f"{label} segments"
    )
    checked_region(
        data, be32(data, 0x08), segment_count, 12, f"{label} bounding boxes"
    )
    bsp_offset = be32(data, 0x14)
    if bsp_offset:
        checked_region(
            data, bsp_offset, segment_count - 1, 8, f"{label} BSP nodes"
        )
    visibility_offset = be32(data, 0x10)
    visibility_bytes = (segment_count + 7) // 8
    max_visibility_end = 0

    for segment in range(segment_count):
        base = segment_offset + segment * LEVEL_SEGMENT_SIZE
        vertex_count = bes16(data, base + 0x1C)
        triangle_count = bes16(data, base + 0x1E)
        batch_count = bes16(data, base + 0x20)
        if min(vertex_count, triangle_count, batch_count) < 0:
            raise CorpusError(f"{label} segment {segment}: negative nested-array count")
        validate_batches(
            data,
            label=f"{label} segment {segment}",
            kind="level",
            texture_offset=texture_offset,
            texture_count=texture_count,
            vertex_offset=be32(data, base + 0x00),
            vertex_count=vertex_count,
            triangle_offset=be32(data, base + 0x04),
            triangle_count=triangle_count,
            batch_offset=be32(data, base + 0x0C),
            batch_count=batch_count,
            texture_asset_count=texture_asset_count,
            census=census,
        )
        visibility_row = bes16(data, base + 0x28)
        if visibility_row < 0:
            raise CorpusError(
                f"{label} segment {segment}: negative visibility-row offset"
            )
        max_visibility_end = max(
            max_visibility_end, visibility_row + visibility_bytes
        )
        collision_offset = be32(data, base + 0x14)
        if collision_offset:
            checked_region(
                data,
                collision_offset,
                triangle_count,
                8,
                f"{label} segment {segment} collision facets",
            )
    checked_region(
        data,
        visibility_offset,
        max_visibility_end,
        1,
        f"{label} segment visibility bitfields",
    )
    census.records += 1
    census.segments += segment_count


def section(rom: bytes, lut: list[int], data_base: int, index: int) -> bytes:
    if not 0 <= index < ASSET_SECTION_COUNT:
        raise CorpusError(f"invalid asset section {index}")
    start = data_base + lut[index + 1]
    end = data_base + lut[index + 2]
    if start > end or end > len(rom):
        raise CorpusError(
            f"asset section {index}: range {start:#x}..{end:#x} exceeds ROM"
        )
    return rom[start:end]


def parse_records(
    table_raw: bytes,
    blob: bytes,
    expected_count: int,
    kind: str,
    parser,
    texture_asset_count: int,
) -> tuple[Census, list[bytes]]:
    offsets = read_offset_table(table_raw, f"{kind} model table")
    if len(offsets) - 1 != expected_count:
        raise CorpusError(
            f"{kind} model table has {len(offsets) - 1} records, "
            f"expected {expected_count}"
        )
    if offsets[-1] != len(blob):
        raise CorpusError(
            f"{kind} model table ends at {offsets[-1]:#x}, "
            f"section is {len(blob):#x} bytes"
        )
    census = Census()
    inflated: list[bytes] = []
    for record in range(expected_count):
        raw = blob[offsets[record]:offsets[record + 1]]
        data = inflate_record(raw, f"{kind} model {record}")
        parser(data, record, census, texture_asset_count)
        inflated.append(data)
    return census, inflated


def expect_reject(label: str, function) -> None:
    try:
        function()
    except CorpusError:
        return
    raise CorpusError(f"positive control {label!r} was accepted")


def positive_controls(
    object_model: bytes, level_model: bytes, texture_asset_count: int
) -> None:
    obj_batch = be32(object_model, 0x38)
    obj_count = bes16(object_model, 0x28)
    obj_vertices = bes16(object_model, 0x24)
    broken = bytearray(object_model)
    struct.pack_into(">h", broken, obj_batch + obj_count * BATCH_SIZE + 2, obj_vertices - 1)
    expect_reject(
        "object terminal sentinel",
        lambda: parse_object_model(broken, 0, Census(), texture_asset_count),
    )

    broken = bytearray(object_model)
    texture_count = bes16(object_model, 0x22)
    if texture_count <= 0 or texture_count >= 0xFF:
        raise CorpusError("positive-control seed lacks a usable texture table")
    broken[obj_batch] = texture_count
    expect_reject(
        "object texture index",
        lambda: parse_object_model(broken, 0, Census(), texture_asset_count),
    )

    textured_batch = next(
        (
            index
            for index in range(obj_count)
            if object_model[obj_batch + index * BATCH_SIZE] != 0xFF
        ),
        None,
    )
    if textured_batch is None:
        raise CorpusError("positive-control seed has no textured object batch")
    broken = bytearray(object_model)
    texture_index = broken[obj_batch + textured_batch * BATCH_SIZE]
    texture_offset = be32(object_model, 0x00)
    struct.pack_into(
        ">i",
        broken,
        texture_offset + texture_index * TEXTURE_INFO_SIZE,
        texture_asset_count,
    )
    expect_reject(
        "object texture asset",
        lambda: parse_object_model(broken, 0, Census(), texture_asset_count),
    )

    level_segments = be32(level_model, 0x04)
    level_batch = be32(level_model, level_segments + 0x0C)
    level_count = bes16(level_model, level_segments + 0x20)
    broken = bytearray(level_model)
    struct.pack_into(">I", broken, level_segments + 0x0C, len(level_model) - BATCH_SIZE)
    expect_reject(
        "level batch region",
        lambda: parse_level_model(broken, 0, Census(), texture_asset_count),
    )

    broken = bytearray(level_model)
    first_faces = bes16(level_model, level_batch + 4)
    struct.pack_into(">h", broken, level_batch + BATCH_SIZE + 4, first_faces - 1)
    expect_reject(
        "level backward sentinel",
        lambda: parse_level_model(broken, 0, Census(), texture_asset_count),
    )
    if level_count <= 0:
        raise CorpusError("positive-control seed has no level batches")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    try:
        rom_data = bytearray(Path(args.rom).read_bytes())
        byte_order = normalize_rom(rom_data)
        crcs = (be32(rom_data, 0x10), be32(rom_data, 0x14))
        revision = SUPPORTED.get(crcs)
        if revision is None:
            raise CorpusError(
                f"ROM CRC {crcs[0]:08x}/{crcs[1]:08x} is not a supported "
                "revision-1 asset corpus"
            )
        revision_name, lut_start, data_base = revision
        lut_words = (data_base - lut_start) // 4
        if lut_words < ASSET_SECTION_COUNT + 2:
            raise CorpusError("master asset LUT is too short")
        lut = list(struct.unpack_from(f">{lut_words}I", rom_data, lut_start))
        if lut[0] != ASSET_SECTION_COUNT:
            raise CorpusError(
                f"master asset LUT declares {lut[0]} sections, "
                f"expected {ASSET_SECTION_COUNT}"
            )
        if any(right < left for left, right in zip(lut[1:], lut[2:])):
            raise CorpusError("master asset LUT offsets move backwards")

        texture_offsets = read_offset_table(
            section(rom_data, lut, data_base, ASSET_TEXTURES_3D_TABLE),
            "3D texture table",
        )
        texture_blob = section(rom_data, lut, data_base, ASSET_TEXTURES_3D)
        if texture_offsets[-1] != len(texture_blob):
            raise CorpusError(
                f"3D texture table ends at {texture_offsets[-1]:#x}, "
                f"section is {len(texture_blob):#x} bytes"
            )
        texture_asset_count = len(texture_offsets) - 1

        object_census, object_records = parse_records(
            section(rom_data, lut, data_base, ASSET_OBJECT_MODELS_TABLE),
            section(rom_data, lut, data_base, ASSET_OBJECT_MODELS),
            OBJECT_MODEL_COUNT,
            "object",
            parse_object_model,
            texture_asset_count,
        )
        level_census, level_records = parse_records(
            section(rom_data, lut, data_base, ASSET_LEVEL_MODELS_TABLE),
            section(rom_data, lut, data_base, ASSET_LEVEL_MODELS),
            LEVEL_MODEL_COUNT,
            "level",
            parse_level_model,
            texture_asset_count,
        )
        positive_controls(
            object_records[0], level_records[0], texture_asset_count
        )
    except (CorpusError, OSError, struct.error) as exc:
        print(f"check_rom_model_corpus: FAIL — {exc}", file=sys.stderr)
        return 1

    flags = object_census.flags | level_census.flags
    states = object_census.render_states | level_census.render_states
    if object_census.batches < 1_000 or level_census.batches < 1_000:
        print(
            "check_rom_model_corpus: FAIL — implausibly shallow batch census "
            f"object={object_census.batches}, level={level_census.batches}",
            file=sys.stderr,
        )
        return 1
    max_vertices = max(
        object_census.max_batch_vertices, level_census.max_batch_vertices
    )
    max_triangles = max(
        object_census.max_batch_triangles, level_census.max_batch_triangles
    )
    if max_vertices > DKR_VERTEX_CAPACITY:
        print(
            "check_rom_model_corpus: FAIL — authored batch exceeds the "
            f"{DKR_VERTEX_CAPACITY}-slot F3DDKR vertex buffer",
            file=sys.stderr,
        )
        return 1
    if max_triangles > DKR_TRIANGLE_COMMAND_CAPACITY:
        print(
            "check_rom_model_corpus: FAIL — authored batch exceeds G_TRIN's "
            f"{DKR_TRIANGLE_COMMAND_CAPACITY}-triangle encoding",
            file=sys.stderr,
        )
        return 1

    if args.verbose:
        print(
            f"  {revision_name}/{byte_order}: object models={object_census.records}, "
            f"batches={object_census.batches:,}, vertices={object_census.vertices:,}, "
            f"triangles={object_census.triangles:,}"
        )
        print(
            f"  level models={level_census.records}, segments={level_census.segments:,}, "
            f"batches={level_census.batches:,}, vertices={level_census.vertices:,}, "
            f"triangles={level_census.triangles:,}"
        )
        print(
            f"  authored flags={len(flags)}, render states={len(states)}, "
            f"max batch={max_vertices}/{DKR_VERTEX_CAPACITY} vertices, "
            f"{max_triangles}/{DKR_TRIANGLE_COMMAND_CAPACITY} triangles"
        )
    print(
        "  BACKFACE_DRAW histogram (shadow-pass open-shell measure): "
        f"level {level_census.backface_triangles:,}/"
        f"{level_census.triangles:,} two-sided, "
        f"object {object_census.backface_triangles:,}/"
        f"{object_census.triangles:,} two-sided"
    )
    print(
        "check_rom_model_corpus: PASS — "
        f"{object_census.records + level_census.records} models, "
        f"{level_census.segments:,} level segments, "
        f"{object_census.batches + level_census.batches:,} batches, "
        f"{object_census.vertices + level_census.vertices:,} vertices, "
        f"{object_census.triangles + level_census.triangles:,} triangles, "
        f"{len(flags)} authored flag words/{len(states)} render states; "
        "all renderer regions, sentinels, references, and local indices valid"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
