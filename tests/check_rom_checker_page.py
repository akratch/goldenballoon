#!/usr/bin/env python3
"""The hosted ROM checker uploads nothing, and it agrees with the engine.

Why this exists
---------------
`dist/web/rom-check.html` asks a player to hand a 12 MB cartridge dump to a web
page.  Two things have to be true before that is a reasonable thing to ask, and
neither of them is the kind of claim a comment can carry:

  1. **Nothing leaves the machine.**  The page must contain no network call and
     no external asset, so "your ROM is never uploaded" is a property of the
     published file rather than a promise in its prose.  Anything else would be
     indefensible: the file is the player's, and there is no version of this
     feature where sending it somewhere is acceptable.

  2. **It gives the same answer the game gives.**  A checker that says
     "supported" where the binary says "refused" is worse than no checker,
     because it is believed.  `dist/web/rom-id.js` is already the browser mirror
     of `platform/rom_id.c`, and `tests/check_rom_revision.py` compares their
     revision tables row by row and their verdict sentences character for
     character.  The page loads that same file rather than carrying a copy, and
     this check closes the loop over the page's own composition of it.

What is asserted
----------------
  1. NO NETWORK.  The page's text contains no `fetch(`, no `XMLHttpRequest`, no
     `WebSocket`, no `<form action`, no `sendBeacon`, no `EventSource`, no
     `importScripts`, and no `@import`.  Separately, every URL-bearing attribute
     and every CSS `url()` in the file is parsed and required to be
     same-directory relative -- no scheme, no protocol-relative `//host`, no
     absolute path.  That last one is the general guarantee; the literal list
     above is there so the failure names the mechanism.
  2. SELF-CONTAINED.  Every local file the page does reference exists beside it,
     so the published page cannot be a broken shell.
  3. THE LOGIC IS THE MIRROR'S.  The page must load `rom-id.js` rather than
     restate the revision table, and must not declare its own.  Otherwise
     `check_rom_revision.py`'s row-by-row comparison stops covering the copy the
     page actually uses, which is precisely the drift it exists to prevent.
  4. SAME VERDICT AS THE NATIVE BINARY.  For every real cartridge dump found
     under `build/roms/` (and `--rom`), the page's verdict logic is extracted,
     loaded under `node` beside `dist/web/rom-id.js`, run over the dump, and its
     sentence compared to the `[ROM]` line the native binary prints for the same
     file -- character for character, name included.  Both sides are given the
     same relative path as the display name, so the comparison is over the whole
     sentence rather than a suffix of it.
  5. BYTE ORDER.  A synthesised `.v64` and `.n64` of the supported dump must
     produce the same identity and the same whole-image SHA-256 as the `.z64`.
     Without normalisation before hashing, every byteswapped dump would be
     reported as unknown, which is the specific way this page could be quietly
     useless to half the people who try it.

`node` absent SKIPS assertions 4 and 5 with a printed reason, and so does a
machine with no cartridge dump -- this project is bring-your-own-ROM.  1, 2 and
3 are pure text and always run, which matters: they are the ones carrying the
privacy claim, and that claim must be checkable on a machine with no ROM at all.

Usage::

    MDKR_AUDIO=0 python3 tests/check_rom_checker_page.py [--build build]
                         [--rom baserom.us.v80.z64] [--roms build/roms] [-v]

Every engine run is muted and headless.  Exit 0 = pass.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env, test_save_dir

# The revision-locating helpers are IMPORTED, not copied: a second copy of the
# CRC table would be a second thing to keep in step with platform/rom_id.c, and
# the whole subject of this check is copies that drift.
from check_rom_revision import JS_ROW, REVISIONS, resolve_roms, rom_message

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / "dist/web/rom-check.html"
ROM_ID_JS = ROOT / "dist/web/rom-id.js"

# The verdict logic the page and this check share. The page brackets it with
# these markers precisely so it can be lifted out and run headlessly.
LOGIC_BEGIN = "// ==== rom-check verdict logic (BEGIN) ===="
LOGIC_END = "// ==== rom-check verdict logic (END) ===="

# Named mechanisms. Each is a (needle, why) pair so a failure explains itself.
FORBIDDEN = [
    ("fetch(", "a network request"),
    ("XMLHttpRequest", "a network request"),
    ("WebSocket", "a socket to a server"),
    ("<form action", "a form submission"),
    ("sendBeacon", "a fire-and-forget upload"),
    ("EventSource", "a server-sent-event stream"),
    ("importScripts", "loading a script from elsewhere"),
    ("@import", "pulling in a stylesheet"),
]

# Every attribute that can name a resource, plus CSS url(). Parsed rather than
# grepped, so a scheme smuggled into any of them is caught.
URL_ATTR_RE = re.compile(
    r"""\b(src|href|action|data|poster|srcset|formaction)\s*=\s*["']([^"']*)["']""",
    re.IGNORECASE)
CSS_URL_RE = re.compile(r"""url\(\s*['"]?([^'")]+)""", re.IGNORECASE)
# A scheme (http:, https:, blob:, ...), a protocol-relative //host, or an
# absolute path. `data:` and in-page `#fragment` are neither a host nor a file.
REMOTE_RE = re.compile(r"^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//|/)")


def check_no_network(failures: list[str]) -> str:
    print("1. the page contains no network call and no external asset")
    if not PAGE.is_file():
        failures.append("dist/web/rom-check.html is missing")
        print("   FAIL missing %s" % PAGE.relative_to(ROOT))
        return ""
    text = PAGE.read_text(encoding="utf-8")
    for needle, why in FORBIDDEN:
        if needle in text:
            line = next((n for n, l in enumerate(text.splitlines(), 1)
                         if needle in l), 0)
            failures.append(
                "dist/web/rom-check.html contains %r (line %d) - %s. The page "
                "must be provably local-only: asking a player to hand over a "
                "cartridge dump is only acceptable because it never travels"
                % (needle, line, why))
            print("   FAIL %r present" % needle)
            return text
    print("   ok  none of: %s" % ", ".join(n for n, _w in FORBIDDEN))

    remote = []
    local = []
    for _attr, value in URL_ATTR_RE.findall(text):
        (remote if REMOTE_RE.match(value.strip()) else local).append(value.strip())
    for value in CSS_URL_RE.findall(text):
        (remote if REMOTE_RE.match(value.strip()) else local).append(value.strip())
    remote = [v for v in remote if not v.lower().startswith(("data:", "#"))]
    if remote:
        failures.append(
            "dist/web/rom-check.html references %s, which is not a file beside "
            "it. Every reference must be same-directory relative or the page is "
            "not self-contained" % ", ".join(sorted(set(remote))))
        print("   FAIL external reference(s): %s" % ", ".join(sorted(set(remote))))
    else:
        print("   ok  every URL reference is same-directory relative")

    print("2. every file the page references ships beside it")
    missing = []
    for value in sorted(set(local)):
        target = value.split("#", 1)[0].split("?", 1)[0]
        if not target:
            continue
        if not (PAGE.parent / target).exists():
            missing.append(target)
    if missing:
        failures.append("dist/web/rom-check.html references %s, which is not "
                        "published beside it" % ", ".join(missing))
        print("   FAIL missing %s" % ", ".join(missing))
    else:
        print("   ok  %d local reference(s), all present" % len(set(local)))
    return text


def check_uses_the_mirror(text: str, failures: list[str]) -> str:
    print("3. the page uses dist/web/rom-id.js rather than a copy of it")
    if 'src="rom-id.js"' not in text:
        failures.append(
            "dist/web/rom-check.html does not load rom-id.js. It must use the "
            "copy tests/check_rom_revision.py compares against platform/rom_id.c "
            "row by row, or the page's classification stops being covered")
        print("   FAIL rom-id.js not loaded")
    else:
        # Detected with check_rom_revision.py's OWN row parser, so "the page has
        # its own revision table" means exactly what that check means by it.
        copied = JS_ROW.findall(text)
        if copied:
            failures.append(
                "dist/web/rom-check.html carries %d revision row(s) of its own. "
                "That is the second copy this whole arrangement exists to "
                "prevent: check_rom_revision.py compares rom-id.js against "
                "platform/rom_id.c and would not see this one" % len(copied))
            print("   FAIL %d revision row(s) copied into the page" % len(copied))
        else:
            print("   ok  loads rom-id.js, declares no revision row of its own")

    if LOGIC_BEGIN not in text or LOGIC_END not in text:
        failures.append(
            "dist/web/rom-check.html has no delimited verdict-logic block "
            "(%r .. %r), so its logic cannot be run headlessly and assertion 4 "
            "cannot be made" % (LOGIC_BEGIN, LOGIC_END))
        print("   FAIL no extractable logic block")
        return ""
    block = text.split(LOGIC_BEGIN, 1)[1].split(LOGIC_END, 1)[0]
    for dom in ("document.", "window.", "localStorage", "navigator."):
        if dom in block:
            failures.append(
                "the extractable verdict-logic block touches %s, so it cannot be "
                "run outside a browser and this check would stop covering it"
                % dom)
            print("   FAIL logic block touches %s" % dom)
            return ""
    print("   ok  verdict logic is a %d-line DOM-free block" % len(block.splitlines()))
    return block


NODE_DRIVER = r"""
const fs = require("fs");
Object.assign(globalThis, require(process.env.MDKR_ROMID_JS));
if (!globalThis.crypto || !globalThis.crypto.subtle) {
  globalThis.crypto = require("node:crypto").webcrypto;
}
const page = require(process.env.MDKR_PAGE_LOGIC);
const bytes = new Uint8Array(fs.readFileSync(process.env.MDKR_JS_ROM));
page.dkrCheckerReport(bytes, process.env.MDKR_JS_NAME).then((report) => {
  console.log(JSON.stringify(report));
}, (error) => {
  console.error("driver failed: " + (error && error.stack ? error.stack : error));
  process.exit(2);
});
"""


def page_report(node: str, logic_js: Path, driver_js: Path, rom: Path,
                name: str) -> dict:
    """Run the page's own verdict logic over `rom`, under node."""
    env = dict(os.environ)
    env["MDKR_ROMID_JS"] = str(ROM_ID_JS)
    env["MDKR_PAGE_LOGIC"] = str(logic_js)
    env["MDKR_JS_ROM"] = str(rom)
    env["MDKR_JS_NAME"] = name
    done = subprocess.run([node, str(driver_js)], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=300)
    if done.returncode != 0:
        raise RuntimeError(done.stderr.decode("utf-8", "replace").strip()
                           or "node exited %d" % done.returncode)
    return json.loads(done.stdout.decode("utf-8", "replace"))


def native_message(build: str, rom: Path, verbose: bool) -> str:
    """The `[ROM]` verdict line the binary prints for `rom`."""
    # Built from a clean base for the same reason every sibling check does: an
    # inherited MDKR_* would change what is being compared.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    save_env(env, test_save_dir())
    env["MDKR_AUDIO"] = "0"     # belt-and-braces; --headless-frames is the guarantee
    command = [build, "--headless-frames", "20", "--rom", str(rom)]
    if verbose:
        print("      $ %s" % " ".join(command))
    done = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=300)
    return rom_message(done.stdout.decode("utf-8", "replace"))


