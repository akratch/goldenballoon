#!/usr/bin/env python3
"""ROM-free rendered proof for resumable raw-GLB Workshop intake."""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

from test_character_asset_probe import (  # noqa: E402
    make_animated_glb,
    rewrite_glb_document,
)
from test_collada_to_glb import DAE  # noqa: E402
import character_package_manager as manager  # noqa: E402
import character_source_adapter as source_adapter  # noqa: E402
from character_validation_fixture import (  # noqa: E402
    accepted_character_validation,
)


def isolated_environment(root: Path, model: Path, shot: Path, *,
                         compact: bool, drop: bool) -> dict[str, str]:
    prefs = root / "prefs"
    saves = root / "saves"
    characters = root / "characters"
    prefs.mkdir(parents=True, exist_ok=True)
    saves.mkdir(parents=True, exist_ok=True)
    characters.mkdir(parents=True, exist_ok=True)
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update({
        # A dropped GLB is inspected by a real background compiler job. The
        # token-gated smoke contract below waits on its actual state while
        # continuing to present and publish results on the UI thread.
        "MDKR_APP_SMOKE_FRAMES": "60" if drop else ("12" if compact else "8"),
        "MDKR_APP_SMOKE_WINDOW_SIZE": "640x480" if compact else "1280x720",
        "MDKR_APP_SMOKE_SHOT": str(shot),
        "MDKR_APP_PANEL": "Character Workshop",
        "MDKR_APP_UI_TRACE": "1",
        "MDKR_APP_PREFS_DIR": str(prefs),
        "MDKR_VIDEO_CONFIG_PATH": str(root / "video.ini"),
        "MDKR_SAVE_DIR": str(saves),
        "MDKR_CHARACTER_MANAGER": str(
            ROOT / "tests" / "run_character_manager_fixture.py"
        ),
        "MDKR_NO_CRASH_HANDLER": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_AUDIO": "0",
    })
    if drop:
        environment["MDKR_APP_SMOKE_DROP"] = str(model)
        environment["MDKR_APP_SMOKE_DROP_FRAME"] = "1"
        environment["MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS"] = "1"
        environment["MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS_TOKEN"] = \
            "mdkr64-character-jobs-v1"
    if compact:
        environment.update({
            "MDKR_APP_SMOKE_TOUCH_SCROLL": "1",
            "MDKR_APP_SMOKE_TOUCH_TOKEN": "mdkr64-app-touch-v1",
        })
    return environment


def run(binary: Path, root: Path, environment: dict[str, str],
        expected: tuple[str, ...]) -> str:
    completed = subprocess.run(
        [str(binary)], cwd=root, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180, check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"raw intake app exited {completed.returncode}\n"
            f"{completed.stdout[-8000:]}"
        )
    for marker in expected:
        if marker not in completed.stdout:
            raise RuntimeError(
                f"missing raw intake marker {marker!r}\n"
                f"{completed.stdout[-8000:]}"
            )
    return completed.stdout


def check_bmp(path: Path, logical_width: int, logical_height: int) -> None:
    payload = path.read_bytes()
    if len(payload) < 54 or payload[:2] != b"BM":
        raise RuntimeError(f"raw intake capture is not a BMP: {path}")
    width, height = struct.unpack_from("<ii", payload, 18)
    if width < logical_width or abs(height) < logical_height:
        raise RuntimeError(
            f"raw intake capture is undersized: {width}x{height}"
        )


def preserve_capture(source: Path, evidence_dir: Path | None,
                     name: str) -> None:
    if evidence_dir is None:
        return
    destination = evidence_dir / name
    if destination.exists() or destination.is_symlink():
        raise RuntimeError(f"evidence capture already exists: {destination}")
    payload = source.read_bytes()
    with destination.open("xb") as output:
        output.write(payload)


def raw_inventory(root: Path) -> tuple[str, list[list[str]]]:
    path = root / "saves" / "character_raw_drafts-v1.tsv"
    lines = path.read_text(encoding="ascii").splitlines()
    header = lines[0].split("\t")
    if len(header) != 4 or header[0] not in {
            "mdkr-character-raw-drafts-v1",
            "mdkr-character-raw-drafts-v2"}:
        raise RuntimeError("raw draft inventory header is malformed")
    count = int(header[1])
    rows = [line.split("\t") for line in lines[1:]]
    expected_fields = 19 if header[0].endswith("v2") else 18
    if count != len(rows) or any(len(row) != expected_fields for row in rows):
        raise RuntimeError("raw draft inventory rows are malformed")
    return header[2], rows


