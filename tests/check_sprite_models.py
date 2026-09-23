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


def png(rgb):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes((0, *rgb, 255)))) + chunk(b"IEND", b""))


def pack(root, priority, rgb, canopy_only=False, partial=False):
    root.mkdir(parents=True)
    (root / "pack.ini").write_text(f"[pack]\nname = {root.name}\npriority = {priority}\n")
    models = root / "models/palm"
    models.mkdir(parents=True)
    triangle = b"DKRPM001" + struct.pack("<II", 3, 3)
    for pos in ((0, 0, 0), (0.5, 0, 0), (0, 1, 0)):
        triangle += struct.pack("<8f", *pos, 0, 0, 1, 0, 0)
    triangle += struct.pack("<III", 0, 1, 2)
    for name in ("canopy-near", "canopy-far") if canopy_only else ("near", "far", "canopy-near", "canopy-far"):
        if partial and name == "canopy-far":
            continue
        (models / f"{name}.dkrmesh").write_bytes(triangle)
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
        for case in ("shared", "zipped", "split", "partial", "bad-atlas", "bad-mesh"):
            subprocess.run([probe, str(root / case), case], check=True, env=env, timeout=60)


if __name__ == "__main__":
    main()
