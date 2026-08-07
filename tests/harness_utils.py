"""Shared, side-effect-free helpers for the headless regression checks."""

from __future__ import annotations

import contextlib
import errno
import fcntl
import hashlib
import os
import re
import shutil
import tempfile
from collections.abc import Mapping
from pathlib import Path

# Everything eeprom_store()/eeprom_load() (platform/stubs_dkr.c) can leave in a
# save directory. A check that clears EEPROM state must clear all of it, and a
# check that borrows a real save directory must put all of it back.
EEPROM_ARTIFACTS = (
    "eeprom.bin",
    "eeprom.bin.tmp",
    "eeprom.bin.bad",
    "eeprom.bin.previous",
    "eeprom.bin.importing",
    "eeprom.bin.autosave.tmp",
    "eeprom.bin.autosave.1",
    "eeprom.bin.autosave.2",
    "eeprom.bin.autosave.3",
)

# The build directory a check falls back to when it is run by hand. The suite
# never relies on it: tools/run_checks.py always passes --build explicitly, so
# this is only the convenience default for `python3 tests/check_foo.py`. Checks
# that must run against a *differently configured* tree (sanitizers, alternate
# alignment) keep their own literal instead -- their default is part of what
# they test, not a convenience.
DEFAULT_BUILD_DIR = "build"


def resolve_binary(build: str | os.PathLike[str]) -> str:
    """Resolve ``--build`` consistently from either a directory or executable.

    Every behavioural check accepts the same contract:

    - ``--build build-rel`` resolves to ``build-rel/mdkr64``;
    - ``--build build-rel/mdkr64`` remains that executable.

    Existence is intentionally checked by each caller so its existing failure
    message and any additional input validation remain intact.
    """

    path = Path(build).expanduser()
    if path.is_dir():
        path = path / "mdkr64"
    return str(path)


def _debt_bound(value: int, expected: int | tuple[int, int]) -> bool:
    """Accept an exact expectation, or an inclusive ``(low, high)`` window."""

    if isinstance(expected, tuple):
        low, high = expected
        return low <= value <= high
    return value == expected


def _debt_text(expected: int | tuple[int, int]) -> str:
    if isinstance(expected, tuple):
        return f"{expected[0]}..{expected[1]}"
    return str(expected)


def completed_tick_conservation(
        summary: Mapping[str, int], expected_ticks: int, label: str, *,
        expected_lead: int | tuple[int, int] = 1,
        expected_max_pending: int | tuple[int, int] = 1) -> str | None:
    """Validate a finite run after its final pass, before its next ticket.

    The scheduler records the pass that reached the headless budget and only
    then prevents dispatch of a *next* pass. Consequently one already-earned
    host ticket remains pending at clean termination. This is distinct from a
    live-frame debt assertion: callers keep their own intermediate pacing
    checks, while this helper is only for the terminal summary.

    The debt high-water marks are the caller's *declared* expectation, never a
    value read back out of the run being judged. A steady arm holds only that
    one terminal ticket (``1``); an arm that deliberately groups catch-up
    passes states the number its own rate implies, and an arm whose debt is
    genuinely variable (deliberate late frames, a suspension rebase) states the
    inclusive window it is allowed to occupy. Comparing ``lead`` against
    ``maxpending`` instead would assert nothing: with ``rebases=0`` the trace
    samples clock-minus-issued at exactly the points that set the driver's own
    pending high-water, so the two sides are the same quantity and a 40-tick
    backlog satisfies the comparison as happily as a clean run does.
    """
    ticks = summary.get("ticks", -1)
    simticks = summary.get("simticks", -1)
    issued = summary.get("issued", -1)
    pending = summary.get("pending", -1)
    updates = summary.get("updates", -1)
    lead = summary.get("lead", -1)
    max_pending = summary.get("maxpending", -1)
    if ticks != expected_ticks or simticks != expected_ticks:
        return (f"{label}: completed ticks/simticks={ticks}/{simticks}, "
                f"expected {expected_ticks}/{expected_ticks}")
    if issued + pending != ticks:
        return (f"{label}: clock conservation failed: ticks={ticks}, "
                f"issued={issued}, pending={pending}")
    # With the two checks above, this is equivalent to "exactly one ticket is
    # still pending" -- issued=ticks-1 and pending=1 say the same thing here --
    # so only one of the pair is stated.
    if simticks != issued + 1:
        return (f"{label}: bootstrap completion failed: simticks={simticks}, "
                f"issued={issued}, pending={pending}")
    if updates + 1 != simticks:
        return (f"{label}: game-update completion failed: updates={updates}, "
                f"simticks={simticks}")
    if not _debt_bound(lead, expected_lead):
        return (f"{label}: scheduler lead={lead}, expected "
                f"{_debt_text(expected_lead)}")
    if not _debt_bound(max_pending, expected_max_pending):
        return (f"{label}: scheduler maxpending={max_pending}, expected "
                f"{_debt_text(expected_max_pending)}")
    return None


