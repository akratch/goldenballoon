#!/usr/bin/env python3
"""Mechanically enumerate three bug SHAPES across game/src -- CONTRIBUTING.md rule 6.

Each of these three classes is invisible to every runtime instrument this project
owns, for a different reason, which is why the sweep has to be static:

  bare-pointer   A callee writes an unknown number of elements through a pointer
                 parameter that carries no count. UBSan `array-bounds` needs an
                 indexed *array type* to reason about and the callee only has a
                 pointer; ASan only helps if the overrun is reached, and the
                 measured peaks say none of them is. So the only way to know the
                 list is complete is to enumerate the signatures.
                 Worked instance: get_inside_segment_count_xz() writing into the
                 caller's segmentsInside[8] (docs/OPEN_ITEMS.md "splitsweep").

  equality-cap   A saturation cap tested with `==`/`!=` against a counter that can
                 be advanced past the boundary by a *second*, unguarded increment.
                 Nothing can see this until a level saturates the list, and then it
                 is a silent overrun, not a trap.
                 Worked instance: generate_collision_candidates()'s
                 `if (j == MAX_COLLISION_CANDIDATES)` (docs/OPEN_ITEMS.md
                 "gridmask").

  shift-count    A shift whose count is a variable that can reach or exceed the
                 width of the type. The decomp writes MIPS `sllv` -- which masks
                 the count to 5 bits -- as `1 << (j + 31)`, which is UB in C.
                 `-fsanitize=shift-exponent` finds the ones that are REACHED;
                 this pass finds the ones that exist.
                 Worked instance: objects.c's `settings->tajFlags |= 1 << (j+31)`.

The parser is a brace matcher over comment-stripped text, not a C front end. That
is a deliberate trade: it has no include paths, no macro expansion and no build to
keep working, so it runs on any checkout in under a second and cannot silently stop
covering a file. It is validated the only way that matters -- SELFTEST below pins
the known instances, so a parser regression that stopped finding them fails loudly
instead of reporting "0 findings, all clear".

Usage:
    tools/sweep_bug_shapes.py                       # all three classes
    tools/sweep_bug_shapes.py bare-pointer -v
    tools/sweep_bug_shapes.py --selftest            # prove the parser still sees
                                                    # the known instances
Output is one `class|file|line|key|text` record per finding, sorted, for
tests/check_array_bounds_sweep.py to diff against its triage list. That check
runs `--selftest` first; either mode exits non-zero when a file was parsed
incompletely, so a shortfall can never read as "0 findings, all clear".
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = ["game/src"]

# The parser must keep finding these. Each is a triaged instance documented in
# docs/OPEN_ITEMS.md; if the enumeration stops producing one, the sweep is broken.
SELFTEST = [
    ("bare-pointer", "game/src/tracks.c", "get_inside_segment_count_xz"),
    ("bare-pointer", "game/src/tracks.c", "get_inside_segment_count_xyz"),
    ("equality-cap", "game/src/hasm/collision.c", "generate_collision_candidates"),
    # The three MIPS-mask shift sites, spelled with DKR_SHL32 since wave
    # "keyshift". Pinned by FUNCTION, not by the operator, so a future respelling
    # cannot make the class look empty.
    ("shift-count", "game/src/objects.c", "track_setup_racers:MIPS-MASK-IDIOM"),
    ("shift-count", "game/src/game.c", "level_load:MIPS-MASK-IDIOM"),
    ("shift-count", "game/src/waves.c", "obj_wave_height:MIPS-MASK-IDIOM"),
]

IDENT = r"[A-Za-z_]\w*"
KEYWORDS = {"if", "for", "while", "switch", "return", "else", "do", "sizeof",
            "case", "default", "typedef", "struct", "union", "enum", "static",
            "extern", "GLOBAL_ASM"}
# Parameter names that ARE a bound. Used only to annotate a finding, never to
# suppress it -- a parameter called `count` may well be an input length rather
# than the capacity of the output buffer, and only reading the code settles it.
BOUNDISH = re.compile(r"(?i)(count|num|len|size|max|bound|limit|cap|total|slots)")


def strip_comments(text):
    """Blank out comments and string/char literals, preserving line numbering."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


