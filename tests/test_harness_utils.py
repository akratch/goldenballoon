#!/usr/bin/env python3
"""Controls for the shared check helpers themselves.

Every one of these helpers replaced a dozen hand-written copies whose
validation had drifted apart. The point of this file is that the surviving
copy actually rejects what the loose copies waved through, so consolidation
cannot quietly become "everyone now uses the weakest reader".
"""

import os
import tempfile
from pathlib import Path

from harness_utils import (ABORT_MARKERS, ASSERT_MARKERS, DEFAULT_BUILD_DIR,
                           FATAL_MARKERS, SLOT_BYTES, config_block,
                           config_checksum, find_fatal, pack_bits, parse_last,
                           parse_rows, put_bits, read_ppm, row_fields,
                           resolve_binary, seal_slot, slot_checksum,
                           slot_checksum_valid)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def rejects(call, message: str) -> None:
    try:
        call()
    except (ValueError, RuntimeError):
        return
    raise SystemExit(f"FAIL: {message}")


# --------------------------------------------------------------------------- #
#  Native process capability
# --------------------------------------------------------------------------- #

with tempfile.TemporaryDirectory() as resolver_scratch:
    product = Path(resolver_scratch) / (
        "mdkr64.exe" if os.name == "nt" else "mdkr64"
    )
    # Resolution is a path contract, nothing more: there is no environment
    # capability to satisfy, because desktop safety lives in the window layer.
    require(resolve_binary(product) == str(product),
            "native product path did not resolve")
    require(resolve_binary(Path(resolver_scratch)) == str(product),
            "build directory did not resolve to the product executable")
    helper = Path(resolver_scratch) / "mdkr-helper"
    require(resolve_binary(helper) == str(helper),
            "non-product command-line helper did not resolve")


# --------------------------------------------------------------------------- #
#  read_ppm
# --------------------------------------------------------------------------- #

with tempfile.TemporaryDirectory() as scratch:
    directory = Path(scratch)

    def ppm(name: str, payload: bytes) -> Path:
        path = directory / name
        path.write_bytes(payload)
        return path

    raster = bytes(range(24))  # 2x4 RGB
    good = ppm("good.ppm", b"P6\n2 4\n255\n" + raster)
    width, height, pixels = read_ppm(good)
    require((width, height, pixels) == (2, 4, raster), "a valid P6 image was misread")

    require(read_ppm(ppm("comment.ppm", b"P6\n# dumped by --dump-frames\n2 4\n255\n"
                         + raster))[2] == raster,
            "a header comment was not skipped")
    require(read_ppm(ppm("crlf.ppm", b"P6\n2 4\n255\r\n" + raster))[2] == raster,
            "a CRLF raster delimiter ate a pixel")

    # A first raster byte that is itself whitespace must survive: the loose
    # copies skipped arbitrary whitespace here and lost it.
    space_first = b"\x20" + raster[1:]
    require(read_ppm(ppm("wsfirst.ppm", b"P6\n2 4\n255\n" + space_first))[2]
            == space_first,
            "a whitespace-valued first sample was consumed as the delimiter")

    rejects(lambda: read_ppm(ppm("p5.ppm", b"P5\n2 4\n255\n" + raster)),
            "a non-P6 magic was accepted")
    # The divergence that motivated the strict reader: several copies never
    # looked at maxval, so a 16-bit-sample image read as 8-bit garbage.
    rejects(lambda: read_ppm(ppm("maxval.ppm", b"P6\n2 4\n65535\n" + raster)),
            "a maxval other than 255 was accepted")
    rejects(lambda: read_ppm(ppm("zero.ppm", b"P6\n0 4\n255\n")),
            "a zero dimension was accepted")
    rejects(lambda: read_ppm(ppm("short.ppm", b"P6\n2 4\n255\n" + raster[:-1])),
            "a truncated raster was accepted")
    # Trailing bytes matter too: a concatenated or doubled dump is a capture
    # failure, and the copies that sliced to width*height*3 could not see it.
    rejects(lambda: read_ppm(ppm("long.ppm", b"P6\n2 4\n255\n" + raster + b"\x00")),
            "an over-long raster was accepted")
    rejects(lambda: read_ppm(ppm("nodelim.ppm", b"P6\n2 4\n255")),
            "a header with no raster delimiter was accepted")
    rejects(lambda: read_ppm(ppm("truncated.ppm", b"P6\n2 4")),
            "a truncated header was accepted")
    rejects(lambda: read_ppm(ppm("nonnum.ppm", b"P6\nwide 4\n255\n" + raster)),
            "a non-numeric dimension was accepted")

    # The copies this replaced were split between ValueError and RuntimeError,
    # and their callers' except clauses followed suit. Both must still catch,
    # or adopting the shared reader turns a reported FAIL into a traceback.
    for caught in (ValueError, RuntimeError):
        try:
            read_ppm(directory / "p5.ppm")
        except caught:
            pass
        else:
            raise SystemExit(f"FAIL: a malformed PPM did not raise {caught.__name__}")


