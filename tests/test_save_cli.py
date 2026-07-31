#!/usr/bin/env python3
"""Black-box custody tests for the standalone mdkr-save utility."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(cli: Path, *arguments: object, env: dict[str, str] | None = None,
        success: bool = True) -> subprocess.CompletedProcess[str]:
    command = [str(cli), *(str(value) for value in arguments)]
    process = subprocess.run(
        command,
        capture_output=True,
        text=True,
        env={**os.environ, **(env or {})},
        check=False,
    )
    if success and process.returncode != 0:
        raise AssertionError(
            f"{command!r} failed ({process.returncode}):\n"
            f"{process.stdout}\n{process.stderr}"
        )
    if not success and process.returncode == 0:
        raise AssertionError(f"{command!r} unexpectedly succeeded")
    return process


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    arguments = parser.parse_args()
    cli = arguments.cli.resolve()

    with tempfile.TemporaryDirectory(prefix="mdkr-save-cli-") as directory:
        root = Path(directory)
        raw = root / "source.eep"
        portable = root / "backup.mdkr-save"
        restored = root / "restored.eep"
        destination = root / "live.eep"
        corrupt = root / "corrupt.eep"
        recovered = root / "recovered.eep"

        source = bytes(512)
        raw.write_bytes(source)
        run(cli, "export", raw, portable)
        document = json.loads(portable.read_text(encoding="utf-8"))
        assert document["format"] == "mdkr64-save"
        assert document["version"] == 1
        assert document["sha256"] == hashlib.sha256(source).hexdigest()

        inspected = run(cli, "inspect", portable)
        summary = json.loads(inspected.stdout)
        assert summary["sha256"] == hashlib.sha256(source).hexdigest()

        run(cli, "import", portable, restored)
        assert restored.read_bytes() == source

        old = bytes([0xFF]) * 512
        destination.write_bytes(old)
        failed = run(
            cli,
            "import",
            portable,
            destination,
            env={"MDKR_SAVE_FAULT": "after-install"},
            success=False,
        )
        assert "injected transaction failure" in failed.stderr
        assert destination.read_bytes() == old

        broken = bytearray(source)
        broken[40] = 1
        corrupt.write_bytes(broken)
        run(cli, "import", corrupt, destination, success=False)
        assert destination.read_bytes() == old
        run(cli, "recover", corrupt, recovered)
        repaired = recovered.read_bytes()
        assert len(repaired) == 512
        assert repaired[:40] == source[:40]
        assert repaired[40:80] == bytes([0xFF]) * 40
        assert repaired[80:] == source[80:]

        oversized = root / "oversized.mdkr-save"
        oversized.write_bytes(bytes(65537))
        run(cli, "inspect", oversized, success=False)

    print("save CLI: import/export/recovery/rollback tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