# The macros this build defines (CMakeLists.txt target_compile_definitions). The
# sweep must see the arms we COMPILE, not both arms: objects.c's ANTI_TAMPER block
# opens a brace in BOTH arms, so keeping both desynchronises the brace matcher and
# swallowed 8006 lines of objects.c into one "function" before this existed.
DEFINED = {"VERSION_us_v80", "_LANGUAGE_C", "MODERN_CC", "NON_MATCHING", "AVOID_UB",
           "NATIVE_PORT", "F3DDKR_GBI", "_FINALROM", "NDEBUG", "SDL_MAIN_HANDLED"}
COND_RE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")


def select_arms(text):
    """Blank the preprocessor arms this build does not compile, and the directives.

    Only `#ifdef`/`#ifndef`/`#if defined(X)`/`#if !defined(X)` are evaluated; any
    other `#if` is treated as TAKEN, which keeps code rather than losing coverage.
    """
    out = []
    stack = []  # (this_arm_live, chain_already_taken, decidable)
    for line in text.split("\n"):
        m = COND_RE.match(line)
        live = all(s[0] for s in stack)
        if m:
            d, rest = m.group(1), m.group(2)
            if d in ("ifdef", "ifndef", "if"):
                val = None
                if d == "ifdef":
                    val = rest.strip() in DEFINED
                elif d == "ifndef":
                    val = rest.strip() not in DEFINED
                else:
                    dm = re.fullmatch(r"\s*(!?)\s*defined\s*\(?\s*(\w+)\s*\)?\s*", rest)
                    if dm:
                        val = (dm.group(2) in DEFINED) != bool(dm.group(1))
                if val is None:
                    stack.append((True, True, False))
                else:
                    stack.append((val, val, True))
            elif d in ("else", "elif") and stack:
                arm_live, taken, decidable = stack[-1]
                if not decidable:
                    stack[-1] = (True, True, False)
                else:
                    stack[-1] = (not taken, True, True)
            elif d == "endif" and stack:
                stack.pop()
            out.append("")
            continue
        out.append(line if live else "")
    return "\n".join(out)


def match_back(text, close, open_ch, close_ch):
    """Index of the opener matching the closer at `close`."""
    depth = 0
    i = close
    while i >= 0:
        if text[i] == close_ch:
            depth += 1
        elif text[i] == open_ch:
            depth -= 1
            if depth == 0:
                return i
        i -= 1
    return -1


DEF_START = re.compile(r"^[A-Za-z_][^;#]*$")


def functions(text):
    """Yield (name, params_src, body, first_line) for every top-level definition.

    Line-based, not brace-based, and deliberately so: a preprocessor arm is allowed
    to carry an unbalanced brace (printf.c's `#ifdef HAVE_LONGLONG` opens an `else {`
    whose `}` is outside the `#endif`; menu.c and game_ui.c do the same), which
    defeats any pure brace matcher no matter which arms you select. The decomp is
    uniformly clang-formatted, so a top-level definition starts in column 0 and ends
    at the first line that is exactly `}` in column 0 -- and `_selftest_parser()`
    below pins that assumption to a countable invariant.
    """
    lines = text.split("\n")
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if not line or line[0].isspace() or line[0] == "#" or line[0] == "}":
            i += 1
            continue
        if not DEF_START.match(line):
            i += 1
            continue
        # Accumulate the header until its parentheses balance.
        hdr, j = line, i
        while hdr.count("(") > hdr.count(")") and j + 1 < n:
            j += 1
            hdr += " " + lines[j].strip()
        if "(" not in hdr or hdr.count("(") != hdr.count(")"):
            i += 1
            continue
        rest = hdr[hdr.rindex(")") + 1:].strip()
        if rest not in ("", "{"):
            i += 1
            continue
        if rest == "":  # brace on the next line
            if j + 1 >= n or lines[j + 1].strip() != "{":
                i += 1
                continue
            j += 1
        close = match_back(hdr, hdr.rindex(")"), "(", ")")
        head = hdr[:close]
        m = re.search(r"(%s)\s*$" % IDENT, head)
        if not m or m.group(1) in KEYWORDS:
            i += 1
            continue
        # Body: to the first `}` in column 0.
        k = j + 1
        while k < n and not lines[k].startswith("}"):
            k += 1
        yield m.group(1), hdr[close + 1:hdr.rindex(")")], \
            "\n".join(lines[j:k + 1]), j + 1
        i = k + 1