# --------------------------------------------------------------------------- #
#  EEPROM slot encoding
# --------------------------------------------------------------------------- #

require(SLOT_BYTES == 40, "SaveFile slot width drifted from save_layout.h")

bits: list[int] = []
put_bits(bits, 16, 0)
put_bits(bits, 8, 0xA5)
require(bits[:16] == [0] * 16 and bits[16:] == [1, 0, 1, 0, 0, 1, 0, 1],
        "put_bits did not emit an MSB-first stream")
require(bytes(pack_bits(bits)) == b"\x00\x00\xa5", "pack_bits mis-folded the stream")
rejects(lambda: pack_bits([1, 0, 1]), "a partial byte was packed")

slot = bytearray(SLOT_BYTES)
slot[5] = 0x10
slot[9] = 0x21
require(not slot_checksum_valid(slot), "an unsealed slot passed validation")
seal_slot(slot)
require(slot_checksum_valid(slot), "a sealed slot failed validation")
require(int.from_bytes(slot[:2], "big") == (5 + 0x10 + 0x21),
        "the slot checksum is not 5 plus the bytes after the header")
require(len(slot) == SLOT_BYTES, "seal_slot changed the slot length")
slot[20] ^= 0xFF
require(not slot_checksum_valid(slot), "a mutated slot still validated")

# The seed is load-bearing: a zero-seeded writer produces a slot the engine
# rejects, and that is exactly the bug the shared constant prevents.
require(slot_checksum(bytearray(SLOT_BYTES)) == 5,
        "an empty slot's checksum lost the seed")
# CourseRecords blocks run through the same routine at a different width.
records = seal_slot(bytearray(192))
require(slot_checksum_valid(records), "a 192-byte records block failed validation")

require(config_checksum(0) == 5, "the SaveConfig checksum lost the seed")
require(config_checksum(0xF) == 5 + 0xF, "the SaveConfig checksum missed nibble 0")
# Nibble 14 and up sit outside the summed range.
require(config_checksum(1 << 56) == 5, "the SaveConfig checksum summed past nibble 13")

word = config_block(1 << 25)
require(len(word) == 8, "a SaveConfig block is not 8 bytes")
value = int.from_bytes(word, "big")
require((value & 0x00FFFFFFFFFFFFFF) == 1 << 25, "config_block lost its payload")
require((value >> 56) == config_checksum(1 << 25) & 0xFF,
        "config_block stored the wrong checksum byte")
rejects(lambda: config_block(1 << 60), "a payload overlapping the checksum was accepted")


# --------------------------------------------------------------------------- #
#  Fatal markers
# --------------------------------------------------------------------------- #

require(find_fatal("frame 12 ok\nframe 13 ok") is None,
        "a clean log reported a fatal marker")
