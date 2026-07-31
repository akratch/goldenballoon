#!/usr/bin/env python3
"""Guard tracked images against embedded provenance/generator metadata.

Scans every TRACKED image
(png/jpg/jpeg/ico/icns) for C2PA, EXIF, XMP and AI-generator markers, so a
generation tool's provenance metadata (which can name the tool, a prompt, an
account, or otherwise be undesirable to ship) cannot slip into a committed
asset unnoticed. `brand/appicon-source.png` is project-supplied artwork of
AI-assisted origin with that metadata deliberately stripped before commit
(see THIRD_PARTY.md) -- this guard is what keeps that claim true over time,
for that file and every other tracked image.

Method, in order of rigor:
  1. PNG: a real chunk walk. Any chunk outside the small allowlist of
     standard image-data chunk types (IHDR/PLTE/IDAT/IEND/... plus the
     animated-PNG chunks) is reported by name, and every chunk's raw bytes
     (not just the flagged ones) are scanned for marker keywords -- a
     metadata payload smuggled inside a chunk with a boring or malformed name
     is still caught by the keyword scan.
  2. JPEG: a real segment walk over the marker stream (APPn / COM segments,
     where EXIF/XMP/ICC/JUMBF-C2PA payloads live), plus a whole-file keyword
     scan as a backstop for anything a strict segment parse might miss.
  3. ICO/ICNS (opaque multi-image containers): whole-file keyword scan.

Marker keywords are matched case-insensitively. The rule is that NO tracked
image may carry provenance or generator metadata of any kind, so the list is
deliberately broader than the container formats alone require: the standard
metadata carriers (c2pa, jumb -- the JUMBF box 4CC that holds C2PA manifests
-- xmp, exif) plus generator names that appear in real-world output
(openai, midjourney, dall-e, stable diffusion, firefly, "ai generated").
Matching a keyword is a failure regardless of which chunk or segment it
came from.

Usage: tools/check_image_metadata.py [--repo-root .]
Exit 0 = every tracked image is clean. Exit 1 = at least one marker found.
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path


IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".ico", ".icns")

# Case-insensitive marker keywords. Order doesn't matter; kept as a tuple so
# failure messages can report which specific keyword hit.
MARKER_KEYWORDS = (
    "c2pa",
    "jumb",
    "openai",
    "xmp",
    "exif",
    "midjourney",
    "dall-e",
    "dalle",
    "stable diffusion",
    "firefly",
    "ai generated",
    "ai-generated",
)
MARKER_RE = re.compile("|".join(re.escape(k) for k in MARKER_KEYWORDS), re.IGNORECASE)

# Chunk types a "boring" PNG (no metadata, no color profile assertions beyond
# the basics) is expected to contain. Ancillary chunks outside this set are
# reported by name even if the keyword scan below finds nothing textual in
# them -- e.g. eXIf is pure binary EXIF TIFF and would otherwise slip past a
# naive text-only keyword scan.
PNG_ALLOWED_CHUNKS = {
    "IHDR", "PLTE", "IDAT", "IEND",   # required / core
    "tRNS", "gAMA", "cHRM", "sRGB", "iCCP",  # color
    "bKGD", "hIST", "sBIT",           # rendering hints
    "pHYs", "sPLT",                   # physical/palette hints
    "tIME",                           # last-modification timestamp (no PII)
    "acTL", "fcTL", "fdAT",           # APNG animation
}

# PNG chunk types that are ALWAYS metadata/provenance-shaped, regardless of
# keyword content, and are therefore reported even with empty payloads.
PNG_METADATA_CHUNKS = {"tEXt", "zTXt", "iTXt", "eXIf"}


def find_tracked_images(root: Path) -> list[Path]:
    """List every tracked image under root, by extension, git-ls-files-first."""
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode == 0:
        paths = [root / line for line in result.stdout.splitlines() if line.strip()]
    else:
        # Not a git repo (or git unavailable): fall back to a filesystem walk.
        paths = [p for p in root.rglob("*") if p.is_file()]
    return sorted(p for p in paths if p.suffix.lower() in IMAGE_EXTENSIONS and p.is_file())


def scan_keywords(label: str, data: bytes) -> list[str]:
    hits = []
    for match in MARKER_RE.finditer(data.decode("latin-1")):
        hits.append(f"{label}: matched marker keyword {match.group(0)!r}")
    return hits


def walk_png_chunks(path: Path) -> list[str]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return [f"{path}: does not start with the PNG signature"]

    problems: list[str] = []
    offset = 8
    length_total = len(data)
    while offset + 8 <= length_total:
        (chunk_len,) = struct.unpack(">I", data[offset : offset + 4])
        chunk_type = data[offset + 4 : offset + 8].decode("ascii", errors="replace")
        chunk_data_start = offset + 8
        chunk_data_end = chunk_data_start + chunk_len
        if chunk_data_end + 4 > length_total:
            problems.append(f"{path}: truncated chunk {chunk_type!r} (malformed PNG)")
            break
        chunk_data = data[chunk_data_start:chunk_data_end]

        if chunk_type in PNG_METADATA_CHUNKS:
            problems.append(f"{path}: metadata chunk {chunk_type!r} present ({chunk_len} bytes)")
        elif chunk_type not in PNG_ALLOWED_CHUNKS:
            problems.append(f"{path}: unrecognised ancillary chunk {chunk_type!r} present ({chunk_len} bytes)")

        problems.extend(scan_keywords(f"{path} chunk {chunk_type!r}", chunk_data))

        offset = chunk_data_end + 4  # skip CRC
        if chunk_type == "IEND":
            break
    return problems


# JPEG markers that start a segment with a 2-byte length (everything from
# 0xFFD0 except SOI/EOI carries one; SOS ends the marker stream).
_JPEG_NO_LENGTH = {0xD8, 0xD9, 0x01} | set(range(0xD0, 0xD8))


def walk_jpeg_segments(path: Path) -> list[str]:
    data = path.read_bytes()
    if data[:2] != b"\xff\xd8":
        return [f"{path}: does not start with the JPEG SOI marker"]

    problems: list[str] = []
    offset = 2
    length_total = len(data)
    while offset + 1 < length_total:
        if data[offset] != 0xFF:
            # Not aligned on a marker (e.g. inside entropy-coded scan data);
            # stop the structured walk here and rely on the whole-file scan.
            break
        marker = data[offset + 1]
        if marker == 0xD8:
            offset += 2
            continue
        if marker == 0xD9:  # EOI
            break
        if marker == 0xDA:  # SOS: entropy-coded data follows, stop segment walk
            break
        if marker in _JPEG_NO_LENGTH:
            offset += 2
            continue
        if offset + 4 > length_total:
            problems.append(f"{path}: truncated JPEG segment (malformed JPEG)")
            break
        (seg_len,) = struct.unpack(">H", data[offset + 2 : offset + 4])
        seg_data_start = offset + 4
        seg_data_end = offset + 2 + seg_len
        if seg_data_end > length_total:
            problems.append(f"{path}: truncated JPEG segment 0xFF{marker:02X} (malformed JPEG)")
            break
        seg_data = data[seg_data_start:seg_data_end]

        # APP1 (Exif/XMP), APP2 (ICC/MPF), APP11 (JPEG XT / C2PA JUMBF), and
        # COM (free-text comment) are exactly where provenance/generator
        # metadata lives in a JPEG.
        if marker in (0xE1, 0xE2, 0xEB, 0xFE):
            marker_name = {0xE1: "APP1", 0xE2: "APP2", 0xEB: "APP11", 0xFE: "COM"}[marker]
            problems.append(f"{path}: {marker_name} segment present ({seg_len} bytes)")

        problems.extend(scan_keywords(f"{path} segment 0xFF{marker:02X}", seg_data))
        offset = seg_data_end

    # Backstop: scan the whole file regardless of how far the segment walk
    # got, so a marker sitting somewhere the structured walk didn't reach is
    # still caught.
    problems.extend(scan_keywords(f"{path} (whole file)", data))
    return problems


def scan_opaque_container(path: Path) -> list[str]:
    """ICO/ICNS: no shared standard chunk model worth hand-rolling here;
    string-scan the whole file for marker keywords."""
    data = path.read_bytes()
    return scan_keywords(f"{path} (whole file)", data)


def scan_image(path: Path) -> list[str]:
    suffix = path.suffix.lower()
    if suffix == ".png":
        return walk_png_chunks(path)
    if suffix in (".jpg", ".jpeg"):
        return walk_jpeg_segments(path)
    return scan_opaque_container(path)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".", help="repository root")
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    images = find_tracked_images(root)

    if not images:
        print("FAIL: no tracked images found to scan (unexpected)", file=sys.stderr)
        return 1

    all_problems: list[str] = []
    for image in images:
        rel = image.relative_to(root)
        problems = scan_image(image)
        if problems:
            print(f"FAIL: {rel}", file=sys.stderr)
            for problem in problems:
                print(f"  - {problem}", file=sys.stderr)
            all_problems.extend(problems)
        else:
            print(f"ok: {rel}")

    print()
    if all_problems:
        print(
            f"check_image_metadata: FAIL -- {len(all_problems)} issue(s) across "
            f"{len(images)} tracked image(s)",
            file=sys.stderr,
        )
        return 1

    print(f"check_image_metadata: PASS -- {len(images)} tracked image(s) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