def split_params(src):
    out, depth, cur = [], 0, ""
    for ch in src:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def param_name(p):
    m = re.search(r"(%s)\s*(\[[^\]]*\])?\s*$" % IDENT, p)
    return m.group(1) if m else None


def line_at(raw, ln):
    lines = raw.split("\n")
    return re.sub(r"\s+", " ", lines[ln - 1]).strip() if 0 < ln <= len(lines) else ""


# --------------------------------------------------------------------------- #
# class 1: an unbounded write through a bare pointer parameter
# --------------------------------------------------------------------------- #
def sweep_bare_pointer(rel, raw, text):
    hits = []
    for name, psrc, body, base in functions(text):
        params = split_params(psrc)
        if not params or params == ["void"]:
            continue
        pnames = [param_name(p) or "" for p in params]
        for p, pn in zip(params, pnames):
            # A pointer-to-object parameter, not a pointer-to-pointer (those are
            # out-parameters for a single value) and not const.
            if "*" not in p or "const" in p or p.count("*") > 1:
                continue
            if not pn or "[" in p:
                continue
            # A write through the pointer, of an a-priori unknown number of
            # elements. Two shapes:
            #   walking:  *p = ... together with p++ / p += ...
            #   indexed:  p[<non-literal>] = ...
            # NB the advance must be of the POINTER, not of the pointee: `*idx +=`
            # in particles.c's get_next_particle_behaviour() is a scalar
            # out-parameter being wrapped, not a walk, and matching it produced
            # three false positives.
            advance = re.search(r"(?<![*&\w])%s\s*(\+\+|\+=)" % pn, body)
            walk = re.search(r"\*\s*%s\s*(\+\+)?\s*(=[^=]|\+=|\|=|&=|\^=)" % pn, body) \
                and advance
            walk = walk or re.search(r"\*\s*%s\s*\+\+\s*=[^=]" % pn, body)
            # `(?<![.>\w])` so that `attachPoint->obj[i] = ` is not read as a
            # write through a parameter called `obj`: that is an indexed ARRAY
            # MEMBER, which UBSan array-bounds already sees, and a different class.
            idx = re.search(r"(?<![.>\w])%s\s*\[\s*[^\]]*[A-Za-z_][^\]]*\]\s*"
                            r"(?:(?:\.|->)\w+\s*)?(?:=[^=]|\+=|\|=|&=|\^=)" % pn, body)
            if not (walk or idx):
                continue
            # ...inside a loop, i.e. the count is not statically one.
            if not re.search(r"\b(for|while)\s*\(", body):
                continue
            kind = "walk" if walk else "index"
            bound = [q for q in pnames if q and q != pn and BOUNDISH.search(q)]
            ln = base
            hits.append(("bare-pointer", rel, ln, "%s:%s" % (name, pn), "TRIAGE",
                         "%s write through `%s` in %s(%s)%s"
                         % (kind, pn, name, ", ".join(params),
                            "  [bound-ish params: %s]" % ",".join(bound) if bound else
                            "  [NO bound-ish param]")))
    return hits


# --------------------------------------------------------------------------- #
# class 2: a cap tested by equality against a counter with a second increment
# --------------------------------------------------------------------------- #
CMP_RE = re.compile(r"(%s)\s*(==|!=)\s*(0x[0-9A-Fa-f]+|\d+|[A-Z][A-Z0-9_]{2,})" % IDENT)
CMP_RE2 = re.compile(r"(0x[0-9A-Fa-f]+|\d+|[A-Z][A-Z0-9_]{2,})\s*(==|!=)\s*(%s)" % IDENT)


