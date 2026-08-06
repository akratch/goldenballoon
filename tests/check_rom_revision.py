#!/usr/bin/env python3
"""ROM identification + byte-order normalisation must hold, on both sides.

Why this exists
---------------
The ROM gate used to be "12 MB, N64 magic, internal title contains 'diddy'".
ALL FIVE released Diddy Kong Racing revisions pass all three, and each one keeps
its master asset lookup table at a DIFFERENT ROM offset (from the decomp's own
ver/splat/dkr.*.yaml: us.v77 0xECB60, pal.v77 0xECBF0, jpn.v79 0xEE5D0,
us.v80 0xED0E0, pal.v80 0xED170). So handing this build a European or Japanese
cart did not produce an error -- it read the LUT from the wrong offset and
carried on. Measured, before platform/rom_id.c existed:

    us.v77   entry count reads 3,886,268,156 -> derived ASSET_AUDIO_TABLE size
             -453,982,075 -> SIGSEGV in sw32_arr (asset_swap_lut)
    pal.v77  entry count reads   830,361,298 -> size -1,327,275,831 -> same SIGSEGV
    pal.v80  entry count reads 0 -> 0 < 38 so asset_table_load() returns NULL ->
             SIGSEGV in audio_init dereferencing addrPtr[ASSET_AUDIO_2]
    jpn.v79  entry count reads 0 -> the same NULL path

Four SIGSEGVs before the first rendered frame, from a gate that said the ROM was
fine. That is HANDOFF.md section 3's dominant shape: no diagnostic, no useful
error. The fix is to identify the revision (header CRC1/CRC2 first, then media +
cart id + country + revision nibble), point the loader at THAT revision's LUT, and
refuse the revisions this build is not validated for, NAMING them.
docs/ROM_REVISIONS.md has the full per-revision taxonomy.

Two revisions are supported: us.v80 and pal.v80. pal.v80's entire claim to support
is that its asset payload is BYTE-IDENTICAL to us.v80's -- all 50 LUT-delimited
sections hash the same -- so assertion 5 below requires it to render and drive
byte-identically. If that ever stops holding, the premise was wrong and the
support claim has to be withdrawn, which is why it is a check and not a comment.

The second half is byte order. The browser shell advertised .v64/.n64 support by
magic while neither side said whether anything CONVERTED them (rom_io.c did; the
shell did not know, and persisted the swapped bytes to IDBFS). Accepting a
byteswapped image without converting it is worse than rejecting it -- every asset
would be read through a transposition, which in this codebase means silent
denormals, not a crash. So both sides now normalise, and this check proves the
converted images are not merely accepted but behave IDENTICALLY.

What is asserted
----------------
  1. IDENTIFICATION, per revision actually present (any absent one SKIPS, so this
     check still passes on a machine with only the supported ROM):
       - us.v80 and pal.v80 are accepted, and the accept path NAMES the revision
       - every other revision is refused CLEANLY (exit 1, no crash trace, no
         renderer bring-up) and the message names that revision AND the revisions
         this build does support. "Clean" matters: an earlier version of this check
         accepted any non-zero exit, so a build whose revision gate was broken
         still passed -- it accepted the ROM, SIGSEGV'd in asset_swap_lut, and the
         signal exit read as a refusal.
  2. A non-DKR N64 ROM is refused and said to be non-DKR. Synthesised at runtime
     from the supported ROM by rewriting the cart id and title, then deleted.
  3. BYTE ORDER: a synthesised .v64 and .n64 boot and race BYTE-IDENTICALLY to the
     .z64 -- same frame set, same frame hashes, same racer trajectory. Synthesised
     at runtime under a git-ignored path and deleted afterwards.
  4. BOTH SIDES AGREE: dist/web/rom-id.js is the browser mirror of
     platform/rom_id.c. Their revision TABLES are parsed and compared row by row,
     and (when node is available) the JS is run over each real ROM and its verdict
     string must match the native binary's, character for character. Without this
     the "mirrors rom_io.c" comment is just a claim that rots.
  5. Every supported revision other than us.v80 drives the same trajectory over
     the same race and selects its authored source-video standard. PAL rendering
     is not required to be byte-identical to NTSC: the game intentionally uses
     osTvType for region-specific presentation and timing.

A note on how this check was made honest
----------------------------------------
An earlier revision compared only the INTERSECTION of the two frame sets. A run
that died after one (black) frame therefore "matched" its healthy counterpart:
the deliberately-corrupted positive control reported "1/1 identical, 0 differ"
while the process had actually SIGSEGV'd. A differing frame COUNT is now a
failure in its own right, and every comparison asserts a minimum frame count, so
a crash can never read as agreement.

Verified in both directions -- see docs/ROM_REVISIONS.md for the pasted output of
each. With rom_id.c's revision check reverted, assertions 1 and 2 fail (every
revision is accepted); with dkr_rom_normalize_byte_order()'s two swap loops
removed, assertion 3 fails on the frame COUNT (the .v64/.n64 runs die early).

Usage:
    tests/check_rom_revision.py [--build build] [--rom baserom.us.v80.z64]
                                [--roms build/roms] [--frames 4300] [-v]

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md. Exit 0 = pass; exit 1 = at least one assertion failed.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from harness_utils import (DEFAULT_BUILD_DIR, resolve_binary, save_env,
                           test_save_dir)

# Product scope is deliberately frozen to the two revision-1 asset layouts.
# Keep this declaration separate from the per-ROM metadata below: the check must
# fail if a table edit silently broadens or narrows the supported retail set.
SUPPORTED_REVISIONS = frozenset({"us.v80", "pal.v80"})
SUPPORTED_ROM_ENDS = {
    "us.v80": 0xB8CFD0,
    "pal.v80": 0xB8D060,
}

# Canonical short names, the phrase each revision's refusal must contain, and
# the runtime policy expected for that digest.
REVISIONS = [
    ("us.v80",  "US 1.1",   True),
    ("pal.v80", "European", True),   # asset payload byte-identical to us.v80
    ("us.v77",  "US 1.0",   False),
    ("pal.v77", "European", False),
    ("jpn.v79", "Japanese", False),
]

declared_supported = {short_name for short_name, _label, supported in REVISIONS if supported}
if declared_supported != SUPPORTED_REVISIONS:
    raise AssertionError(
        "revision metadata does not match the frozen product scope: "
        f"metadata={sorted(declared_supported)}, scope={sorted(SUPPORTED_REVISIONS)}"
    )
if set(SUPPORTED_ROM_ENDS) != SUPPORTED_REVISIONS:
    raise AssertionError("every supported revision must own one checksum extent")

# Supported revisions must not merely load — they must render and drive
# IDENTICALLY. pal.v80's whole claim to support is that its asset payload is
# byte-identical to us.v80's, so a divergence here means that claim is wrong.
PARITY_REVISIONS = ["pal.v80"]

# CRC1/CRC2 from each revision's header, so a ROM is located by CONTENT rather than
# by filename. Requiring build/roms/<build>.z64 meant a correctly-dumped ROM under
# its release name -- "Diddy Kong Racing (Europe) (En,Fr,De) (Rev 1).z64", which is
# what a real dump is actually called -- was silently SKIPped, and the check then
# failed for finding nothing. Identify the way platform/rom_id.c does: read the
# header. (These 8 bytes per revision are identification, not game content.)
ROM_CRCS = {
    (0x53D440E7, 0x7519B011): "us.v77",
    (0xFD73F775, 0x9724755A): "pal.v77",
    (0x7435C9BB, 0x39763CF4): "jpn.v79",
    (0xE402430D, 0xD2FCFC9D): "us.v80",
    (0x596E145B, 0xF7D9879F): "pal.v80",
}


def rom_crcs(path):
    """(crc1, crc2) from an N64 header, normalising the two swapped byte orders."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x18)
    except OSError:
        return None
    if len(head) < 0x18:
        return None
    magic = head[:4]
    if magic == b"\x80\x37\x12\x40":
        pass
    elif magic == b"\x37\x80\x40\x12":                       # .v64, 16-bit swapped
        head = bytes(b for i in range(0, len(head), 2) for b in (head[i + 1], head[i]))
    elif magic == b"\x40\x12\x37\x80":                       # .n64, 32-bit swapped
        head = bytes(b for i in range(0, len(head), 4)
                     for b in reversed(head[i:i + 4]))
    else:
        return None
    return (int.from_bytes(head[0x10:0x14], "big"),
            int.from_bytes(head[0x14:0x18], "big"))