def check_verdicts(args, logic_js: Path, driver_js: Path, node: str,
                   failures: list[str]) -> None:
    print("4. the page's verdict equals the native binary's, character for character")
    seen = 0
    for short, _phrase, _supported in REVISIONS:
        rom = args.found.get(short)
        if not rom:
            print("   %-8s SKIP (not present under %s)" % (short, args.roms))
            continue
        seen += 1
        # Both sides are handed the SAME display name -- the path as given, which
        # is also what the binary is passed as --rom -- so the two sentences are
        # comparable in full and not merely from " is " on.
        name = str(rom)
        try:
            report = page_report(node, logic_js, driver_js, Path(rom), name)
        except Exception as error:          # noqa: BLE001 - reported, not raised
            failures.append("%s: the page's verdict logic failed under node: %s"
                            % (short, error))
            print("   %-8s FAIL node error" % short)
            continue
        native = native_message(args.build, Path(rom), args.verbose)
        if report["sentence"] != native:
            failures.append(
                "%s: the page and the binary disagree.\n     native: %s\n"
                "     page:   %s" % (short, native, report["sentence"]))
            print("   %-8s FAIL verdicts differ" % short)
            continue
        extra = ""
        if report["status"] == "refused":
            if not report["changes"]:
                failures.append(
                    "%s is refused but the page does not say what would have to "
                    "change. A player with an unsupported cartridge must be able "
                    "to tell a permanent no from a not-yet" % short)
                print("   %-8s FAIL refusal says nothing about what is missing" % short)
                continue
            extra = ", %d thing(s) named as missing" % len(report["changes"])
        if report["shaState"] == "verified":
            extra += ", whole-image SHA-256 verified"
        print("   %-8s ok  same sentence%s" % (short, extra))
    if seen == 0:
        failures.append(
            "no cartridge dump was found under %s or at --rom %s, so the "
            "page-versus-binary comparison would have passed vacuously"
            % (args.roms, args.rom))


