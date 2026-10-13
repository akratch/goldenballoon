#!/usr/bin/env python3
"""Validate third-party notice coverage.

Deliberately narrow: it checks the specific vendored components whose licence
and provenance posture matters for this repository, and cross-references each
against THIRD_PARTY.md. A licence that only appears in THIRD_PARTY.md does not
satisfy a source-redistribution condition, so where the notice has to travel
with the file the check is made against the file itself.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check mdkr64 third-party notice coverage.")
    parser.add_argument("--repo-root", default=".", help="repository root")
    return parser.parse_args()


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def native_party_pin_problems(recipe: str, notices: dict[str, str]) -> list[str]:
    """Compare source notices with the authoritative recipe, not stale copies."""
    problems: list[str] = []
    for setting in ("MDKR_LIBDATACHANNEL_VERSION", "MDKR_LIBDATACHANNEL_COMMIT"):
        values = re.findall(r"set\(" + setting + r'\s+"([^"\n]+)"\s*\)', recipe)
        if len(values) != 1:
            problems.append(f"cmake/datachannel.cmake must define exactly one {setting}")
            continue
        value = values[0]
        value_pattern = r"(?<![0-9A-Za-z.])" + re.escape(value) + r"(?![0-9A-Za-z.])"
        for rel_path, text in notices.items():
            if re.search(value_pattern, text) is None:
                problems.append(f"{rel_path} does not identify current {setting}={value}")
    return problems


def main() -> int:
    args = parse_args()
    root = Path(args.repo_root).resolve()
    problems: list[str] = []

    # Required files and the notice text each must contain. Each entry is a
    # source-of-truth check: the file that actually carries the license, not
    # just a mention of it elsewhere.
    required_files = {
        "third_party/qrcodegen/qrcodegen.ts": (
            "Copyright (c) Project Nayuki. (MIT License)",
            "Permission is hereby granted, free of charge",
        ),
        "dist/web/party/qrcodegen.js": (
            "Copyright (c) Project Nayuki. (MIT License)",
            "Permission is hereby granted, free of charge",
        ),
        "third_party/qrcodegen/qrcodegen.hpp": (
            "Copyright (c) Project Nayuki. (MIT License)",
            "Permission is hereby granted, free of charge",
        ),
        "third_party/qrcodegen/qrcodegen.cpp": (
            "Copyright (c) Project Nayuki. (MIT License)",
            "Permission is hereby granted, free of charge",
        ),
        "third_party/native_phone_party/NOTICE.txt": (
            "443f6934d9007eb7076ab7825ba330f355fcbead",
            "3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6",
            "fec583d54493f879d2ae44a743423bf8a04371ab",
            "https://github.com/paullouisageneau/usrsctp/tree/fec583d54493f879d2ae44a743423bf8a04371ab",
            "55f93686c01528224f448c19128836e7df245f72",
            "94899e0b926ac1b0f4750bfbd495167b4a6ae9ef",
            "cmake/apply_libdatachannel_patch.cmake",
            "434ba641795184ec8af565ef37c097c76f64b84dc81b8570aad4aab523f2a5d0",
            "cmake/verify_datachannel_startup.cmake",
            "8f47cf1212a9fc7148757e3f9c951c7568f28f68ea51b8839ca1f4a963a88be2",
            "__PRETTY_FUNCTION__ to __func__",
            "cmake/patches/libdatachannel-cleanup-worker.patch",
            "platform/online/reserved_cleanup_worker.h",
            "eb697e5b4c0cf368c73d1fec010e0666438cab6be88935e979f17f60b3c3f2cf",
            "25b834a6e85361c7a92464a403bfbade7f8abd90bf2efa397960d0a645ca28bc",
            "cmake/patches/libdatachannel-initialization.patch",
            "8baa28ca33a2bda267f705f4421cd893dd477f10a621ff5b81b9415876f6d3b8",
            "cmake/patches/libdatachannel-startup-stages.patch",
            "6ca224f6e854f552c14f7a58c3a51ed0db51f2e08088c80065d6531bb5cbaee9",
            "platform/online/rtc_initialization_transaction.h",
            "3ca47672b3d592c3ab7fe173a607850bea36be01b22d7f7f188f26b28ec39063",
            "platform/online/startup_stage_ownership.h",
            "237215c96916d572fc35d3105e83cdb1e0998eb00e1cdb15777760d7a83ebb70",
            "cmake/patches/libdatachannel-work-admission.patch",
            "c5a510b2f7e1e11193a768ca1fda9536c3900bbeca5b04f4e54c147bbb883df7",
            "platform/online/queued_work_admission.h",
            "8b20dd49ccfc6cadf249ed431f2df186d94b49fdce85952a6970bee71be71a19",
            "cmake/patches/libdatachannel-prepared-work.patch",
            "1b68e4b688d1f7f9828fcac91ee191f55fec7666aa6fd20ba65958d14da31131",
            "platform/online/prepared_work_queue.h",
            "fe94925f249243a2b801ecc122059bea51418cc5269adaa0a27660268d100f4e",
            "cmake/patches/libdatachannel-transport-retirement.patch",
            "384ef834c198f2bdf641a8ceb88591ee1539b767b178671bad4b050f3cc17c24",
            "platform/online/transport_retirement.h",
            "554a003764072ebc70fb3bf606e5c99dbf8d4449c5225eb201d75b2e1b1f7af8",
            "seven libdatachannel patches",
            "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6",
            "Mozilla Public License Version 2.0",
            "Mbed TLS upstream dual-license file",
            "usrsctp BSD-3-Clause License",
            "Project Nayuki QR Code generator MIT License",
            "b47d81ee765b2eed51f75b9cb599522fdd6f9eda7511b025214c63cc4c3336a7",
        ),
        "third_party/character_importer/CPython-LICENSE.txt": (
            "PYTHON SOFTWARE FOUNDATION LICENSE VERSION 2",
            "A. HISTORY OF THE SOFTWARE",
            "ZERO-CLAUSE BSD LICENSE FOR CODE IN THE PYTHON DOCUMENTATION",
        ),
        "third_party/character_importer/PyInstaller-COPYING.txt": (
            "The PyInstaller licensing terms",
            "Bootloader Exception",
            "GNU GENERAL PUBLIC LICENSE",
        ),
        "third_party/gltf_validator/LICENSE.txt": (
            "Apache License", "Version 2.0, January 2004",
        ),
        "third_party/character_text/HarfBuzz-COPYING.txt": (
            "HarfBuzz is licensed under the so-called \"Old MIT\" license",
            "Permission is hereby granted",
        ),
        "third_party/gltf_validator/NOTICES.txt": (
            "Dart SDK", "args", "collection",
        ),
        "third_party/gltf_validator/README.md": (
            "2.0.0-dev.3.10", "Dart SDK 2.19.6", "pubspec.lock",
        ),
        "third_party/basisu/LICENSE.txt": (
            "Apache License", "Version 2.0, January 2004",
        ),
        "third_party/basisu/Zstd-LICENSE.txt": (
            "BSD License", "For Zstandard software",
        ),
        "third_party/basisu/README.md": (
            "4d6fc70eaf62ad0558e63e8d97eb9766118327a6",
            "MDKR_BASISU_LOCAL_CACHE", "No encoder",
        ),
        "third_party/meshoptimizer/LICENSE.md": (
            "Copyright (c) 2016-2026 Arseny Kapoulkine",
            "Permission is hereby granted, free of charge",
        ),
        "third_party/meshoptimizer/README.md": (
            "meshoptimizer v1.2",
            "9d9890c73011d75920af614485296d1e03e95448",
            "MDKR_MESHOPTIMIZER_LOCAL_CACHE",
        ),
        "cmake/patches/libdatachannel-windows-mbedtls-verify.patch": (
            "defined(_WIN32) && !USE_MBEDTLS",
            "TLS certificate verification with root CA is not supported on Windows",
        ),
        "cmake/patches/libdatachannel-cleanup-worker.patch": (
            "src/impl/init.cpp",
            "reserved_cleanup_worker.h",
            "cleanupWorker.arm();",
        ),
        "platform/online/reserved_cleanup_worker.h": (
            "class MdkrReservedCleanupWorker",
        ),
        "cmake/apply_libdatachannel_patch.cmake": (
            "PLOG_GET_FUNC()",
            "__PRETTY_FUNCTION__",
            "__func__",
            "all seven RTC patch paths",
            'mdkr_apply_checked_patch("${RETIREMENT_PATCH_FILE}")',
        ),
        "cmake/patches/libdatachannel-initialization.patch": (
            "src/impl/init.cpp", "src/impl/init.hpp", "mInitialization.admit",
        ),
        "cmake/patches/libdatachannel-startup-stages.patch": (
            "src/impl/pollservice.cpp", "src/impl/sctptransport.cpp",
            "startup_stage_ownership.h",
        ),
        "platform/online/rtc_initialization_transaction.h": (
            "class MdkrRtcInitializationTransaction",
        ),
        "platform/online/startup_stage_ownership.h": (
            "mdkrStartOwnedPolling", "mdkrStartOwnedRegistry",
        ),
        "cmake/verify_datachannel_startup.cmake": (
            "function(mdkr_verify_datachannel_startup source_dir)",
            "file(SHA256", "libdatachannel-startup-stages.patch",
            "libdatachannel-transport-retirement.patch",
            "init.cpp|", "init.hpp|",
            "threadpool.hpp|", "threadpool.cpp|", "processor.hpp|",
            "processor.cpp|", "queue.hpp|",
            "transport.hpp|", "transport.cpp|", "peerconnection.cpp|",
            "websocket.cpp|", "icetransport.cpp|", "icetransport.hpp|",
            "sctptransport.cpp|", "sctptransport.hpp|", "tcpserver.cpp|",
        ),
        "cmake/patches/libdatachannel-work-admission.patch": (
            "src/impl/threadpool.hpp", "src/impl/threadpool.cpp", "src/impl/queue.hpp",
            "takeCallable()", "mdkrCommitQueueInsertion",
        ),
        "platform/online/queued_work_admission.h": (
            "struct MdkrScheduledWork", "mdkrCommitQueueInsertion",
        ),
        "cmake/patches/libdatachannel-prepared-work.patch": (
            "src/impl/threadpool.hpp", "src/impl/threadpool.cpp",
            "src/impl/processor.hpp", "src/impl/processor.cpp",
            "enqueuePrepared", "MdkrNoAllocScopeExit",
        ),
        "platform/online/prepared_work_queue.h": (
            "class MdkrPreparedWorkQueue", "class MdkrNoAllocScopeExit",
        ),
        "cmake/patches/libdatachannel-transport-retirement.patch": (
            "src/impl/processor.hpp", "src/impl/transport.hpp",
            "src/impl/transport.cpp", "src/impl/peerconnection.cpp",
            "src/impl/websocket.cpp", "src/impl/icetransport.cpp",
            "src/impl/icetransport.hpp", "src/impl/sctptransport.cpp",
            "src/impl/sctptransport.hpp", "src/impl/tcpserver.cpp",
            "transport_retirement.h",
        ),
        "platform/online/transport_retirement.h": (
            "class MdkrTransportRetirement", "enqueuePrepared",
            "requestStop", "beginStart",
        ),
        "lib/glad/LICENSE": (
            "The glad source code",
            "Copyright (c) 2013-2021 David Herberth",
        ),
        "lib/glad/README.md": ("OpenGL function loader", "LICENSE"),
        "lib/sdl_gamecontrollerdb/LICENSE.txt": (
            "Sam Lantinga",
            "Permission is granted to anyone to use this software",
        ),
        "platform/fast3d/PROVENANCE.md": (
            "Copyright (c) 2020 Emill, MaikelChan",
            "modified BSD-2-Clause",
            "binary contains no assets you do not have the right to distribute",
        ),
    }

    for rel_path, needles in required_files.items():
        path = root / rel_path
        if not path.is_file():
            problems.append(f"missing third-party notice file: {rel_path}")
            continue
        text = read_text(path)
        for needle in needles:
            if needle not in text:
                problems.append(f"{rel_path} is missing expected notice text: {needle!r}")

    pinned_hashes = {
        "third_party/qrcodegen/qrcodegen.ts":
            "e332e4ab0c2530fdd5a412387c389385b51592701ae7b04abdd948bb44976fa0",
        "dist/web/party/qrcodegen.js":
            "79f419f267ce5a80d97f8099e0a789a4ecc4697b5348d86371a1f3b2a75d6e03",
        "third_party/qrcodegen/qrcodegen.hpp":
            "b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793",
        "third_party/qrcodegen/qrcodegen.cpp":
            "8948b57053deb5d132bfc675ca2688b7abef9f03ec633c0de59770c945a66fc9",
        "third_party/native_phone_party/NOTICE.txt":
            "dd753a65bb8de6de10a5bfc5de8ff6ae3690e333466c034ef4efbaa7ddb54a20",
        "third_party/character_importer/CPython-LICENSE.txt":
            "78b12c3a81360b357002334f0e70ea0e92eebf7a9b358805c03c48484945f3bb",
        "third_party/character_importer/PyInstaller-COPYING.txt":
            "dcf75fdb959db1e3b41c0f8505069d2ece781b5ec6b3d0a4d30975cfc6580245",
        "third_party/gltf_validator/LICENSE.txt":
            "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
        "third_party/character_text/HarfBuzz-COPYING.txt":
            "ba8f810f2455c2f08e2d56bb49b72f37fcf68f1f4fade38977cfd7372050ad64",
        "third_party/gltf_validator/NOTICES.txt":
            "d7a1cefe85110c1308632d0384b7a67a18c125193e54175c50d1982d8c81a2f4",
        "third_party/gltf_validator/pubspec.lock":
            "9fec69b760a6789506e1d03881e48c0b9024779559b67ba427a1b0309c841749",
        "third_party/basisu/LICENSE.txt":
            "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4",
        "third_party/basisu/Zstd-LICENSE.txt":
            "2c1a7fa704df8f3a606f6fc010b8b5aaebf403f3aeec339a12048f1ba7331a0b",
        "third_party/basisu/README.md":
            "5336f7e852dcc067ffaa0a12c3d36892d7fe7c09ddf74a035da6a2811d0d4cbf",
        "third_party/meshoptimizer/LICENSE.md":
            "f03037ca7bad1e3eb7f4a63fa6084a8baabd5ba30d3c239a9a7f35705d873e26",
        "cmake/patches/libdatachannel-windows-mbedtls-verify.patch":
            "b47d81ee765b2eed51f75b9cb599522fdd6f9eda7511b025214c63cc4c3336a7",
        "cmake/patches/libjuice-picohash-callbacks.patch":
            "dbc8fa9d906af33327d629b857914f1a4237ed7bd03ffdb68ef221755f859f0e",
        "cmake/patches/libdatachannel-cleanup-worker.patch":
            "eb697e5b4c0cf368c73d1fec010e0666438cab6be88935e979f17f60b3c3f2cf",
        "platform/online/reserved_cleanup_worker.h":
            "25b834a6e85361c7a92464a403bfbade7f8abd90bf2efa397960d0a645ca28bc",
        "cmake/apply_libdatachannel_patch.cmake":
            "434ba641795184ec8af565ef37c097c76f64b84dc81b8570aad4aab523f2a5d0",
        "cmake/verify_datachannel_startup.cmake":
            "8f47cf1212a9fc7148757e3f9c951c7568f28f68ea51b8839ca1f4a963a88be2",
        "cmake/patches/libdatachannel-initialization.patch":
            "8baa28ca33a2bda267f705f4421cd893dd477f10a621ff5b81b9415876f6d3b8",
        "cmake/patches/libdatachannel-startup-stages.patch":
            "6ca224f6e854f552c14f7a58c3a51ed0db51f2e08088c80065d6531bb5cbaee9",
        "platform/online/rtc_initialization_transaction.h":
            "3ca47672b3d592c3ab7fe173a607850bea36be01b22d7f7f188f26b28ec39063",
        "platform/online/startup_stage_ownership.h":
            "237215c96916d572fc35d3105e83cdb1e0998eb00e1cdb15777760d7a83ebb70",
        "cmake/patches/libdatachannel-work-admission.patch":
            "c5a510b2f7e1e11193a768ca1fda9536c3900bbeca5b04f4e54c147bbb883df7",
        "platform/online/queued_work_admission.h":
            "8b20dd49ccfc6cadf249ed431f2df186d94b49fdce85952a6970bee71be71a19",
        "cmake/patches/libdatachannel-prepared-work.patch":
            "1b68e4b688d1f7f9828fcac91ee191f55fec7666aa6fd20ba65958d14da31131",
        "platform/online/prepared_work_queue.h":
            "fe94925f249243a2b801ecc122059bea51418cc5269adaa0a27660268d100f4e",
        "cmake/patches/libdatachannel-transport-retirement.patch":
            "384ef834c198f2bdf641a8ceb88591ee1539b767b178671bad4b050f3cc17c24",
        "platform/online/transport_retirement.h":
            "554a003764072ebc70fb3bf606e5c99dbf8d4449c5225eb201d75b2e1b1f7af8",
    }
    for rel_path, expected in pinned_hashes.items():
        candidate = root / rel_path
        if candidate.is_file():
            actual = hashlib.sha256(candidate.read_bytes()).hexdigest()
            if actual != expected:
                problems.append(
                    f"{rel_path} SHA-256 changed: expected {expected}, got {actual}; "
                    "review upstream provenance and update the pin deliberately"
                )

    # The package validators deliberately pin redistributed notices too. A
    # reviewed source-notice update must reach every shipping platform, not
    # merely pass this source-tree guard and fail later during packaging.
    basisu_readme_hash = pinned_hashes["third_party/basisu/README.md"]
    phone_party_notice_hash = pinned_hashes["third_party/native_phone_party/NOTICE.txt"]
    for rel_path in (
        "macos/Scripts/verify_unsigned_release.sh",
        "tools/package_windows_zip.sh",
        "tools/package_linux_appimage.sh",
    ):
        path = root / rel_path
        if not path.is_file() or basisu_readme_hash not in read_text(path):
            problems.append(
                f"{rel_path} must pin the reviewed BasisU README SHA-256 "
                f"{basisu_readme_hash}"
            )
        if not path.is_file() or phone_party_notice_hash not in read_text(path):
            problems.append(
                f"{rel_path} must pin the reviewed native Phone Party notice SHA-256 "
                f"{phone_party_notice_hash}"
            )

    # The build recipe is authoritative for the reviewed top-level pin. A
    # dependency update must not leave all notices consistently describing an
    # older release while this guard accepts their old independent literals.
    recipe = root / "cmake/datachannel.cmake"
    if not recipe.is_file():
        problems.append("missing native Phone Party dependency recipe")
    else:
        notices = {}
        for rel_path in ("NOTICE.md", "THIRD_PARTY.md",
                         "third_party/native_phone_party/NOTICE.txt"):
            path = root / rel_path
            notices[rel_path] = read_text(path) if path.is_file() else ""
        problems.extend(native_party_pin_problems(read_text(recipe), notices))

    # mixer.c/.h must credit the perfect_dark origin in the file itself, not
    # just in THIRD_PARTY.md -- the source-level attribution is what a
    # downstream consumer sees if they only grab the file.
    for rel_path in ("platform/mixer.c", "platform/mixer.h"):
        path = root / rel_path
        if not path.is_file():
            problems.append(f"missing mixer source: {rel_path}")
            continue
        text = read_text(path)
        if "fgsfdsfgs/perfect_dark" not in text:
            problems.append(f"{rel_path} is missing its fgsfdsfgs/perfect_dark attribution")

    # Every n64-fast3d-engine-derived backend file must carry its own
    # attribution comment -- the license's redistribution condition is a
    # source-redistribution one, so the notice has to travel with the file.
    fast3d_attribution_files = {
        "platform/fast3d/gfx_opengl.c": "Emill/n64-fast3d-engine",
        "platform/fast3d/gfx_cc.c": "Emill/n64-fast3d-engine",
        "platform/fast3d/gfx_rendering_api.h": "Emill/n64-fast3d-engine",
    }
    for rel_path, needle in fast3d_attribution_files.items():
        path = root / rel_path
        if not path.is_file():
            problems.append(f"missing fast3d backend source: {rel_path}")
            continue
        text = read_text(path)
        if needle not in text:
            problems.append(f"{rel_path} is missing expected attribution text: {needle!r}")

    third_party = root / "THIRD_PARTY.md"
    third_party_text = ""
    if third_party.is_file():
        third_party_text = read_text(third_party)
        if phone_party_notice_hash not in third_party_text:
            problems.append("THIRD_PARTY.md does not identify the reviewed native Phone Party notice SHA-256")
        for needle in (
            "lib/glad/LICENSE",
            "lib/sdl_gamecontrollerdb",
            "fgsfdsfgs/perfect_dark",
            "Emill/n64-fast3d-engine",
            "mgb64",
            "SMAA",
            "github.com/iryoku/smaa",
            "wgpu-native",
            "MSYS2",
            "brand/appicon-source.png",
            "game/include/PR/",
            "docs/ref/",
            "third_party/qrcodegen/qrcodegen.ts",
            "third_party/qrcodegen/qrcodegen.hpp",
            "third_party/native_phone_party/NOTICE.txt",
            "third_party/character_importer/CPython-LICENSE.txt",
            "third_party/character_importer/PyInstaller-COPYING.txt",
            "tools/character_importer_build_requirements.txt",
            "third_party/gltf_validator/",
            "third_party/basisu/",
            "third_party/meshoptimizer/",
            "9d9890c73011d75920af614485296d1e03e95448",
            "4d6fc70eaf62ad0558e63e8d97eb9766118327a6",
            "Zstd-LICENSE.txt",
            "KhronosGroup/glTF-Validator",
            "bcd52cc4ba5f333b2999a58f67cc05ddf28b4fb1",
            "Dart SDK 2.19.6",
            "CPython",
            "PyInstaller",
            "cmake/patches/libdatachannel-windows-mbedtls-verify.patch",
            "2c9044de6b049ca25cb3cd1649ed7e27aa055138",
            "443f6934d9007eb7076ab7825ba330f355fcbead",
            "3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6",
            "fec583d54493f879d2ae44a743423bf8a04371ab",
            "55f93686c01528224f448c19128836e7df245f72",
            "94899e0b926ac1b0f4750bfbd495167b4a6ae9ef",
            "cmake/apply_libdatachannel_patch.cmake",
            "cmake/patches/libdatachannel-cleanup-worker.patch",
            "platform/online/reserved_cleanup_worker.h",
            "cmake/patches/libdatachannel-initialization.patch",
            "cmake/patches/libdatachannel-startup-stages.patch",
            "platform/online/rtc_initialization_transaction.h",
            "platform/online/startup_stage_ownership.h",
            "cmake/verify_datachannel_startup.cmake",
            "cmake/patches/libdatachannel-work-admission.patch",
            "platform/online/queued_work_admission.h",
            "cmake/patches/libdatachannel-prepared-work.patch",
            "platform/online/prepared_work_queue.h",
            "cmake/patches/libdatachannel-transport-retirement.patch",
            "platform/online/transport_retirement.h",
            "Mbed TLS",
        ):
            if needle not in third_party_text:
                problems.append(f"THIRD_PARTY.md is missing expected text: {needle!r}")
    else:
        problems.append("missing THIRD_PARTY.md")

    notice = root / "NOTICE.md"
    if notice.is_file():
        notice_text = read_text(notice)
        for needle in (
            "SMAA",
            "THIRD_PARTY.md",
            "game/include/PR/",
            "Project Nayuki",
            "third_party/native_phone_party/NOTICE.txt",
            "third_party/character_importer/",
            "CPython 3.13.13",
            "PyInstaller 6.22.2",
            "Khronos glTF Validator 2.0.0-dev.3.10",
            "third_party/gltf_validator/",
            "third_party/basisu/",
            "Basis Universal KTX2",
            "meshoptimizer v1.2",
            "third_party/meshoptimizer/",
        ):
            if needle not in notice_text:
                problems.append(f"NOTICE.md is missing expected text: {needle!r}")
    else:
        problems.append("missing NOTICE.md")

    # The SMAA lookup tables are carried as generated output; their generator
    # is not vendored here. Each header must therefore keep naming its upstream
    # so the attribution travels with the data, and must not point at a local
    # generator path that does not exist. This is a presence check on the
    # upstream reference, not an absence check on the generator.
    for rel_path in (
        "platform/fast3d/smaa_area_tex.h",
        "platform/fast3d/smaa_search_tex.h",
    ):
        path = root / rel_path
        if not path.is_file():
            problems.append(f"missing SMAA LUT header: {rel_path}")
            continue
        text = read_text(path)
        if "iryoku/smaa" not in text:
            problems.append(f"{rel_path} is missing its SMAA upstream attribution")
        if "tools/smaa" in text:
            problems.append(
                f"{rel_path} points at tools/smaa, which this repository does "
                "not contain -- name the upstream generator instead"
            )

    # None of these build tools is vendored here; THIRD_PARTY.md documents each
    # as living upstream rather than in this tree. If one is ever vendored in,
    # the THIRD_PARTY.md row describing it must land in the SAME change -- fail
    # loudly instead of letting the guard go silently stale.
    not_vendored_dirs = (
        "tools/smaa",
        "tools/mktex",
        "tools/asm-processor",
        "tools/gzipsrc",
        "tools/ido5.3_recomp",
    )
    for rel_dir in not_vendored_dirs:
        if (root / rel_dir).exists() and rel_dir not in third_party_text:
            problems.append(
                f"{rel_dir} now exists but THIRD_PARTY.md has no row describing "
                "it as vendored in this repository"
            )

    if problems:
        print("FAIL: third-party notice guard found issue(s):", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print("PASS: third-party notice guard passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