def resolve_roms(*dirs):
    """{build_name: path} for every DKR revision found in `dirs`, by header CRC.
    First match wins, so an explicitly named ROM takes precedence."""
    found = {}
    for d in dirs:
        if not d:
            continue
        if os.path.isfile(d):
            entries = [d]
        elif os.path.isdir(d):
            entries = [os.path.join(d, n) for n in sorted(os.listdir(d))]
        else:
            continue
        for path in entries:
            if not os.path.isfile(path):
                continue
            if os.path.getsize(path) != 12 * 1024 * 1024:
                continue
            name = ROM_CRCS.get(rom_crcs(path) or ())
            if name and name not in found:
                found[name] = path
    return found


# The race route is the one with real forward progress; at 4300 frames the clock
# is past the countdown and the kart has crossed 8 checkpoints (measured).
RACE_SCRIPT = "tests/input_scripts/race_drive_long.txt"
LANGUAGE_SCRIPT = "tests/input_scripts/options_language_cycle.txt"
MIN_FRAMES = 5          # dumping every 900 over 4300 frames
DUMP_EVERY = 900


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def run_engine(build, rom, frames, script=None, dumpdir=None, extra_env=None,
               verbose=False):
    """One muted, headless run. Returns (returncode, combined output)."""
    # Every sibling check builds its engine environment from a clean base: an
    # inherited MDKR_* (a trace, a fault injection, a cadence override) would
    # otherwise silently change what this gate measures. The save directory is
    # the one inherited value that must survive, and it is re-established from
    # the shared harness contract rather than from a stray variable.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    save_env(env, test_save_dir())
    env["MDKR_AUDIO"] = "0"        # belt-and-braces; --headless-frames is the guarantee
    env["MDKR_SIMULATION_CADENCE"] = "enhanced"
    env["MDKR_SYNTH_FIELDS"] = "1"
    if dumpdir:
        env["MDKR_DUMP_EVERY"] = str(DUMP_EVERY)
    if extra_env:
        env.update(extra_env)
    cmd = [build, "--headless-frames", str(frames), "--rom", rom]
    if script:
        cmd += ["--input-script", script]
    if dumpdir:
        cmd += ["--dump-frames", dumpdir]
    if verbose:
        print("      $ " + " ".join(cmd))
    p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=900)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def frame_hashes(dumpdir):
    out = {}
    for n in sorted(os.listdir(dumpdir)):
        if n.endswith(".ppm"):
            with open(os.path.join(dumpdir, n), "rb") as f:
                out[n] = hashlib.sha1(f.read()).hexdigest()
    return out