class PpmError(ValueError, RuntimeError):
    """A PPM that cannot be read as the image the check asked for.

    The hand-written readers this replaced were split between ``ValueError``
    and ``RuntimeError``, and their callers' ``except`` clauses followed
    whichever their own copy raised. Inheriting from both means adopting the
    shared reader cannot silently turn a check's reported failure into an
    escaped traceback, which is the one thing a consolidation like this must
    not do. New callers should catch ``ValueError``.
    """


def read_ppm(path: str | os.PathLike[str]) -> tuple[int, int, bytes]:
    """Read the binary P6 image ``--dump-frames`` writes.

    Every part of the header is checked, because a reader that skips a field
    cannot tell a real image from a truncated or mislabelled one, and a check
    that then compares pixels reports a *content* failure for what was really a
    capture failure. So: the magic must be ``P6``, the dimensions must be
    positive, the maxval must be 255 (nothing here emits 16-bit samples), and
    the raster must be exactly ``width * height * 3`` bytes -- no more, so a
    doubled or concatenated dump is caught too.

    Comments are honoured between header fields, and exactly one whitespace
    byte (or a CRLF pair) is consumed as the raster delimiter. Skipping
    arbitrary whitespace there would silently eat pixels: a legal first sample
    may itself be 0x09, 0x0A, 0x0D or 0x20.

    Returns ``(width, height, pixels)``; callers with their own ``Image`` type
    construct it as ``Image(*read_ppm(path))``.
    """

    path = Path(path)
    data = path.read_bytes()
    index = 0
    fields: list[bytes] = []
    while len(fields) < 4:
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if index >= len(data):
            raise PpmError(f"{path}: truncated PPM header")
        if data[index:index + 1] == b"#":
            newline = data.find(b"\n", index)
            if newline < 0:
                raise PpmError(f"{path}: unterminated PPM comment")
            index = newline + 1
            continue
        end = index
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        fields.append(data[index:end])
        index = end

    if fields[0] != b"P6":
        raise PpmError(f"{path}: expected a P6 PPM, got magic {fields[0]!r}")
    try:
        width, height, maximum = (int(field) for field in fields[1:])
    except ValueError as error:
        raise PpmError(
            f"{path}: non-numeric PPM header field in {fields[1:]!r}") from error
    if width <= 0 or height <= 0:
        raise PpmError(f"{path}: PPM dimensions {width}x{height} are not positive")
    if maximum != 255:
        raise PpmError(f"{path}: PPM maxval is {maximum}, expected 255")
    if index >= len(data) or not data[index:index + 1].isspace():
        raise PpmError(f"{path}: missing the whitespace before the PPM raster")
    index += 2 if data[index:index + 2] == b"\r\n" else 1

    pixels = data[index:]
    expected = width * height * 3
    if len(pixels) != expected:
        raise PpmError(
            f"{path}: raster is {len(pixels)} bytes, expected {expected} "
            f"for {width}x{height} RGB")
    return width, height, pixels


# --------------------------------------------------------------------------- #
#  EEPROM slot encoding
#
#  Layout from game/include/save_layout.h: 3 x SaveFile(40) | SaveConfig(8) |
#  CourseRecords(192) x 2 == 512. A slot's fields are an MSB-first bit stream
#  from byte 0, exactly as func_80072C54 reads it in
#  populate_settings_from_save_data() (game/src/save_data.c).
# --------------------------------------------------------------------------- #

SLOT_BYTES = 40


