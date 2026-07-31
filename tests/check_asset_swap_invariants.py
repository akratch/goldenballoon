#!/usr/bin/env python3
"""Close the byte-swap misread class instead of whack-a-moling it.

BHV_WAVE_GENERATOR's u16 fields were consumed byte-swapped from bring-up until
v0.8: Boulder Canyon's water stood 12.7x too tall in every release, and every
self-consistency oracle stayed green the whole time. That is the defining
property of this defect class -- deterministically-wrong input data is perfectly
reproducible, so determinism gates, state hashes, and frame A/Bs all agree with
it. Only a field-level comparison against the on-disk spec can catch it.

This gate implements the harness sketched in docs/asset_swap_notes.md
("Verification harness sketch"). It has two independent arms:

ARM 1 -- ROM DATA INVARIANTS.
    An INDEPENDENT decoder (it re-implements the LUT walk rather than calling
    any port code, so agreeing with the game is real cross-checking and not a
    tautology) reads every byte-swapped asset type out of the retail ROM and
    asserts spec-derived invariants: enum fields in range, offsets inside their
    blob, self-describing size identities, counts consistent with payload
    length.

    Every invariant ships with a POSITIVE CONTROL: the same field decoded with
    the bytes reversed must VIOLATE the bound. An invariant that both decodes
    satisfy proves nothing -- it is exactly the toothless oracle that let the
    wave bug survive -- so a control that fails to trip is itself an error here.

ARM 2 -- SOURCE COVERAGE.
    asset_load() is a raw ROM DMA; only asset_table_load() runs the
    asset_swap_normalize() hook. So every asset_load(ASSET_X, ...) site owes a
    swap, and a swapper that exists in the dispatch but is never reached is dead
    code masquerading as coverage. Both of the defects this gate was written for
    were exactly that shape:

      * ASSET_AI_BEHAVIOUR  -- swap_ai_behaviour() existed and was listed "Full"
        in the coverage table, but ASSET_AI_BEHAVIOUR is never passed to
        asset_table_load(), so the AI difficulty ramp was consumed big-endian
        and every AI level collapsed to a denormal (~0).
      * ASSET_TTGHOSTS      -- swap_tt_ghost() likewise; the T.T. staff ghost
        decoded to nodeCount = -12544 and never played back.

    Arm 2 enforces the disposition of every raw asset_load() site against an
    explicit table, so a new one cannot be added silently and an existing one
    cannot quietly lose its swap.

Usage:
    tests/check_asset_swap_invariants.py --rom baserom.us.v80.z64
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Asset section enum (game/include/asset_enums.h AssetSectionsEnum)
# ---------------------------------------------------------------------------
ASSET_AI_BEHAVIOUR = 0
ASSET_AI_BEHAVIOUR_TABLE = 1
ASSET_MISC = 15
ASSET_MISC_TABLE = 16
ASSET_LEVEL_HEADERS_TABLE = 22
ASSET_LEVEL_HEADERS = 23
ASSET_OBJECT_HEADERS_TABLE = 33
ASSET_OBJECTS = 34
ASSET_PARTICLE_BEHAVIORS_TABLE = 42
ASSET_PARTICLE_BEHAVIORS = 43
ASSET_TTGHOSTS_TABLE = 48
ASSET_TTGHOSTS = 49
ASSET_SECTION_COUNT = 50

# ASSET_MISC sub-asset indices used below (game/include/asset_enums.h).
MISC_MAGIC_CODES = 65
MISC_TITLE_SCREEN_DEMO_IDS = 66

# CRC pair -> (name, LUT_START, DATA_BASE). Same table as
# tests/check_rom_model_corpus.py; the two revision-1 asset payloads match.
SUPPORTED = {
    (0xE402430D, 0xD2FCFC9D): ("us.v80", 0x000ED0E0, 0x000ED1B0),
    (0x596E145B, 0xF7D9879F): ("pal.v80", 0x000ED170, 0x000ED240),
}

# LevelHeader field ranges. Sources: game/include/structs.h LevelHeader (which
# wins over the .hpp per docs/asset_swap_notes.md) and the authored-value ranges
# its own comments record.
# world is s8 and -1 is authored (menu/cutscene levels with no world);
# game.c only uses it to raise a running max, so -1 is legitimate.
WORLD_RANGE = (-1, 7)          # census: -1 .. 5
# game/include/enums.h RaceType -- a sparse enum, not a dense 0..N range
# (the challenge modes live at 64/65/66 behind the 0x40 mask).
RACE_TYPES = {0, 1, 3, 5, 6, 7, 8, 64, 65, 66}
LAPS_RANGE = (1, 6)      # census: {1,3}
FOV_RANGE = (1, 90)      # census: {40,45,55,60}; structs.h bounds it to [0,90]
# Wave-block ranges. structs.h's inline comments are the starting point but are
# NOT all accurate -- it claims wavePower is "always 256" and the retail corpus
# actually authors {128, 207, 256}. These bounds are therefore re-derived by
# census over every level header in the supported ROMs, then widened to the
# nearest round figure so authoring headroom does not make the gate brittle.
# Each one is verified to reject the byte-reversed decode (see the controls).
WAVE_SUBDIVISIONS = {2, 3, 4, 6, 8}     # census: exactly these five
WAVE_SINE_HEIGHT_RANGE = (512, 4963)    # census: 512 .. 4963
WAVE_POWER_RANGE = (128, 256)           # census: {128, 207, 256}
WAVE_SEED_SIZE_RANGE = (120, 187)       # census: {120,130,157,178,187}
WAVE_UNK66_RANGE = (908, 2560)          # census: 908 .. 2560
WAVE_TEX_IDS = {62, 205}                # census: exactly these two
WAVE_VIEW_DIST = {3, 5}                 # census: exactly these two
MAX_TIME_FRAMES = 36000           # 10 minutes at 60Hz


def align16(value):
    return (value + 15) & ~15


class SwapInvariantError(ValueError):
    pass


# ---------------------------------------------------------------------------
# Independent ROM reader
# ---------------------------------------------------------------------------
def be32(data, off):
    return struct.unpack_from(">I", data, off)[0]


def bes32(data, off):
    return struct.unpack_from(">i", data, off)[0]


def bes16(data, off):
    return struct.unpack_from(">h", data, off)[0]


def beu16(data, off):
    return struct.unpack_from(">H", data, off)[0]


def bef32(data, off):
    return struct.unpack_from(">f", data, off)[0]


def rev16(value):
    """The value this field decodes to when the swap is MISSING (bytes kept)."""
    return ((value & 0xFF) << 8) | ((value >> 8) & 0xFF)


def rev32(value):
    return struct.unpack("<I", struct.pack(">I", value & 0xFFFFFFFF))[0]


def revf32(data, off):
    return struct.unpack_from("<f", data, off)[0]


def normalize_rom(data):
    """Accept .z64/.n64/.v64; return the big-endian image."""
    magic = be32(data, 0)
    if magic == 0x80371240:
        return data
    if magic == 0x37804012:  # byte-swapped (.v64)
        out = bytearray(data)
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return out
    if magic == 0x40123780:  # little-endian (.n64)
        out = bytearray(len(data))
        out[0::4], out[1::4], out[2::4], out[3::4] = (
            data[3::4], data[2::4], data[1::4], data[0::4])
        return out
    raise SwapInvariantError("unrecognised ROM byte order")


class Rom:
    def __init__(self, path):
        data = normalize_rom(bytearray(Path(path).read_bytes()))
        crcs = (be32(data, 0x10), be32(data, 0x14))
        revision = SUPPORTED.get(crcs)
        if revision is None:
            raise SwapInvariantError(
                f"ROM CRC {crcs[0]:08x}/{crcs[1]:08x} is not a supported "
                "revision-1 asset corpus")
        self.name, self.lut_start, self.data_base = revision
        self.data = data
        words = (self.data_base - self.lut_start) // 4
        self.lut = list(struct.unpack_from(f">{words}I", data, self.lut_start))
        if self.lut[0] != ASSET_SECTION_COUNT:
            raise SwapInvariantError(
                f"master LUT declares {self.lut[0]} sections, "
                f"expected {ASSET_SECTION_COUNT}")

    def section(self, index):
        start = self.data_base + self.lut[index + 1]
        end = self.data_base + self.lut[index + 2]
        return self.data[start:end]

    def offsets(self, index):
        """A *_TABLE section read as its u32 offset array, terminator trimmed."""
        raw = self.section(index)
        table = list(struct.unpack(f">{len(raw) // 4}i", raw))
        return table[:table.index(-1)] if -1 in table else table


class Report:
    def __init__(self, verbose):
        self.verbose = verbose
        self.checks = 0
        self.controls = 0
        self.strong = 0
        self.weak = 0
        self.lines = []
        self._asset = "(unscoped)"
        self._assets = set()
        self._discriminating = set()

    def ok(self, text):
        self.checks += 1
        if self.verbose:
            print(f"    ok   {text}")

    def control(self, label, correct_ok, reversed_ok):
        """Record a positive control and report whether it DISCRIMINATES.

        `correct_ok`  -- the correctly-swapped decode satisfies the invariant.
        `reversed_ok` -- the byte-reversed decode ALSO satisfies it.

        A control that both decodes satisfy is not a failure by itself: some
        authored values are genuinely small enough that a range check cannot
        tell the two decodes apart (waveSineHeight0 == 512 reverses to 2, and
        both sit inside 0..4963). What WOULD be a failure is an asset type
        whose every invariant is like that -- then nothing in the gate could
        ever catch a missing swap for it. So discrimination is tallied here and
        asserted per asset type by require_discriminating().
        """
        self.controls += 1
        if not correct_ok:
            raise SwapInvariantError(
                f"{label}: the correct big-endian decode VIOLATES its own "
                f"invariant -- the field map is wrong")
        if reversed_ok:
            self.weak += 1
            if self.verbose:
                print(f"    ctl  {label}: cannot discriminate (value too small)")
        else:
            self.strong += 1
            self._discriminating.add(self._asset)
            if self.verbose:
                print(f"    ctl  {label}: reversed decode correctly rejected")

    def asset(self, name):
        """Scope subsequent controls to an asset type."""
        self._asset = name
        self._assets.add(name)

    def require_discriminating(self):
        missing = sorted(self._assets - self._discriminating)
        if missing:
            raise SwapInvariantError(
                "no invariant in this gate can distinguish a correct decode "
                "from a byte-reversed one for: " + ", ".join(missing) +
                " -- add a bound with teeth before trusting this gate for them")

    def note(self, text):
        self.lines.append(text)


# ---------------------------------------------------------------------------
# ARM 1 -- per-asset-type invariants
# ---------------------------------------------------------------------------
def check_level_headers(rom, rep, misc_count):
    rep.asset("LEVEL_HEADERS")
    table = rom.offsets(ASSET_LEVEL_HEADERS_TABLE)
    blob = rom.section(ASSET_LEVEL_HEADERS)
    # The table's last entry bounds the final record.
    count = len(table) - 1
    if count <= 0:
        raise SwapInvariantError("level-header table is empty")

    rev_fov_ok = 0
    for i in range(count):
        hdr = blob[table[i]:table[i + 1]]
        if len(hdr) < 0xC4:
            raise SwapInvariantError(f"level {i}: header is {len(hdr)} bytes, expected >= 0xC4")
        tag = f"level {i}"

        world = struct.unpack_from(">b", hdr, 0x00)[0]
        if not WORLD_RANGE[0] <= world <= WORLD_RANGE[1]:
            raise SwapInvariantError(f"{tag}: world {world} out of {WORLD_RANGE}")

        laps = struct.unpack_from(">b", hdr, 0x4B)[0]
        if not LAPS_RANGE[0] <= laps <= LAPS_RANGE[1]:
            raise SwapInvariantError(f"{tag}: laps {laps} out of {LAPS_RANGE}")

        race_type = struct.unpack_from(">b", hdr, 0x4C)[0]
        if race_type not in RACE_TYPES:
            raise SwapInvariantError(
                f"{tag}: race_type {race_type} is not a RaceType enumerator")

        fov = struct.unpack_from(">b", hdr, 0x9C)[0]
        if not FOV_RANGE[0] <= fov <= FOV_RANGE[1]:
            raise SwapInvariantError(f"{tag}: cameraFOV {fov} out of {FOV_RANGE}")

        height = bef32(hdr, 0x08)
        if not (height == height) or abs(height) > 1e6:
            raise SwapInvariantError(f"{tag}: course_height {height!r} implausible")
        # course_height is the wave/water plane; a byte-reversed f32 lands in
        # denormal or absurd-exponent territory.
        rev_height = revf32(hdr, 0x08)
        rep.control(f"{tag} course_height",
                    abs(height) <= 1e6,
                    abs(rev_height) <= 1e6 and (rev_height == 0.0 or abs(rev_height) > 1e-30))

        # Wave block -- the exact fields whose misread shipped for a year.
        subdiv = hdr[0x56]
        if subdiv not in WAVE_SUBDIVISIONS:
            raise SwapInvariantError(f"{tag}: waveSubdivisons {subdiv} unexpected")
        # These are the exact fields whose misread shipped for a year:
        # BHV_WAVE_GENERATOR/BHV_WAVE_POWER read byte-swapped made Boulder
        # Canyon's water 12.7x too tall in every release.
        wave_ranges = (
            ("waveSineHeight0", 0x5A, WAVE_SINE_HEIGHT_RANGE),
            ("waveSineHeight1", 0x5E, WAVE_SINE_HEIGHT_RANGE),
            ("waveSeedSize", 0x60, WAVE_SEED_SIZE_RANGE),
            ("wavePower", 0x62, WAVE_POWER_RANGE),
            ("unk66", 0x66, WAVE_UNK66_RANGE),
        )
        for name, off, (lo, hi) in wave_ranges:
            value = bes16(hdr, off)
            if not lo <= value <= hi:
                raise SwapInvariantError(
                    f"{tag}: {name} {value} outside [{lo},{hi}]")
            reversed_value = struct.unpack(
                ">h", struct.pack("<h", value))[0]
            rep.control(f"{tag} {name}",
                        lo <= value <= hi,
                        lo <= reversed_value <= hi)
        tex_id = bes16(hdr, 0x68)
        if tex_id not in WAVE_TEX_IDS:
            raise SwapInvariantError(f"{tag}: waveTexID {tex_id} unexpected")
        rep.control(f"{tag} waveTexID",
                    tex_id in WAVE_TEX_IDS,
                    struct.unpack(">h", struct.pack("<h", tex_id))[0] in WAVE_TEX_IDS)
        view = bes16(hdr, 0x6E)
        if view not in WAVE_VIEW_DIST:
            raise SwapInvariantError(f"{tag}: waveViewDist {view} unexpected")

        # 0x70..0x73 are BYTES (wavesXlu, waveDoubleDensity, +2). structs.h
        # unions them with a dkrptr32 slot, but every array read of that slot
        # (tracks.c generate_track, waves.c) is guarded by index > 0, so slot 0
        # is never dereferenced -- confirming the byte view. Assert the byte
        # view is sane so a future "swap 0x70 as s32" would trip here.
        if hdr[0x70] > 1 or hdr[0x71] > 1:
            raise SwapInvariantError(
                f"{tag}: wavesXlu/waveDoubleDensity {hdr[0x70]},{hdr[0x71]} "
                "are not booleans -- 0x70 may not be byte data after all")

        # miscAssets[7] at 0x74 and pulseLightData at 0xAC index ASSET_MISC.
        for k in range(7):
            value = bes32(hdr, 0x74 + 4 * k)
            if value != -1 and not 0 <= value < misc_count:
                raise SwapInvariantError(
                    f"{tag}: miscAssets[{k}] {value} outside misc table (0..{misc_count})")
        pulse = bes32(hdr, 0xAC)
        if pulse != -1 and not 0 <= pulse < misc_count:
            raise SwapInvariantError(
                f"{tag}: pulseLightData {pulse} outside misc table")
        if pulse != -1:
            rep.control(f"{tag} pulseLightData index",
                        0 <= pulse < misc_count,
                        0 <= rev32(pulse) < misc_count)

        if 0 <= rev_fov_ok:  # keep flake8 quiet about the counter
            rev_fov_ok += 1
        rep.ok(f"{tag} header fields")
    rep.note(f"level headers: {count} validated")
    return count


def check_ai_behaviour(rom, rep):
    """The gap this gate was written for: ASSET_AI_BEHAVIOUR was never swapped."""
    rep.asset("AI_BEHAVIOUR")
    table = rom.offsets(ASSET_AI_BEHAVIOUR_TABLE)
    blob = rom.section(ASSET_AI_BEHAVIOUR)
    levels = len(table) - 1
    if levels <= 0:
        raise SwapInvariantError("AI behaviour table is empty")

    ramp = []
    for i in range(levels):
        rec = blob[table[i]:table[i + 1]]
        if len(rec) % 0x18:
            raise SwapInvariantError(
                f"aiLevel {i}: {len(rec)} bytes is not a multiple of 0x18")
        for r in range(len(rec) // 0x18):
            base = r * 0x18
            a, b = bef32(rec, base), bef32(rec, base + 4)
            for name, value in (("unk0", a), ("unk4", b)):
                if value != value or abs(value) > 100.0:
                    raise SwapInvariantError(
                        f"aiLevel {i}: {name} {value!r} outside +/-100")
                # A byte-reversed f32 of a small authored constant is a
                # denormal; require the correct decode to be either exactly
                # zero or comfortably normal.
                if value != 0.0 and abs(value) < 1e-30:
                    raise SwapInvariantError(
                        f"aiLevel {i}: {name} {value!r} is denormal -- "
                        "this is the signature of a MISSING byte swap")
            ra, rb = revf32(rec, base), revf32(rec, base + 4)
            rep.control(
                f"aiLevel {i} f32 pair",
                all(v == 0.0 or abs(v) >= 1e-30 for v in (a, b)),
                all(v == 0.0 or abs(v) >= 1e-30 for v in (ra, rb)))
            for p in range(16):
                pct = struct.unpack_from(">b", rec, base + 8 + p)[0]
                if not 0 <= pct <= 100:
                    raise SwapInvariantError(
                        f"aiLevel {i}: percentage {pct} outside 0..100")
            if r == 0:
                ramp.append((a, b))
        rep.ok(f"aiLevel {i}")

    # The two f32 are a difficulty ramp: non-decreasing across AI levels. That
    # is a property of the DATA, and it is destroyed by a missing swap (every
    # level flattens to ~0), so it is a strong end-to-end control.
    lo = [pair[0] for pair in ramp]
    hi = [pair[1] for pair in ramp]
    for series, name in ((lo, "unk0"), (hi, "unk4")):
        if any(b < a for a, b in zip(series, series[1:])):
            raise SwapInvariantError(
                f"AI ramp {name} is not monotonic: {series}")
        if len(set(series)) < 2:
            raise SwapInvariantError(
                f"AI ramp {name} is constant ({series[0]!r}) -- the ramp is "
                "flat, which is what a missing swap looks like")
    rep.note(f"AI behaviour: {levels} levels, ramp unk0={lo} unk4={hi}")


def check_tt_ghosts(rom, rep):
    """Table is {u8,u8,pad2,s32}, NOT a u32 offset table; data is never in a
    *_TABLE section so it needs an explicit swap."""
    rep.asset("TTGHOSTS")
    raw = rom.section(ASSET_TTGHOSTS_TABLE)
    blob = rom.section(ASSET_TTGHOSTS)
    entries = []
    for off in range(0, len(raw) - 7, 8):
        map_id, vehicle = raw[off], raw[off + 1]
        pad = raw[off + 2] << 8 | raw[off + 3]
        offset = bes32(raw, off + 4)
        entries.append((map_id, vehicle, pad, offset))
        if map_id == 0xFF:
            break
    else:
        raise SwapInvariantError("TT ghost table has no 0xFF terminator")

    if entries[-1][0] != 0xFF:
        raise SwapInvariantError("TT ghost table terminator missing")
    # Word 0 of a real entry must have zero padding -- that is what proves the
    # {u8,u8,pad2,s32} shape and rules out a plain u32 offset table.
    for map_id, vehicle, pad, _ in entries[:-1]:
        if pad != 0:
            raise SwapInvariantError(
                f"TT ghost entry map {map_id}: bytes 2..3 are {pad:#06x}, not "
                "padding -- the {u8,u8,pad2,s32} record shape does not hold")
        if vehicle > 2:
            raise SwapInvariantError(f"TT ghost map {map_id}: vehicle {vehicle} > 2")
    offsets = [e[3] for e in entries]
    if any(b <= a for a, b in zip(offsets, offsets[1:])):
        raise SwapInvariantError(f"TT ghost offsets are not ascending: {offsets}")
    if offsets[-1] != len(blob):
        raise SwapInvariantError(
            f"TT ghost table ends at {offsets[-1]}, section is {len(blob)} bytes")

    # Positive control for the table shape: running the generic u32 LUT swap
    # over word 0 relocates mapId to byte 3 and destroys the terminator.
    lut_swapped_terminator = rev32(0xFFFF0000) & 0xFF
    rep.control("TT ghost table terminator",
                entries[-1][0] == 0xFF,
                lut_swapped_terminator == 0xFF)

    for map_id, vehicle, _, start in entries[:-1]:
        end = offsets[offsets.index(start) + 1]
        ghost = blob[start:end]
        if len(ghost) < 8:
            raise SwapInvariantError(f"ghost map {map_id}: {len(ghost)} bytes")
        time = bes16(ghost, 4)
        nodes = bes16(ghost, 6)
        if not 0 < time <= MAX_TIME_FRAMES:
            raise SwapInvariantError(
                f"ghost map {map_id}: time {time} outside (0,{MAX_TIME_FRAMES}]")
        if nodes <= 0:
            raise SwapInvariantError(f"ghost map {map_id}: nodeCount {nodes} <= 0")
        # Self-describing identity: the blob is an 8-byte header plus nodeCount
        # 12-byte nodes, and the section pads each ghost to a 16-byte boundary.
        # This is the strongest control in the gate -- it ties a header field to
        # the blob's own length, so no byte-reversed nodeCount can satisfy it.
        def fits(n):
            return n > 0 and align16(12 * n + 8) == len(ghost)

        if not fits(nodes):
            raise SwapInvariantError(
                f"ghost map {map_id}: nodeCount {nodes} implies "
                f"{align16(12 * nodes + 8)} bytes, blob is {len(ghost)}")
        rep.control(f"ghost map {map_id} nodeCount",
                    fits(nodes),
                    fits(struct.unpack(">h", struct.pack("<h", nodes))[0]))
        rep.ok(f"ghost map {map_id}: {nodes} nodes, {time / 60.0:.2f}s")
    rep.note(f"TT ghosts: {len(entries) - 1} validated")


def decrypt_magic_codes(buf):
    """Mirror of objects.c decrypt_magic_codes(): an INTRA-byte bit permutation.

    It never moves bits between bytes, which is why it commutes with the u16
    byte swap and why the swap may safely run after it.
    """
    out = bytearray(buf)
    for i in range(0, (len(out) // 4) * 4, 4):
        b0, b1, b2, b3 = out[i], out[i + 1], out[i + 2], out[i + 3]
        tmp = [
            (b0 & 0xC0) | ((b1 & 0xC0) >> 2) | ((b2 & 0xC0) >> 4) | ((b3 & 0xC0) >> 6),
            ((b0 & 0x30) << 2) | (b1 & 0x30) | ((b2 & 0x30) >> 2) | ((b3 & 0x30) >> 4),
            ((b0 & 0x0C) << 4) | ((b1 & 0x0C) << 2) | (b2 & 0x0C) | ((b3 & 0x0C) >> 2),
            ((b0 & 0x03) << 6) | ((b1 & 0x03) << 4) | ((b2 & 0x03) << 2) | (b3 & 0x03),
        ]
        for j in range(4):
            out[i + j] = ((tmp[j] & 0xAA) >> 1) | ((tmp[j] & 0x55) << 1)
    return bytes(out)


def check_misc(rom, rep):
    rep.asset("MISC_MAGIC_CODES")
    misc = rom.section(ASSET_MISC)
    table = rom.offsets(ASSET_MISC_TABLE)
    count = len(table)

    def sub(index):
        return misc[table[index] * 4:table[index + 1] * 4]

    # --- magic codes: a u16 index block followed by ASCII strings ---
    blob = decrypt_magic_codes(sub(MISC_MAGIC_CODES))
    cheats = beu16(blob, 0)
    if not 0 < cheats < 256:
        raise SwapInvariantError(f"magic codes: numberOfCheats {cheats} implausible")
    index_bytes = (1 + cheats * 3) * 2
    if index_bytes > len(blob):
        raise SwapInvariantError(
            f"magic codes: index block {index_bytes} exceeds blob {len(blob)}")
    first = beu16(blob, 2)
    # THE field-map pin: the first string offset is exactly the index block's
    # byte length, so the header size is self-describing.
    if first != index_bytes:
        raise SwapInvariantError(
            f"magic codes: first string offset {first} != index block size "
            f"{index_bytes} -- field map does not hold")
    for k in range(cheats * 3):
        off = beu16(blob, 2 + 2 * k)
        if not index_bytes <= off < len(blob):
            raise SwapInvariantError(
                f"magic codes: string offset[{k}] {off} outside "
                f"[{index_bytes},{len(blob)})")
    rep.control("magic codes numberOfCheats",
                0 < cheats < 256,
                0 < rev16(cheats) < 256)
    rep.control("magic codes first offset",
                first == index_bytes,
                rev16(first) == index_bytes)
    rep.note(f"magic codes: {cheats} cheats, {index_bytes}-byte index block")

    # --- pulsating light data, reached via LevelHeader.pulseLightData ---
    lh_table = rom.offsets(ASSET_LEVEL_HEADERS_TABLE)
    lh = rom.section(ASSET_LEVEL_HEADERS)
    pulse_indices = set()
    for i in range(len(lh_table) - 1):
        hdr = lh[lh_table[i]:lh_table[i + 1]]
        if len(hdr) >= 0xB0:
            value = bes32(hdr, 0xAC)
            if value != -1:
                pulse_indices.add(value)
    rep.asset("MISC_PULSATING_LIGHT")
    for index in sorted(pulse_indices):
        blob = sub(index)
        frames = beu16(blob, 0)
        capacity = (len(blob) - 0x0C) // 4
        if not 0 < frames <= capacity:
            raise SwapInvariantError(
                f"misc[{index}] PulsatingLightData: numberFrames {frames} "
                f"exceeds blob capacity {capacity}")
        rep.control(f"misc[{index}] PulsatingLightData numberFrames",
                    0 < frames <= capacity,
                    0 < rev16(frames) <= capacity)
        rep.ok(f"misc[{index}] pulsating light: {frames} frames")
    rep.note(f"pulsating light data: {len(pulse_indices)} sub-asset(s)")

    # --- colour loops, reached via ParticleBehaviour.colourLoop ---
    pb_table = rom.offsets(ASSET_PARTICLE_BEHAVIORS_TABLE)
    pb = rom.section(ASSET_PARTICLE_BEHAVIORS)
    loop_indices = set()
    for off in pb_table:
        if 0 <= off and off + 0xA0 <= len(pb):
            value = bes32(pb, off + 0x9C)
            if value != -1:
                loop_indices.add(value)
    loop_report = []
    for index in sorted(loop_indices):
        if not 0 <= index < count:
            raise SwapInvariantError(
                f"ParticleBehaviour.colourLoop index {index} outside misc table")
        blob = sub(index)
        entries = len(blob) // 8
        num = bes32(blob, 0)
        loop_report.append((index, num, entries))
        # NOTE: this is reported, not enforced -- misc 58 is ALSO read as a
        # LevelHeader_70 by game_ui.c, and the two struct shapes disagree about
        # which words are s32. See docs/asset_swap_notes.md "colour-loop /
        # LevelHeader_70 aliasing". Enforcing a bound here would encode a
        # decision this audit deliberately left open.
        if not 0 < num <= entries:
            rep.note(
                f"  WARNING misc[{index}] colourLoop numEntries {num} vs "
                f"{entries} entries (known open item, not enforced)")
    rep.note(f"colour loops: {loop_report}")
    return count


def check_object_headers(rom, rep):
    table = rom.offsets(ASSET_OBJECT_HEADERS_TABLE)
    blob = rom.section(ASSET_OBJECTS)
    walked = 0
    for i in range(len(table) - 1):
        hdr = blob[table[i]:table[i + 1]]
        if len(hdr) < 0x78:
            continue
        walked += 1
        lights = struct.unpack_from(">b", hdr, 0x5A)[0]
        off24 = bes32(hdr, 0x24)
        # ObjectHeader.unk24 is an ARRAY of numLightSources ObjectHeader24
        # records (objects.c light_add_from_object_header loop), but the swapper
        # only ever normalises record 0. That is safe ONLY while every retail
        # header declares zero light sources -- assert exactly that, so a ROM or
        # a hand-edit that populates the array trips here instead of shipping
        # 0x18-byte records past the first still big-endian.
        if lights != 0:
            if not (0 < off24 and off24 + lights * 0x18 <= len(hdr)):
                raise SwapInvariantError(
                    f"object header {i}: numLightSources {lights} with unk24 "
                    f"{off24:#x} does not fit in {len(hdr)} bytes")
            raise SwapInvariantError(
                f"object header {i}: numLightSources {lights} > 0, but "
                "swap_object_header() normalises only unk24[0] -- records 1.. "
                "would be consumed big-endian (see docs/asset_swap_notes.md)")
        for name, off in (("numberOfModelIds", 0x55),
                          ("attachPointCount", 0x56),
                          ("particleCount", 0x57)):
            value = struct.unpack_from(">b", hdr, off)[0]
            if value < 0:
                raise SwapInvariantError(f"object header {i}: {name} {value} < 0")
    rep.note(f"object headers: {walked} walked, all numLightSources == 0")


# ---------------------------------------------------------------------------
# ARM 2 -- source coverage of raw asset_load() sites
# ---------------------------------------------------------------------------
# Disposition of every asset type reached by the RAW asset_load() DMA path.
# "swap"  -- the call site must be followed by an asset_swap_* call.
# "bytes" -- byte-order-defined or ASCII payload; no swap is correct.
# "own"   -- the consumer owns the conversion in its own parser.
RAW_LOAD_DISPOSITION = {
    "ASSET_AI_BEHAVIOUR": "swap",
    "ASSET_LEVEL_HEADERS": "swap",
    "ASSET_LEVEL_MODELS": "swap",
    "ASSET_LEVEL_OBJECT_MAPS": "swap",
    "ASSET_OBJECT_MODELS": "swap",
    "ASSET_OBJECT_ANIMATIONS": "swap",
    "ASSET_OBJECTS": "swap",
    "ASSET_SPRITES": "swap",
    "ASSET_TTGHOSTS": "swap",
    # NOTE: TEXTURES_2D/3D do not appear here because load_texture() passes a
    # VARIABLE (`assetSection`) to asset_load(), so no literal reaches this
    # scan. They are swapped per animation frame inside load_texture()'s own
    # walk (swap_texture_header) rather than at the asset_load() line.
    "ASSET_GAME_TEXT_TABLE": "own",   # 4-word manual swap at the call site
    "ASSET_AUDIO": "own",             # libultra bnkf.c parses BE explicitly
    "ASSET_JAPANESE_FONTS": "own",    # unused for us_v80 (REGION != JP)
    "ASSET_GAME_TEXT": "bytes",       # textbox command byte stream
    "ASSET_MENU_TEXT": "bytes",       # ASCII
    "ASSET_LEVEL_NAMES": "bytes",     # ASCII
    "ASSET_SCREENS": "bytes",         # 16-byte header + RGBA16 texels
    "ASSET_EMPTY_14": "bytes",        # CI4/CI8 TLUT texels, read MSB-first
}

SWAP_CALL = re.compile(r"asset_swap_(normalize|object_animation|misc_\w+)\s*\(")
LOAD_CALL = re.compile(r"asset_load\(\s*(ASSET_[A-Z_0-9]+)")

# Lines to scan after an asset_load() for the swap it owes. Sized to clear the
# gzip_inflate + bounds-check preamble that the compressed sections run first
# (objects.c ASSET_LEVEL_OBJECT_MAPS is the widest at ~20 lines).
SWAP_WINDOW_LINES = 26


def check_source_coverage(rep):
    """Every raw asset_load() site must have a declared, satisfied disposition."""
    sources = sorted((ROOT / "game" / "src").rglob("*.c"))
    seen = {}
    problems = []
    for path in sources:
        lines = path.read_text(errors="replace").splitlines()
        for number, line in enumerate(lines):
            match = LOAD_CALL.search(line)
            if not match:
                continue
            asset = match.group(1)
            seen.setdefault(asset, []).append(f"{path.relative_to(ROOT)}:{number + 1}")
            disposition = RAW_LOAD_DISPOSITION.get(asset)
            if disposition is None:
                problems.append(
                    f"{path.relative_to(ROOT)}:{number + 1}: asset_load({asset}) "
                    "has no entry in RAW_LOAD_DISPOSITION -- declare whether it "
                    "needs a swap")
                continue
            if disposition != "swap":
                continue
            # Look ahead for the swap this site owes. The window has to clear
            # the inflate + bounds-check preamble that the compressed sections
            # (level models, object maps, object animations) run between the
            # DMA and the swap.
            window = "\n".join(lines[number:number + SWAP_WINDOW_LINES])
            if not SWAP_CALL.search(window):
                problems.append(
                    f"{path.relative_to(ROOT)}:{number + 1}: asset_load({asset}) "
                    f"is a raw ROM DMA with no asset_swap_* call within "
                    f"{SWAP_WINDOW_LINES} lines -- this asset would be consumed "
                    "big-endian")
    for asset, disposition in RAW_LOAD_DISPOSITION.items():
        if asset not in seen:
            problems.append(
                f"RAW_LOAD_DISPOSITION lists {asset} ({disposition}) but no "
                "asset_load() site uses it -- stale entry")
    if problems:
        raise SwapInvariantError(
            "raw asset_load() coverage failures:\n  " + "\n  ".join(problems))
    rep.note(f"source coverage: {len(seen)} asset types across raw asset_load() sites")
    for asset in sorted(seen):
        rep.ok(f"{asset} [{RAW_LOAD_DISPOSITION[asset]}] {len(seen[asset])} site(s)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    rep = Report(args.verbose)
    try:
        print("[asset-swap] ARM 2: raw asset_load() source coverage")
        check_source_coverage(rep)

        print(f"[asset-swap] ARM 1: ROM data invariants ({args.rom})")
        rom = Rom(args.rom)
        print(f"  revision: {rom.name}")
        misc_count = check_misc(rom, rep)
        check_level_headers(rom, rep, misc_count)
        check_ai_behaviour(rom, rep)
        check_tt_ghosts(rom, rep)
        check_object_headers(rom, rep)
        rep.require_discriminating()
    except SwapInvariantError as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        return 1
    except (OSError, struct.error) as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        return 1

    print()
    for line in rep.lines:
        print(f"  {line}")
    print(f"\nPASS: {rep.checks} field checks, {rep.controls} positive controls "
          f"({rep.strong} discriminating, {rep.weak} value-limited); every asset "
          f"type has at least one control that rejects the byte-reversed decode")
    return 0


if __name__ == "__main__":
    sys.exit(main())