def pace_line(out):
    """Last [PACE] probe with the wall-clock field stripped.

    dtms= is true elapsed milliseconds, so it is NOT reproducible by
    construction; comparing it would make every run 'differ'.
    """
    lines = [l for l in out.splitlines() if "[PACE]" in l]
    return re.sub(r"dtms=[\d.]+ ", "", lines[-1]) if lines else ""


def rom_message(out):
    """The [ROM] verdict line, minus the '[ROM] ' prefix and any path prefix."""
    for l in out.splitlines():
        if l.startswith("[ROM] ") and " is " in l:
            return l[len("[ROM] "):]
    return ""


SOURCE_VIDEO_RE = re.compile(
    r"^\[ROM\] source video: (PAL|NTSC) \((50|60) Hz fields\)$", re.MULTILINE
)


def source_video(out):
    match = SOURCE_VIDEO_RE.search(out)
    return match.groups() if match else None


# --------------------------------------------------------------------------
# 1 + 2. identification
# --------------------------------------------------------------------------
def check_identification(args, failures):
    print("1. identification (each revision present is classified and named)")
    seen = 0
    for short, phrase, supported in REVISIONS:
        rom = args.found.get(short)
        if not rom:
            # A machine with only the supported ROM must still PASS.
            print("   %-8s SKIP (not present under %s)" % (short, args.roms))
            continue
        seen += 1
        rc, out = run_engine(args.build, rom, 20, verbose=args.verbose)
        msg = rom_message(out)
        if supported:
            if rc != 0:
                failures.append("%s: a SUPPORTED revision was refused (exit %d): %s"
                                % (short, rc, msg[:200]))
                print("   %-8s FAIL refused" % short)
                continue
            if phrase not in out:
                failures.append("%s: loaded but never named %r — the accept path must still "
                                "say which revision it accepted" % (short, phrase))
                print("   %-8s FAIL unnamed" % short)
                continue
            extent = "ROM end 0x%06X" % SUPPORTED_ROM_ENDS[short]
            if extent not in out:
                failures.append(
                    "%s: the loader did not publish its revision-owned checksum extent %s"
                    % (short, extent)
                )
                print("   %-8s FAIL wrong ROM extent" % short)
                continue
            print("   %-8s ok  accepted and named %r" % (short, phrase))
        else:
            if rc == 0:
                failures.append("%s: an UNSUPPORTED revision was ACCEPTED (exit 0). This is "
                                "the original defect: it will read its asset lookup table "
                                "from the us.v80 offset and go wrong silently." % short)
                print("   %-8s FAIL accepted" % short)
                continue
            # It must be refused CLEANLY, not crash. Without this, a build whose
            # revision check is broken still "passes" here: it accepts the ROM,
            # SIGSEGVs in asset_swap_lut/audio_init, and the non-zero exit reads as
            # a refusal. (That is exactly what positive control A did.) A clean
            # refusal is exit 1, before SDL/renderer bring-up.
            if rc != 1:
                failures.append("%s: exited %d rather than refusing cleanly with 1 — a "
                                "recognised-but-unsupported ROM must be rejected at the "
                                "gate, not crash later" % (short, rc))
                print("   %-8s FAIL exit %d (not a clean refusal)" % (short, rc))
                continue
            if "[CRASH]" in out or "Window created" in out:
                failures.append("%s: the ROM got past the gate (crash trace and/or renderer "
                                "bring-up present in the output) — it was not refused"
                                % short)
                print("   %-8s FAIL booted" % short)
                continue
            miss = [w for w in (phrase, "US 1.1", "us.v80") if w not in msg]
            if miss:
                failures.append("%s: refused, but the message does not mention %s — a user "
                                "must be told WHICH ROM they have and which one this build "
                                "wants. Got: %s" % (short, miss, msg[:200]))
                print("   %-8s FAIL vague (%s)" % (short, ",".join(miss)))
                continue
            print("   %-8s ok  refused, named %r" % (short, phrase))
    if seen == 0:
        failures.append("no ROM was found at all under %s and --rom %s; this check would "
                        "have passed vacuously" % (args.roms, args.rom))
    return seen


