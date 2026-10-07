#!/usr/bin/env python3
"""Recognize ROM-magic false positives that are proven PE64 code pointers.

Called only after the artifact guard finds the four-byte N64 signature. A
little-endian pointer to 0x140123780 starts with those same bytes. Accept only
aligned, DIR64-relocated pointers in writable non-executable data whose full
64-bit value points into executable, non-writable, file-backed code. Every
other occurrence, malformed image, or read error remains a refusal.
"""
from pathlib import Path
import struct
import sys

MAGIC = bytes.fromhex("80371240")


def verified_pointer_offsets(data: bytes) -> set[int]:
    def read(fmt, offset):
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError("not a PE image")
    pe, = read("<I", 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("invalid PE signature")
    machine, count = read("<HH", pe + 4)
    optional_size, = read("<H", pe + 20)
    optional = pe + 24
    kind, = read("<H", optional)
    if machine != 0x8664 or kind != 0x20B or optional_size < 160:
        raise ValueError("not an AMD64 PE32+ image with relocations")
    image_base, = read("<Q", optional + 24)
    directory_count, = read("<I", optional + 108)
    if directory_count < 6:
        raise ValueError("missing relocation directory")
    reloc_rva, reloc_size = read("<II", optional + 112 + 5 * 8)
    headers_end = optional + optional_size + count * 40
    sections = []
    for index in range(count):
        offset = optional + optional_size + index * 40
        virtual_size, rva, size, raw = read("<IIII", offset + 8)
        flags, = read("<I", offset + 36)
        if raw + size > len(data) or rva + max(size, virtual_size) > 0x100000000:
            raise ValueError("section outside image")
        if size and raw < headers_end:
            raise ValueError("section overlaps PE headers")
        for prior_rva, prior_size, prior_raw, _ in sections:
            if size and prior_size and (
                max(raw, prior_raw) < min(raw + size, prior_raw + prior_size) or
                max(rva, prior_rva) < min(rva + size, prior_rva + prior_size)
            ):
                raise ValueError("overlapping file-backed sections")
        sections.append((rva, size, raw, flags))

    def locate(rva, size):
        matches = [(raw + rva - start, flags)
                   for start, length, raw, flags in sections
                   if start <= rva and rva + size <= start + length]
        if len(matches) != 1:
            raise ValueError("ambiguous or non-file-backed RVA")
        return matches[0]

    relocation_offset, _ = locate(reloc_rva, reloc_size)
    end = relocation_offset + reloc_size
    cursor = relocation_offset
    pointers = set()
    while cursor < end:
        if end - cursor < 8:
            raise ValueError("truncated relocation block")
        page, size = read("<II", cursor)
        if page % 0x1000 or size < 8 or size % 2 or cursor + size > end:
            raise ValueError("invalid relocation block size")
        for entry_offset in range(cursor + 8, cursor + size, 2):
            entry, = read("<H", entry_offset)
            if entry >> 12 != 10:  # IMAGE_REL_BASED_DIR64 only
                continue
            rva = page + (entry & 0xFFF)
            offset, flags = locate(rva, 8)
            if rva % 8 or offset % 8 or not flags & 0x80000000 or flags & 0x20000000:
                continue
            pointer, = read("<Q", offset)
            if not image_base <= pointer < image_base + 0x100000000:
                continue
            try:
                _, target_flags = locate(pointer - image_base, 1)
            except ValueError:
                continue
            if target_flags & 0x20000000 and not target_flags & 0x80000000:
                pointers.add(offset)
        cursor += size
    return pointers


def all_matches_are_code_pointers(data: bytes) -> bool:
    matches = set()
    start = 0
    while (start := data.find(MAGIC, start)) >= 0:
        matches.add(start)
        start += 1
    if not matches:
        return False  # This exception must never clear an unexamined match.
    try:
        return matches <= verified_pointer_offsets(data)
    except (ValueError, struct.error, OverflowError):
        return False


def main():
    if len(sys.argv) != 2:
        return 2
    try:
        accepted = all_matches_are_code_pointers(Path(sys.argv[1]).read_bytes())
    except OSError:
        accepted = False
    if accepted:
        print("verified PE64 code-pointer signature collision: " + sys.argv[1])
    return 0 if accepted else 1


if __name__ == "__main__":
    sys.exit(main())
