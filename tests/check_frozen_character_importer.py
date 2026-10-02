#!/usr/bin/env python3
"""Exercise an offline authoring lifecycle through a Character Workshop importer."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

from test_character_asset_probe import make_animated_glb  # noqa: E402
import harness_utils  # noqa: F401,E402 -- a direct run gets private saves, prefs and mods


MAX_OUTPUT_BYTES = 64 * 1024
IMPORTER_COMMAND_TIMEOUT_SECONDS = 120
INDEX_NAME = ".launcher-character-glb-intake.tsv"
RESULT_NAME = ".launcher-character-result.json"


class SmokeError(ValueError):
    """The frozen helper did not fulfill its authoring contract."""


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise SmokeError(f"duplicate result key: {key}")
        result[key] = value
    return result


def _load_result(path: Path) -> dict[str, Any]:
    payload = path.read_bytes()
    if not payload or len(payload) > MAX_OUTPUT_BYTES:
        raise SmokeError("raw intake result has an invalid size")
    try:
        result = json.loads(
            payload.decode("utf-8"), object_pairs_hook=_strict_object,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SmokeError("raw intake result is not strict UTF-8 JSON") from exc
    if not isinstance(result, dict):
        raise SmokeError("raw intake result root must be an object")
    return result


def _offline_environment() -> dict[str, str]:
    """Make accidental network dependencies fail quickly and consistently."""
    environment = os.environ.copy()
    dead_proxy = "http://127.0.0.1:9"
    for name in (
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
        "http_proxy", "https_proxy", "all_proxy",
    ):
        environment[name] = dead_proxy
    environment["NO_PROXY"] = ""
    environment["no_proxy"] = ""
    environment["PIP_NO_INDEX"] = "1"
    return environment


def _invoke(command_prefix: list[str], character_dir: Path,
            result_path: Path, temporary: Path, arguments: list[str], *,
            succeeds: bool = True) -> dict[str, Any]:
    result_path.unlink(missing_ok=True)
    command = [
        *command_prefix, "--directory", str(character_dir),
        "--result-file", str(result_path), *arguments,
    ]
    try:
        completed = subprocess.run(
            command, cwd=temporary, check=False, env=_offline_environment(),
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            # Every user transaction cold-launches the sealed executable.
            # Signature assessment, endpoint security, and archive publication
            # can all dominate the tiny fixture on a freshly built app. Keep
            # the lifecycle gate aligned with the release verifier's bounded
            # first-launch allowance instead of failing healthy slow hosts.
            timeout=IMPORTER_COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise SmokeError(f"could not execute character importer: {exc}") from exc
    if (len(completed.stdout) > MAX_OUTPUT_BYTES or
            len(completed.stderr) > MAX_OUTPUT_BYTES):
        raise SmokeError("character importer emitted oversized output")
    if not result_path.is_file() or result_path.is_symlink():
        raise SmokeError("character importer did not write a regular result file")
    result = _load_result(result_path)
    expected_status = 0 if succeeds else 2
    if completed.returncode != expected_status:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise SmokeError(
            f"character importer {arguments[0]!r} exited "
            f"{completed.returncode}, expected {expected_status}: "
            f"{detail or result.get('error', 'no diagnostic')}"
        )
    if result.get("ok") is not succeeds:
        raise SmokeError(
            f"character importer result success mismatch: {result!r}"
        )
    return result


def _encoded(value: str) -> str:
    return value.encode("utf-8").hex()


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _installed_snapshot(character_dir: Path) -> dict[str, str]:
    """Fingerprint durable installed state, excluding launcher scratch files."""
    return {
        path.name: _digest(path)
        for path in character_dir.iterdir()
        if path.is_file() and not path.is_symlink()
        and path.name not in {INDEX_NAME, RESULT_NAME}
        and not path.name.startswith(".launcher-character-")
    }


def check(executable_path: Path, *, source_python: bool = False) -> None:
    if executable_path.is_symlink() or not executable_path.is_file():
        raise SmokeError("importer must be a regular, non-symlink file")
    executable = executable_path.resolve(strict=True)
    command_prefix = (
        [sys.executable, str(executable)] if source_python else [str(executable)]
    )
    with tempfile.TemporaryDirectory(prefix="mdkr-frozen-importer-smoke.") as raw:
        temporary = Path(raw)
        character_dir = temporary / "player data"
        character_dir.mkdir()
        model = temporary / "fixture model.glb"
        model_payload = make_animated_glb()
        model.write_bytes(model_payload)
        result_path = character_dir / RESULT_NAME
        index_path = character_dir / INDEX_NAME
        result = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["write-raw-glb-index", str(model), str(index_path)],
        )
        expected = {
            "ok": True,
            "schema": "mdkr-character-glb-intake-v3",
            "model_sha256": hashlib.sha256(model_payload).hexdigest(),
            "vertices": 3,
            "triangles": 1,
            "materials": 1,
            "textures": 1,
            "skins": 1,
            "joints": 2,
            "lod_levels": 1,
            "source_height_m": 1.0,
            "mesh_local_bounds": [[-0.5, 0.0, 0.0], [0.5, 1.0, 0.0]],
            "scene_world_bounds": [[-0.5, 0.0, 0.0], [0.5, 1.0, 0.0]],
            "source_animation_count": 1,
            "clips": ["idle"],
            "nodes": ["root", "head", "character"],
            "fallback": "idle",
            "seat": "root",
            "head": "head",
            "index_file": INDEX_NAME,
            "warnings": [],
        }
        for key, value in expected.items():
            if result.get(key) != value:
                raise SmokeError(
                    f"raw intake {key} mismatch: expected {value!r}, "
                    f"found {result.get(key)!r}"
                )
        validation = result.get("validation")
        if not isinstance(validation, dict) or validation.get("errors") != 0:
            raise SmokeError("raw intake did not preserve validator success")
        if (
            validation.get("schema") != "mdkr-gltf-validation-v1"
            or validation.get("source_sha256") != expected["model_sha256"]
            or validation.get("validator_version") != "2.0.0-dev.3.10"
            or validation.get("validator_commit") !=
                "bcd52cc4ba5f333b2999a58f67cc05ddf28b4fb1"
            or not isinstance(validation.get("validator_sha256"), str)
            or len(validation["validator_sha256"]) != 64
        ):
            raise SmokeError("raw intake validator provenance is malformed")
        expected_keys = set(expected) | {"model", "validation"}
        if set(result) != expected_keys:
            raise SmokeError("raw intake result has an unexpected schema")
        if Path(result["model"]).resolve(strict=True) != model.resolve(strict=True):
            raise SmokeError("raw intake result identifies the wrong model")
        expected_index = (
            "mdkr-character-glb-intake-v3\t"
            f"{expected['model_sha256']}\t3\t1\t1\t1\t1\t2\t1\t1\t3"
            "\t-0.5\t0\t0\t0.5\t1\t0\t-0.5\t0\t0\t0.5\t1\t0\t1\n"
            "defaults\t69646c65\t726f6f74\t68656164\n"
            "clip\t69646c65\n"
            "node\t726f6f74\n"
            "node\t68656164\n"
            "node\t636861726163746572\n"
        )
        if index_path.read_text(encoding="ascii") != expected_index:
            raise SmokeError("raw intake fixed-field index changed")
        if sorted(path.name for path in character_dir.iterdir()) != [
                INDEX_NAME, RESULT_NAME]:
            raise SmokeError("raw inventory smoke published unexpected files")

        # Prove the same shipped executable can complete the user-facing,
        # source-only authoring lifecycle without its build tree or a network.
        package_id = "org.mdkr.packaged-lifecycle"
        license_file = temporary / "fixture LICENSE.txt"
        license_payload = b"CC0-1.0 packaged lifecycle fixture\n"
        license_file.write_bytes(license_payload)
        model_before = _digest(model)
        license_before = _digest(license_file)
        built = _invoke(
            command_prefix, character_dir, result_path, temporary,
            [
                "build-raw-glb", _encoded(str(model)),
                _encoded(str(license_file)), _encoded(package_id),
                _encoded("Packaged Lifecycle"), _encoded("CC0-1.0"),
                _encoded("MDKR generated fixture"),
                _encoded("https://example.invalid/packaged-lifecycle"),
                _encoded("diddy"), _encoded("+z"), _encoded("idle"),
                _encoded("root"), _encoded("head"), model_before, "7", "1.0",
            ],
        )
        candidate = Path(str(built.get("candidate", "")))
        if (
            built.get("action") != "build-raw-glb-candidate"
            or built.get("id") != package_id
            or built.get("model_sha256") != model_before
            or built.get("license_sha256") != license_before
            or not isinstance(built.get("package_sha256"), str)
            or len(built["package_sha256"]) != 64
            or candidate.parent.resolve() != character_dir.resolve()
            or candidate.is_symlink()
            or not candidate.is_file()
            or _digest(candidate) != built["package_sha256"]
        ):
            raise SmokeError("raw candidate build contract is malformed")

        inspected = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["inspect", str(candidate)],
        )
        source_sha = inspected.get("source_sha256")
        if (
            inspected.get("action") != "inspect"
            or inspected.get("id") != package_id
            or inspected.get("portable") is not False
            or source_sha != built["package_sha256"]
        ):
            raise SmokeError("candidate review contract is malformed")

        installed = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["install-reviewed", str(candidate), source_sha, "absent"],
        )
        active_cache = character_dir / f"{package_id}.mdkc"
        retained_source = character_dir / f"{package_id}.{source_sha}.mdkrchar"
        provenance = character_dir / f"{package_id}.{source_sha}.json"
        if (
            installed.get("action") != "install-reviewed"
            or installed.get("id") != package_id
            or installed.get("source_sha256") != source_sha
            or installed.get("enabled") is not True
            or any(path.is_symlink() or not path.is_file() for path in (
                active_cache, retained_source, provenance,
            ))
            or retained_source.read_bytes() != candidate.read_bytes()
        ):
            raise SmokeError("reviewed install contract is malformed")
        cache_payload = active_cache.read_bytes()

        listing = _invoke(
            command_prefix, character_dir, result_path, temporary, ["list"],
        )
        entries = listing.get("entries")
        if (
            not isinstance(entries, list) or len(entries) != 1
            or entries[0].get("id") != package_id
            or entries[0].get("active") is not True
            or entries[0].get("enabled") is not True
            or entries[0].get("source_present") is not True
        ):
            raise SmokeError("installed character inventory is malformed")

        disabled = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["disable", package_id],
        )
        disabled_cache = character_dir / f"{package_id}.mdkc.disabled"
        if (
            disabled.get("enabled") is not False
            or disabled.get("id") != package_id
            or active_cache.exists()
            or disabled_cache.is_symlink()
            or not disabled_cache.is_file()
            or disabled_cache.read_bytes() != cache_payload
        ):
            raise SmokeError("disable transition did not preserve the cache")

        rebuilt = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["rebuild", package_id],
        )
        if (
            rebuilt.get("action") != "rebuild-current"
            or rebuilt.get("id") != package_id
            or rebuilt.get("rebuilt_source_sha256") != source_sha
            or rebuilt.get("enabled") is not False
            or active_cache.exists()
            or disabled_cache.read_bytes() != cache_payload
        ):
            raise SmokeError("disabled rebuild contract is malformed")
        installed_before_export = _installed_snapshot(character_dir)

        portable = temporary / "portable export.mdkrchar"
        exported = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["export-portable", package_id, source_sha, str(portable)],
        )
        if (
            exported.get("action") != "export-portable-revision"
            or exported.get("source_sha256") != source_sha
            or portable.is_symlink()
            or not portable.is_file()
        ):
            raise SmokeError("portable export contract is malformed")
        portable_payload = portable.read_bytes()
        portable_review = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["inspect", str(portable)],
        )
        if (
            portable_review.get("id") != package_id
            or portable_review.get("portable") is not True
            or portable_review.get("compiled_sha256") !=
                installed.get("compiled_sha256")
            or portable_review.get("license_spdx") != "CC0-1.0"
            or portable_review.get("attribution") !=
                "MDKR generated fixture"
            or portable_review.get("source_url") !=
                "https://example.invalid/packaged-lifecycle"
        ):
            raise SmokeError("portable package did not pass independent review")

        refused = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["export-portable", package_id, source_sha, str(portable)],
            succeeds=False,
        )
        if (
            "already exists" not in str(refused.get("error", ""))
            or portable.read_bytes() != portable_payload
            or _installed_snapshot(character_dir) != installed_before_export
        ):
            raise SmokeError(
                "portable export mutated installed state or violated "
                "no-overwrite safety"
            )

        enabled = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["enable", package_id],
        )
        if (
            enabled.get("enabled") is not True
            or enabled.get("id") != package_id
            or disabled_cache.exists()
            or active_cache.read_bytes() != cache_payload
        ):
            raise SmokeError("enable transition did not preserve the cache")

        removed = _invoke(
            command_prefix, character_dir, result_path, temporary,
            ["remove", package_id],
        )
        expected_removed = {
            active_cache.name, retained_source.name, provenance.name,
        }
        removed_files = removed.get("removed")
        if (
            removed.get("id") != package_id
            or not isinstance(removed_files, list)
            or set(removed_files) != expected_removed
        ):
            raise SmokeError("remove contract is malformed")
        if any(
            path.name == f"{package_id}.mdkc"
            or path.name == f"{package_id}.mdkc.disabled"
            or path.name.startswith(f"{package_id}.")
            for path in character_dir.iterdir()
        ):
            raise SmokeError("remove left package-owned files behind")
        if _digest(model) != model_before or _digest(license_file) != license_before:
            raise SmokeError("authoring lifecycle mutated external source files")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-python", action="store_true",
        help="run the supplied source script with this Python interpreter",
    )
    parser.add_argument("executable", type=Path)
    args = parser.parse_args(argv)
    try:
        check(args.executable, source_python=args.source_python)
    except (OSError, SmokeError) as exc:
        print(f"frozen character importer smoke failed: {exc}", file=sys.stderr)
        return 2
    print("character importer offline lifecycle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