def check_non_dkr(args, failures, workdir):
    print("2. a non-DKR N64 ROM is refused as non-DKR")
    with open(args.rom, "rb") as f:
        d = bytearray(f.read())
    # Rewrite the cart id + title, and clear the CRC pair so identification cannot
    # fall back on it. Result: an N64 ROM that is not Diddy Kong Racing.
    struct.pack_into(">I", d, 0x10, 0)
    struct.pack_into(">I", d, 0x14, 0)
    d[0x20:0x34] = b"NOT A DKR ROM       "
    d[0x3B] = ord("N")
    d[0x3C] = ord("Z")
    d[0x3D] = ord("Z")
    path = os.path.join(workdir, "not_dkr.z64")
    with open(path, "wb") as f:
        f.write(bytes(d))
    rc, out = run_engine(args.build, path, 20, verbose=args.verbose)
    msg = rom_message(out)
    os.remove(path)
    if rc == 0:
        failures.append("a non-DKR ROM (cart id NZZ) was ACCEPTED")
        print("   FAIL accepted")
    elif "not Diddy Kong Racing" not in msg:
        failures.append("a non-DKR ROM was refused but not identified as non-DKR: %s"
                        % msg[:200])
        print("   FAIL vague")
    else:
        print("   ok  refused as non-DKR")