def sweep_equality_cap(rel, raw, text):
    """`i == CAP` where `i` is an INSERT INDEX -- i.e. it also subscripts a write.

    The narrowing to write-subscripts is what makes this class the defect rather
    than the 300-odd harmless `state == SOME_ENUM` equality tests in the tree: a
    cap only matters when overshooting it writes somewhere. A finding is a cap that
    can be stepped over iff the counter has MORE THAN ONE increment site, because
    then one of them can advance past the boundary the other one tests.
    """
    hits = []
    for name, _psrc, body, base in functions(text):
        for m in list(CMP_RE.finditer(body)) + list(CMP_RE2.finditer(body)):
            var = m.group(1) if m.re is CMP_RE else m.group(3)
            const = m.group(3) if m.re is CMP_RE else m.group(1)
            if var in KEYWORDS:
                continue
            # The variable must be a counter: incremented somewhere in the body.
            incs = re.findall(r"(?:\+\+\s*%s\b|\b%s\s*\+\+|\b%s\s*\+=\s*(\d+)?)"
                              % (var, var, var), body)
            if not incs:
                continue
            # A stride > 1 with an equality test is the purest form of the shape:
            # the counter can jump the boundary without ever equalling it.
            stride = max([int(k) for k in incs if k] or [1])
            # ...and an INSERT INDEX: it subscripts something that is written.
            writes = re.findall(r"(?<![.>\w])%s\s*\[[^\]]*\b%s\b[^\]]*\]\s*"
                                r"(?:(?:\.|->)\w+\s*)?(?:=[^=]|\+=|\|=|&=|\^=)"
                                % (IDENT, var), body)
            if not writes:
                continue
            ln = base + body.count("\n", 0, m.start())
            # TRIAGE only where the constant is plausibly a CAPACITY (an ALL_CAPS
            # macro or an ARRAY_COUNT) or the stride can jump it. Everything else
            # is `i == 0`-style state testing, which is not this shape.
            # A capacity is an ALL_CAPS macro, an ARRAY_COUNT, or a literal big
            # enough to be an array size rather than a state code. 8 is the
            # threshold because the smallest real capacity in the tree is 10
            # (generate_collision_candidates' 10-segment list) and the state codes
            # that produce the noise are all 0..7.
            capacity = bool(re.match(r"[A-Z][A-Z0-9_]{2,}$", const)) or "ARRAY_COUNT" in const
            if not capacity and re.match(r"(0x[0-9A-Fa-f]+|\d+)$", const):
                capacity = int(const, 16 if const.lower().startswith("0x") else 10) >= 8
            sev = "TRIAGE" if (capacity or stride > 1) else "INFO"
            hits.append(("equality-cap", rel, ln,
                         "%s:%s%s%s" % (name, var, m.group(2), const), sev,
                         "`%s %s %s` guards %d write(s) indexed by `%s` in %s(); "
                         "%d increment site(s), stride %d%s"
                         % (var, m.group(2), const, len(writes), var, name, len(incs),
                            stride,
                            "  <-- STEPPABLE: more than one increment"
                            if len(incs) > 1 else "")))
    return hits


# --------------------------------------------------------------------------- #
# class 3: a shift whose count is not a literal
# --------------------------------------------------------------------------- #
SHIFT_RE = re.compile(r"(<<=?|>>=?)\s*([^;,)\]}&|]+)")
# DKR_SHL32(x, n) is a shift too -- game/include/macros.h, wave "keyshift".
# Matching it is what keeps this class enumerated AFTER it was fixed: the five
# sites no longer contain a `<<` at all, so a `<<`-only sweep would report the
# class as empty and the SELFTEST below would be the only thing that noticed.
SHL32_RE = re.compile(r"DKR_SHL32\s*\(([^;]*)\)")
LITERAL = re.compile(r"^\s*\(?\s*(0x[0-9A-Fa-f]+|\d+)\s*\)?\s*$")
# `var + K` with K >= 24 -- i.e. a count that is already within one nibble of a
# 32-bit width before the variable is even added, which is how the decomp renders
# a MIPS `sllv`/`srlv` whose count the hardware masks to 5 bits. 24 rather than 32
# so that `(worldId + 24)`-style variants cannot slip through.
ADDED_CONST = re.compile(r"\+\s*(0x[0-9A-Fa-f]+|\d+)\b")


def mask_idiom(rhs):
    for m in ADDED_CONST.finditer(rhs):
        tok = m.group(1)
        if int(tok, 16 if tok.lower().startswith("0x") else 10) >= 24:
            return True
    return False