def check_byte_order(args, logic_js: Path, driver_js: Path, node: str,
                     failures: list[str], workdir: Path) -> None:
    print("5. a synthesised .v64 / .n64 is normalised before hashing")
    source = Path(args.rom)
    if not source.is_file():
        source = ROOT / args.rom
    if not source.is_file():
        print("   SKIP (no %s)" % args.rom)
        return
    raw = source.read_bytes()
    z = bytearray(raw)
    v = bytearray(raw)
    v[0::2], v[1::2] = z[1::2], z[0::2]
    n = bytearray(len(z))
    n[0::4], n[1::4], n[2::4], n[3::4] = z[3::4], z[2::4], z[1::4], z[0::4]
    assert bytes(v[:4]) == b"\x37\x80\x40\x12", v[:4].hex()
    assert bytes(n[:4]) == b"\x40\x12\x37\x80", n[:4].hex()

    try:
        base = page_report(node, logic_js, driver_js, source, "rom.z64")
        for tag, buf in (("v64", v), ("n64", n)):
            # Written under the check's own temporary directory and removed with
            # it; nothing ROM-derived is ever left in the tree.
            path = workdir / ("synth." + tag)
            path.write_bytes(bytes(buf))
            try:
                report = page_report(node, logic_js, driver_js, path,
                                     "rom." + tag)
            finally:
                path.unlink(missing_ok=True)
            if report["order"] != tag:
                failures.append(".%s was not recognised as a byteswapped image "
                                "(order=%s)" % (tag, report["order"]))
                print("   %-4s FAIL order=%s" % (tag, report["order"]))
                continue
            if report["decompBuild"] != base["decompBuild"]:
                failures.append(
                    ".%s identified as %s where the .z64 identified as %s - the "
                    "page did not convert it before reading the header"
                    % (tag, report["decompBuild"], base["decompBuild"]))
                print("   %-4s FAIL identified as %s" % (tag, report["decompBuild"]))
                continue
            if report["sha256"] != base["sha256"]:
                failures.append(
                    ".%s hashed to a different whole-image SHA-256 than the .z64 "
                    "- it was identified but hashed in the wrong byte order, so "
                    "every byteswapped dump would read as damaged" % tag)
                print("   %-4s FAIL hash differs" % tag)
                continue
            print("   %-4s ok  converted, same release and same SHA-256 as .z64"
                  % tag)
    except Exception as error:              # noqa: BLE001 - reported, not raised
        failures.append("the byte-order comparison failed under node: %s" % error)
        print("   FAIL node error")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64",
                    help="the SUPPORTED dump; the byteswapped inputs derive from it")
    ap.add_argument("--roms", default="build/roms",
                    help="directory of cartridge dumps; any absent one is skipped")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    args.build = resolve_binary(args.build)

    failures: list[str] = []
    text = check_no_network(failures)
    block = check_uses_the_mirror(text, failures) if text else ""

    node = shutil.which("node")
    workdir = Path(tempfile.mkdtemp(prefix="mdkr_romcheck_"))
    try:
        if not block:
            print("4. SKIP - no verdict-logic block to run")
            print("5. SKIP - no verdict-logic block to run")
        elif not os.path.exists(args.rom):
            # Bring-your-own-ROM, the same contract every sibling ROM check
            # keeps: a machine with no dump still runs the three text
            # assertions, which are the ones that carry the privacy claim.
            print("4. SKIP - no ROM at %s (bring-your-own-ROM)" % args.rom)
            print("5. SKIP - no ROM at %s (bring-your-own-ROM)" % args.rom)
        elif not node:
            print("4. SKIP - node is not installed, so the page's logic cannot be "
                  "run; the text assertions above still hold")
            print("5. SKIP - node is not installed")
        elif not os.path.exists(args.build):
            print("4. SKIP - no build at %s" % args.build)
            print("5. SKIP - no build at %s" % args.build)
        else:
            # The extracted block plus one export line. Nothing is rewritten: the
            # bytes under test are the bytes the page ships.
            logic_js = workdir / "page_logic.js"
            logic_js.write_text(
                block + "\nmodule.exports = { dkrCheckerReport, "
                        "dkrCheckerRequirements };\n", encoding="utf-8")
            driver_js = workdir / "driver.js"
            driver_js.write_text(NODE_DRIVER, encoding="utf-8")
            args.found = resolve_roms(args.rom, args.roms)
            for name in sorted(args.found):
                print("   found %-8s %s" % (name, os.path.basename(args.found[name])))
            check_verdicts(args, logic_js, driver_js, node, failures)
            check_byte_order(args, logic_js, driver_js, node, failures, workdir)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print()
    if failures:
        for failure in failures:
            print("check_rom_checker_page: FAIL - %s" % failure)
        print("check_rom_checker_page: FAIL (%d)" % len(failures))
        return 1
    print("check_rom_checker_page: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