# --------------------------------------------------------------------------
# 3. byte order
# --------------------------------------------------------------------------
def synth_byteswapped(src, workdir):
    """Write .v64 (16-bit swapped) and .n64 (32-bit reversed) copies of `src`.

    Under a git-ignored path, and deleted by the caller. Nothing ROM-derived is
    ever committed (tools/check_no_rom.sh and tools/check_clean_room.sh gate it).
    """
    with open(src, "rb") as f:
        z = bytearray(f.read())
    v = bytearray(z)
    v[0::2], v[1::2] = z[1::2], z[0::2]
    n = bytearray(len(z))
    n[0::4], n[1::4], n[2::4], n[3::4] = z[3::4], z[2::4], z[1::4], z[0::4]
    # Sanity: the synthesised magic must be what the loader looks for, or this
    # check would be testing nothing.
    assert bytes(v[:4]) == b"\x37\x80\x40\x12", v[:4].hex()
    assert bytes(n[:4]) == b"\x40\x12\x37\x80", n[:4].hex()
    out = {}
    for tag, buf in (("v64", v), ("n64", n)):
        p = os.path.join(workdir, "synth." + tag)
        with open(p, "wb") as f:
            f.write(bytes(buf))
        out[tag] = p
    return out


def check_byte_order(args, failures, workdir):
    print("3. byte order: synthesised .v64 / .n64 race identically to the .z64")
    if not os.path.exists(args.script):
        print("   SKIP (no %s)" % args.script)
        return
    swapped = synth_byteswapped(args.rom, workdir)
    results = {}
    try:
        for tag, rom in [("z64", args.rom)] + sorted(swapped.items()):
            d = tempfile.mkdtemp(prefix="mdkr_bo_", dir=workdir)
            rc, out = run_engine(args.build, rom, args.frames, script=args.script,
                                 dumpdir=d, verbose=args.verbose)
            h = frame_hashes(d)
            shutil.rmtree(d, ignore_errors=True)
            results[tag] = (rc, h, pace_line(out))
            print("   %-4s exit=%-4s frames=%d" % (tag, rc, len(h)))
    finally:
        for p in swapped.values():
            if os.path.exists(p):
                os.remove(p)

    rc0, base, pace0 = results["z64"]
    if rc0 != 0 or len(base) < MIN_FRAMES:
        failures.append("the .z64 reference run itself failed (exit %s, %d frames) — the "
                        "byte-order comparison would have been vacuous" % (rc0, len(base)))
        print("   FAIL reference run bad")
        return
    for tag in ("v64", "n64"):
        rc, h, pace = results[tag]
        if rc != 0:
            failures.append(".%s exited %s: a byteswapped image must be converted on load, "
                            "not merely accepted" % (tag, rc))
            print("   %-4s FAIL exit %s" % (tag, rc))
            continue
        # A differing frame COUNT is a failure in its own right. Comparing only the
        # intersection is how an earlier version of this check let a crashing run
        # read as agreement.
        if len(h) != len(base):
            failures.append(".%s rendered %d frames against the .z64's %d — it did not get "
                            "as far, so the byte order was not normalised" % (tag, len(h), len(base)))
            print("   %-4s FAIL frame count %d != %d" % (tag, len(h), len(base)))
            continue
        diff = [n for n in sorted(base) if base[n] != h[n]]
        if diff:
            failures.append(".%s rendered %d of %d frames differently from the .z64 (first %s) "
                            "— the image was accepted but read in the wrong byte order"
                            % (tag, len(diff), len(base), diff[0]))
            print("   %-4s FAIL %d/%d frames differ" % (tag, len(diff), len(base)))
            continue
        if pace != pace0:
            failures.append(".%s drove a different trajectory:\n     z64 %s\n     %s %s"
                            % (tag, pace0, tag, pace))
            print("   %-4s FAIL trajectory differs" % tag)
            continue
        print("   %-4s ok  %d frames byte-identical, same trajectory" % (tag, len(base)))