def put_bits(bits: list[int], width: int, value: int) -> None:
    """Append ``value`` to an MSB-first bit stream as ``width`` bits."""

    bits.extend((value >> shift) & 1 for shift in range(width - 1, -1, -1))


def pack_bits(bits: list[int]) -> bytearray:
    """Fold an MSB-first bit stream into bytes; the length must be a multiple of 8."""

    if len(bits) % 8:
        raise ValueError(f"bit stream is {len(bits)} bits, not a whole number of bytes")
    out = bytearray()
    for index in range(0, len(bits), 8):
        byte = 0
        for bit in bits[index:index + 8]:
            byte = (byte << 1) | bit
        out.append(byte)
    return out


def slot_checksum(block: bytes | bytearray) -> int:
    """The 16-bit checksum the engine stores in the first two bytes of a block.

    populate_settings_from_save_data() sums every byte *after* the stored
    checksum and adds 5. The seed is not a hash constant: the engine's loop
    starts its accumulator at the number of 16-bit words it skips over -- the
    two checksum bytes plus the header words ahead of the payload -- so the
    figure is part of the on-disk format and a slot written with a 0 seed is
    rejected as corrupt. The same routine covers the 192-byte CourseRecords
    blocks, which is why this takes any block rather than only a 40-byte slot.
    """

    return (5 + sum(block[2:])) & 0xFFFF


def seal_slot(block: bytearray) -> bytearray:
    """Write :func:`slot_checksum` into ``block``'s first two bytes, in place."""

    block[0:2] = slot_checksum(block).to_bytes(2, "big")
    return block


def slot_checksum_valid(block: bytes | bytearray) -> bool:
    """Whether ``block`` carries the checksum the engine would compute for it."""

    return int.from_bytes(block[:2], "big") == slot_checksum(block)


def config_checksum(payload: int) -> int:
    """The SaveConfig checksum: 5 plus the low 56 bits' nibbles.

    read_eeprom_settings() keeps this one in the *top* byte of the 8-byte word
    and sums nibbles 0..13 of what is left, so it is a different routine from
    :func:`slot_checksum` despite sharing the seed.
    """

    return 5 + sum((payload >> (index * 4)) & 0xF for index in range(14))


def config_block(payload: int) -> bytes:
    """An 8-byte SaveConfig word carrying ``payload`` and a valid checksum."""

    if payload >> 56:
        raise ValueError(
            f"SaveConfig payload 0x{payload:x} overlaps the checksum byte")
    return (payload | ((config_checksum(payload) & 0xFF) << 56)).to_bytes(8, "big")


# --------------------------------------------------------------------------- #
#  Fatal markers
# --------------------------------------------------------------------------- #

# The markers that mean a run died rather than disagreed. Every check wants all
# of these: [CRASH]/[FATAL] are the engine's own last words, and the sanitizer
# lines are the ones a check would otherwise scroll past while asserting on
# telemetry that a sanitizer already proved untrustworthy.
FATAL_MARKERS = (
    r"\[CRASH\]",
    r"\[FATAL\]",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
    "runtime error:",
)

# Named groups a check can add to the common set, so the intent reads at the
# call site instead of arriving as a re-typed alternation.
ABORT_MARKERS = ("SIGSEGV", "Segmentation fault", "SIGABRT", "Abort trap")
ASSERT_MARKERS = ("Assertion",)
FX_MARKERS = (r"\[FX BUG\]",)
GPU_MARKERS = ("frame queue failed", "GPU completion fence failed")
VALIDATION_MARKERS = ("Validation Error",)
DEVICE_MARKERS = ("validation error", "device lost", "shader compilation failed")


def fatal_re(*extra: str, ignore_case: bool = False) -> "re.Pattern[str]":
    """:data:`FATAL_MARKERS` plus whatever else this check treats as fatal.

    ``extra`` is regex source, not literals, so a check can pass either a bare
    word or one of the ``*_MARKERS`` groups above (``*ABORT_MARKERS``).
    """

    flags = re.IGNORECASE if ignore_case else 0
    return re.compile("|".join((*FATAL_MARKERS, *extra)), flags)


