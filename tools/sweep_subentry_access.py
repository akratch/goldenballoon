#!/usr/bin/env python3
"""Mechanically enumerate every read of an ENTRY INSIDE an asset section -- S5 M1.

WHY THIS EXISTS
===============
`asset_table_load()` bounds-checks the SECTION index against the master lookup
table's own entry count (`game/src/asset_loading.c`, `asset_section_bounds`).
Nothing bounds-checks an index *within* a section. This build is compiled for
us.v80 data and asks for us.v80 indices; us.v77's `GAME_TEXT` has 259 entries
where us.v80 has 343 (`docs/ROM_REVISIONS.md` sections 5 and 6), so a v80-only
index on a v77 ROM is an out-of-range read with no guard and no diagnostic. It is
unreachable today only because unsupported revisions are refused before engine
boot; widening the supported set makes it reachable. This sweep is the coverage
claim that says the fix touched EVERY such site rather than a sample.

THE ASSET MODEL, AS THE CODE ACTUALLY SPELLS IT
===============================================
`platform/asset_swap.h` and `game/src/asset_loading.c` establish three layers:

    master LUT   gAssetsLookupTable -- word 0 is the section count, words
                 1..count+1 are section start offsets. Indexed by section.
                 BOUNDS-CHECKED already; out of scope here, and structurally
                 invisible to this sweep anyway because asset_section_bounds()
                 subscripts a derived pointer rather than the LUT symbol.

    section      One asset section, materialised whole by asset_table_load()
                 or read piecewise by asset_load(). Many sections come in
                 pairs: `ASSET_X_TABLE` holds the offsets, `ASSET_X` holds the
                 payload.

    sub-entry    One entry inside a section. THIS is what is unbounded.

Four syntactic forms carry a sub-entry index. They were established by reading
the code, not guessed, and together they are the complete set of ways an entry
index can be spelled in this tree:

  table-index      A subscript of an in-RAM section table -- any symbol whose
                   value came from `asset_table_load()` /
                   `asset_table_load_zipped()`. Worked instance:
                   `gObjectModelTable[modelID]` (object_models.c). Three
                   variants the sweep handles explicitly, each because the tree
                   contains it:
                     - ARRAY OF TABLES. `gTextureAssetTable[tableType][assetIndex]`
                       -- the symbol holds two table pointers, so the entry index
                       is the SECOND subscript. The arity is learned from the
                       assignment's own subscript, not assumed.
                     - REBUILT TABLE. `gParticlesAssetTable[i] = ... rawTable[i]`
                       -- ASSET_PARTICLES_TABLE and ASSET_PARTICLE_BEHAVIORS_TABLE
                       are 4-byte offset arrays the game reinterprets in place as
                       pointer arrays, which LP64 makes impossible, so the port
                       rebuilds them element-wise into a second array with one
                       slot per section entry. `gLevelNames` is the same shape.
                       Those arrays are followed one hop; without that, a quarter
                       of particles.c sits outside the sweep. An alias of an alias
                       is not followed.
                     - SECTION BASE. `gAssetsMiscSection[...]` -- a section loaded
                       whole that is NOT an offset table. The subscript is a word
                       offset rather than an entry ordinal, but the read is
                       unbounded in exactly the same way, so it is reported. Such
                       a site is recognisable by its section name lacking the
                       `_TABLE` suffix.

  terminator-walk  The same subscript, but appearing in a `while`/`for`
                   condition that compares the entry against the section's
                   `-1` / `0xFFFFFFFF` / `0` terminator. This is how every count
                   in the tree is derived (`while (gAssetsMiscTable[len] != -1)
                   len++;`). Reported as its own kind because the read is bounded
                   by a terminator BYTE PATTERN, never by a count -- a section
                   whose terminator is missing or relocated walks off the end.

  misc-accessor    `get_misc_asset(i)` / `get_misc_asset_size(i)`. ASSET_MISC is
                   heterogeneous, so its sub-assets are reached through a named
                   accessor instead of a bare subscript (`game/src/objects.c`).
                   The index crosses a function boundary here, so the call sites
                   are listed with the verdict the ACCESSOR earns, not one the
                   caller proves -- and the accessor's guard, while a real
                   entry-count comparison, falls back to returning the SECTION
                   BASE rather than aborting. Every one of these rows therefore
                   reads CHECKED while still describing a silent wrong-data path,
                   which is precisely what Task 2 replaces.

  section-offset   `asset_load(ASSET_X, dst, offset, size)` and
                   `asset_rom_offset(ASSET_X, offset)` with a non-zero offset.
                   Structurally invisible to `table-index`: the offset can be an
                   entry index scaled to bytes with no table symbol anywhere in
                   the expression. Worked instance, and the one the sprint is
                   named after: `asset_load(ASSET_GAME_TEXT_TABLE, ...,
                   (textID & ~1) << 2, 16)` in game_text.c -- a raw entry index
                   into the 343-vs-259 table.

HOW A SITE IS CLASSIFIED
========================
A site is CHECKED when a comparison of the index expression's root identifier
against one of THAT SECTION'S entry-count symbols appears earlier in the same
function, and after that table's most recent load. The "after the most recent
load" floor is load-bearing rather than fussy: game/src/game.c reloads
`gTempAssetTable` from a second section inside one function, and without the
floor the bound belonging to the first section reads as a bound on the second.

Entry-count symbols are discovered mechanically, from the terminator-walk idiom
only, in both of the spellings the tree uses -- `T[count]` and `T[count + 1]` --
plus any symbol assigned from the walk variable on the following few lines
(`gTextureTableSize[TEX_TABLE_2D] = --i;`). A rebuilt table inherits its source
table's counts, since it has one slot per section entry. A count symbol is
repo-wide when it looks like a decomp global (`g[A-Z]`, `s[A-Z]`, `D_`) and
function-scoped otherwise, so a loop temp named `i` in one function is never
mistaken for a bound in another.

A comparison may carry a small arithmetic tail: `index + 1 > count` is how
game/src/objects.c bounds every accessor that reads both `table[index]` and
`table[index + 1]`, and an operator-anchored match misses all of them.

Everything else is UNCHECKED or UNKNOWN, and the tie always breaks toward the
hazard:

  UNCHECKED  the index is a compile-time constant (an ALL_CAPS enum or a
             literal) -- which is exactly the v80-index-on-a-v77-ROM shape, so
             it can never be "checked"; or the root identifier is never
             compared with anything before the access.
  UNKNOWN    the root identifier is compared, but with something this tool
             cannot show is the section's entry count; or the index reaches
             through a pointer or struct field; or no entry-count symbol is
             derivable for the section at all. UNKNOWN is NOT a pass. It means
             the classifier could not prove the bound, and it fails the gate
             exactly like UNCHECKED does.

WHAT THIS TOOL CANNOT SEE -- read this before quoting its numbers
=================================================================
It is a regex-and-brace pass over comment-stripped, arm-selected text, not a C
front end. It has no include paths, no macro expansion, no types and no control
flow graph. Concretely:

  * NO DOMINANCE. "Earlier in the same function" is textual precedence, not
    dominance. A comparison in an unrelated branch counts as a check, and a
    guard whose failure path falls through to the read still counts. CHECKED
    therefore means "a plausible bound exists", never "proven safe".
  * FUNCTION BOUNDARIES. An index validated in a caller and passed down, or
    validated in a callee, is invisible. The `misc-accessor` kind is the one
    boundary handled explicitly, and only because the accessor is named.
  * COMPUTED AND STORED INDICES. An index that arrives via a struct field, an
    array element, a global written elsewhere, or arithmetic on any of those is
    reported UNKNOWN. The tool cannot follow provenance.
  * INDIRECTION. A section table reached through a pointer alias, a cast, or a
    parameter -- rather than through the symbol the `asset_table_load()` result
    was assigned to -- is not recognised as a table at all, so its subscripts
    are not sites. This is the one class that can cause an under-count, and
    there is no cheap static fix for it.
  * COUNTS THIS TOOL CANNOT DERIVE. Only the terminator-walk idiom yields an
    entry-count symbol. A count computed some other way -- game_text.c's
    `gTextTableEntries`, which comes from `game_text_table_entry_count()` --
    is not recognised, so real checks against it read as UNKNOWN.
  * PREPROCESSOR. Only this build's arms are scanned (see DEFINED). N64-only
    arms are deliberately invisible; `#if` expressions that are not a plain
    `defined(X)` are treated as taken, which keeps code rather than losing it.
    A read that exists only inside a multi-line macro body is not seen either.
  * READS AND WRITES ARE NOT DISTINGUISHED. `T[i] = ...` is reported alongside
    `... = T[i]`; both are out-of-range accesses when `i` is out of range, and
    telling them apart needs a parse this pass does not have.
  * DUPLICATE ROWS ARE DELIBERATE. One line can produce several rows -- a
    `table-index` for the offset it reads and a `section-offset` for the DMA
    that consumes it, or an inner and an outer subscript of the same
    expression. Each is a distinct bound that is or is not present, and
    collapsing them would hide one of them.
  * DECLARATIONS. A prototype or definition header is skipped by shape
    (`s32 asset_load(u32 assetIndex, ...)` is otherwise indistinguishable from
    a call), as is a subscript at file scope, which can only be a declarator or
    a static initialiser.

The ANCHORS list below pins one instance of each form. If a parser change stops
finding one, the run fails loudly instead of reporting a smaller, cleaner table.

Usage:
    tools/sweep_subentry_access.py            # the table and the summary
    tools/sweep_subentry_access.py --verbose  # plus each site's source line

Exit status: 0 only when every site is CHECKED. Any UNCHECKED site, any UNKNOWN
site, a missing anchor or an incomplete parse exits 1, so this can become a CI
gate without its meaning changing.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# game/src is the vendored decomp; platform/ is the port's own C. Both are
# scanned for the same reason tools/sweep_bug_shapes.py scans both: a port file
# is not exempt from an unbounded index. docs/ref/, lib/ and build trees hold
# reference material and third-party code that this build does not compile.
SCAN_DIRS = ["game/src", "platform"]
SKIP_DIRS = {"build", "__pycache__", ".git", "lib", "ref"}
SOURCE_SUFFIXES = (".c", ".h")

# One instance of every form the sweep claims to cover. Presence only -- pinning
# a VERDICT would make the anchors change whenever the tree is fixed, which is
# the opposite of what they are for.
ANCHORS = [
    ("table-index", "game/src/object_models.c", "gObjectModelTable"),
    ("table-index", "game/src/textures_sprites.c", "gTextureAssetTable"),
    ("table-index", "game/src/screen_asset.c", "screenTable"),
    ("table-index", "game/src/menu.c", "gAssetsMenuElementIds"),
    ("table-index", "game/src/objects.c", "gAssetsMiscTable"),
    ("terminator-walk", "game/src/objects.c", "gAssetsMiscTable"),
    ("misc-accessor", "game/src/racer.c", "get_misc_asset"),
    ("section-offset", "game/src/game_text.c", "ASSET_GAME_TEXT_TABLE"),
]

IDENT = r"[A-Za-z_]\w*"
IDENT_RE = re.compile(IDENT)
# The decomp's global naming conventions. A symbol matching one of these is
# visible across functions and files; anything else is treated as a local and
# scoped to the function it was written in.
GLOBALISH = re.compile(r"^(g[A-Z]|s[A-Z]|D_)")
CONSTISH = re.compile(r"^[A-Z][A-Z0-9_]*$")
KEYWORDS = {"if", "for", "while", "switch", "return", "else", "do", "sizeof",
            "case", "default", "typedef", "struct", "union", "enum", "static",
            "extern", "const", "unsigned", "signed", "void", "char", "short",
            "int", "long", "float", "double", "u8", "u16", "u32", "u64", "s8",
            "s16", "s32", "s64", "f32", "f64", "uintptr_t", "size_t"}

# The macros this build defines (CMakeLists.txt target_compile_definitions),
# copied from tools/sweep_bug_shapes.py for the same reason it needs them: the
# sweep must see the arms we COMPILE, not both arms.
DEFINED = {"VERSION_us_v80", "_LANGUAGE_C", "MODERN_CC", "NON_MATCHING", "AVOID_UB",
           "NATIVE_PORT", "F3DDKR_GBI", "_FINALROM", "NDEBUG", "SDL_MAIN_HANDLED"}
COND_RE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")

# The loaders that materialise a whole section into RAM. Whatever their result
# is assigned to is a section table for the purposes of this sweep.
LOADERS = ("asset_table_load", "asset_table_load_zipped")
# A leading cast, at most one, with no nested parentheses. Both this and the
# missing `\*?\s*` prefix are performance-load-bearing: a nested quantifier, or
# any quantifier that can start on whitespace, backtracks quadratically over
# platform/app/app_font.h -- a generated header whose base85 payload
# strip_comments() turns into half a megabyte of spaces. Getting this wrong took
# the sweep from under a second to nearly four minutes. The pointer star before
# the name (`u32 *rawTable = ...`, `*gAssetsMenuElementIds = ...`) is admitted by
# the lookbehind instead, which does not scan.
CAST = r"(?:\([^()\n]{0,60}\)\s*)?"
ASSIGN_RE = re.compile(
    r"(?<![\w.>])(%s)\s*(\[[^\]\n]*\])?\s*=\s*%s(%s)\s*\(\s*(ASSET_\w+)"
    % (IDENT, CAST, "|".join(LOADERS)))
# The named ASSET_MISC sub-asset accessors (game/src/objects.c). Both take an
# entry index and both bounds-check it against gAssetsMiscTableLength.
MISC_ACCESSORS = ("get_misc_asset", "get_misc_asset_size")
MISC_CALL_RE = re.compile(r"(?<![\w.>])(%s)\s*\(" % "|".join(MISC_ACCESSORS))
# Piecewise reads of a section by byte offset.
OFFSET_FUNCS = {"asset_load": 2, "asset_rom_offset": 1}
OFFSET_CALL_RE = re.compile(r"(?<![\w.>])(%s)\s*\(" % "|".join(OFFSET_FUNCS))
LOOP_RE = re.compile(r"(?<![\w.>])(while|for)\s*\(")
# A comparison against a section terminator: -1, 0xFFFFFFFF or 0.
TERMINATOR_RE = re.compile(
    r"(?:(?:==|!=)\s*\(?\s*(?:-\s*1|0[xX][fF]{8}[uU]?|0)\s*\)?"
    r"|\(?\s*(?:-\s*1|0[xX][fF]{8}[uU]?)\s*\)?\s*(?:==|!=))")
CMP = r"(?:<=|>=|==|!=|<|>)"
LITERAL_RE = re.compile(r"^\s*\(?\s*[+-]?\s*(?:0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\s*\)?\s*$")

VERDICT_CHECKED = "CHECKED"
VERDICT_UNCHECKED = "UNCHECKED"
VERDICT_UNKNOWN = "UNKNOWN"

PARSE_ERRORS = []


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
                stack.append((True, True, False) if val is None else (val, val, True))
            elif d in ("else", "elif") and stack:
                _arm_live, taken, decidable = stack[-1]
                stack[-1] = (True, True, False) if not decidable else (not taken, True, True)
            elif d == "endif" and stack:
                stack.pop()
            out.append("")
            continue
        out.append(line if live else "")
    return "\n".join(out)


def span_end(text, open_idx, op, cl):
    """Index of the bracket matching the one at `open_idx`, or -1."""
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == op:
            depth += 1
        elif text[i] == cl:
            depth -= 1
            if depth == 0:
                return i
    return -1


DEF_START = re.compile(r"^[A-Za-z_][^;#]*$")


def function_lines(text):
    """Yield (name, first_line, last_line) for every top-level definition.

    Line-based rather than brace-based, and deliberately so -- the reasoning is
    tools/sweep_bug_shapes.py's: a preprocessor arm may carry an unbalanced
    brace, which defeats any pure brace matcher no matter which arms you select.
    The decomp is uniformly clang-formatted, so a top-level definition starts in
    column 0 and ends at the first line beginning with `}` in column 0.
    """
    lines = text.split("\n")
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if not line or line[0].isspace() or line[0] in "#}":
            i += 1
            continue
        if not DEF_START.match(line):
            i += 1
            continue
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
        if rest == "":
            if j + 1 >= n or lines[j + 1].strip() != "{":
                i += 1
                continue
            j += 1
        open_paren = -1
        depth = 0
        for k in range(hdr.rindex(")"), -1, -1):
            if hdr[k] == ")":
                depth += 1
            elif hdr[k] == "(":
                depth -= 1
                if depth == 0:
                    open_paren = k
                    break
        if open_paren < 0:
            i += 1
            continue
        m = re.search(r"(%s)\s*$" % IDENT, hdr[:open_paren])
        if not m or m.group(1) in KEYWORDS:
            i += 1
            continue
        k = j + 1
        while k < n and not lines[k].startswith("}"):
            k += 1
        yield m.group(1), i + 1, min(k, n - 1) + 1
        i = k + 1


class Source(object):
    """One scanned file: processed text plus the offset/line machinery."""

    def __init__(self, rel, raw):
        self.rel = rel
        self.raw_lines = raw.split("\n")
        self.text = select_arms(strip_comments(raw))
        self.starts = [0]
        for line in self.text.split("\n"):
            self.starts.append(self.starts[-1] + len(line) + 1)
        self.functions = list(function_lines(self.text))
        # line -> (name, first, last), so "which function is this site in" is a
        # lookup rather than a scan; the sweep asks it tens of thousands of times.
        self.fn_at = {}
        for name, first, last in self.functions:
            for ln in range(first, last + 1):
                self.fn_at[ln] = (name, first, last)
        self.loop_spans = None
        self.assigns = list(ASSIGN_RE.finditer(self.text))
        # Fail closed the same way sweep_bug_shapes.py does: every top-level
        # definition ends at a `}` in column 0, so losing track of function
        # boundaries would silently shrink every "earlier in the same function"
        # search window. A shortfall is an error, never an all-clear.
        closers = self._closer_count()
        if len(self.functions) < closers:
            PARSE_ERRORS.append(
                "%s -- %d definition(s) parsed but %d column-0 '}' line(s); "
                "the enclosing-function window for this file is INCOMPLETE"
                % (rel, len(self.functions), closers))

    def _closer_count(self):
        """Column-0 `}` lines that could be a function's closing brace.

        The upper bound the shortfall check compares against. Two things in this
        tree end in column 0 without being a function, and counting them would
        raise a false shortfall on files that hold no sites at all: the body of a
        multi-line `#define` (game/src/math_util.h's CLAMP) and a C++ `namespace`
        close in the launcher headers (`}  // namespace ui`). The first is
        excluded by tracking backslash continuations, the second by requiring the
        RAW line to be exactly `}` -- a trailing comment disqualifies it.
        """
        count, continued = 0, False
        for i, ln in enumerate(self.text.split("\n")):
            raw = self.raw_lines[i] if i < len(self.raw_lines) else ""
            was_continued = continued
            stripped = raw.strip()
            if stripped.startswith("#") or was_continued:
                continued = stripped.endswith("\\")
            else:
                continued = False
            if was_continued:
                continue
            if ln.rstrip() == "}" and raw.rstrip() == "}":
                count += 1
        return count

    def line_of(self, off):
        lo, hi = 0, len(self.starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.starts[mid] <= off:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def offset_of(self, line):
        return self.starts[min(max(line - 1, 0), len(self.starts) - 1)]

    def enclosing(self, line):
        return self.fn_at.get(line, (None, None, None))

    def loops(self):
        """Every `while`/`for` header, as (open, close, condition text)."""
        if self.loop_spans is None:
            spans = []
            for m in LOOP_RE.finditer(self.text):
                open_idx = m.end() - 1
                close = span_end(self.text, open_idx, "(", ")")
                if close > 0:
                    spans.append((open_idx, close, self.text[open_idx + 1:close]))
            self.loop_spans = spans
        return self.loop_spans

    def source_line(self, line):
        if 0 < line <= len(self.raw_lines):
            return re.sub(r"\s+", " ", self.raw_lines[line - 1]).strip()
        return ""


def load_sources():
    out = []
    for d in SCAN_DIRS:
        base = os.path.join(ROOT, d)
        for dirpath, dirs, files in os.walk(base):
            dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS)
            for f in sorted(files):
                if not f.endswith(SOURCE_SUFFIXES):
                    continue
                path = os.path.join(dirpath, f)
                rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                with open(path, errors="replace") as fh:
                    out.append(Source(rel, fh.read()))
    return out


def subscripts_after(text, i):
    """Collect the consecutive `[...]` groups starting at or after `i`."""
    out = []
    while i < len(text):
        j = i
        while j < len(text) and text[j] in " \t\n":
            j += 1
        if j >= len(text) or text[j] != "[":
            break
        k = span_end(text, j, "[", "]")
        if k < 0:
            break
        out.append(text[j + 1:k])
        i = k + 1
    return out


def call_args(text, open_idx):
    """Split one call's argument list, given the index of its `(`."""
    close = span_end(text, open_idx, "(", ")")
    if close < 0:
        return None, -1
    inner = text[open_idx + 1:close]
    args, depth, cur = [], 0, ""
    for ch in inner:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip() or args:
        args.append(cur.strip())
    return args, close


PARAM_RE = re.compile(r"(?:const\s+)?[A-Za-z_]\w*\s*\**\s*[A-Za-z_]\w*(?:\s*\[\s*\d*\s*\])?")


def is_prototype(args):
    """True when this `name(...)` is a declaration or definition, not a call.

    `s32 asset_load(u32 assetIndex, uintptr_t address, s32 assetOffset, s32 size)`
    is indistinguishable from a call by shape alone. A parameter is a bare
    `type name` pair with no operators and no parentheses, which no real
    argument in this tree is -- a cast argument such as `(uintptr_t)gSoundBank`
    carries parentheses and is not mistaken for one.
    """
    for a in args:
        a = a.strip()
        if a and PARAM_RE.fullmatch(a) and IDENT_RE.match(a).group(0) in KEYWORDS:
            return True
    return False


def idents_in(expr):
    return [t for t in IDENT_RE.findall(expr) if t not in KEYWORDS]


def index_root(expr):
    """The identifier a bound would have to be compared against, or None."""
    for tok in idents_in(expr):
        if not CONSTISH.match(tok):
            return tok
    return None


# --------------------------------------------------------------------------- #
# pass 1: which symbols hold a materialised asset section
# --------------------------------------------------------------------------- #
def table_key(src, name, line):
    """Repo-wide for a decomp global, function-scoped otherwise."""
    if GLOBALISH.match(name):
        return (name,)
    fn, _first, _last = src.enclosing(line)
    return (name, src.rel, fn)


def discover_tables(sources):
    tables = {}
    for src in sources:
        for m in src.assigns:
            name, sub, _loader, section = m.group(1), m.group(2), m.group(3), m.group(4)
            if name in KEYWORDS:
                continue
            line = src.line_of(m.start(1))
            key = table_key(src, name, line)
            rec = tables.setdefault(key, {
                "name": name, "arity": 0, "sections": set(), "assigns": [],
                "counts": set(), "derived_from": None,
            })
            if sub is not None:
                rec["arity"] = 1
            rec["sections"].add(section)
            rec["assigns"].append((src.rel, line, section, norm_sub(sub)))
    return tables


def norm_sub(sub):
    return re.sub(r"\s+", "", sub or "")[1:-1] if sub else ""


# `X[i] = <expression mentioning T[...]>` -- an element-wise REBUILD of a section
# table into a second array. The port does this twice, because ASSET_PARTICLES_TABLE
# and ASSET_PARTICLE_BEHAVIORS_TABLE are 4-byte offset arrays the game reinterprets
# in place as pointer arrays, which LP64 makes impossible (particles.c). The rebuilt
# array has one slot per section entry and exactly the same bound, so its subscripts
# are sub-entry reads too -- and without this pass every one of them, roughly a
# quarter of particles.c, is outside the sweep entirely. One hop only: an alias of
# an alias is not followed.
DERIVED_RE = re.compile(r"(?<![\w.>])(%s)\s*\[\s*(%s)\s*\]\s*=\s*([^;]{0,400})"
                        % (IDENT, IDENT))


def discover_derived(tables, sources):
    added = {}
    for src in sources:
        local = tables_in_file(tables, src)
        if not local:
            continue
        for m in DERIVED_RE.finditer(src.text):
            name, rhs = m.group(1), m.group(3)
            if name in KEYWORDS:
                continue
            line = src.line_of(m.start(1))
            key = table_key(src, name, line)
            if key in tables or key in added:
                continue
            for skey, rec in local:
                if not in_scope(skey, src, line):
                    continue
                if not re.search(r"(?<![\w.>])%s\s*\[" % re.escape(rec["name"]), rhs):
                    continue
                # Attribute the rebuild to the source table's most recent load,
                # not to the union: particles.c loads two different sections
                # into the same local `rawTable`, one after the other.
                sect = resolve_section(rec, src, line, "")
                added[key] = {
                    "name": name, "arity": 0, "sections": set(sect.split("|")),
                    "assigns": [], "counts": set(rec["counts"]),
                    "derived_from": rec["name"],
                }
                break
    tables.update(added)


def tables_in_file(tables, src):
    """The table records that could possibly be referenced in this file."""
    return [(key, rec) for key, rec in tables.items()
            if (len(key) == 1 or key[1] == src.rel) and rec["name"] in src.text]


def in_scope(key, src, line):
    return len(key) == 1 or (key[1] == src.rel and key[2] == src.enclosing(line)[0])


# --------------------------------------------------------------------------- #
# pass 2: the entry-count symbol for each section table
# --------------------------------------------------------------------------- #
FOLLOWUP_RE = re.compile(r"(?<![\w.>])(%s)\s*(?:\[[^\]\n]*\])?\s*=\s*[^;]*?"
                         r"(?<![\w.>])%s(?![\w])")


def discover_counts(tables, sources):
    """Learn each table's entry-count symbol from the terminator-walk idiom.

    `while (T[len] != -1) len++;` and `for (i = 0; T[i] != -1; i++) {}` are the
    only ways this tree computes a count, so they are the only ways this sweep
    learns one. A count that is computed some other way stays unknown, and the
    sites that compare against it are reported UNKNOWN rather than CHECKED.
    """
    for src in sources:
        local = tables_in_file(tables, src)
        if not local:
            continue
        for open_idx, close, cond in src.loops():
            if not TERMINATOR_RE.search(cond):
                continue
            line = src.line_of(open_idx)
            for key, rec in local:
                if rec["name"] not in cond or not in_scope(key, src, line):
                    continue
                for hit in re.finditer(r"(?<![\w.>])%s(?![\w])" % re.escape(rec["name"]),
                                       cond):
                    subs = subscripts_after(cond, hit.end())
                    if len(subs) < rec["arity"] + 1:
                        continue
                    # `T[count]` and `T[count + 1]` are both the walk idiom --
                    # particles.c uses the second form so that the count lands
                    # one below the terminator slot, and reading only the bare
                    # form left every particle table with no derivable bound.
                    cm = re.fullmatch(r"\s*(%s)\s*(?:[-+]\s*\d+\s*)?" % IDENT,
                                      subs[rec["arity"]])
                    if not cm or cm.group(1) in KEYWORDS:
                        continue
                    counter = cm.group(1)
                    rec["counts"].add(count_key(src, counter, line))
                    # `gTextureTableSize[TEX_TABLE_2D] = --i;` -- the durable
                    # count is assigned FROM the walk variable just after the
                    # loop. Three lines is the whole idiom in this tree.
                    tail = src.text[close:src.offset_of(line + 4)]
                    for fm in re.finditer(FOLLOWUP_RE.pattern % (IDENT, re.escape(counter)),
                                          tail):
                        derived = fm.group(1)
                        if derived in KEYWORDS or derived == counter:
                            continue
                        rec["counts"].add(count_key(src, derived, line))


def count_key(src, name, line):
    if GLOBALISH.match(name):
        return (name,)
    fn, _first, _last = src.enclosing(line)
    return (name, src.rel, fn)


def counts_in_scope(rec, src, line):
    fn, _first, _last = src.enclosing(line)
    return set(k[0] for k in rec["counts"]
               if len(k) == 1 or (k[1] == src.rel and k[2] == fn))


def section_counts(tables, section, src, line):
    """Every entry-count symbol known for `section` and in scope here."""
    out = set()
    for _key, rec in tables.items():
        if section in rec["sections"]:
            out |= counts_in_scope(rec, src, line)
    return out


# --------------------------------------------------------------------------- #
# pass 3: classify one site
# --------------------------------------------------------------------------- #
def preceding_window(src, off, floor=None):
    """The function text before `off`, or the file text when outside a function.

    `floor` narrows the window to start at the table's most recent load. It
    matters: game/src/game.c reloads `gTempAssetTable` from a second section in
    the same function, and without the floor the bound belonging to the FIRST
    section reads as a bound on the second.
    """
    line = src.line_of(off)
    _fn, first, _last = src.enclosing(line)
    start = src.offset_of(first) if first else 0
    if floor is not None:
        start = max(start, floor)
    return src.text[start:off]


def comparisons_of(window, root):
    """Every other operand `root` is compared against in `window`.

    The optional `[+-] term` tail matters: `index + 1 > gAssetsMiscTableLength`
    is how game/src/objects.c spells the bound on every accessor that reads both
    `table[index]` and `table[index + 1]`, and a regex anchored straight onto the
    comparison operator misses all of them.
    """
    others = []
    pat = (r"(?<![\w.>])%s(?![\w])\s*(?:[-+]\s*\w+\s*)?(?:%s)\s*\(?\s*([-+]?\w+)"
           % (re.escape(root), CMP))
    for m in re.finditer(pat, window):
        others.append(m.group(1))
    pat = (r"([-+]?\w+)\s*\)?\s*(?:%s)\s*\(?\s*(?<![\w.>])%s(?![\w])"
           % (CMP, re.escape(root)))
    for m in re.finditer(pat, window):
        others.append(m.group(1))
    return others


def classify(src, off, expr, counts, extra_note="", floor=None):
    """CHECKED / UNCHECKED / UNKNOWN for one index expression at `off`."""
    root = index_root(expr)
    if root is None:
        return (VERDICT_UNCHECKED,
                "compile-time index; a v80 constant cannot be bounded at all")
    if "->" in expr or re.search(r"[\w\]\)]\s*\.\s*[A-Za-z_]", expr):
        return (VERDICT_UNKNOWN,
                "index reaches through `%s`; provenance not tracked" % root)
    window = preceding_window(src, off, floor)
    others = comparisons_of(window, root)
    hits = [o for o in others if IDENT_RE.match(o or "") and
            IDENT_RE.match(o).group(0) in counts]
    if hits:
        return (VERDICT_CHECKED,
                "`%s` compared against `%s` earlier in the function"
                % (root, sorted(set(IDENT_RE.match(h).group(0) for h in hits))[0]))
    # Tested only after a real comparison has been ruled out: a loop variable can
    # be BOTH this function's terminator-walk counter and a properly bounded
    # index further down (game/src/game.c does exactly that with `i`).
    if root in counts:
        return (VERDICT_UNKNOWN,
                "index IS the table's own derived entry count (`%s`)" % root)
    if not counts:
        return (VERDICT_UNKNOWN,
                "no entry-count symbol is derivable for this section%s" % extra_note)
    if others:
        return (VERDICT_UNKNOWN,
                "`%s` compared against %s, none of which is the section count"
                % (root, ", ".join("`%s`" % o for o in sorted(set(others))[:3])))
    return (VERDICT_UNCHECKED,
            "`%s` is never compared with anything before the access" % root)


# --------------------------------------------------------------------------- #
# pass 3: find the sites
# --------------------------------------------------------------------------- #
def sweep_tables(src, tables):
    """table-index and terminator-walk sites."""
    hits = []
    walks = [(a, b) for a, b, c in src.loops() if TERMINATOR_RE.search(c)]
    assign_spans = set(m.start(1) for m in src.assigns)
    for key, rec in tables_in_file(tables, src):
        name = rec["name"]
        forms = [r"(?<![\w.>])%s(?![\w])" % re.escape(name),
                 r"\(\s*\*\s*%s\s*\)" % re.escape(name)]
        seen = set()
        for form in forms:
            for m in re.finditer(form, src.text):
                if m.start() in seen or m.start() in assign_spans:
                    continue
                line = src.line_of(m.start())
                if not in_scope(key, src, line):
                    continue
                # A subscript at file scope is a declarator or a static
                # initialiser (`s16 *gAssetsMenuElementIds[1] = { NULL };`), not
                # a read; nothing executes there.
                if src.enclosing(line)[0] is None:
                    continue
                subs = subscripts_after(src.text, m.end())
                if len(subs) < rec["arity"] + 1:
                    continue
                seen.add(m.start())
                expr = subs[rec["arity"]].strip()
                off = m.start()
                in_loop = any(a < off < b for a, b in walks)
                counts = counts_in_scope(rec, src, line)
                section = resolve_section(rec, src, line,
                                          norm_sub("[%s]" % subs[0]) if rec["arity"] else "")
                if in_loop:
                    hits.append((src.rel, line, section, VERDICT_UNCHECKED,
                                 "terminator-walk", "%s[%s]" % (name, expr),
                                 "walks to the section terminator; bounded by a "
                                 "byte pattern, not by an entry count"))
                    continue
                verdict, note = classify(src, off, expr, counts,
                                         floor=reload_floor(rec, src, line))
                hits.append((src.rel, line, section, verdict, "table-index",
                             "%s[%s]" % (name, expr), note))
    return hits


def reload_floor(rec, src, line):
    """Offset of this table's most recent load inside the enclosing function."""
    _fn, first, last = src.enclosing(line)
    best = None
    for rel, aline, _section, _sub in rec["assigns"]:
        if rel == src.rel and first and first <= aline <= last and aline <= line:
            if best is None or aline > best:
                best = aline
    return src.offset_of(best) if best else None


def resolve_section(rec, src, line, selector):
    """Which section this site reads, when the symbol serves more than one.

    Two disambiguators, in order: the array-of-tables selector subscript
    (`gTextureAssetTable[TEX_TABLE_2D]` vs `[TEX_TABLE_3D]`), then the nearest
    preceding assignment inside the same function (`gTempAssetTable` is reloaded
    from a different section in each of its four users). Neither applies when
    the selector is itself a variable, so the union is reported rather than a
    guess.
    """
    if rec["arity"] and selector:
        matches = set(s for _r, _l, s, sub in rec["assigns"] if sub == selector)
        if len(matches) == 1:
            return matches.pop()
    fn, first, last = src.enclosing(line)
    best = None
    for rel, aline, section, _sub in rec["assigns"]:
        if rel == src.rel and first and first <= aline <= last and aline <= line:
            if best is None or aline > best[0]:
                best = (aline, section)
    if best:
        return best[1]
    return "|".join(sorted(rec["sections"]))


def sweep_misc(src, _tables):
    """misc-accessor sites: the named ASSET_MISC sub-asset accessors."""
    hits = []
    for m in MISC_CALL_RE.finditer(src.text):
        fname = m.group(1)
        args, _close = call_args(src.text, m.end() - 1)
        if args is None or not args or is_prototype(args):
            continue
        line = src.line_of(m.start())
        if src.enclosing(line)[0] in MISC_ACCESSORS:
            continue  # the accessor's own definition header
        expr = args[0]
        # The accessor's guard is `index < 0 || index >= gAssetsMiscTableLength`
        # (game/src/objects.c). Every call site inherits it; that is a real
        # entry-count comparison, so the verdict is CHECKED -- but the fallback
        # is a silent one, which Task 2 has to know about.
        hits.append((src.rel, line, "ASSET_MISC", VERDICT_CHECKED, "misc-accessor",
                     "%s(%s)" % (fname, expr.strip()),
                     "bounded inside %s() against gAssetsMiscTableLength; "
                     "out of range returns the section base SILENTLY" % fname))
    return hits


def sweep_offsets(src, tables):
    """section-offset sites: a piecewise read at a computed offset."""
    hits = []
    for m in OFFSET_CALL_RE.finditer(src.text):
        fname = m.group(1)
        args, _close = call_args(src.text, m.end() - 1)
        if args is None or is_prototype(args):
            continue
        pos = OFFSET_FUNCS[fname]
        if len(args) <= pos:
            continue
        expr = args[pos].strip()
        if LITERAL_RE.match(expr) and not re.search(r"[1-9]", expr):
            continue  # a literal 0 offset is the section base, not an entry
        line = src.line_of(m.start())
        section = args[0].strip()
        if not section.startswith("ASSET_"):
            section = "?"
        counts = section_counts(tables, section, src, line) if section != "?" else set()
        note_tail = ("" if section != "?"
                     else "; the section argument is not a literal ASSET_* enum")
        verdict, note = classify(src, m.start(), expr, counts, note_tail)
        hits.append((src.rel, line, section, verdict, "section-offset",
                     "%s(..., %s, ...)" % (fname, re.sub(r"\s+", " ", expr)[:48]),
                     note))
    return hits


def collect():
    sources = load_sources()
    tables = discover_tables(sources)
    discover_counts(tables, sources)
    discover_derived(tables, sources)
    hits = []
    for src in sources:
        hits += sweep_tables(src, tables)
        hits += sweep_misc(src, tables)
        hits += sweep_offsets(src, tables)
    return sources, tables, sorted(set(hits), key=lambda h: (h[0], h[1], h[4], h[5]))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print the matched source line under each site")
    args = ap.parse_args()

    sources, tables, hits = collect()
    by_source = {s.rel: s for s in sources}

    widths = [9, 15, 0, 0]
    for h in hits:
        widths[2] = max(widths[2], len("%s:%d" % (h[0], h[1])))
        widths[3] = max(widths[3], len(h[2]))
    header = ("%-*s  %-*s  %-*s  %-*s  %s"
              % (widths[0], "VERDICT", widths[1], "KIND", widths[2], "FILE:LINE",
                 widths[3], "SECTION", "INDEX EXPRESSION / WHY"))
    print(header)
    print("-" * len(header))
    for rel, line, section, verdict, kind, expr, note in hits:
        print("%-*s  %-*s  %-*s  %-*s  %s -- %s"
              % (widths[0], verdict, widths[1], kind, widths[2],
                 "%s:%d" % (rel, line), widths[3], section, expr, note))
        if args.verbose:
            print("%s| %s" % (" " * 4, by_source[rel].source_line(line)))

    checked = sum(1 for h in hits if h[3] == VERDICT_CHECKED)
    unchecked = sum(1 for h in hits if h[3] == VERDICT_UNCHECKED)
    unknown = sum(1 for h in hits if h[3] == VERDICT_UNKNOWN)
    kinds = {}
    for h in hits:
        kinds[h[4]] = kinds.get(h[4], 0) + 1
    print("")
    print("sweep_subentry_access: %d site(s) in %d file(s); %d section table symbol(s), "
          "%d with a derivable entry count"
          % (len(hits), len(set(h[0] for h in hits)), len(tables),
             sum(1 for r in tables.values() if r["counts"])))
    print("  by kind:    %s"
          % ", ".join("%s %d" % (k, kinds[k]) for k in sorted(kinds)))
    print("  by verdict: CHECKED %d, UNCHECKED %d, UNKNOWN %d"
          % (checked, unchecked, unknown))
    print("  UNKNOWN is not a pass: it means the classifier could not prove a bound.")

    missing = []
    for kind, rel, needle in ANCHORS:
        if not any(h[4] == kind and h[0] == rel and (needle in h[5] or needle in h[2])
                   for h in hits):
            missing.append("%s %s %s" % (kind, rel, needle))
    for entry in missing:
        print("ANCHOR MISSING: %s" % entry, file=sys.stderr)
    for problem in PARSE_ERRORS:
        print("PARSE SHORTFALL: %s" % problem, file=sys.stderr)
    if missing:
        print("ANCHORS: %d pinned instance(s) no longer found; this table is NOT a "
              "complete enumeration" % len(missing), file=sys.stderr)
    if PARSE_ERRORS:
        print("PARSE ERRORS: %d file(s) incompletely parsed; findings are NOT a "
              "complete enumeration" % len(PARSE_ERRORS), file=sys.stderr)

    if missing or PARSE_ERRORS or unchecked or unknown:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
