#!/usr/bin/env python3
"""Generate synthetic directory/ZIP packs for atlas lifetime tests (no ROM/art)."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(rgb, alpha=255, size=1):
    rows = b"".join(b"\0" + bytes((*rgb, alpha)) * size for _ in range(size))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def bomb_png(inflated_mib=384):
    """A 64x64 RGBA header whose IDAT inflates to `inflated_mib` MiB of zeros."""
    deflate = zlib.compressobj(9)
    block = bytes(1 << 20)
    data = b"".join(deflate.compress(block) for _ in range(inflated_mib)) + deflate.flush()
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", data) + chunk(b"IEND", b""))


def triangle(slant=0.0):
    mesh = b"DKRPM001" + struct.pack("<II", 3, 3)
    for pos in ((0, 0, 0), (0.5, slant, 0), (0, 1, 0)):
        mesh += struct.pack("<8f", *pos, 0, 0, 1, 0, 0)
    return mesh + struct.pack("<III", 0, 1, 2)


# (family directory, mesh stems, atlas names) for every replacement slot in
# platform/sprite_models.c. Atlas names are the hex of the sprite texture key.
FAMILIES = (
    ("palm", ("", "canopy-"), ("50414c4d33440001",)),
    ("balloons", ("", "collectible-"),
     ("42414c4c33440093", "42414c4c33440094", "42414c4c33440095", "42414c4c33440096",
      "42414c4c33440097", "42414c4c3344009a", "42414c4c3344009b")),
    ("banana", ("",), ("42414e414e413301",)),
    ("beach-tree", ("",), ("4245414348330001",)),
    ("blueberry", ("",), ("424c554542330001",)),
    ("rubber-tree", ("",), ("5255424252330001",)),
)


def family_pack(root, priority, rgb, alpha=255, families=None):
    """Synthetic art for the named families (all by default); one triangle per
    mesh file, a distinct slant per file so no two files are byte-identical."""
    root.mkdir(parents=True)
    (root / "pack.ini").write_text(f"[pack]\nname = {root.name}\npriority = {priority}\n")
    slant = 0.0
    for family, stems, atlases in FAMILIES:
        if families is not None and family not in families:
            continue
        models = root / "models" / family
        models.mkdir(parents=True)
        for stem in stems:
            for lod in ("near", "far"):
                slant += 0.01
                (models / f"{stem}{lod}.dkrmesh").write_bytes(triangle(slant))
        for atlas in atlases:
            (models / f"{atlas}.png").write_bytes(png(rgb, alpha))


def pack(root, priority, rgb, canopy_only=False, partial=False):
    root.mkdir(parents=True)
    (root / "pack.ini").write_text(f"[pack]\nname = {root.name}\npriority = {priority}\n")
    models = root / "models/palm"
    models.mkdir(parents=True)
    for name in ("canopy-near", "canopy-far") if canopy_only else ("near", "far", "canopy-near", "canopy-far"):
        if partial and name == "canopy-far":
            continue
        (models / f"{name}.dkrmesh").write_bytes(triangle())
    (models / "50414c4d33440001.png").write_bytes(png(rgb))


def main():
    probe = str(Path(sys.argv[1]).resolve())
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MDKR", "GE007_"))}
    with tempfile.TemporaryDirectory(prefix="mdkr-sprite-models-") as tmp:
        root = Path(tmp)
        # The current probe is ROM-free and writes no saves. Keep its process
        # environment hermetic even if the probe grows engine-backed checks.
        (root / "save").mkdir()
        env["MDKR_SAVE_DIR"] = str(root / "save")
        pack(root / "shared/base", 100, (255, 0, 0))
        (root / "zipped").mkdir()
        with zipfile.ZipFile(root / "zipped/base.zip", "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted((root / "shared/base").rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(root / "shared/base"))
        for case in ("split", "partial"):
            pack(root / case / "base", 100, (255, 0, 0))
            pack(root / case / "override", 200, (0, 0, 255), True, case == "partial")
        for case in ("bad-atlas", "bad-mesh"):
            pack(root / case / "base", 100, (255, 0, 0))
            model_dir = root / case / "base/models/palm"
            if case == "bad-atlas":
                (model_dir / "50414c4d33440001.png").write_bytes(b"not a PNG")
            else:
                for path in model_dir.glob("*.dkrmesh"):
                    path.write_bytes(b"DKRPM001" + struct.pack("<II", 0xffffffff, 3))
        family_pack(root / "families/base", 100, (255, 0, 0))
        pack(root / "bomb/base", 100, (255, 0, 0))
        (root / "bomb/base/models/palm/50414c4d33440001.png").write_bytes(bomb_png())
        for case in ("shared", "zipped", "split", "partial", "bad-atlas", "bad-mesh", "families", "bomb"):
            subprocess.run([probe, str(root / case), case], check=True, env=env, timeout=120)


if __name__ == "__main__":
    main()
