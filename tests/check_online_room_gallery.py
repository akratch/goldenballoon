#!/usr/bin/env python3
"""Render every authoritative Online Room gallery case in the native shell.

The scenario list comes from the executable, not a duplicated Python table.
Each case starts a fresh process/profile, renders through the ordinary launcher
panel, emits one semantic witness and writes one new BMP. This catches stale
captures, gallery/view drift, blank rendering and cases that accidentally need
a provider or a prior process's state.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import resolve_binary


ROW_RE = re.compile(
    r"^online-gallery-v1 slug=(?P<slug>[a-z0-9-]+) "
    r"kind=(?P<kind>\d+) failure=(?P<failure>\d+) "
    r"primary=(?P<primary>\d+) timeout=(?P<timeout>[01]) "
    r"admission=(?P<admission>[01]) "
    r"timeout-visible=(?P<timeout_visible>[01])$"
)
TRACE_RE = re.compile(
    r"\[online-gallery\] rendered slug=(?P<slug>[a-z0-9-]+) "
    r"kind=(?P<kind>\d+) failure=(?P<failure>\d+) "
    r"primary=(?P<primary>\d+) secondary=\d+ cancel=\d+ "
    r"timeout=(?P<timeout>[01]) timeout-visible=(?P<timeout_visible>[01]) "
    r"admission=(?P<admission>[01])"
)


@dataclass(frozen=True)
class Case:
    slug: str
    kind: int
    failure: int
    primary: int
    timeout: int
    admission: int
    timeout_visible: int


class GalleryError(RuntimeError):
    pass


def run(command: list[str], environment: dict[str, str], timeout: int) -> str:
    completed = subprocess.run(
        command,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise GalleryError(
            f"process exited {completed.returncode}:\n{completed.stdout[-5000:]}"
        )
    return completed.stdout


def discover(binary: str, root: Path, timeout: int) -> list[Case]:
    prefs = root / "inventory-prefs"
    saves = root / "inventory-saves"
    prefs.mkdir()
    saves.mkdir()
    environment = os.environ.copy()
    environment.update({
        "MDKR_APP_DUMP_ONLINE_GALLERY": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(root / "inventory-video.ini"),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_AUDIO": "0",
    })
    output = run([binary], environment, timeout)
    header = re.search(r"^online-gallery-v1 count=(\d+)$", output, re.MULTILINE)
    if header is None:
        raise GalleryError("executable did not publish gallery-v1 inventory")
    cases = [Case(
        slug=match.group("slug"),
        kind=int(match.group("kind")),
        failure=int(match.group("failure")),
        primary=int(match.group("primary")),
        timeout=int(match.group("timeout")),
        admission=int(match.group("admission")),
        timeout_visible=int(match.group("timeout_visible")),
    ) for match in map(ROW_RE.match, output.splitlines()) if match]
    expected = int(header.group(1))
    if len(cases) != expected or expected < 30:
        raise GalleryError(
            f"inventory count mismatch: header={expected}, rows={len(cases)}"
        )
    slugs = {case.slug for case in cases}
    if len(slugs) != len(cases):
        raise GalleryError("gallery inventory contains duplicate slugs")
    if {case.kind for case in cases} != set(range(1, 11)):
        raise GalleryError("gallery does not cover every view kind 1..10")
    failures = {case.failure for case in cases if case.failure}
    if failures != set(range(1, 18)):
        raise GalleryError("gallery does not cover every typed failure 1..17")
    return cases


def inspect_bmp(path: Path) -> tuple[int, int, str]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise GalleryError(f"{path.name}: missing/invalid BMP capture")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    height = abs(signed_height)
    stride = (width * 3 + 3) & ~3
    if (width < 640 or height < 480 or bits != 24 or compression != 0 or
            offset + stride * height != len(data)):
        raise GalleryError(
            f"{path.name}: unsupported/incomplete frame {width}x{height}"
        )
    # Sample directly from BGR rows. The dedicated launcher capture gates own
    # detailed palette/placement checks; this catalog gate only needs to prove
    # every semantic case produced a non-flat, readable frame of its own.
    pixel_count = width * height
    step = max(1, pixel_count // 20_000)
    colors: set[bytes] = set()
    light = 0
    for linear in range(0, pixel_count, step):
        y, x = divmod(linear, width)
        start = offset + y * stride + x * 3
        blue, green, red = data[start:start + 3]
        colors.add(data[start:start + 3])
        if (77 * red + 150 * green + 29 * blue) // 256 >= 150:
            light += 1
    if len(colors) < 48 or light < 30:
        raise GalleryError(
            f"{path.name}: visually flat/unreadable "
            f"(sampled colors={len(colors)}, light={light})"
        )
    return width, height, hashlib.sha256(data).hexdigest()


def render_case(binary: str, root: Path, case: Case, timeout: int) -> str:
    case_root = root / case.slug
    prefs = case_root / "prefs"
    saves = case_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    (prefs / "mdkr64_app.ini").write_text(
        "# deterministic Online Room gallery\n", encoding="utf-8"
    )
    capture = case_root / f"{case.slug}.bmp"
    environment = os.environ.copy()
    environment.update({
        "MDKR_APP_SMOKE_FRAMES": "4",
        "MDKR_ONLINE_ROOM_PREVIEW": "1",
        "MDKR_APP_SMOKE_SHOT": str(capture),
        "MDKR_APP_SMOKE_WINDOW_SIZE": "960x720",
        "MDKR_APP_PANEL": "Online Room",
        "MDKR_APP_ONLINE_FAKE": "1",
        "MDKR_APP_ONLINE_GALLERY": case.slug,
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(case_root / "video.ini"),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
    })
    if case.admission:
        environment["MDKR_APP_ONLINE_FAKE_ALLOW_START"] = "1"
    else:
        environment.pop("MDKR_APP_ONLINE_FAKE_ALLOW_START", None)
    output = run([binary], environment, timeout)
    traces = [match for match in map(TRACE_RE.search, output.splitlines()) if match]
    if len(traces) != 1:
        raise GalleryError(
            f"{case.slug}: expected one semantic render witness, got {len(traces)}\n"
            f"{output[-3000:]}"
        )
    trace = traces[0]
    actual = (
        trace.group("slug"), int(trace.group("kind")),
        int(trace.group("failure")), int(trace.group("primary")),
        int(trace.group("timeout")), int(trace.group("admission")),
        int(trace.group("timeout_visible")),
    )
    expected = (
        case.slug, case.kind, case.failure, case.primary,
        case.timeout, case.admission,
        case.timeout_visible,
    )
    if actual != expected:
        raise GalleryError(
            f"{case.slug}: rendered semantic witness {actual} != {expected}"
        )
    _, _, digest = inspect_bmp(capture)
    return digest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-online-gallery-") as temporary:
            root = Path(temporary)
            cases = discover(binary, root, args.timeout)
            digests: dict[str, str] = {}
            for case in cases:
                digest = render_case(binary, root, case, args.timeout)
                if digest in digests:
                    raise GalleryError(
                        f"{case.slug}: capture is byte-identical to "
                        f"{digests[digest]}"
                    )
                digests[digest] = case.slug
    except (GalleryError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL online room gallery: {error}")
        return 1
    print(
        "PASS online room gallery: "
        f"cases={len(cases)} views=10 failures=17 unique-captures={len(digests)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