def sweep_shift_count(rel, raw, text):
    hits = []
    for name, _psrc, body, base in functions(text):
        for m in list(SHIFT_RE.finditer(body)) + list(SHL32_RE.finditer(body)):
            if m.re is SHL32_RE:
                # the count is the second argument
                parts = split_params(m.group(1))
                rhs = parts[1] if len(parts) > 1 else ""
            else:
                rhs = m.group(2).strip()
            if not rhs or LITERAL.match(rhs):
                continue
            if not re.search(IDENT, rhs):
                continue
            ln = base + body.count("\n", 0, m.start())
            flavour = "MIPS-MASK-IDIOM" if mask_idiom(rhs) else "var-count"
            hits.append(("shift-count", rel, ln, "%s:%s" % (name, flavour),
                         "TRIAGE" if flavour == "MIPS-MASK-IDIOM" else "INFO",
                         "%s :: %s" % (flavour, line_at(raw, ln)[:90])))
    return hits


SWEEPS = {
    "bare-pointer": sweep_bare_pointer,
    "equality-cap": sweep_equality_cap,
    "shift-count": sweep_shift_count,
}


PARSE_ERRORS = 0


def collect(which):
    out = []
    for d in SCAN_DIRS:
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, d)):
            for f in sorted(files):
                if not f.endswith(".c"):
                    continue
                path = os.path.join(dirpath, f)
                rel = os.path.relpath(path, ROOT)
                raw = open(path, errors="replace").read()
                text = select_arms(strip_comments(raw))
                # FAILS CLOSED. Every top-level definition ends at a `}` in column
                # 0, so the count of those lines is an upper bound on the number of
                # definitions -- and, because the decomp has no other construct
                # closing in column 0 except data initialisers (`};`), the parser
                # must account for nearly all of them. Losing track of function
                # boundaries is exactly how a file goes quiet, so a shortfall is a
                # reported error and not an "all clear".
                # A function's closer is a line that is exactly `}`; a struct or
                # array initialiser's is `} Name;` or `};`.
                closers = sum(1 for ln in text.split("\n") if ln.rstrip() == "}")
                found = sum(1 for _ in functions(text))
                if found < closers:
                    print("PARSE SHORTFALL: %s -- %d definition(s) parsed but %d "
                          "column-0 '}' line(s); coverage of this file is INCOMPLETE"
                          % (rel, found, closers), file=sys.stderr)
                    global PARSE_ERRORS
                    PARSE_ERRORS += 1
                for cls in which:
                    for h in SWEEPS[cls](rel, raw, text):
                        out.append(h)
    return sorted(set(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("classes", nargs="*", choices=sorted(SWEEPS) + [],
                    help="default: all three")
    ap.add_argument("--selftest", action="store_true",
                    help="fail unless the known instances are still found")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    which = args.classes or sorted(SWEEPS)
    hits = collect(which)

    if args.selftest:
        missing = []
        for cls, rel, needle in SELFTEST:
            if not any(h[0] == cls and h[1] == rel and
                       (needle in h[3] or needle in h[5]) for h in hits):
                missing.append("%s %s %s" % (cls, rel, needle))
        for m in missing:
            print("SELFTEST MISSING: %s" % m)
        print("sweep_bug_shapes selftest: %s (%d finding(s))"
              % ("FAIL" if missing else "PASS", len(hits)))
        return 1 if missing or PARSE_ERRORS else 0

    for cls, rel, ln, key, sev, txt in hits:
        print("%s|%s|%d|%s|%s|%s" % (cls, rel, ln, key, sev, txt))
    if args.verbose:
        for cls in which:
            n = sum(1 for h in hits if h[0] == cls)
            t = sum(1 for h in hits if h[0] == cls and h[4] == "TRIAGE")
            print("# %-14s %d finding(s), %d to triage, %d informational"
                  % (cls, n, t, n - t), file=sys.stderr)
    # The PARSE SHORTFALL lines above already told the consumer on stderr that a
    # file's coverage is incomplete. The exit status has to say the same thing,
    # so a caller that only reads it cannot mistake a partial sweep for a clean
    # one.
    if PARSE_ERRORS:
        print("PARSE ERRORS: %d file(s) incompletely parsed; findings are not a "
              "complete enumeration" % PARSE_ERRORS, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
