#!/usr/bin/env python3
"""Create the browser payload for a release with cloud features absent.

The tracked web tree contains deferred Phone Party and Online Room code so it
can be tested before deployment. A local-only release must not publish that
code's routes, controls, dialogs, scripts, service-worker cache entries, or
online-only wasm. This copies a fully built and tested tree, removes only the
explicitly marked cloud regions, and fails closed if the source shape drifts.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path


HTML_REGIONS = {
    "styles", "launcher-actions", "dialogs", "stage-control", "scripts"
}
JS_REGIONS = {"precache"}
CLOUD_DIRECTORIES = ("controller", "online", "party", "room")
CLOUD_FILES = (
    "mdkr-online-tools.js",
    "mdkr-online-tools.js.symbols",
    "mdkr-online-tools.wasm",
)
REQUIRED_LOCAL_FILES = (
    "_headers",
    "build-info.json",
    "index.html",
    "manifest.webmanifest",
    "mdkr-save-tools.js",
    "mdkr-save-tools.wasm",
    "mdkr-save-ui.js",
    "mdkr64-shell.js",
    "mdkr64_web.js",
    "mdkr64_web.wasm",
    "rom-check.html",
    "rom-id.js",
    "sw.js",
)


def strip_regions(text: str, syntax: str, expected: set[str]) -> str:
    if syntax == "html":
        marker = re.compile(
            r"^\s*<!-- LOCAL_ONLY_CLOUD_(START|END): ([a-z-]+) -->\s*$")
    else:
        marker = re.compile(
            r"^\s*// LOCAL_ONLY_CLOUD_(START|END): ([a-z-]+)\s*$")

    output: list[str] = []
    active: str | None = None
    removed: set[str] = set()
    for line in text.splitlines(keepends=True):
        match = marker.match(line.rstrip("\r\n"))
        if match:
            edge, name = match.groups()
            if edge == "START":
                if active is not None or name not in expected or name in removed:
                    raise ValueError(f"invalid or duplicate cloud-region start: {name}")
                active = name
            else:
                if active != name:
                    raise ValueError(f"unmatched cloud-region end: {name}")
                removed.add(name)
                active = None
            continue
        if active is None:
            output.append(line)

    if active is not None:
        raise ValueError(f"unterminated cloud region: {active}")
    if removed != expected:
        missing = ", ".join(sorted(expected - removed)) or "none"
        extra = ", ".join(sorted(removed - expected)) or "none"
        raise ValueError(f"cloud-region mismatch (missing: {missing}; extra: {extra})")
    return "".join(output)


def local_headers(text: str) -> str:
    blocks = [block for block in re.split(r"\n\s*\n", text.strip()) if block]
    routes = {block.splitlines()[0] for block in blocks}
    expected = {"/*", "/controller/*", "/api/controller/*"}
    if routes != expected:
        raise ValueError(f"unexpected _headers routes: {sorted(routes)}")
    root = next(block for block in blocks if block.splitlines()[0] == "/*")
    return root + "\n"


def prepare(source: Path, output: Path, allow_dirty: bool = False) -> None:
    source = source.resolve()
    output = output.resolve()
    if not source.is_dir():
        raise ValueError(f"source directory does not exist: {source}")
    if output.exists():
        raise ValueError(f"output already exists: {output}")
    if source == output or source in output.parents:
        raise ValueError("output must not be the source or live inside it")

    for relative in REQUIRED_LOCAL_FILES:
        if not (source / relative).is_file():
            raise ValueError(f"built web source is missing {relative}")
    for relative in CLOUD_DIRECTORIES:
        if not (source / relative).is_dir():
            raise ValueError(f"full web source is missing cloud directory {relative}")

    shutil.copytree(source, output)
    try:
        index = output / "index.html"
        index.write_text(
            strip_regions(index.read_text(encoding="utf-8"), "html", HTML_REGIONS),
            encoding="utf-8",
        )
        worker = output / "sw.js"
        worker.write_text(
            strip_regions(worker.read_text(encoding="utf-8"), "js", JS_REGIONS),
            encoding="utf-8",
        )
        headers = output / "_headers"
        headers.write_text(local_headers(headers.read_text(encoding="utf-8")),
                           encoding="utf-8")

        for relative in CLOUD_DIRECTORIES:
            shutil.rmtree(output / relative)
        for relative in CLOUD_FILES:
            path = output / relative
            if path.exists():
                path.unlink()

        build_info_path = output / "build-info.json"
        build_info = json.loads(build_info_path.read_text(encoding="utf-8"))
        if build_info.get("source_dirty") is not False and not allow_dirty:
            raise ValueError("local-only release requires a clean source build")
        build_info["release_scope"] = "local-only"
        build_info["cloud_features"] = {
            "online_room": False,
            "phone_party": False,
        }
        build_info_path.write_text(
            json.dumps(build_info, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

        index_text = index.read_text(encoding="utf-8")
        worker_text = worker.read_text(encoding="utf-8")
        forbidden = (
            "add-phone-controllers", "online-room", "party-dialog",
            "party-stage-button", "Phone Party", "Private online",
        )
        for token in forbidden:
            if token in index_text or token in worker_text:
                raise ValueError(f"local-only public shell still contains {token!r}")
        if any((output / relative).exists()
               for relative in (*CLOUD_DIRECTORIES, *CLOUD_FILES)):
            raise ValueError("cloud-only artifact survived local-only preparation")
    except Exception:
        shutil.rmtree(output, ignore_errors=True)
        raise

    print(f"prepared local-only browser release: {output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default="dist/web")
    parser.add_argument("--out", required=True)
    parser.add_argument("--allow-dirty", action="store_true")
    args = parser.parse_args()
    try:
        prepare(Path(args.source), Path(args.out), args.allow_dirty)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"prepare_local_only_release: FAIL -- {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