def row_text(row: list[str], field: int) -> str:
    return bytes.fromhex(row[field]).decode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path(DEFAULT_BUILD_DIR))
    parser.add_argument("--rom", type=Path,
                        help="accepted for run_checks.py compatibility")
    parser.add_argument(
        "--evidence-dir", type=Path,
        help="create this new directory and retain recipient-review BMPs",
    )
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    try:
        evidence_dir = args.evidence_dir
        if evidence_dir is not None:
            evidence_dir = evidence_dir.absolute()
            if (
                evidence_dir.exists() or evidence_dir.is_symlink()
                or evidence_dir.parent.is_symlink()
                or not evidence_dir.parent.is_dir()
            ):
                raise RuntimeError(
                    "evidence directory must be a new path inside an existing "
                    "real directory"
                )
            evidence_dir.mkdir()
        with tempfile.TemporaryDirectory(
                prefix="mdkr-raw-intake-ui-") as temporary:
            root = Path(temporary)
            model = root / "author-model.glb"
            model.write_bytes(make_animated_glb())
            model_b = root / "author-model-b.glb"
            changed_fixture = make_animated_glb().replace(
                b"mdkr-test-fixture", b"mdkr-test-fixturE"
            )
            if changed_fixture == model.read_bytes():
                raise RuntimeError("second GLB fixture did not change")
            model_b.write_bytes(changed_fixture)
            source_before = {
                model: hashlib.sha256(model.read_bytes()).hexdigest(),
                model_b: hashlib.sha256(model_b.read_bytes()).hexdigest(),
            }

            missing_importer = root / "missing-importer"
            missing_importer.mkdir()
            missing_shot = missing_importer / "missing-importer.bmp"
            missing_environment = isolated_environment(
                missing_importer, model, missing_shot,
                compact=False, drop=True,
            )
            missing_environment["MDKR_CHARACTER_MANAGER"] = str(
                missing_importer / "tools" / "character_importer"
            )
            run(
                binary, missing_importer, missing_environment,
                (
                    "active-panel=Character Workshop",
                    "character-importer-recovery missing_or_untrusted=1 "
                    "draft_preserved=1 source_mutated=0",
                ),
            )
            check_bmp(missing_shot, 1280, 720)
            _, missing_rows = raw_inventory(missing_importer)
            if (
                hashlib.sha256(model.read_bytes()).hexdigest()
                    != source_before[model]
                or len(missing_rows) != 1
                or row_text(missing_rows[0], 2) != str(model)
                or any((missing_importer / "characters").iterdir())
            ):
                raise RuntimeError(
                    "missing importer recovery did not preserve exactly one "
                    "resumable source-path draft without installed state"
                )

            guidance_cases = (
                ("author-model.FBX", "FBX"),
                ("author-model.obj", "OBJ"),
                ("author-model.blend", "Blender scene"),
                ("author-model.gltf", "glTF JSON"),
                ("author-model.usdz", "USD"),
                ("author-model.ma", "native DCC scene"),
            )
            for index, (filename, format_name) in enumerate(guidance_cases):
                guidance_root = root / f"guidance-{index}"
                guidance_root.mkdir()
                source = guidance_root / filename
                source.write_bytes(
                    b"untrusted authoring source must remain byte exact\x00" +
                    bytes([index])
                )
                source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
                guidance_environment = isolated_environment(
                    guidance_root, source,
                    guidance_root / "guidance.bmp",
                    compact=False, drop=True,
                )
                expected = [
                    "active-panel=Character Workshop",
                    f"character-source-guidance kind={format_name} "
                    "direct_import=0 mutated=0",
                ]
                if index == 0:
                    (guidance_root / "video.ini").write_text(
                        "[Accessibility]\nSpeech=1\n", encoding="utf-8"
                    )
                    guidance_environment.update({
                        "MDKR_APP_SMOKE_FRAMES": "220",
                        "MDKR_APP_SMOKE_A11Y_WALK": "1",
                        "MDKR_APP_SMOKE_INPUT": "keyboard",
                        "MDKR_APP_SMOKE_INPUT_TOKEN":
                            "mdkr64-app-ui-input-v1",
                        "MDKR_A11Y_TRACE": "1",
                    })
                    expected.append("text=Copy GLB export checklist")
                    expected.append(
                        "text=Copy data-only adapter handoff requirements"
                    )
                run(
                    binary, guidance_root, guidance_environment,
                    tuple(expected),
                )
                if (
                    hashlib.sha256(source.read_bytes()).hexdigest()
                        != source_digest
                    or (guidance_root / "saves" /
                        "character_raw_drafts-v1.tsv").exists()
                    or any((guidance_root / "characters").iterdir())
                ):
                    raise RuntimeError(
                        f"{format_name} guidance mutated the source, created "
                        "a raw draft, or published character state"
                    )

            hostile = root / "hostile-glb"
            hostile.mkdir()
            hostile_model = hostile / "bad-accessor.glb"
            hostile_model.write_bytes(rewrite_glb_document(
                make_animated_glb(),
                lambda document: document["accessors"][0].update({
                    "count": 0,
                }),
            ))
            hostile_shot = hostile / "raw-intake-hostile.bmp"
            hostile_environment = isolated_environment(
                hostile, hostile_model, hostile_shot,
                compact=False, drop=True,
            )
            # Inspection and the follow-up metadata-only recovery inventory
            # are two non-blocking jobs. The smoke wait contract observes both
            # publications without assuming local process or frame latency.
            hostile_environment["MDKR_APP_SMOKE_FRAMES"] = "60"
            run(
                binary, hostile,
                hostile_environment,
                (
                    "active-panel=Character Workshop",
                    "accessors[0].count must be a positive integer",
                    "raw-intake resumed=1 inspected=0 mappings=0 drafts=1",
                    "character-failure-recovery total=1 visible=1 metadata_only=1",
                ),
            )
            check_bmp(hostile_shot, 1280, 720)
            if (
                list((hostile / "characters").glob("*.mdkc"))
                or (hostile / "characters" /
                    ".launcher-character-raw-candidate.mdkrchar").exists()
            ):
                raise RuntimeError(
                    "a rejected GLB created a package candidate or cache"
                )
            recovery_files = sorted(
                path.name for path in (
                    hostile / "characters" /
                    manager.FAILURE_DIRECTORY_NAME
                ).iterdir()
            )
            if len(recovery_files) != 1 or not recovery_files[0].endswith(
                    ".json"):
                raise RuntimeError(
                    "probe rejection did not create exactly one metadata-only "
                    "recovery record"
                )
            (hostile / "video.ini").write_text(
                "[Accessibility]\nSpeech=1\n", encoding="utf-8"
            )
            hostile_accessible = isolated_environment(
                hostile, hostile_model, hostile / "raw-intake-recovery-a11y.bmp",
                compact=False, drop=False,
            )
            hostile_accessible.update({
                "MDKR_APP_SMOKE_FRAMES": "260",
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
                "MDKR_APP_SMOKE_CHARACTER_RECOVERY_ACTION": "check",
                "MDKR_APP_SMOKE_CHARACTER_RECOVERY_TOKEN":
                    "mdkr64-character-recovery-v1",
                # The recovery check is a wall-clock subprocess and the walk
                # counts frames: unpaced frames could finish the Tab pass
                # before the rows below existed. Walk the settled panel.
                "MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS": "1",
                "MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS_TOKEN":
                    "mdkr64-character-jobs-v1",
            })
            run(
                binary, hostile, hostile_accessible,
                (
                    "character-failure-recovery total=1 visible=1 metadata_only=1",
                    "text=Retry exact failed import",
                    "text=Use failed source as a new import",
                    "text=Copy failed-import error",
                    "text=Forget failed-import diagnostic",
                ),
            )

            conversion = root / "conversion"
            conversion.mkdir()
            nested_bytes = io.BytesIO()
            with zipfile.ZipFile(nested_bytes, "w") as nested:
                nested.writestr("Character/model.dae", DAE)
            archive = conversion / "author-download.zip"
            with zipfile.ZipFile(archive, "w") as outer:
                outer.writestr(
                    "source/model-files.zip", nested_bytes.getvalue()
                )
            archive_digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            converted = conversion / "converted.glb"
            conversion_environment = isolated_environment(
                conversion, archive,
                conversion / "raw-intake-conversion.bmp",
                compact=False, drop=True,
            )
            conversion_environment.update({
                # Conversion and the follow-on GLB inventory are two explicit
                # background publications. The smoke wait contract keeps the
                # UI alive and rendered until both have actually completed.
                "MDKR_APP_SMOKE_FRAMES": "120",
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_OUTPUT":
                    str(converted),
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_TOKEN":
                    "mdkr64-character-conversion-v1",
            })
            run(
                binary, conversion, conversion_environment,
                ("character-source-conversion kind=zip converted=1 "
                 "inspected=1 missing_license=1",
                 "raw-intake resumed=1 inspected=1 mappings=1 drafts=1"),
            )
            _, conversion_rows = raw_inventory(conversion)
            if (
                not converted.is_file()
                or len(conversion_rows) != 1
                or row_text(conversion_rows[0], 2) != str(converted)
                or conversion_rows[0][13] != hashlib.sha256(
                    converted.read_bytes()
                ).hexdigest()
                or hashlib.sha256(archive.read_bytes()).hexdigest()
                    != archive_digest
                or list((conversion / "characters").glob("*.mdkc"))
            ):
                raise RuntimeError(
                    "ZIP conversion did not produce one source-bound raw "
                    "draft while preserving the archive and installed state"
                )

            conversion_accessible = root / "conversion-accessible"
            conversion_accessible.mkdir()
            (conversion_accessible / "video.ini").write_text(
                "[Accessibility]\nSpeech=1\n", encoding="utf-8"
            )
            conversion_a11y_environment = isolated_environment(
                conversion_accessible, archive,
                conversion_accessible / "conversion-a11y.bmp",
                compact=False, drop=False,
            )
            conversion_a11y_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "260",
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_SOURCE": str(archive),
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_OUTPUT": str(
                    conversion_accessible / "converted.glb"
                ),
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_TOKEN":
                    "mdkr64-character-conversion-v1",
            })
            run(
                binary, conversion_accessible,
                conversion_a11y_environment,
                ("character-workshop-primary kind=review-selected-source ",
                 "text=Converted GLB destination",
                 "text=Convert, inspect, and continue"),
            )

            selected_source = root / "selected-source-compact"
            selected_source.mkdir()
            selected_source_shot = (
                selected_source / "selected-source-compact.bmp"
            )
            selected_source_environment = isolated_environment(
                selected_source, archive, selected_source_shot,
                compact=True, drop=False,
            )
            (selected_source / "prefs" / "mdkr64_app.ini").write_text(
                "ui_scale=2.00\n", encoding="utf-8"
            )
            selected_source_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "16",
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_SOURCE": str(archive),
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_OUTPUT": str(
                    selected_source / "converted.glb"
                ),
                "MDKR_APP_SMOKE_CHARACTER_CONVERSION_TOKEN":
                    "mdkr64-character-conversion-v1",
            })
            run(
                binary, selected_source, selected_source_environment,
                (
                    "character-workshop-primary "
                    "kind=review-selected-source "
                    "label=Review selected source",
                    "compact-layout dense=1 contained=1 overlap=0 ",
                ),
            )
            check_bmp(selected_source_shot, 640, 480)
            preserve_capture(
                selected_source_shot, evidence_dir,
                "selected-source-review-compact.bmp",
            )
            if (any((selected_source / "characters").iterdir())):
                raise RuntimeError(
                    "reviewing a selected source in the compact journey "
                    "published character state before validation"
                )

            adapter_root = root / "adapter-output"
            adapter_root.mkdir()
            adapter_model = adapter_root / "export.glb"
            adapter_model.write_bytes(make_animated_glb())
            adapter_license = adapter_root / "NOTICE.txt"
            adapter_license.write_text(
                "CC0 adapter fixture notice\n", encoding="utf-8"
            )
            adapter_artifact = adapter_root / "artist.mdkrsource"
            source_adapter.pack(
                model_path=adapter_model, output_path=adapter_artifact,
                adapter_name="Fixture Blender exporter",
                adapter_version="1.0.0",
                adapter_homepage="https://example.invalid/adapter",
                source_format="Blender scene",
                source_sha256=hashlib.sha256(
                    b"exact fixture blend bytes"
                ).hexdigest(),
                conversion_profile="Golden Balloon character-v1",
                conversion_settings_sha256=hashlib.sha256(
                    b'{"animations":"baked","modifiers":"evaluated"}'
                ).hexdigest(),
                license_path=adapter_license,
                license_spdx="CC0-1.0",
                attribution="Fixture artist",
                source_url="https://example.invalid/fixture",
            )
            adapter_digest = hashlib.sha256(
                adapter_artifact.read_bytes()
            ).hexdigest()
            extracted_model = adapter_root / "reviewed.glb"
            adapter_environment = isolated_environment(
                adapter_root, adapter_artifact,
                adapter_root / "adapter-handoff.bmp",
                compact=False, drop=True,
            )
            adapter_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "180",
                "MDKR_APP_SMOKE_CHARACTER_ADAPTER_ACTION":
                    "extract-reviewed",
                "MDKR_APP_SMOKE_CHARACTER_ADAPTER_OUTPUT":
                    str(extracted_model),
                "MDKR_APP_SMOKE_CHARACTER_ADAPTER_TOKEN":
                    "mdkr64-character-adapter-output-v1",
            })
            run(
                binary, adapter_root, adapter_environment,
                ("character-adapter-review inspected=1 extracted=0 "
                 "executed=0 authenticated=0",
                 "character-adapter-handoff reviewed=1 extracted=1 "
                 "inspected=1 executed=0 authenticated=0 license=1",
                 "raw-intake resumed=1 inspected=1 mappings=1 drafts=1",
                 "character-workshop-primary kind=continue-raw-draft "),
            )
            _, adapter_rows = raw_inventory(adapter_root)
            adapter_provenance = adapter_root / "reviewed.mdkrsource.json"
            if (
                len(adapter_rows) != 1
                or row_text(adapter_rows[0], 2) != str(extracted_model)
                or row_text(adapter_rows[0], 3) != str(
                    adapter_root / "reviewed.LICENSE.txt"
                )
                or row_text(adapter_rows[0], 6) != "CC0-1.0"
                or row_text(adapter_rows[0], 7) != "Fixture artist"
                or not extracted_model.is_file()
                or not adapter_provenance.is_file()
                or hashlib.sha256(adapter_artifact.read_bytes()).hexdigest()
                    != adapter_digest
                or list((adapter_root / "characters").glob("*.mdkc"))
            ):
                raise RuntimeError(
                    "adapter handoff did not preserve its exact artifact, "
                    "prefill rights, and open one uninstalled source draft"
                )
            preserve_capture(
                adapter_root / "adapter-handoff.bmp", evidence_dir,
                "adapter-handoff.bmp",
            )

            adapter_review = root / "adapter-review"
            adapter_review.mkdir()
            (adapter_review / "video.ini").write_text(
                "[Accessibility]\nSpeech=1\n", encoding="utf-8"
            )
            adapter_review_environment = isolated_environment(
                adapter_review, adapter_artifact,
                adapter_review / "adapter-review-200.bmp",
                compact=False, drop=True,
            )
            (adapter_review / "prefs" / "mdkr64_app.ini").write_text(
                "ui_scale=2.00\n", encoding="utf-8"
            )
            adapter_review_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "220",
                "MDKR_APP_SMOKE_WINDOW_SIZE": "640x480",
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
            })
            run(
                binary, adapter_review, adapter_review_environment,
                ("character-adapter-review inspected=1 extracted=0 "
                 "executed=0 authenticated=0",
                 "character-workshop-primary kind=review-adapter "
                 "label=Review adapter result",
                 "text=Accept external adapter result",
                 "text=Extracted GLB destination",
                 "text=Extract verified data and continue"),
            )
            check_bmp(adapter_review / "adapter-review-200.bmp", 640, 480)
            if any(adapter_review.glob("reviewed.*")):
                raise RuntimeError(
                    "mutation-free adapter review extracted output unexpectedly"
                )
            preserve_capture(
                adapter_review / "adapter-review-200.bmp", evidence_dir,
                "adapter-review-200.bmp",
            )

            adapter_visual = root / "adapter-visual"
            adapter_visual.mkdir()
            adapter_visual_environment = isolated_environment(
                adapter_visual, adapter_artifact,
                adapter_visual / "adapter-review-visible-200.bmp",
                compact=False, drop=True,
            )
            (adapter_visual / "prefs" / "mdkr64_app.ini").write_text(
                "ui_scale=2.00\n", encoding="utf-8"
            )
            adapter_visual_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "90",
                "MDKR_APP_SMOKE_WINDOW_SIZE": "640x480",
                "MDKR_APP_SMOKE_CHARACTER_ADAPTER_FOCUS_REVIEW": "1",
                "MDKR_APP_SMOKE_CHARACTER_ADAPTER_TOKEN":
                    "mdkr64-character-adapter-output-v1",
            })
            run(
                binary, adapter_visual, adapter_visual_environment,
                ("character-adapter-review inspected=1 extracted=0 "
                 "executed=0 authenticated=0",
                 "character-adapter-review-focused=1",
                 "compact-layout dense=1 contained=1 overlap=0 "),
            )
            check_bmp(
                adapter_visual / "adapter-review-visible-200.bmp", 640, 480
            )
            preserve_capture(
                adapter_visual / "adapter-review-visible-200.bmp",
                evidence_dir, "adapter-review-visible-200.bmp",
            )

            wide = root / "wide"
            wide.mkdir()
            wide_shot = wide / "raw-intake-wide.bmp"
            run(
                binary, wide,
                isolated_environment(
                    wide, model, wide_shot, compact=False, drop=True
                ),
                ("active-panel=Character Workshop",
                 "raw-intake resumed=1 inspected=1 mappings=1 drafts=1",
                 "character-workshop-primary kind=continue-raw-draft "
                 "label=Continue character draft",
                 "raw-transform bounds=1 valid=1 severity=0 candidates=4 "),
            )
            check_bmp(wide_shot, 1280, 720)
            selected_a, rows = raw_inventory(wide)
            if (
                len(rows) != 1
                or rows[0][0] != selected_a
                or row_text(rows[0], 2) != str(model)
                or rows[0][13] != source_before[model]
                or tuple(row_text(rows[0], field) for field in (14, 15, 16))
                != ("idle", "root", "head")
            ):
                raise RuntimeError(
                    "first source-bound raw draft was not persisted exactly"
                )
            if (
                list((wide / "characters").glob("*.mdkc"))
                or (
                    wide / "characters" /
                    ".launcher-character-raw-candidate.mdkrchar"
                ).exists()
            ):
                raise RuntimeError(
                    "inspection published a cache or package candidate"
                )

            resume_shot = wide / "raw-intake-resume.bmp"
            run(
                binary, wide,
                isolated_environment(
                    wide, model, resume_shot, compact=False, drop=False
                ),
                ("raw-intake resumed=1 inspected=0 mappings=0",),
            )
            check_bmp(resume_shot, 1280, 720)

            close_environment = isolated_environment(
                wide, model, wide / "raw-intake-close.bmp",
                compact=False, drop=False
            )
            close_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "close-editor",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, close_environment,
                ("raw-draft-action action=close-editor applied=1",),
            )
            close_preferences = (
                wide / "prefs" / "mdkr64_app.ini"
            ).read_text(encoding="utf-8")
            if "character_raw_editor_open=0" not in close_preferences:
                raise RuntimeError(
                    "closing raw authoring was not durable navigation state"
                )
            closed_shot = wide / "raw-intake-closed.bmp"
            closed_output = run(
                binary, wide,
                isolated_environment(
                    wide, model, closed_shot, compact=False, drop=False
                ),
                ("raw-draft-library open=0 drafts=1",
                 "character-workshop-primary kind=resume-raw-draft "),
            )
            if "raw-intake resumed=1" in closed_output:
                raise RuntimeError(
                    "a closed raw draft still monopolized the installed editor"
                )
            resume_environment = isolated_environment(
                wide, model, wide / "raw-intake-explicit-resume.bmp",
                compact=False, drop=False
            )
            resume_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "resume-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, resume_environment,
                ("raw-draft-action action=resume-selected applied=1",
                 "raw-intake resumed=1 inspected=0 mappings=0 drafts=1"),
            )

            duplicate_shot = wide / "raw-intake-duplicate.bmp"
            duplicate_environment = isolated_environment(
                wide, model, duplicate_shot, compact=False, drop=False
            )
            duplicate_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "duplicate-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, duplicate_environment,
                ("raw-draft-action action=duplicate-selected applied=1 drafts=2",),
            )
            duplicate_selected, duplicate_rows = raw_inventory(wide)
            if (
                len(duplicate_rows) != 2
                or {row_text(row, 2) for row in duplicate_rows} != {str(model)}
                or len({row[0] for row in duplicate_rows}) != 2
                or duplicate_selected == selected_a
                or any(row[13] != source_before[model]
                       for row in duplicate_rows)
            ):
                raise RuntimeError(
                    "same-source authoring branch was not independent and "
                    "source-bound"
                )
            delete_duplicate_environment = isolated_environment(
                wide, model, wide / "raw-intake-delete-duplicate.bmp",
                compact=False, drop=False
            )
            delete_duplicate_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "delete-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, delete_duplicate_environment,
                ("raw-draft-action action=delete-selected applied=1 drafts=1",),
            )
            selected_after_branch_delete, branch_rows = raw_inventory(wide)
            if (
                len(branch_rows) != 1
                or branch_rows[0][0] != selected_after_branch_delete
                or row_text(branch_rows[0], 2) != str(model)
            ):
                raise RuntimeError(
                    "deleting a same-source branch did not preserve its peer"
                )

            second_shot = wide / "raw-intake-second.bmp"
            run(
                binary, wide,
                isolated_environment(
                    wide, model_b, second_shot, compact=False, drop=True
                ),
                ("raw-intake resumed=1 inspected=1 mappings=1 drafts=2",),
            )
            check_bmp(second_shot, 1280, 720)
            selected_b, rows = raw_inventory(wide)
            by_path = {row_text(row, 2): row for row in rows}
            if (
                len(rows) != 2
                or set(by_path) != {str(model), str(model_b)}
                or by_path[str(model)][13] != source_before[model]
                or by_path[str(model_b)][13] != source_before[model_b]
                or selected_b != by_path[str(model_b)][0]
            ):
                raise RuntimeError(
                    "two raw sources did not retain independent fingerprints"
                )

            switch_shot = wide / "raw-intake-switch.bmp"
            switch_environment = isolated_environment(
                wide, model, switch_shot, compact=False, drop=False
            )
            switch_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION":
                    "select-sha:" + source_before[model],
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, switch_environment,
                ("raw-draft-action action=select-sha:",
                 "applied=1 drafts=2", "raw-intake resumed=1 inspected=0"),
            )
            selected_after_switch, rows = raw_inventory(wide)
            by_path = {row_text(row, 2): row for row in rows}
            if selected_after_switch != by_path[str(model)][0]:
                raise RuntimeError("explicit raw draft switch was not durable")

            restore_shot = wide / "raw-intake-mapping-restore.bmp"
            run(
                binary, wide,
                isolated_environment(
                    wide, model, restore_shot, compact=False, drop=True
                ),
                ("raw-intake resumed=1 inspected=1 mappings=1 drafts=2",),
            )
            _, rows = raw_inventory(wide)
            by_path = {row_text(row, 2): row for row in rows}
            if tuple(row_text(by_path[str(model)], field)
                     for field in (14, 15, 16)) != ("idle", "root", "head"):
                raise RuntimeError(
                    "reinspection did not restore the exact source-bound mappings"
                )

            # Re-select B through the ordinary GLB source path, then exercise
            # the exact UI deletion action. Only the selected authoring record
            # may disappear; neither external source is mutable.
            run(
                binary, wide,
                isolated_environment(
                    wide, model_b, wide / "raw-intake-reselect-b.bmp",
                    compact=False, drop=True
                ),
                ("raw-intake resumed=1 inspected=1 mappings=1 drafts=2",),
            )
            delete_environment = isolated_environment(
                wide, model_b, wide / "raw-intake-delete.bmp",
                compact=False, drop=False
            )
            delete_environment.update({
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "delete-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, wide, delete_environment,
                ("raw-draft-action action=delete-selected applied=1 drafts=1",),
            )
            selected_after_delete, rows = raw_inventory(wide)
            if (
                len(rows) != 1
                or row_text(rows[0], 2) != str(model)
                or selected_after_delete != rows[0][0]
            ):
                raise RuntimeError(
                    "deleting the selected raw draft did not preserve its peer"
                )
            for source, digest in source_before.items():
                if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                    raise RuntimeError(
                        f"raw draft lifecycle changed external source {source}"
                    )

            legacy = root / "legacy"
            legacy.mkdir()
            legacy_prefs = legacy / "prefs"
            legacy_prefs.mkdir()
            (legacy_prefs / "mdkr64_app.ini").write_text(
                f"character_raw_intake_model={model}\n"
                "character_raw_intake_donor=9\n"
                "character_raw_intake_vehicles=7\n"
                "character_raw_intake_forward=0\n"
                "character_raw_intake_height=1.25\n"
                f"character_raw_intake_mapping_sha256={source_before[model]}\n"
                "character_raw_intake_fallback=idle\n"
                "character_raw_intake_seat=root\n"
                "character_raw_intake_head=head\n",
                encoding="utf-8",
            )
            run(
                binary, legacy,
                isolated_environment(
                    legacy, model, legacy / "raw-intake-legacy.bmp",
                    compact=False, drop=False
                ),
                ("raw-intake resumed=1 inspected=0 mappings=0 drafts=1",),
            )
            _, legacy_rows = raw_inventory(legacy)
            migrated_preferences = (
                legacy / "prefs" / "mdkr64_app.ini"
            ).read_text(encoding="utf-8")
            if (
                len(legacy_rows) != 1
                or row_text(legacy_rows[0], 2) != str(model)
                or "character_raw_intake_" in migrated_preferences
            ):
                raise RuntimeError(
                    "legacy singleton draft did not migrate transactionally"
                )
            (legacy / "prefs" / "mdkr64_app.ini").write_text(
                migrated_preferences + "character_raw_editor_open=0\n",
                encoding="utf-8",
            )
            (legacy / "video.ini").write_text(
                "[Accessibility]\nSpeech=1\n", encoding="utf-8"
            )
            closed_a11y_environment = isolated_environment(
                legacy, model, legacy / "raw-intake-closed-a11y.bmp",
                compact=False, drop=False
            )
            closed_a11y_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "300",
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
            })
            run(
                binary, legacy, closed_a11y_environment,
                ("raw-draft-library open=0 drafts=1",
                 "text=Resume raw authoring draft"),
            )
            legacy_store = (
                legacy / "saves" / "character_raw_drafts-v1.tsv"
            )
            corrupt_bytes = bytearray(legacy_store.read_bytes())
            checksum_byte = len(corrupt_bytes) - 2
            corrupt_bytes[checksum_byte] = (
                ord("0") if corrupt_bytes[checksum_byte] != ord("0")
                else ord("1")
            )
            legacy_store.write_bytes(corrupt_bytes)
            corrupt_before = legacy_store.read_bytes()
            corrupt_shot = legacy / "raw-intake-corrupt.bmp"
            run(
                binary, legacy,
                isolated_environment(
                    legacy, model, corrupt_shot, compact=False, drop=False
                ),
                ("raw-draft-store writable=0 error=",),
            )
            check_bmp(corrupt_shot, 1280, 720)
            if legacy_store.read_bytes() != corrupt_before:
                raise RuntimeError(
                    "malformed raw draft inventory was partially loaded or overwritten"
                )

            invalid_spdx = root / "invalid-spdx"
            invalid_spdx.mkdir()
            invalid_spdx_prefs = invalid_spdx / "prefs"
            invalid_spdx_prefs.mkdir()
            invalid_spdx_license = invalid_spdx / "LICENSE.txt"
            invalid_spdx_license.write_text(
                "Fixture notice\n", encoding="utf-8"
            )
            (invalid_spdx_prefs / "mdkr64_app.ini").write_text(
                f"character_raw_intake_model={model}\n"
                f"character_raw_intake_license={invalid_spdx_license}\n"
                "character_raw_intake_id=org.example.invalid-spdx\n"
                "character_raw_intake_display_name=Invalid SPDX Proof\n"
                "character_raw_intake_spdx=MIT Or Apache-2.0\n"
                "character_raw_intake_attribution=Generated test fixture\n"
                "character_raw_intake_source_url=https://example.invalid/spdx\n"
                "character_raw_intake_donor=9\n"
                "character_raw_intake_vehicles=7\n"
                "character_raw_intake_forward=0\n"
                "character_raw_intake_height=1.25\n"
                f"character_raw_intake_mapping_sha256={source_before[model]}\n"
                "character_raw_intake_fallback=idle\n"
                "character_raw_intake_seat=root\n"
                "character_raw_intake_head=head\n",
                encoding="utf-8",
            )
            invalid_spdx_environment = isolated_environment(
                invalid_spdx, model, invalid_spdx / "invalid-spdx.bmp",
                compact=False, drop=True,
            )
            invalid_spdx_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "16",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION":
                    "build-reviewed-install",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            invalid_spdx_log = run(
                binary, invalid_spdx, invalid_spdx_environment,
                ("raw-spdx valid=0 build-ready=0 "
                 "error=Unexpected SPDX token 'Or' at byte 4.",),
            )
            if "raw-draft-action action=build-reviewed-install applied=1" in invalid_spdx_log:
                raise RuntimeError(
                    "invalid SPDX expression reached source-package build"
                )
            if (
                invalid_spdx / "characters" /
                ".launcher-character-raw-candidate.mdkrchar"
            ).exists():
                raise RuntimeError(
                    "invalid SPDX expression produced a review candidate"
                )

            recipient = root / "recipient-review"
            recipient.mkdir()
            recipient_prefs = recipient / "prefs"
            recipient_prefs.mkdir()
            recipient_license = recipient / "LICENSE.txt"
            recipient_license.write_text(
                "CC0 1.0 Universal recipient fixture\n", encoding="utf-8"
            )
            recipient_source_before = {
                model: hashlib.sha256(model.read_bytes()).hexdigest(),
                recipient_license: hashlib.sha256(
                    recipient_license.read_bytes()
                ).hexdigest(),
            }
            (recipient_prefs / "mdkr64_app.ini").write_text(
                f"character_raw_intake_model={model}\n"
                f"character_raw_intake_license={recipient_license}\n"
                "character_raw_intake_id=org.example.recipient-review\n"
                "character_raw_intake_display_name=Recipient Review Proof\n"
                "character_raw_intake_spdx=CC0-1.0\n"
                "character_raw_intake_attribution=Generated recipient fixture\n"
                "character_raw_intake_source_url=https://example.invalid/recipient-review\n"
                "character_raw_intake_donor=9\n"
                "character_raw_intake_vehicles=7\n"
                "character_raw_intake_forward=0\n"
                "character_raw_intake_height=1.25\n"
                f"character_raw_intake_mapping_sha256={source_before[model]}\n"
                "character_raw_intake_fallback=idle\n"
                "character_raw_intake_seat=root\n"
                "character_raw_intake_head=head\n",
                encoding="utf-8",
            )
            recipient_shot = recipient / "recipient-review.bmp"
            recipient_environment = isolated_environment(
                recipient, model, recipient_shot,
                compact=False, drop=True,
            )
            recipient_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "180",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "build-review-only",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, recipient, recipient_environment,
                (
                    "raw-draft-action action=build-review-only applied=1",
                    "character-recipient-review compatibility=1 mode=source "
                    "relationship=new rig_ready=1 performance=Excellent "
                    "lods=1 webgpu_required=1 rights_confirmed=0 "
                    "tangent_diag=1 tangent_fallback=0 "
                    "normal_map_fallback=0",
                    "character-workshop-primary kind=review-candidate "
                    "label=Continue candidate review",
                ),
            )
            check_bmp(recipient_shot, 1280, 720)
            preserve_capture(
                recipient_shot, evidence_dir, "source-recipient-review.bmp"
            )
            if (
                not (recipient / "characters" /
                     ".launcher-character-raw-candidate.mdkrchar").is_file()
                or list((recipient / "characters").glob("*.mdkc"))
            ):
                raise RuntimeError(
                    "recipient review installed bytes before rights "
                    "confirmation or lost its mutation-free candidate"
                )
            _, recipient_rows = raw_inventory(recipient)
            if len(recipient_rows) != 1:
                raise RuntimeError(
                    "recipient review did not preserve its resumable raw draft"
                )
            for source, digest in recipient_source_before.items():
                if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                    raise RuntimeError(
                        f"recipient review changed external source {source}"
                    )

            source_candidate = (
                recipient / "characters" /
                ".launcher-character-raw-candidate.mdkrchar"
            )
            portable_package = root / "received-portable.mdkrchar"
            with accepted_character_validation(manager):
                manager.prepare(source_candidate, portable_package)
            portable_before = hashlib.sha256(
                portable_package.read_bytes()
            ).hexdigest()
            portable_recipient = root / "portable-recipient-review"
            portable_recipient.mkdir()
            (portable_recipient / "video.ini").write_text(
                "[Accessibility]\nSpeech=1\n", encoding="utf-8"
            )
            portable_shot = portable_recipient / "portable-review.bmp"
            portable_environment = isolated_environment(
                portable_recipient, portable_package, portable_shot,
                compact=False, drop=True,
            )
            portable_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "320",
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
            })
            run(
                binary, portable_recipient, portable_environment,
                (
                    "character-recipient-review compatibility=1 "
                    "mode=portable relationship=new rig_ready=1 "
                    "performance=Excellent lods=1 webgpu_required=1 "
                    "rights_confirmed=0 tangent_diag=0 "
                    "tangent_fallback=0 normal_map_fallback=0",
                    "character-workshop-primary kind=review-candidate "
                    "label=Continue candidate review",
                    "text=Local-use rights confirmation. Package "
                    "compatibility passed. This is a received portable package",
                ),
            )
            check_bmp(portable_shot, 1280, 720)
            preserve_capture(
                portable_shot, evidence_dir,
                "portable-recipient-review.bmp",
            )
            if (
                hashlib.sha256(portable_package.read_bytes()).hexdigest()
                    != portable_before
                or any((portable_recipient / "characters").iterdir())
                or (portable_recipient / "saves" /
                    "character_raw_drafts-v1.tsv").exists()
            ):
                raise RuntimeError(
                    "portable recipient review mutated the package or "
                    "published authoring/install state before confirmation"
                )

            compact_recipient = root / "portable-recipient-compact"
            compact_recipient.mkdir()
            compact_shot = compact_recipient / "portable-review-compact.bmp"
            compact_environment = isolated_environment(
                compact_recipient, portable_package, compact_shot,
                compact=True, drop=True,
            )
            (compact_recipient / "prefs" / "mdkr64_app.ini").write_text(
                "ui_scale=2.00\n", encoding="utf-8"
            )
            compact_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "120",
                "MDKR_APP_SMOKE_CHARACTER_CANDIDATE_ACTION":
                    "show-discard-confirmation",
                "MDKR_APP_SMOKE_CHARACTER_CANDIDATE_TOKEN":
                    "mdkr64-character-candidate-v1",
            })
            run(
                binary, compact_recipient, compact_environment,
                (
                    "character-recipient-review compatibility=1 "
                    "mode=portable relationship=new rig_ready=1 "
                    "performance=Excellent lods=1 webgpu_required=1 "
                    "rights_confirmed=0 tangent_diag=0 "
                    "tangent_fallback=0 normal_map_fallback=0",
                    "character-workshop-primary kind=review-candidate "
                    "label=Review candidate",
                    "character-candidate-discard-modal contained=1 ",
                    "compact-layout dense=1 contained=1 overlap=0 ",
                ),
            )
            check_bmp(compact_shot, 640, 480)
            preserve_capture(
                compact_shot, evidence_dir,
                "portable-recipient-review-compact.bmp",
            )
            if (
                hashlib.sha256(portable_package.read_bytes()).hexdigest()
                    != portable_before
                or any((compact_recipient / "characters").iterdir())
            ):
                raise RuntimeError(
                    "compact recipient review mutated package or install state"
                )

            discard_recipient = root / "portable-recipient-discard"
            discard_recipient.mkdir()
            discard_environment = isolated_environment(
                discard_recipient, portable_package,
                discard_recipient / "portable-discard.bmp",
                compact=False, drop=True,
            )
            discard_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "180",
                "MDKR_APP_SMOKE_CHARACTER_CANDIDATE_ACTION":
                    "discard-confirmed",
                "MDKR_APP_SMOKE_CHARACTER_CANDIDATE_TOKEN":
                    "mdkr64-character-candidate-v1",
            })
            run(
                binary, discard_recipient, discard_environment,
                (
                    "character-recipient-review compatibility=1 "
                    "mode=portable relationship=new",
                    "character-candidate-action "
                    "action=discard-confirmed applied=1",
                ),
            )
            if (
                hashlib.sha256(portable_package.read_bytes()).hexdigest()
                    != portable_before
                or any((discard_recipient / "characters").iterdir())
                or (discard_recipient / "saves" /
                    "character_raw_drafts-v1.tsv").exists()
            ):
                raise RuntimeError(
                    "confirmed candidate discard changed its external package "
                    "or published authoring/install state"
                )

            install = root / "install"
            install.mkdir()
            install_prefs = install / "prefs"
            install_prefs.mkdir()
            license_file = install / "LICENSE.txt"
            license_file.write_text(
                "CC0 1.0 Universal test fixture\n", encoding="utf-8"
            )
            install_source_before = {
                model: hashlib.sha256(model.read_bytes()).hexdigest(),
                license_file: hashlib.sha256(
                    license_file.read_bytes()
                ).hexdigest(),
            }
            (install_prefs / "mdkr64_app.ini").write_text(
                f"character_raw_intake_model={model}\n"
                f"character_raw_intake_license={license_file}\n"
                "character_raw_intake_id=org.example.raw-install\n"
                "character_raw_intake_display_name=Raw Install Proof\n"
                "character_raw_intake_spdx=CC0-1.0\n"
                "character_raw_intake_attribution=Generated test fixture\n"
                "character_raw_intake_source_url=https://example.invalid/raw-install\n"
                "character_raw_intake_donor=9\n"
                "character_raw_intake_vehicles=7\n"
                "character_raw_intake_forward=0\n"
                "character_raw_intake_height=1.25\n"
                f"character_raw_intake_mapping_sha256={source_before[model]}\n"
                "character_raw_intake_fallback=idle\n"
                "character_raw_intake_seat=root\n"
                "character_raw_intake_head=head\n",
                encoding="utf-8",
            )
            duplicate_environment = isolated_environment(
                install, model, install / "raw-intake-duplicate.bmp",
                compact=False, drop=False,
            )
            duplicate_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "20",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "duplicate-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, install, duplicate_environment,
                ("raw-draft-action action=duplicate-selected applied=1 "
                 "drafts=2",),
            )
            install_environment = isolated_environment(
                install, model, install / "raw-intake-install.bmp",
                compact=False, drop=True
            )
            install_environment.update({
                # GLB inspection, source build, and candidate compilation are
                # three separately published background jobs before the
                # reviewed native install arm can run.
                "MDKR_APP_SMOKE_FRAMES": "240",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION":
                    "build-reviewed-install",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, install, install_environment,
                ("raw-draft-action action=build-reviewed-install applied=1",
                 "raw-reviewed-install reviewed=1 installed=1",
                 "remaining=1"),
            )
            selected_after_install, install_rows = raw_inventory(install)
            if (
                len(install_rows) != 1
                or selected_after_install != install_rows[0][0]
                or row_text(install_rows[0], 4)
                    != "org.example.raw-install"
            ):
                raise RuntimeError(
                    "successful reviewed install did not remove only its "
                    "exact completed raw draft"
                )
            if not (
                install / "characters" / "org.example.raw-install.mdkc"
            ).is_file():
                raise RuntimeError(
                    "reviewed raw-source install did not publish its cache"
                )
            if (
                install / "characters" /
                ".launcher-character-raw-candidate.mdkrchar"
            ).exists():
                raise RuntimeError(
                    "reviewed raw-source install retained its disposable candidate"
                )
            for source, digest in install_source_before.items():
                if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                    raise RuntimeError(
                        f"reviewed install changed external source {source}"
                    )

            installed_before_handoff = hashlib.sha256(
                (install / "characters" /
                 "org.example.raw-install.mdkc").read_bytes()
            ).hexdigest()
            (install_prefs / "mdkr64_app.ini").write_text(
                "character_workshop_last_selected=org.example.raw-install\n"
                "character_workshop_last_tab=performance\n"
                "character_raw_editor_open=0\n",
                encoding="utf-8",
            )
            performance_environment = isolated_environment(
                install, model, install / "performance-source-handoff.bmp",
                compact=False, drop=False,
            )
            performance_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "30",
                "MDKR_APP_SMOKE_PERFORMANCE_SOURCE_ACTION": "open-unique",
                "MDKR_APP_SMOKE_PERFORMANCE_SOURCE_ACTION_TOKEN":
                    "mdkr64-app-performance-source-v1",
            })
            run(
                binary, install, performance_environment,
                (
                    "character-performance-source-handoff "
                    "package=org.example.raw-install matches=1 ",
                    "applied=1 ambiguity=unique",
                    "raw-intake resumed=1 inspected=0 mappings=0 drafts=1",
                ),
            )
            selected_after_handoff, rows_after_handoff = raw_inventory(install)
            if (
                len(rows_after_handoff) != 1
                or selected_after_handoff != rows_after_handoff[0][0]
                or row_text(rows_after_handoff[0], 2) != str(model)
                or hashlib.sha256(
                    (install / "characters" /
                     "org.example.raw-install.mdkc").read_bytes()
                ).hexdigest() != installed_before_handoff
            ):
                raise RuntimeError(
                    "Performance source handoff guessed, changed, or lost "
                    "the retained source or installed character"
                )
            for source, digest in install_source_before.items():
                if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                    raise RuntimeError(
                        "Performance source handoff changed external source "
                        f"{source}"
                    )

            ambiguous_duplicate_environment = isolated_environment(
                install, model, install / "performance-duplicate.bmp",
                compact=False, drop=False,
            )
            ambiguous_duplicate_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "20",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION": "duplicate-selected",
                "MDKR_APP_SMOKE_RAW_DRAFT_ACTION_TOKEN":
                    "mdkr64-app-raw-draft-v1",
            })
            run(
                binary, install, ambiguous_duplicate_environment,
                ("raw-draft-action action=duplicate-selected applied=1 "
                 "drafts=2",),
            )
            (install_prefs / "mdkr64_app.ini").write_text(
                "character_workshop_last_selected=org.example.raw-install\n"
                "character_workshop_last_tab=performance\n"
                "character_raw_editor_open=0\n",
                encoding="utf-8",
            )
            ambiguous_environment = isolated_environment(
                install, model, install / "performance-ambiguous.bmp",
                compact=False, drop=False,
            )
            ambiguous_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "20",
                "MDKR_APP_SMOKE_PERFORMANCE_SOURCE_ACTION": "open-unique",
                "MDKR_APP_SMOKE_PERFORMANCE_SOURCE_ACTION_TOKEN":
                    "mdkr64-app-performance-source-v1",
            })
            ambiguous_log = run(
                binary, install, ambiguous_environment,
                (
                    "character-performance-source-handoff "
                    "package=org.example.raw-install matches=2 selected=- "
                    "applied=0 ambiguity=unresolved",
                ),
            )
            if "raw-intake resumed=1" in ambiguous_log:
                raise RuntimeError(
                    "Performance source handoff guessed between ambiguous "
                    "raw drafts"
                )
            _, ambiguous_rows = raw_inventory(install)
            if (
                len(ambiguous_rows) != 2
                or hashlib.sha256(
                    (install / "characters" /
                     "org.example.raw-install.mdkc").read_bytes()
                ).hexdigest() != installed_before_handoff
            ):
                raise RuntimeError(
                    "ambiguous Performance source handoff changed authoring "
                    "or installed state"
                )

            accessible = root / "accessible"
            accessible.mkdir()
            (accessible / "video.ini").write_text(
                "[Accessibility]\nSpeech=1\n", encoding="utf-8"
            )
            accessible_shot = accessible / "raw-intake-a11y.bmp"
            accessible_environment = isolated_environment(
                accessible, model, accessible_shot, compact=False, drop=True
            )
            accessible_environment.update({
                "MDKR_APP_SMOKE_FRAMES": "220",
                "MDKR_APP_SMOKE_A11Y_WALK": "1",
                "MDKR_APP_SMOKE_INPUT": "keyboard",
                "MDKR_APP_SMOKE_INPUT_TOKEN": "mdkr64-app-ui-input-v1",
                "MDKR_A11Y_TRACE": "1",
            })
            run(
                binary, accessible, accessible_environment,
                ("text=Package ID", "text=SPDX license expression",
                 "text=Raw authoring draft",
                 "text=Duplicate as a new raw draft",
                 "text=Close raw editor",
                 "text=Gameplay donor",
                 "text=LOD profile",
                 "text=New LOD GLB destination",
                 "text=Create LOD copy and continue",
                 "text=Model faces +Z",
                 "text=Standing height in metres",
                 "text=Accept scale and facing proposal",
                 "text=Delete raw authoring draft"),
            )

            compact = root / "compact"
            compact.mkdir()
            compact_prefs = compact / "prefs"
            compact_prefs.mkdir()
            (compact_prefs / "mdkr64_app.ini").write_text(
                "ui_scale=2.00\n", encoding="utf-8"
            )
            compact_shot = compact / "raw-intake-compact.bmp"
            run(
                binary, compact,
                isolated_environment(
                    compact, model, compact_shot, compact=True, drop=True
                ),
                ("raw-intake resumed=1 inspected=1 mappings=1 drafts=1",
                 "raw-transform bounds=1 valid=1 severity=0 candidates=4 ",
                 "compact-layout dense=1 contained=1 overlap=0 "),
            )
            check_bmp(compact_shot, 640, 480)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"check_character_raw_intake_ui: FAIL -- {error}",
              file=sys.stderr)
        return 1
    print("check_character_raw_intake_ui: PASS -- canonical review-first "
          "data-only adapter handoff, bounded DAE/ZIP conversion, "
          "ZIP-bomb, invalid-SPDX, and hostile-GLB refusal, "
          "actionable missing-importer recovery, "
          "contextual journey actions, recipient compatibility/readiness "
          "review and confirmed no-mutation discard, "
          "mutation-free FBX/OBJ/BLEND/glTF/USD/DCC export guidance, "
          "multi-draft GLB intake, "
          "accessible no-overwrite LOD-copy authoring, "
          "same-source branching, source-bound mapping restore, exact "
          "switch/delete/install cleanup, "
          "legacy migration, corruption fail-closed behavior, source-byte "
          "purity, keyboard speech, and a contained safe-default 200% "
          "confirmation modal")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