# --------------------------------------------------------------------------
# 5. every supported revision must preserve gameplay and authored region timing
# --------------------------------------------------------------------------
def check_supported_parity(args, failures, workdir):
    print("5. every other supported revision preserves gameplay and source timing")
    if not os.path.exists(args.script):
        print("   SKIP (no %s)" % args.script)
        return
    present = [r for r in PARITY_REVISIONS if args.found.get(r)]
    if not present:
        print("   SKIP (none of %s present)" % ", ".join(PARITY_REVISIONS))
        return

    def race(rom):
        d = tempfile.mkdtemp(prefix="mdkr_par_", dir=workdir)
        rc, out = run_engine(args.build, rom, args.frames, script=args.script,
                             dumpdir=d, verbose=args.verbose)
        h = frame_hashes(d)
        shutil.rmtree(d, ignore_errors=True)
        return rc, h, pace_line(out), source_video(out)

    rc0, base, pace0, video0 = race(args.rom)
    if rc0 != 0 or len(base) < MIN_FRAMES:
        failures.append("the us.v80 reference race failed (exit %s, %d frames); the parity "
                        "comparison would have been vacuous" % (rc0, len(base)))
        print("   FAIL reference race bad")
        return
    if video0 != ("NTSC", "60"):
        failures.append("us.v80 did not resolve its authored NTSC 60 Hz source: %s"
                        % (video0,))
        print("   FAIL reference source video %s" % (video0,))
        return
    for short in present:
        rc, h, pace, video = race(args.found[short])
        if rc != 0:
            failures.append("%s is marked SUPPORTED but its race exited %s" % (short, rc))
            print("   %-8s FAIL exit %s" % (short, rc))
            continue
        if len(h) != len(base):
            failures.append("%s rendered %d frames against us.v80's %d — it is marked "
                            "supported but does not get as far" % (short, len(h), len(base)))
            print("   %-8s FAIL frame count %d != %d" % (short, len(h), len(base)))
            continue
        expected_video = ("PAL", "50") if short.startswith("pal.") else ("NTSC", "60")
        if video != expected_video:
            failures.append("%s resolved source video %s, expected %s"
                            % (short, video, expected_video))
            print("   %-8s FAIL source video %s" % (short, video))
            continue
        if pace != pace0:
            failures.append("%s drove a different trajectory from us.v80:\n"
                            "     us.v80 %s\n     %s %s"
                            % (short, pace0, short, pace))
            print("   %-8s FAIL trajectory differs" % short)
            continue
        diff = [n for n in sorted(base) if base[n] != h[n]]
        print("   %-8s ok  same trajectory, source=%s/%sHz; "
              "%d/%d presentation frames region-specific"
              % (short, video[0], video[1], len(diff), len(base)))


# --------------------------------------------------------------------------
# 6. PAL v80 retains its authored language selector
# --------------------------------------------------------------------------
LANGUAGE_TRACE_RE = re.compile(
    r"menu_language: selected=(?P<language>\d+) european_rom=(?P<european>[01])"
)


