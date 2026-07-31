#!/usr/bin/env python3
"""Dump ASSET_MISC sub-assets straight from the ROM, without running the game.

ASSET_MISC is heterogeneous and is deliberately punted by asset_swap_normalize(),
so each sub-asset is normalized individually by objects.c dkr_misc_normalize_tables()
according to its layout. Classifying a new sub-asset means looking at its raw
big-endian bytes and deciding which swap kind it needs -- that is what this does.

It is an INDEPENDENT decoder: it re-implements asset_table_load()'s ROM lookup
rather than reusing any port code, so agreeing with the game's [TRACE] lines is
real cross-checking, not a tautology.

Usage:
    tools/dump_misc_asset.py baserom.us.v80.z64            # all classified indices
    tools/dump_misc_asset.py baserom.us.v80.z64 21         # one index
    tools/dump_misc_asset.py baserom.us.v80.z64 21 --as u16

Layout (mirrors game/src/asset_loading.c asset_table_load):
  * the asset LUT is a big-endian u32 array at ROM 0x000ED0E0; LUT[0] is the
    section count and section S occupies [LUT[S+1], LUT[S+2]) relative to the
    asset-data base 0x000ED1B0;
  * ASSET_MISC is section 15 and ASSET_MISC_TABLE is section 16;
  * gAssetsMiscTable entries are s32-WORD offsets into the misc section (see
    get_misc_asset), not byte offsets -- so every sub-asset length is a multiple
    of 4, which is why a halfword walk never has a partial tail.
"""
import argparse
import struct
import sys

LUT_START = 0x000ED0E0
DATA_BASE = 0x000ED1B0
ASSET_MISC = 15
ASSET_MISC_TABLE = 16

# Sub-assets whose type is known, and the swap kind objects.c applies.
# Keep in step with the lists in dkr_misc_normalize_tables().
CLASSIFIED = {
    4: ("MISC_4 per-character model scale", "f32", "words"),
    8: ("MISC_8 per-character steer-slide divisor", "f32", "words"),
    9: ("RACER_WEIGHT", "f32", "words"),
    10: ("RACER_HANDLING", "f32", "words"),
    19: ("RUMBLE_DATA {strength,duration} pairs", "u16", "halfwords"),
    20: ("MISC_20 boost/exhaust record", "raw", "per-field + stride copy"),
    21: ("SHIELD_DATA RacerShieldGfx[]", "shield", "per-field, in place"),
    22: ("MAGNET_DATA", "f32", "words"),
    23: ("MISC_23 default lap/course records", "u16", "halfwords"),
    24: ("GHOST_UNLOCK_TIMES staff times", "u16", "halfwords"),
}


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def section(rom, idx):
    start = be32(rom, LUT_START + 4 * (idx + 1))
    end = be32(rom, LUT_START + 4 * (idx + 2))
    return rom[DATA_BASE + start:DATA_BASE + end]


def load(path):
    rom = open(path, "rb").read()
    misc = section(rom, ASSET_MISC)
    raw = section(rom, ASSET_MISC_TABLE)
    table = list(struct.unpack(">%di" % (len(raw) // 4), raw))
    count = table.index(-1) if -1 in table else len(table)
    return misc, table, count


def hexdump(blob, limit=128):
    for r in range(0, min(len(blob), limit), 16):
        row = blob[r:r + 16]
        words = " ".join("%08x" % be32(row, i) for i in range(0, len(row) - 3, 4))
        print("  %04x: %s" % (r, words))
    if len(blob) > limit:
        print("  ... (%d more bytes)" % (len(blob) - limit))


def decode(blob, kind):
    """Print the blob decoded both correctly (big-endian) and as the host would
    read it unswapped -- the second column is what the bug looks like."""
    if kind == "shield":
        n = len(blob) // 16
        print("  -> %d records of 16 bytes (4xs16 + 2xf32):" % n)
        seen = set()
        for i in range(n):
            rec = blob[i * 16:(i + 1) * 16]
            if rec in seen:
                continue
            seen.add(rec)
            x, y, z, yo = struct.unpack_from(">4h", rec, 0)
            sc, ts = struct.unpack_from(">2f", rec, 8)
            bad_sc = struct.unpack_from("<f", rec, 8)[0]
            print("     [%2d] pos=(%d,%d,%d) y_off=%d scale=%.4f turnSpeed=%.4f"
                  "   | unswapped scale=%g" % (i, x, y, z, yo, sc, ts, bad_sc))
        if len(seen) < n:
            print("     (%d distinct record(s); identical ones collapsed)" % len(seen))
    elif kind == "u16":
        n = len(blob) // 2
        good = struct.unpack(">%dH" % n, blob)
        bad = struct.unpack("<%dH" % n, blob)
        print("  -> %d u16, big-endian : %s%s" % (n, list(good[:24]), " ..." if n > 24 else ""))
        print("     unswapped (wrong)  : %s%s" % (list(bad[:24]), " ..." if n > 24 else ""))
        print("     max BE=%d  max unswapped=%d" % (max(good), max(bad)))
    elif kind == "f32":
        n = len(blob) // 4
        good = struct.unpack(">%df" % n, blob)
        bad = struct.unpack("<%df" % n, blob)
        print("  -> %d f32, big-endian : %s" % (n, ["%.4g" % v for v in good[:12]]))
        print("     unswapped (wrong)  : %s" % ["%.4g" % v for v in bad[:12]])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", nargs="?", default="baserom.us.v80.z64")
    ap.add_argument("index", nargs="?", type=int, help="ASSET_MISC_* sub-asset index")
    ap.add_argument("--as", dest="kind", choices=("u16", "f32", "shield", "raw"),
                    help="force a decode for an unclassified index")
    args = ap.parse_args()

    try:
        misc, table, count = load(args.rom)
    except (IOError, OSError) as e:
        sys.exit("cannot read ROM: %s" % e)

    print("misc section = %d bytes, %d sub-assets" % (len(misc), count))
    indices = [args.index] if args.index is not None else sorted(CLASSIFIED)

    for idx in indices:
        if idx + 1 > count:
            print("\n[%d] out of range (table has %d entries)" % (idx, count))
            continue
        name, kind, how = CLASSIFIED.get(idx, ("unclassified", args.kind or "raw", "NOT NORMALIZED"))
        if args.kind:
            kind = args.kind
        wstart, wend = table[idx], table[idx + 1]
        blob = misc[wstart * 4:wend * 4]
        print("\n=== [%d] %s ===  words %d..%d, %d bytes  (swap: %s)"
              % (idx, name, wstart, wend, len(blob), how))
        hexdump(blob)
        if kind != "raw":
            decode(blob, kind)


if __name__ == "__main__":
    main()
