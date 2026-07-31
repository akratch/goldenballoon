#!/usr/bin/env python3
"""What object sits next to this global — on LP64 Mach-O, and on wasm32?

The split-array bug class (docs/OPEN_ITEMS.md "wavetable", "gTrackSelectIDs") turns
on exactly one question: when the decomp indexes past the end of a global array,
which *other* object does it hit? The answer differs per target, and the two
answers are what you need before deciding whether an overrun is load-bearing,
harmless, or fatal.

    tools/compare_data_layout.py gTrackSelectIDs gFFLUnlocked
    tools/compare_data_layout.py --pair gTrackSelectIDs,gFFLUnlocked
    tools/compare_data_layout.py --diff-adjacency | head -40
    tools/compare_data_layout.py --stats

Where the numbers come from
---------------------------
native  `nm -n build/mdkr64`. Mach-O carries no symbol sizes, so what is printed is
        the distance to the next symbol, which is size + padding.
wasm32  a wasm-ld link map, which DOES carry sizes. emcc does not emit one by
        default, so this tool re-links build-web's existing objects with
        `-Wl,--Map=`, reading the link command straight out of
        build-web/CMakeFiles/mdkr64_web.dir/link.txt so nothing can drift.
        **It then checks that the relinked wasm is byte-identical to
        build-web/mdkr64_web.wasm** and refuses to print anything if it is not —
        otherwise the map would describe a module nobody runs. (The web build is
        bit-reproducible, and keeping it that way is what makes this work.)

Prerequisites: `cmake --build build` and `tools/web/build_web.sh` must both have
run. Nothing here executes the game, so no ROM and no audio device are involved.
"""
import argparse
import hashlib
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIVE = os.path.join(ROOT, "build", "mdkr64")
WEBDIR = os.path.join(ROOT, "build-web")
LINKTXT = os.path.join(WEBDIR, "CMakeFiles", "mdkr64_web.dir", "link.txt")
WEBWASM = os.path.join(WEBDIR, "mdkr64_web.wasm")


def md5(path):
    with open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def native_symbols():
    """[(addr, name)] sorted, data/bss only."""
    if not os.path.exists(NATIVE):
        sys.exit("compare_data_layout: no %s — run `cmake --build build`" % NATIVE)
    out = subprocess.run(["nm", "-n", NATIVE], capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]{16}) ([A-Za-z]) _?(\S+)$", line)
        if m and m.group(2).upper() in ("D", "B", "S", "C"):
            rows.append((int(m.group(1), 16), m.group(3)))
    rows.sort()
    return rows


def wasm_map(verbose=False):
    """Produce (and validate) a wasm-ld link map for the CURRENT build-web wasm."""
    for p in (LINKTXT, WEBWASM):
        if not os.path.exists(p):
            sys.exit("compare_data_layout: no %s — run tools/web/build_web.sh" % p)
    cmd = shlex.split(open(LINKTXT).read().strip())
    tmp = tempfile.mkdtemp(prefix="mdkr_layoutmap_")
    mappath = os.path.join(tmp, "web.map")
    out = os.path.join(tmp, "mdkr64_web.js")
    # rewrite the -o target and add the map flag; everything else is verbatim
    for i, a in enumerate(cmd):
        if a == "-o":
            cmd[i + 1] = out
    cmd.insert(1, "-Wl,--Map=%s" % mappath)
    r = subprocess.run(cmd, cwd=WEBDIR, capture_output=True, text=True)
    relinked = os.path.join(tmp, "mdkr64_web.wasm")
    if not os.path.exists(mappath) or not os.path.exists(relinked):
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit("compare_data_layout: the map re-link produced no map/wasm.")
    if md5(relinked) != md5(WEBWASM):
        sys.exit("compare_data_layout: the map re-link is NOT byte-identical to\n"
                 "  %s\n"
                 "so the map does not describe the module that ships. Refusing to\n"
                 "print layout that might be wrong. Rebuild with "
                 "tools/web/build_web.sh and retry." % WEBWASM)
    if verbose:
        print("# wasm map validated against %s (md5 %s)" % (WEBWASM, md5(WEBWASM)))
    return mappath