def find_fatal(output: str, *extra: str, ignore_case: bool = False) -> str | None:
    """The first fatal marker ``output`` contains, or ``None``.

    Returning the matched text rather than a bool means the failure message can
    say *which* marker fired without the caller re-scanning the log.
    """

    match = fatal_re(*extra, ignore_case=ignore_case).search(output)
    return match.group(0) if match else None


# --------------------------------------------------------------------------- #
#  "[TAG] key=value ..." telemetry rows
# --------------------------------------------------------------------------- #


def row_fields(text: str) -> dict[str, int]:
    """The integer ``key=value`` tokens in one telemetry row's payload.

    Non-integer values are skipped rather than raised on: these rows mix in
    string fields (``backend=gl``, ``reason=raf-ceiling``) that the numeric
    callers do not read, and a reader that died on them would turn adding a
    descriptive field to a log line into a test failure.
    """

    fields: dict[str, int] = {}
    for token in text.split():
        key, separator, value = token.partition("=")
        if not separator:
            continue
        try:
            fields[key] = int(value)
        except ValueError:
            continue
    return fields


def _tag_re(tags: "tuple[str, ...]") -> "re.Pattern[str]":
    if not tags:
        raise ValueError("parse_rows/parse_last need at least one tag")
    alternation = "|".join(re.escape(tag) for tag in tags)
    return re.compile(rf"\[(?:{alternation})\] (.*)")


def parse_rows(output: str, *tags: str) -> list[dict[str, int]]:
    """Every ``[TAG] key=value ...`` row in ``output``, oldest first.

    Several tags may be given when a check accepts either of two backends'
    spellings of the same row (``WGPU-BACKPRESSURE``/``GL-BACKPRESSURE``).
    """

    return [row_fields(match.group(1)) for match in _tag_re(tags).finditer(output)]


def parse_last(output: str, *tags: str, label: str | None = None,
               expect_one: bool = False) -> dict[str, int]:
    """The last ``[TAG]`` row in ``output``; raises if there is none.

    ``expect_one`` is for the checks whose whole point is that a run emitted a
    single summary -- a second row there means the run restarted something, so
    reading the last one would quietly hide it.
    """

    rows = parse_rows(output, *tags)
    prefix = f"{label}: " if label else ""
    name = "/".join(tags)
    if not rows:
        raise RuntimeError(f"{prefix}no [{name}] row was emitted")
    if expect_one and len(rows) != 1:
        raise RuntimeError(
            f"{prefix}expected one [{name}] row, got {len(rows)}")
    return rows[-1]


def text_rows(output: str, *tags: str) -> list[dict[str, str]]:
    """Every ``[TAG] key=value ...`` row, with values kept AS TEXT.

    The counterpart to parse_rows: that one drops non-integer values, which is
    right for the numeric callers and wrong for a caller reading
    ``policy=display-margin`` or ``reason=raf-ceiling``. Reading those as fields
    rather than as substrings is what stops ``requested=uncapped`` also matching
    ``requested=uncappedfoo`` and stops the field ORDER in a log line being
    load-bearing.
    """
    rows: list[dict[str, str]] = []
    for match in _tag_re(tags).finditer(output):
        row: dict[str, str] = {}
        for token in match.group(1).split():
            key, separator, value = token.partition("=")
            if separator:
                row[key] = value
        rows.append(row)
    return rows


def present_mode_rows(output: str) -> list[dict[str, str]]:
    """Every ``[PRESENT-MODE]`` row a run emitted, as key/value pairs."""
    return text_rows(output, "PRESENT-MODE")


def present_policy_rows(output: str) -> list[dict[str, str]]:
    """Every ``[PRESENT-POLICY]`` row a run emitted, as key/value pairs.

    Distinct from present_mode_rows and easy to confuse with it, which is
    exactly what went wrong once: PRESENT-MODE is the SWAPCHAIN's decision
    (``requested=fifo effective=fifo``) and PRESENT-POLICY is the PACER's
    (``requested=uncapped effective=display``). A browser-coercion assertion
    that reads the wrong one finds no matching row and can never pass.
    """
    return text_rows(output, "PRESENT-POLICY")