def check_language_capability(args, failures):
    """Exercise the real Options route, not just the loader's German asset arm.

    Gameplay.MenuLanguages defaults to "all" (GitHub issue #19): us.v80 is
    byte-identical to pal.v80 in every asset section (docs/ROM_REVISIONS.md
    Sec 5), so its German text and fonts are already on the disc, and by
    default the selector now offers them on both. Starting with English, the
    full small cycle proves it: PAL and default-configuration US both go
    EN -> DE -> EN -> FR -> DE. A third arm proves the opt-out
    (MDKR_MENU_LANGUAGES=authentic) still restores the retail US menu's
    EN <-> FR toggle, with no ROM edit and no restart either way.
    """
    print("6. Menu languages default to all (PAL and US); authentic opts "
          "US back out")
    script = LANGUAGE_SCRIPT
    if not os.path.exists(script):
        failures.append("language selector fixture is missing: %s" % script)
        print("   FAIL missing fixture")
        return
    expected = {
        "us.v80": (
            [1, 0, 2, 1], 1, "EN -> DE -> EN -> FR -> DE (default, US disc)",
            None),
        "us.v80 +MDKR_MENU_LANGUAGES=authentic": (
            [2, 0, 2, 0], 0, "EN <-> FR (opt-out)",
            {"MDKR_MENU_LANGUAGES": "authentic"}),
        "pal.v80": ([1, 0, 2, 1], 1, "EN -> DE -> EN -> FR -> DE", None),
    }
    for short, (want_languages, want_european, label, extra_env) in expected.items():
        rom_short = short.split()[0]
        rom = args.found.get(rom_short)
        if not rom:
            print("   %-8s SKIP (not present)" % short)
            continue
        run_env = {"MDKR_TRACE": "1"}
        if extra_env:
            run_env.update(extra_env)
        with tempfile.TemporaryDirectory(prefix="mdkr-language-") as run_root:
            save = os.path.join(run_root, "save")
            os.mkdir(save)
            run_env["MDKR_SAVE_DIR"] = save
            rc, out = run_engine(
                args.build, rom, 1900, script=script,
                extra_env=run_env,
                verbose=args.verbose)
        events = [(int(m.group("language")), int(m.group("european")))
                  for m in LANGUAGE_TRACE_RE.finditer(out)]
        got_languages = [language for language, _ in events]
        got_european = {european for _, european in events}
        if (rc != 0 or got_languages != want_languages or
                got_european != {want_european}):
            failures.append(
                "%s language selector got %s (events %s, exit %d), want %s/%d (%s)"
                % (short, got_languages, events, rc, want_languages,
                   want_european, label))
            print("   %-8s FAIL got %s" % (short, got_languages))
        else:
            print("   %-8s ok  %s" % (short, label))


# --------------------------------------------------------------------------
# 4. native <-> browser agreement
# --------------------------------------------------------------------------
C_ROW = re.compile(
    r'\{\s*"[^"]*"\s*,\s*"(?P<build>[^"]+)"\s*,\s*\'(?P<country>.)\'\s*,\s*'
    r'(?P<rev>0x[0-9A-Fa-f]+)\s*,\s*(?P<crc1>0x[0-9A-Fa-f]+)\s*,\s*(?P<crc2>0x[0-9A-Fa-f]+)\s*,'
    r'\s*(?P<lut1>0x[0-9A-Fa-f]+)\s*,\s*(?P<lut2>0x[0-9A-Fa-f]+)\s*,\s*'
    r'(?P<romend>0x[0-9A-Fa-f]+)\s*,\s*(?P<sup>[01])\s*\}')
JS_ROW = re.compile(
    r'build:\s*"(?P<build>[^"]+)"\s*,\s*country:\s*"(?P<country>.)"\s*,\s*'
    r'revision:\s*(?P<rev>0x[0-9A-Fa-f]+)\s*,\s*crc1:\s*(?P<crc1>0x[0-9A-Fa-f]+)\s*,\s*'
    r'crc2:\s*(?P<crc2>0x[0-9A-Fa-f]+)\s*,\s*lutStart:\s*(?P<lut1>0x[0-9A-Fa-f]+)\s*,\s*'
    r'lutEnd:\s*(?P<lut2>0x[0-9A-Fa-f]+)\s*,\s*romEnd:\s*(?P<romend>0x[0-9A-Fa-f]+)\s*,\s*'
    r'supported:\s*(?P<sup>true|false)')


def rows(pattern, path, sup_true):
    with open(path, "r") as f:
        txt = f.read()
    out = []
    for m in pattern.finditer(txt):
        g = m.groupdict()
        out.append((g["build"], g["country"], int(g["rev"], 16), int(g["crc1"], 16),
                    int(g["crc2"], 16), int(g["lut1"], 16), int(g["lut2"], 16),
                    int(g["romend"], 16), g["sup"] == sup_true))
    return out