def wasm_symbols(mappath):
    """[(addr, name, section, size)] sorted."""
    rows, cur = [], None
    for line in open(mappath):
        parts = line.split()
        if len(parts) != 4:
            continue
        if not re.fullmatch(r"[0-9a-f]+", parts[0]) or \
           not re.fullmatch(r"[0-9a-f]+", parts[2]):
            continue
        addr, size, rest = int(parts[0], 16), int(parts[2], 16), parts[3]
        m = re.match(r"^.*:\((\.(?:data|bss|rodata)[^)]*)\)$", rest)
        if m:
            cur = m.group(1).split(".")[1]
            continue
        if cur and re.fullmatch(r"[A-Za-z_.$][\w.$]*", rest):
            rows.append((addr, rest, cur, size))
            cur = None
    rows.sort()
    return rows


def show(name, nrows, wrows, ctx):
    print("### %s" % name)
    for tag, rows in (("native", nrows), ("wasm32", wrows)):
        idx = [i for i, r in enumerate(rows) if r[1] == name]
        if not idx:
            print("  %-7s NOT FOUND" % tag)
            continue
        i = idx[0]
        print("  %s:" % tag)
        for r in rows[max(0, i - ctx):i + ctx + 1]:
            mark = "  <<<" if r[1] == name else ""
            if len(r) == 4:
                print("    %#010x  %-34s %-6s size=%-6d next=+%d%s"
                      % (r[0], r[1], r[2], r[3], r[3], mark))
            else:
                nxt = rows[rows.index(r) + 1][0] - r[0] if rows.index(r) + 1 < len(rows) else 0
                print("    %#010x  %-34s        to-next=%d%s" % (r[0], r[1], nxt, mark))
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("symbols", nargs="*", help="globals to show neighbours for")
    ap.add_argument("--pair", action="append", default=[], metavar="A,B",
                    help="print A->B distance on both targets (0 gap == adjacent)")
    ap.add_argument("--diff-adjacency", action="store_true",
                    help="every same-neighbour pair whose spacing differs per target")
    ap.add_argument("--stats", action="store_true",
                    help="how much the two layouts disagree, in one line")
    ap.add_argument("--context", type=int, default=2)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    nrows = native_symbols()
    wrows = wasm_symbols(wasm_map(args.verbose))
    nat = {r[1]: r[0] for r in nrows}
    was = {r[1]: (r[0], r[3]) for r in wrows}

    for spec in args.pair:
        a, b = spec.split(",")
        for tag, tbl in (("native", nat), ("wasm32", was)):
            if a not in tbl or b not in tbl:
                print("%-7s %s -> %s   MISSING: %s" % (tag, a, b, ", ".join(
                    x for x in (a, b) if x not in tbl)))
                continue
            pa = tbl[a][0] if isinstance(tbl[a], tuple) else tbl[a]
            pb = tbl[b][0] if isinstance(tbl[b], tuple) else tbl[b]
            print("%-7s %-30s @%#010x -> %-30s @%#010x   delta=%d"
                  % (tag, a, pa, b, pb, pb - pa))
        print()

    if args.diff_adjacency or args.stats:
        nnext = {nrows[i][1]: (nrows[i + 1][1], nrows[i + 1][0] - nrows[i][0])
                 for i in range(len(nrows) - 1)}
        wnext = {wrows[i][1]: (wrows[i + 1][1], wrows[i + 1][0] - wrows[i][0],
                               wrows[i][3]) for i in range(len(wrows) - 1)}
        same = diff = 0
        for name in sorted(set(nnext) & set(wnext)):
            nn, nsp = nnext[name]
            wn, wsp, sz = wnext[name]
            if sz == 0 or nn != wn:
                continue          # different neighbour == nothing to compare
            same += 1
            if nsp != wsp:
                diff += 1
                if args.diff_adjacency:
                    print("DIFF %-32s -> %-32s size=%-6d native=+%-6d wasm=+%d"
                          % (name, nn, sz, nsp, wsp))
        if args.stats:
            print("%d data pairs have the SAME neighbour on both targets; "
                  "%d of them (%.0f%%) sit at a DIFFERENT distance."
                  % (same, diff, 100.0 * diff / max(1, same)))
            print("That is the reason this bug class exists: adjacency on LP64 "
                  "predicts nothing about wasm32.")

    for s in args.symbols:
        show(s, nrows, wrows, args.context)
    return 0


if __name__ == "__main__":
    sys.exit(main())
