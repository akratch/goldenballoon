#!/usr/bin/env python3
"""Synthetic PE fixtures; no executable code or ROM-derived bytes required."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_pe_rom_magic import MAGIC, all_matches_are_code_pointers


def fixture():
    data = bytearray(0x800)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, 3)
    struct.pack_into("<H", data, 0x94, 240)
    struct.pack_into("<H", data, 0x98, 0x20B)
    struct.pack_into("<Q", data, 0xB0, 0x140000000)
    struct.pack_into("<I", data, 0x104, 16)
    struct.pack_into("<II", data, 0x130, 0x130000, 12)
    for index, (name, rva, raw, flags) in enumerate((
        (b".text", 0x123700, 0x200, 0x60000020),
        (b".data", 0x125000, 0x400, 0xC0000040),
        (b".reloc", 0x130000, 0x600, 0x42000040),
    )):
        offset = 0x188 + 40 * index
        data[offset:offset + len(name)] = name
        struct.pack_into("<IIII", data, offset + 8, 0x100, rva, 0x100, raw)
        struct.pack_into("<I", data, offset + 36, flags)
    struct.pack_into("<Q", data, 0x400, 0x140123780)
    struct.pack_into("<IIHH", data, 0x600, 0x125000, 12, 0xA000, 0)
    return data


class PeRomMagicTests(unittest.TestCase):
    def test_relocated_code_pointer(self):
        self.assertTrue(all_matches_are_code_pointers(fixture()))

    def test_each_broken_proof_is_refused(self):
        mutations = (
            ("wrong machine", "<H", 0x84, 0x14C),
            ("PE32", "<H", 0x98, 0x10B),
            ("short optional header", "<H", 0x94, 152),
            ("no relocation directory", "<I", 0x104, 5),
            ("unmapped relocation directory", "<I", 0x130, 0xFFFFFFF0),
            ("oversized relocation directory", "<I", 0x134, 0xFFFFFFFF),
            ("undersized block", "<I", 0x604, 6),
            ("odd block", "<I", 0x604, 11),
            ("overlong block", "<I", 0x604, 14),
            ("wrong relocation type", "<H", 0x608, 0x3000),
            ("relocation at different site", "<H", 0x608, 0xA008),
            ("unmapped relocation", "<I", 0x600, 0x125FF0),
            ("truncated section", "<I", 0x188 + 20, 0x800),
            ("section overlaps headers", "<I", 0x188 + 20, 0x100),
            ("section overlaps file bytes", "<I", 0x1D8 + 20, 0x400),
            ("read-only pointer storage", "<I", 0x1B0 + 36, 0x40000040),
            ("executable pointer storage", "<I", 0x1B0 + 36, 0xE0000040),
            ("non-executable target", "<I", 0x188 + 36, 0x40000020),
            ("writable target", "<I", 0x188 + 36, 0xE0000020),
            ("target outside image", "<I", 0x404, 2),
            ("overlapping sections", "<I", 0x1D8 + 12, 0x125000),
        )
        for label, fmt, offset, value in mutations:
            with self.subTest(label=label):
                data = fixture()
                struct.pack_into(fmt, data, offset, value)
                self.assertFalse(all_matches_are_code_pointers(data))

    def test_every_signature_must_be_proven(self):
        for offset in (0, 0x250, 0x408, 0x700, 0x800):
            with self.subTest(offset=offset):
                data = fixture()
                data[offset:offset + 4] = MAGIC
                self.assertFalse(all_matches_are_code_pointers(data))

    def test_plain_and_truncated_inputs(self):
        for data in (b"", MAGIC, b"arbitrary" + MAGIC, fixture()[:200],
                     fixture()[:0x610], fixture().replace(MAGIC, b"abcd")):
            self.assertFalse(all_matches_are_code_pointers(data))

    @unittest.skipIf(sys.platform == "win32", "public shell guard runs in MSYS2")
    def test_public_guard_keeps_its_rom_refusal(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "fixture.exe"
            path.write_bytes(fixture())
            command = ["bash", str(ROOT / "tools/check_no_rom.sh"), str(path)]
            good = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
            self.assertIn("verified PE64 code-pointer", good.stdout)
            path.write_bytes(fixture() + MAGIC)
            bad = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("N64 ROM magic sequence embedded", bad.stderr)


if __name__ == "__main__":
    unittest.main()
