#!/usr/bin/env python3
"""The 26 wave-visibility slots must be ONE contiguous run in the wasm build.

The bug this locks down
-----------------------
The first crash ever reported by a player on the browser build:

    RuntimeError: memory access out of bounds
        at mdkr64_web.wasm:0xa50b7      <- initialise_player_viewport_vars
        at mdkr64_web.wasm:0xaa247      <- render_scene
        at mdkr64_web.wasm:0x128992     <- main   (Asyncify / doRewind)

`waves.c` treats `D_8012A5E8[2]` and `D_8012A600[24]` as ONE 26-entry table.
`waves_visibility()` sentinel-clears all 26 slots (its reset loop names both
arrays), then appends every visible wave block through `D_8012A5E8[var_a3]` with
var_a3 measured up to 25 -- indexing the first array straight on into the second.
`func_800B92F4()` and `func_800B97A8()` then walk
`for (k = 0; D_8012A5E8[k].blockID != -1; k++)` across the same 26 slots.

As two separate C objects that only holds if the linker places them adjacently,
and whether it does is luck. Mach-O/arm64 got lucky (gap 0, which is why every
native fixture passed and ASan found nothing -- ASan does not redzone
`__DATA,__common` symbols either). wasm-ld did not: it starts D_8012A5E8 at
16-byte alignment, so its 24 bytes end at 8 mod 16 and the 288-byte D_8012A600 is
padded up to the next 16 -- an 8-BYTE HOLE. Slots 2..25 then land 8 bytes below the
slots that were sentinel-cleared, the `blockID != -1` walk never reliably
terminates, k runs off the table, and func_800B92F4()'s
`D_800E3178[D_8012A5E8[k].unk8]` loads through a garbage index. That load
(`i32.load8_s`) is the faulting instruction at 0xa50b7.

Measured in the exact wasm the player ran (source 63c5d32):
    &D_8012A5E8     = 231504
    &D_8012A5E8[2]  = 231528      <- where slots 2..25 were written
    &D_8012A600     = 231536      <- where the sentinels were cleared   (gap 8)

What is asserted
----------------
Not the addresses -- those move with every link. The *shape*: the sentinel-clearing
stores that `waves_visibility()`'s reset loop emits (the compiler unrolls it into 26
literal `i32.const ADDR; i32.const 65535; i32.store16` sequences) must form a
single arithmetic run of exactly 26 addresses with stride sizeof(unk8012A5E8) == 12
and no gap anywhere in it.

  * broken  -> two runs, len 2 and len 24, separated by 8 bytes  => FAIL
  * fixed   -> one run, len 26, stride 12                        => PASS

Fails closed: if the 26 stores cannot be located at all the check FAILS rather
than passing vacuously, because "found nothing" cannot distinguish a fixed build
from a build whose codegen moved.

Verified in both directions
---------------------------
    build-web-base (fix reverted) : run len=2 232688..232700 | GAP 8 | run len=24 232720..232996  FAIL
    build-web-fix  (fix applied)  : run len=26 232680..232980                                     PASS
    the shipped wasm that actually crashed a player (source 63c5d32, md5
    348a9d80edbef2a3d0b60c5cef9885f9): run len=2 231504..231516 | GAP 8 | run len=24  FAIL

This check reads a built artifact, so it needs no ROM and opens no audio device.

Usage:
    tests/check_wave_visible_table.py
    tests/check_wave_visible_table.py --wasm build-web-base/mdkr64_web.wasm
    tests/check_wave_visible_table.py -v

Exit 0 = the table is contiguous; exit 1 = it is split (or could not be located).
"""
import argparse
import os
import sys

# sizeof(unk8012A5E8) -- s16 blockID, s16 unk2, s16 unk4, s16 unk6, s32 unk8.
SLOT_STRIDE = 12
# D_8012A5E8[2] + D_8012A600[24], as waves_visibility()'s reset loop enumerates them.
SLOT_COUNT = 26

# i32.const 65535 ; i32.store16 align=1 offset=0
# (the reset loop stores -1 into an s16 field, which emits as the constant 65535)
STORE_SENTINEL = bytes([0x41, 0xFF, 0xFF, 0x03, 0x3B, 0x01, 0x00])

DEFAULT_WASM = ["build-web/mdkr64_web.wasm", "dist/web/mdkr64_web.wasm"]


def uleb(buf, i):
    result = shift = 0
    while True:
        b = buf[i]
        i += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return result, i


