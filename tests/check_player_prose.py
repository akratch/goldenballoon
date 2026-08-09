#!/usr/bin/env python3
"""Ban AI-slop vocabulary from every string a player or a reviewer reads.

The owner's standing complaint is that some of the launcher's verbose text
reads like AI slop. `docs/CAMPAIGN_FAITHFUL_ENHANCED.md` section 4 names the
rule: player-facing text should read like a game developer wrote it -- short,
concrete, specific -- and bans a starting vocabulary list (*seamlessly,
leverage, ensure, robust, "enhance your experience", comprehensive, "authored
presentation states", "safe frame boundary", "durable storage", "compatibility
mode"*). This gate makes that rule mechanical instead of aspirational.

Scope, and why it stops where it does
--------------------------------------
"Player-facing" is drawn narrowly on purpose, matching the campaign doc:

* **Launcher/app UI strings** -- every `.cpp`/`.h` under `platform/app/`,
  which is where the player reads a status line, a help paragraph, or an
  error. Only *string literal contents* are scanned, not the surrounding
  code or comments: an engineer's comment explaining a "safe frame boundary"
  precondition is precise and correct; the same phrase inside a status string
  the player reads is the slop this gate exists to catch. Conflating the two
  is exactly the kind of gate that gets disabled within a week, so the
  scanner below tokenizes C/C++ source and only ever looks inside `"..."`.
* **Release notes** -- `RELEASE_NOTES.md`, the only doc this project
  publishes as changelog copy for players (see the top of the file: "*The
  smooth, modern version..."). It is intentionally plain, concrete prose, and
  `tests/test_product_claim_boundaries.py` already pins some of its factual
  claims; this gate pins its register instead.
* **`README.md`** -- the repo's front door; a reviewer or player reads this
  before anything else, and `tests/test_product_claim_boundaries.py` already
  treats its wording as a product claim surface this project owns.

Deliberately OUT of scope: engineering docs (`docs/APP_SHELL.md`,
`docs/open-items/`, `docs/CAMPAIGN_*.md`, `docs/RELEASE_CHECKLIST.md`, ...),
`CHANGELOG.md` (a technical, historical record for contributors -- see its own
header, "detailed test inventory is in tests/README.md" -- not curated player
copy, and rewriting past entries to satisfy a lint would be revising history
rather than fixing prose), code comments anywhere, and test output. Those
legitimately use precise technical vocabulary ("ensure the lock is held",
"the comprehensive fault matrix", "robust to a torn write"), and a gate that
flags them would be turned off within a week -- which is the failure mode
this gate exists to avoid repeating.

Banned vocabulary
------------------
The campaign doc's list, plus the same pattern extended: words whose job is
to sound thorough rather than to inform. Multi-word phrases match as
substrings (case-insensitive); single words match on a word boundary so
"ensure" does not also flag "insurer".

Positive control: `_self_test()` seeds a `"seamlessly"` fixture and asserts
the scanner flags it. Run standalone with `--self-test-only` to check just
the control without touching the real tree.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent

# Single words: matched case-insensitively on a word boundary.
BANNED_WORDS = (
    "seamlessly",
    "seamless",
    "leverage",
    "ensure",
    "robust",
    "comprehensive",
    "utilize",
    "utilizes",
    "utilizing",
    "effortlessly",
    "elevate",
    "empower",
    "streamlined",
    "delve",
    "showcase",
    "boasts",
)

# Multi-word phrases: matched case-insensitively as substrings.
BANNED_PHRASES = (
    "enhance your experience",
    "authored presentation states",
    "safe frame boundary",
    "durable storage",
    "compatibility mode",
    "cutting-edge",
    "cutting edge",
    "state-of-the-art",
    "unlock the full potential",
    "harness the power",
    "best-in-class",
)

_WORD_PATTERNS = [
    (word, re.compile(r"\b" + re.escape(word) + r"\b", re.IGNORECASE))
    for word in BANNED_WORDS
]
_PHRASE_PATTERNS = [(phrase, phrase.lower()) for phrase in BANNED_PHRASES]


def find_banned(text: str) -> list[str]:
    """Return the banned terms present in ``text``, each listed once."""
    hits: list[str] = []
    lowered = text.lower()
    for word, pattern in _WORD_PATTERNS:
        if pattern.search(text):
            hits.append(word)
    for phrase, needle in _PHRASE_PATTERNS:
        if needle in lowered:
            hits.append(phrase)
    return hits


# --- C/C++ string-literal extraction --------------------------------------
#
# Only string literal *contents* are scanned. This walks the source once,
# tracking line/block comments and char/string literals, and yields the text
# inside each string literal along with the source line it starts on.
_TOKEN_RE = re.compile(
    r"""
      (?P<line_comment>//[^\n]*)
    | (?P<block_comment>/\*.*?\*/)
    | (?P<string>L?"(?:\\.|[^"\\])*")
    | (?P<char>L?'(?:\\.|[^'\\])*')
    """,
    re.VERBOSE | re.DOTALL,
)


_SIMPLE_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0",
}


def _unescape(body: str) -> str:
    """Undo the handful of C escapes that matter for prose scanning.

    This deliberately does not attempt a full C escape grammar (octal, hex,
    universal-character-name): the gate only needs the literal's readable
    text, and an unrecognised escape (``\\?`` and friends) is left verbatim
    rather than risking mangling raw UTF-8 bytes the way ``str.encode()
    .decode("unicode_escape")`` does.
    """
    out: list[str] = []
    i = 0
    while i < len(body):
        ch = body[i]
        if ch == "\\" and i + 1 < len(body):
            nxt = body[i + 1]
            out.append(_SIMPLE_ESCAPES.get(nxt, nxt))
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def extract_string_literals(source: str) -> list[tuple[int, str]]:
    """Return (line_number, decoded_content) for every string literal.

    Adjacent string literals (C/C++ implicit concatenation, e.g. a
    ``std::snprintf`` format string split across lines) are joined into one
    logical unit if only whitespace/comments separate them, matching how a
    player actually reads the sentence.
    """
    literals: list[tuple[int, str]] = []
    pos = 0
    line = 1
    pending_text: str | None = None
    pending_line = 0
    for match in _TOKEN_RE.finditer(source):
        # Account for lines consumed between tokens (and outside any token,
        # which is source we don't care about but still must count for line
        # numbers).
        line += source.count("\n", pos, match.start())
        gap = source[pos:match.start()]
        pos = match.end()
        if match.lastgroup == "string":
            raw = match.group("string")
            body = raw[1:-1] if not raw.startswith("L") else raw[2:-1]
            body = _unescape(body)
            if pending_text is not None and gap.strip() == "":
                pending_text += body
            else:
                if pending_text is not None:
                    literals.append((pending_line, pending_text))
                pending_text = body
                pending_line = line
        else:
            # A non-string token (comment/char literal) that isn't pure
            # whitespace breaks an implicit-concatenation run.
            if pending_text is not None and match.lastgroup != "char":
                literals.append((pending_line, pending_text))
                pending_text = None
        line += source[match.start():match.end()].count("\n")
    if pending_text is not None:
        literals.append((pending_line, pending_text))
    return literals


def scan_cpp_source(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    failures: list[str] = []
    for line_no, literal in extract_string_literals(text):
        for term in find_banned(literal):
            snippet = literal.strip()
            if len(snippet) > 90:
                snippet = snippet[:87] + "..."
            failures.append(
                f"{path.relative_to(ROOT)}:{line_no}: banned term {term!r} in "
                f"player-facing string: {snippet!r}"
            )
    return failures


def scan_doc(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    failures: list[str] = []
    for line_no, line in enumerate(text.splitlines(), 1):
        for term in find_banned(line):
            failures.append(
                f"{path.relative_to(ROOT)}:{line_no}: banned term {term!r} in "
                f"player-facing doc: {line.strip()!r}"
            )
    return failures


def player_facing_cpp_files() -> list[Path]:
    app_dir = ROOT / "platform" / "app"
    return sorted(app_dir.glob("*.cpp")) + sorted(app_dir.glob("*.h"))


def player_facing_docs() -> list[Path]:
    return [ROOT / "RELEASE_NOTES.md", ROOT / "README.md"]


def run_scan() -> list[str]:
    failures: list[str] = []
    for path in player_facing_cpp_files():
        failures.extend(scan_cpp_source(path))
    for path in player_facing_docs():
        failures.extend(scan_doc(path))
    return failures


def _self_test() -> None:
    """Positive control: a seeded 'seamlessly' must fail the scanner."""
    seeded_cpp = (
        'const char *ok() { return "the picture changes seamlessly here"; }\n'
    )
    hits = []
    for line_no, literal in extract_string_literals(seeded_cpp):
        for term in find_banned(literal):
            hits.append(term)
    assert "seamlessly" in hits, (
        "positive control failed: seeded 'seamlessly' was not detected by "
        "the string-literal scanner"
    )

    # The same phrase inside a comment (not a string literal) must NOT be
    # flagged -- that is the scope boundary this gate depends on.
    commented = (
        "// this applies seamlessly across restarts\n"
        'const char *ok() { return "applied"; }\n'
    )
    literal_hits = []
    for _line_no, literal in extract_string_literals(commented):
        literal_hits.extend(find_banned(literal))
    assert not literal_hits, (
        "scope control failed: a comment's prose was scanned as if it were "
        "a player-facing string"
    )

    # A doc line with banned vocabulary must be caught by the doc scanner.
    assert "leverage" in find_banned("This feature will leverage your GPU.")


def main() -> int:
    _self_test()
    if "--self-test-only" in sys.argv:
        print("check_player_prose: positive control passed")
        return 0

    failures = run_scan()
    if failures:
        print("check_player_prose: FAIL")
        for failure in failures:
            print(f"  {failure}")
        print(
            f"\n{len(failures)} banned term(s) found in player-facing text. "
            "Reword to short, concrete, specific language -- see "
            "docs/CAMPAIGN_FAITHFUL_ENHANCED.md section 4."
        )
        return 1

    print(
        "check_player_prose: PASS "
        f"({len(player_facing_cpp_files())} platform/app source files, "
        f"{len(player_facing_docs())} player-facing docs clean; "
        "positive control confirmed the scanner is not vacuous)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