def check_mirror(args, failures):
    print("4. platform/rom_id.c and dist/web/rom-id.js agree")
    c_path, js_path = "platform/rom_id.c", "dist/web/rom-id.js"
    for p in (c_path, js_path):
        if not os.path.exists(p):
            failures.append("%s is missing; the two sides cannot be compared" % p)
            print("   FAIL missing %s" % p)
            return
    c_rows = rows(C_ROW, c_path, "1")
    js_rows = rows(JS_ROW, js_path, "true")
    if len(c_rows) < 5:
        failures.append("parsed only %d revision rows from %s (expected 5) — the table "
                        "format changed and this comparison silently stopped covering it"
                        % (len(c_rows), c_path))
        print("   FAIL parsed %d C rows" % len(c_rows))
        return
    if c_rows != js_rows:
        only_c = [r for r in c_rows if r not in js_rows]
        only_js = [r for r in js_rows if r not in c_rows]
        failures.append("the revision tables differ.\n     only in rom_id.c: %s\n"
                        "     only in rom-id.js: %s" % (only_c, only_js))
        print("   FAIL tables differ")
        return
    print("   ok  %d revision rows identical in both" % len(c_rows))

    # Run the JS over each real ROM and require the same sentence as the native
    # binary. This is what makes "mirrors rom_id.c" checkable rather than a claim.
    node = shutil.which("node")
    if not node:
        print("   SKIP node not installed — tables compared, runtime verdicts not")
        return
    for short, _phrase, _sup in REVISIONS:
        rom = args.found.get(short)
        if not rom:
            continue
        # The ROM path goes through the environment: `node -e` argv indexing differs
        # from a script's, and getting it wrong silently throws instead of comparing.
        js = (
            'const m=require(%r);const fs=require("fs");'
            'const b=new Uint8Array(fs.readFileSync(process.env.MDKR_JS_ROM));'
            'const r=m.dkrValidateRom(b,"ROM");'
            'console.log(r.error||r.warning||m.dkrDescribeRom(m.dkrIdentifyRom(b),"ROM"));'
            % os.path.abspath(js_path))
        jenv = dict(os.environ)
        jenv["MDKR_JS_ROM"] = os.path.abspath(rom)
        p = subprocess.run([node, "-e", js], env=jenv, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=300)
        js_msg = p.stdout.decode("utf-8", "replace").strip()
        _rc, out = run_engine(args.build, rom, 20)
        nat_msg = rom_message(out)
        # Both sentences carry the file's name first; compare from " is " on.
        def tail(s):
            i = s.find(" is ")
            return s[i:] if i >= 0 else s
        if tail(js_msg) != tail(nat_msg):
            failures.append("%s: the browser and native verdicts differ.\n     native: %s\n"
                            "     browser: %s" % (short, tail(nat_msg), tail(js_msg)))
            print("   %-8s FAIL verdicts differ" % short)
        else:
            print("   %-8s ok  same verdict on both sides" % short)


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64",
                    help="the SUPPORTED ROM; synthetic inputs are derived from it")
    ap.add_argument("--roms", default="build/roms",
                    help="directory of <build>.z64 revisions; any absent one is skipped")
    ap.add_argument("--script", default=RACE_SCRIPT)
    ap.add_argument("--frames", type=int, default=4300)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    args.build = resolve_binary(args.build)

    if not os.path.exists(args.build):
        sys.exit("no build at %s" % args.build)
    if not os.path.exists(args.rom):
        print("check_rom_revision: SKIP — no ROM at %s (bring-your-own-ROM)" % args.rom)
        return 0

    # Locate every revision by header CRC (see resolve_roms): --rom first, so an
    # explicitly named file wins, then whatever is in --roms under any filename.
    args.found = resolve_roms(args.rom, args.roms)
    print("check_rom_revision: build=%s rom=%s roms=%s" % (args.build, args.rom, args.roms))
    if args.found:
        for name in sorted(args.found):
            print("   found %-8s %s" % (name, os.path.basename(args.found[name])))
    failures = []
    workdir = tempfile.mkdtemp(prefix="mdkr_romrev_")
    try:
        check_identification(args, failures)
        check_non_dkr(args, failures, workdir)
        check_byte_order(args, failures, workdir)
        check_mirror(args, failures)
        check_supported_parity(args, failures, workdir)
        check_language_capability(args, failures)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print()
    if failures:
        for f in failures:
            print("check_rom_revision: FAIL — %s" % f)
        print("check_rom_revision: FAIL (%d)" % len(failures))
        return 1
    print("check_rom_revision: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