for marker in ("[CRASH] pc=0x8012", "[FATAL] out of memory",
               "AddressSanitizer: heap-use-after-free",
               "UndefinedBehaviorSanitizer: undefined-behavior",
               "collision.c:12:5: runtime error: index 9 out of bounds"):
    require(find_fatal(f"ok\n{marker}\nok") is not None,
            f"the common marker set missed {marker!r}")

require(find_fatal("ok\nAssertion failed\n") is None,
        "an assertion was treated as universally fatal")
require(find_fatal("ok\nAssertion failed\n", *ASSERT_MARKERS) is not None,
        "an opted-in assertion marker did not fire")
require(find_fatal("ok\nAbort trap: 6\n", *ABORT_MARKERS) is not None,
        "an opted-in abort marker did not fire")
require(find_fatal("ok\nDEVICE LOST\n", "device lost", ignore_case=True) is not None,
        "ignore_case did not reach the caller's extra markers")
require(find_fatal("ok\nDEVICE LOST\n", "device lost") is None,
        "a case-sensitive scan matched a differently-cased marker")
require(find_fatal("[CRASH] pc=0x8012") == "[CRASH]",
        "find_fatal did not report which marker fired")
require(len(set(FATAL_MARKERS)) == len(FATAL_MARKERS),
        "the common marker set repeats itself")


# --------------------------------------------------------------------------- #
#  "[TAG] key=value" rows
# --------------------------------------------------------------------------- #

require(row_fields("ticks=10 lead=1") == {"ticks": 10, "lead": 1},
        "row_fields dropped an integer token")
require(row_fields("backend=gl ticks=10 bare") == {"ticks": 10},
        "row_fields did not skip non-integer and separator-less tokens")
require(row_fields("delta=-3") == {"delta": -3}, "row_fields rejected a negative")

log = ("noise\n"
       "[PRESENTSCHED-SUMMARY] ticks=10 issued=9\n"
       "more noise\n"
       "[PRESENTSCHED-SUMMARY] ticks=20 issued=19\n"
       "[GL-BACKPRESSURE] cap=2 waits=3\n")
require(parse_rows(log, "PRESENTSCHED-SUMMARY")
        == [{"ticks": 10, "issued": 9}, {"ticks": 20, "issued": 19}],
        "parse_rows did not return every row in order")
require(parse_last(log, "PRESENTSCHED-SUMMARY") == {"ticks": 20, "issued": 19},
        "parse_last did not return the final row")
require(parse_rows(log, "MISSING-TAG") == [], "an absent tag invented rows")
rejects(lambda: parse_last(log, "MISSING-TAG"), "parse_last accepted an absent tag")
rejects(lambda: parse_last(log, "PRESENTSCHED-SUMMARY", expect_one=True),
        "expect_one accepted a repeated row")
require(parse_last(log, "GL-BACKPRESSURE", expect_one=True) == {"cap": 2, "waits": 3},
        "expect_one rejected a single row")
require(parse_last(log, "WGPU-BACKPRESSURE", "GL-BACKPRESSURE")
        == {"cap": 2, "waits": 3},
        "an either-backend tag set did not match the spelling that was emitted")
# A tag is matched literally, not as a regex, so a bracketed tag cannot be
# turned into a character class by its own punctuation.
require(parse_rows("[A.B] x=1\n[AXB] x=2\n", "A.B") == [{"x": 1}],
        "a tag's punctuation was treated as a regex")
rejects(lambda: parse_last(log), "parse_last accepted no tag at all")

try:
    parse_last(log, "MISSING-TAG", label="webgpu uncapped")
except RuntimeError as error:
    require("webgpu uncapped" in str(error) and "MISSING-TAG" in str(error),
            "the failure message named neither the arm nor the tag")

require(DEFAULT_BUILD_DIR == "build", "the shared --build default drifted")

print("PASS: harness helpers")