def tear_free_presentation(output: str, label: str) -> list[str]:
    """A presentation rate must never be a reason to tear.

    Nothing in these harnesses sets ``Video.AllowTearing``, so no policy arm may
    reach a torn present mode: WebGPU must land on fifo or mailbox, and the GL
    diagnostic backend must report the automation swap interval it is entitled
    to rather than a tearing opt-in it was never given.
    """
    problems: list[str] = []
    rows = present_mode_rows(output)
    if not rows:
        return [f"{label}: no [PRESENT-MODE] row was emitted"]
    for row in rows:
        policy = row.get("policy", row.get("effectivePolicy", "?"))
        if row.get("tearing", "0") != "0":
            problems.append(
                f"{label}: {row.get('backend')} reported tearing="
                f"{row.get('tearing')} for policy {policy} without an opt-in")
        if row.get("backend") == "webgpu":
            effective = row.get("effective")
            if effective not in ("fifo", "mailbox"):
                problems.append(
                    f"{label}: WebGPU presented policy {policy} with "
                    f"effective={effective}, expected fifo or mailbox")
        elif row.get("backend") == "gl":
            # --headless-frames is a throughput measurement, not an image a
            # player looks at, so automation keeps the uncapped swap. Any other
            # interval here would mean the policy chose it.
            if row.get("effectiveSwap") != "0":
                problems.append(
                    f"{label}: GL policy {policy} swapped at interval "
                    f"{row.get('effectiveSwap')}, expected the automation 0")
    return problems


def test_save_dir() -> str:
    """The save directory the engine and the check must agree on.

    ``tools/run_checks.py`` points this at a run-scoped temporary directory and
    exports the same path as ``MDKR_SAVE_DIR``, so no suite run writes the
    repository's playable ``save/``. A standalone run keeps that historical
    default, which is why every caller also wraps its run in
    :func:`preserved_eeprom`.
    """

    return os.environ.get("MDKR_TEST_SAVE_DIR", "save")


def save_env(env: dict[str, str], save_dir: str) -> dict[str, str]:
    """Point the engine at ``save_dir``; the caller reads results from there."""

    env["MDKR_SAVE_DIR"] = os.path.abspath(save_dir)
    return env


@contextlib.contextmanager
def exclusive_build_dir(build_dir: str | os.PathLike[str]):
    """Hold an exclusive lock on a shared instrumented build directory.

    These checks configure and rebuild a fixed directory. The suite runs one
    task at a time, but two suites (or a suite and a standalone run) would
    otherwise interleave a reconfigure with a compile and produce a failure
    that has nothing to do with the code under test.
    """

    path = Path(build_dir).resolve()
    # The lock lives outside the repository: .gitignore's build*/ pattern only
    # covers directories, so a sibling lock file would show up as untracked and
    # trip the clean-tree guards.
    digest = hashlib.sha256(str(path).encode("utf-8")).hexdigest()[:16]
    lock_path = Path(tempfile.gettempdir()) / f"mdkr64-build-{digest}.lock"
    handle = open(lock_path, "w")
    try:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            if exc.errno not in (errno.EACCES, errno.EAGAIN):
                raise
            raise RuntimeError(
                f"{path} is locked by another run ({lock_path}); the suite runs "
                "one task at a time — wait for it to finish rather than sharing "
                "the instrumented build directory"
            ) from exc
        yield str(path)
    finally:
        handle.close()


@contextlib.contextmanager
def preserved_eeprom(save_dir: str):
    """Restore whatever EEPROM state ``save_dir`` held before the check ran.

    These checks assert on a specific starting state, so they delete and
    rewrite EEPROM images. When the directory is a developer's real ``save/``
    that would destroy a playable file; the prior bytes go back on the way out
    however the check exits.
    """

    directory = Path(save_dir)
    directory.mkdir(parents=True, exist_ok=True)
    saved: dict[str, bytes | None] = {}
    for name in EEPROM_ARTIFACTS:
        candidate = directory / name
        saved[name] = candidate.read_bytes() if candidate.is_file() else None
    try:
        yield str(directory)
    finally:
        for name, payload in saved.items():
            candidate = directory / name
            if payload is None:
                if candidate.is_dir():
                    shutil.rmtree(candidate, ignore_errors=True)
                elif candidate.exists():
                    candidate.unlink()
            else:
                candidate.write_bytes(payload)
