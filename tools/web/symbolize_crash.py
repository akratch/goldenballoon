#!/usr/bin/env python3
"""Turn a browser wasm stack trace back into function names.

What it does
------------
A release wasm carries no name section, so a browser reports a crash as bare
code offsets:

    RuntimeError: memory access out of bounds
        at mdkr64_web.wasm:0xa50b7
        at mdkr64_web.wasm:0xaa247
        at mdkr64_web.wasm:0x128992

This joins those offsets back to function names:

    file offset --(parse the wasm code section)--> function index
    function index --(symbol map)--> name

Inputs
------
A wasm module and the symbol map emitted by the SAME link. `--emit-symbol-map`
is part of the web link flags and tools/web/build_web.sh stages
`mdkr64_web.js.symbols` next to the wasm, so both are available for any
published build.

Guarantees
----------
Both inputs must come from one build: symbolising against a map from a
different link yields plausible but fabricated names, because a changed link
flag reorders function bodies and invalidates every index. Rebuilding to
recover a missing map is therefore only valid when the rebuild's flags match
the shipped link exactly. The script verifies that each offset lands inside a
function body and reports offsets that do not, rather than guessing a name.

Usage
-----
    tools/web/symbolize_crash.py 0xa50b7 0xaa247 0x128992
    tools/web/symbolize_crash.py --wasm old/mdkr64_web.wasm \
        --symbols old/mdkr64_web.js.symbols 0xa50b7
    # or pipe a pasted stack trace and let it find the offsets:
    pbpaste | tools/web/symbolize_crash.py -
"""
import argparse
import os
import re
import sys


def uleb(buf, i):
    result = shift = 0
    while True:
        byte = buf[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, i


def sections(buf):
    """(section_id, payload_start, payload_size) for each top-level section."""
    i = 8                                    # skip \0asm + version
    while i < len(buf):
        sid = buf[i]
        i += 1
        size, payload = uleb(buf, i)
        yield sid, payload, size
        i = payload + size


def function_bodies(buf):
    """[(body_ordinal, start_file_offset, end_file_offset)] in code-section order."""
    out = []
    for sid, off, size in sections(buf):
        if sid != 10:                        # 10 = code
            continue
        count, i = uleb(buf, off)
        for ordinal in range(count):
            body_size, start = uleb(buf, i)
            out.append((ordinal, start, start + body_size))
            i = start + body_size
    return out


def imported_function_count(buf):
    """Function indices are offset by the imports, which come first in the space."""
    for sid, off, size in sections(buf):
        if sid != 2:                         # 2 = import
            continue
        count, i = uleb(buf, off)
        funcs = 0
        for _ in range(count):
            for _ in range(2):               # module name, field name
                n, i = uleb(buf, i)
                i += n
            kind = buf[i]
            i += 1
            if kind == 0:                    # func
                _, i = uleb(buf, i)
                funcs += 1
            elif kind == 1:                  # table
                i += 1
                has_max = buf[i]
                i += 1
                _, i = uleb(buf, i)
                if has_max:
                    _, i = uleb(buf, i)
            elif kind == 2:                  # memory
                has_max = buf[i]
                i += 1
                _, i = uleb(buf, i)
                if has_max:
                    _, i = uleb(buf, i)
            elif kind == 3:                  # global
                i += 2
        return funcs
    return 0


def load_symbols(path):
    """emcc --emit-symbol-map writes `index:name` lines."""
    table = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            idx, name = line.split(":", 1)
            try:
                table[int(idx)] = name
            except ValueError:
                pass
    return table


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_dir = os.path.join(here, "..", "..", "dist", "web")
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("offsets", nargs="+",
                    help="wasm code offsets (0x… or decimal), or '-' to read a trace on stdin")
    ap.add_argument("--wasm", default=os.path.join(default_dir, "mdkr64_web.wasm"))
    ap.add_argument("--symbols", default=os.path.join(default_dir, "mdkr64_web.js.symbols"))
    args = ap.parse_args()

    if args.offsets == ["-"]:
        text = sys.stdin.read()
        args.offsets = re.findall(r"wasm:(0x[0-9a-fA-F]+)", text) or \
                       re.findall(r"0x[0-9a-fA-F]+", text)
        if not args.offsets:
            sys.exit("no wasm offsets found on stdin")

    if not os.path.exists(args.wasm):
        sys.exit("missing wasm: %s (run tools/web/build_web.sh)" % args.wasm)
    buf = open(args.wasm, "rb").read()
    if buf[:4] != b"\0asm":
        sys.exit("not a wasm module: %s" % args.wasm)

    bodies = function_bodies(buf)
    imports = imported_function_count(buf)
    symbols = load_symbols(args.symbols) if os.path.exists(args.symbols) else {}
    if not symbols:
        print("WARNING: no symbol map at %s - indices only.\n"
              "         A map from a LATER build is worthless here; it must come from\n"
              "         the same link as this wasm (see this script's docstring).\n"
              % args.symbols, file=sys.stderr)

    print("%s: %d function bodies, %d imports, %d symbols"
          % (os.path.basename(args.wasm), len(bodies), imports, len(symbols)))
    print("innermost frame first, as the browser prints it:\n")
    for raw in args.offsets:
        addr = int(raw, 16) if raw.lower().startswith("0x") else int(raw)
        hit = next((b for b in bodies if b[1] <= addr < b[2]), None)
        if hit is None:
            print("  0x%-9x  not inside any function body "
                  "(wrong build, or an offset from a different module)" % addr)
            continue
        ordinal, start, end = hit
        index = ordinal + imports
        name = symbols.get(index, "<index %d, no symbol>" % index)
        print("  0x%-9x  %s\n               body #%d (func index %d), +%d of %d bytes"
              % (addr, name, ordinal, index, addr - start, end - start))
    return 0


if __name__ == "__main__":
    sys.exit(main())
