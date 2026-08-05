#!/usr/bin/env python3
"""dist/web/rom-id.js and platform/rom_id.c must declare the same revisions.

Why this exists separately from tests/check_rom_revision.py
-----------------------------------------------------------
check_rom_revision.py is the full gate: it identifies real cartridge dumps, runs
the JS under node against the native binary, and requires character-identical
verdicts. It therefore needs a compiled engine and at least one ROM, and the
publish lane has neither -- the demo is bring-your-own-ROM and nothing
ROM-derived may exist in CI.

The half of that gate which needs neither is the table comparison: both files
declare the same five revisions with the same CRC pair, asset-LUT bounds, ROM end
and supported flag, and a drift there is exactly the failure that let a European
or Japanese cart boot into garbage. This runs that half, from the SAME parser
check_rom_revision.py uses (imported, not copied -- a second copy of those
regexes would be one more thing to drift), so the shell cannot be published with
a revision table the engine does not agree with.

Passing this does NOT mean the two agree at runtime; it means their declared
tables agree. check_rom_revision.py remains the gate for the rest.

Usage: tools/web/check_rom_id_mirror.py [--repo-root .]
Exit 0 = the tables are identical. Exit 1 = they are not, or cannot be parsed.
"""

import argparse
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

EXPECTED_ROWS = 5


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=ROOT)
    args = ap.parse_args()
    root = os.path.abspath(args.repo_root)

    sys.path.insert(0, os.path.join(root, "tests"))
    try:
        from check_rom_revision import C_ROW, JS_ROW, rows
    except ImportError as error:
        print("check_rom_id_mirror: FAIL -- cannot import the shared table "
              "parser from tests/check_rom_revision.py (%s)" % error)
        return 1

    c_path = os.path.join(root, "platform", "rom_id.c")
    js_path = os.path.join(root, "dist", "web", "rom-id.js")
    for path in (c_path, js_path):
        if not os.path.exists(path):
            print("check_rom_id_mirror: FAIL -- missing %s" % path)
            return 1

    c_rows = rows(C_ROW, c_path, "1")
    js_rows = rows(JS_ROW, js_path, "true")

    # A parse that finds nothing must fail, not pass vacuously: a reformatted
    # table would otherwise silently stop being compared at all.
    for label, parsed in (("platform/rom_id.c", c_rows), ("dist/web/rom-id.js", js_rows)):
        if len(parsed) != EXPECTED_ROWS:
            print("check_rom_id_mirror: FAIL -- parsed %d revision rows from %s "
                  "(expected %d). The table format changed and this comparison "
                  "stopped covering it." % (len(parsed), label, EXPECTED_ROWS))
            return 1

    if c_rows != js_rows:
        only_c = [row for row in c_rows if row not in js_rows]
        only_js = [row for row in js_rows if row not in c_rows]
        print("check_rom_id_mirror: FAIL -- the revision tables differ.")
        print("  only in platform/rom_id.c: %s" % only_c)
        print("  only in dist/web/rom-id.js: %s" % only_js)
        return 1

    print("check_rom_id_mirror: PASS -- %d revision rows identical in "
          "platform/rom_id.c and dist/web/rom-id.js" % len(c_rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