def sleb(buf, i):
    result = shift = 0
    while True:
        b = buf[i]
        i += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40 and shift < 64:
                result -= 1 << shift
            return result, i


def code_section(buf):
    """(start, end) file offsets of the code section payload."""
    i = 8                                        # skip \0asm + version
    while i < len(buf):
        sid = buf[i]
        i += 1
        size, payload = uleb(buf, i)
        if sid == 10:                            # 10 = code
            return payload, payload + size
        i = payload + size
    return None, None


def sentinel_store_addresses(buf):
    """Every ADDR in an `i32.const ADDR; i32.const 65535; i32.store16 0` sequence."""
    start, end = code_section(buf)
    if start is None:
        return []
    out = []
    i = start
    while i < end:
        if buf[i] == 0x41:                       # i32.const
            try:
                val, j = sleb(buf, i + 1)
            except IndexError:
                break
            if val > 0 and buf[j:j + len(STORE_SENTINEL)] == STORE_SENTINEL:
                out.append(val)
                i = j + len(STORE_SENTINEL)
                continue
        i += 1
    return sorted(set(out))


def stride_runs(addrs, stride=SLOT_STRIDE):
    """Maximal runs of addresses spaced exactly `stride` apart."""
    runs = []
    cur = []
    for a in addrs:
        if cur and a - cur[-1] == stride:
            cur.append(a)
        else:
            if cur:
                runs.append(cur)
            cur = [a]
    if cur:
        runs.append(cur)
    return runs


def check(path, verbose=False):
    buf = open(path, "rb").read()
    if buf[:4] != b"\0asm":
        print("  FAIL: %s is not a wasm module" % path)
        return False

    addrs = sentinel_store_addresses(buf)
    runs = [r for r in stride_runs(addrs) if len(r) >= 2]
    if verbose:
        print("  %d sentinel-store address(es), %d stride-%d run(s) of length >= 2"
              % (len(addrs), len(runs), SLOT_STRIDE))
        for r in runs:
            print("    run len=%-3d %d .. %d" % (len(r), r[0], r[-1]))

    full = [r for r in runs if len(r) == SLOT_COUNT]
    if len(full) == 1:
        r = full[0]
        span = r[-1] - r[0]
        if span != (SLOT_COUNT - 1) * SLOT_STRIDE:
            print("  FAIL: run of %d spans %d bytes, expected %d"
                  % (SLOT_COUNT, span, (SLOT_COUNT - 1) * SLOT_STRIDE))
            return False
        print("  PASS: the %d wave-visibility slots are one contiguous run, "
              "%d..%d (stride %d)" % (SLOT_COUNT, r[0], r[-1], SLOT_STRIDE))
        return True

    # Not one run of 26. Diagnose the split we know this bug produces.
    if not runs:
        print("  FAIL: could not locate waves_visibility()'s sentinel-clearing stores")
        print("        (expected %d `i32.const ADDR; i32.const 65535; i32.store16 0`"
              % SLOT_COUNT)
        print("         sequences). Codegen may have changed shape -- this check fails")
        print("         closed rather than pass without having verified anything.")
        return False

    print("  FAIL: the %d wave-visibility slots are NOT contiguous" % SLOT_COUNT)
    for r in runs:
        print("        run len=%-3d %d .. %d" % (len(r), r[0], r[-1]))
    for x, y in zip(runs, runs[1:]):
        gap = y[0] - (x[-1] + SLOT_STRIDE)
        if gap:
            print("        GAP of %d byte(s) between %d and %d -- "
                  "D_8012A600 is not at &D_8012A5E8[2]" % (gap, x[-1], y[0]))
    print("        waves_visibility() clears sentinels at the higher run while")
    print("        appending slots 2..25 at the lower one, so the")
    print("        `blockID != -1` walk in func_800B92F4() runs off the table.")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wasm", help="wasm to inspect (default: first of %s)"
                    % ", ".join(DEFAULT_WASM))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if args.wasm:
        path = args.wasm
    else:
        path = next((p for p in (os.path.join(root, d) for d in DEFAULT_WASM)
                     if os.path.exists(p)), None)
        if path is None:
            sys.exit("check_wave_visible_table: no wasm found (tried %s).\n"
                     "Build it with tools/web/build_web.sh, or pass --wasm PATH."
                     % ", ".join(DEFAULT_WASM))
    if not os.path.exists(path):
        sys.exit("check_wave_visible_table: no wasm at %s" % path)

    print("check_wave_visible_table: %s" % path)
    ok = check(path, args.verbose)
    print("check_wave_visible_table: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
