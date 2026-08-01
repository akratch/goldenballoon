#!/usr/bin/env python3
"""Fail closed if the repository correctness workflow loses required coverage."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "correctness.yml"
MACOS_WORKFLOW = ROOT / ".github" / "workflows" / "macos-release.yml"
MACOS_BUILDER = ROOT / "macos" / "Scripts" / "build_app_bundle.sh"
MACOS_SIGNER = ROOT / "macos" / "Scripts" / "sign_and_notarize.sh"
MACOS_NOTARY = ROOT / "macos" / "Scripts" / "notarize_artifact.sh"
MACOS_DMG = ROOT / "macos" / "Scripts" / "create_dmg.sh"
MACOS_ENTITLEMENTS = ROOT / "macos" / "Resources" / "Entitlements.plist"
PINNED_ACTION_RE = re.compile(r"^\s*-\s+uses:\s+([^@\s]+)@([^\s#]+)", re.MULTILINE)


def validate(source: str) -> list[str]:
    required = {
        "push trigger": "\n  push:",
        "pull-request trigger": "\n  pull_request:",
        "manual trigger": "\n  workflow_dispatch:",
        "read-only permissions": "\npermissions:\n  contents: read",
        "full-history clean-room checkout": "fetch-depth: 0",
        "clean-room history gate": "tools/check_clean_room.sh",
        "native SDK surface gate": "tools/check_native_sdk_surface.py",
        "third-party notice gate": "tools/check_third_party_notices.py",
        "image metadata gate": "tools/check_image_metadata.py",
        "address-domain gate": "tests/check_address_domains.py",
        "Linux WebGPU cell": "label: Linux WebGPU",
        "Linux OpenGL cell": "label: Linux OpenGL-only",
        "explicit Linux GCC compiler": "compiler: gcc",
        "macOS WebGPU cell": "label: macOS WebGPU warnings",
        "matrix compiler selection": "CC: ${{ matrix.compiler }}",
        "warning register": "-Wsometimes-uninitialized",
        "ROM-free CTest": "ctest --test-dir build-ci --output-on-failure",
        "ASan+UBSan": "-fsanitize=address,undefined",
        "linked wasm build": "tools/web/build_web.sh",
        "linked wasm layout": "tests/check_wave_visible_table.py",
        "browser save custody": "tests/check_browser_save_ui.py",
        "artifact ROM guard": "tools/check_no_rom.sh dist/web",
    }
    failures = [
        f"missing {label}: {needle!r}"
        for label, needle in required.items()
        if needle not in source
    ]
    actions = PINNED_ACTION_RE.findall(source)
    if not actions:
        failures.append("workflow has no actions")
    for action, revision in actions:
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            failures.append(
                f"{action} is not pinned to an immutable 40-character SHA: "
                f"{revision!r}"
            )
    forbidden = {
        "write-all permissions": "write-all",
        "repository write permission": "contents: write",
        "ROM secret/input": "MDKR_ROM",
        "untrusted pull-request target": "pull_request_target",
    }
    failures.extend(
        f"forbidden {label}: {needle!r}"
        for label, needle in forbidden.items()
        if needle in source
    )
    return failures


def validate_macos_release(source: str) -> list[str]:
    required = {
        "manual trigger": "\n  workflow_dispatch:",
        "read-only default permission": "\npermissions:\n  contents: read",
        "protected release environment": "environment: macos-release",
        "serialized release runs": "group: macos-release",
        "self-contained SDL bundle": "--bundle-sdl2",
        "certificate secret": "MACOS_CERTIFICATE_P12_BASE64",
        "certificate password secret": "MACOS_CERTIFICATE_PASSWORD",
        "Developer ID identity variable": "DEVELOPER_ID_APPLICATION",
        "notary API private key": "APPLE_API_PRIVATE_KEY_BASE64",
        "notary API key ID": "APPLE_API_KEY_ID",
        "notary API issuer": "APPLE_API_ISSUER_ID",
        "temporary keychain cleanup": "security delete-keychain",
        "inside-out app signing": "sign_and_notarize.sh",
        "distribution bundle verification": "--distribution dist/mdkr64.app",
        "DMG packaging": "create_dmg.sh",
        "DMG signing": "codesign --force --sign",
        "DMG notarization": "notarize_artifact.sh \"$DMG_PATH\"",
        "DMG integrity": "hdiutil verify \"$DMG_PATH\"",
        "asset-free gate": "verify_asset_free.sh dist/mdkr64.app",
        "artifact upload": "actions/upload-artifact@",
        "isolated publish dependency": "needs: package",
        "signed artifact download": "actions/download-artifact@",
        "publish-only write permission": "\n    permissions:\n      contents: write",
        "publish provenance validation": '"provenance sha256 mismatch"',
    }
    failures = [
        f"missing macOS release {label}: {needle!r}"
        for label, needle in required.items()
        if needle not in source
    ]
    for action, revision in PINNED_ACTION_RE.findall(source):
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            failures.append(
                f"macOS release {action} is not pinned to an immutable SHA: "
                f"{revision!r}"
            )
    forbidden = {
        "automatic push trigger": "\n  push:",
        "automatic pull-request trigger": "\n  pull_request:",
        "notarization bypass": "--skip-notarize",
        "Gatekeeper bypass": "--allow-spctl-fail",
    }
    failures.extend(
        f"forbidden macOS release {label}: {needle!r}"
        for label, needle in forbidden.items()
        if needle in source
    )
    if "\n  package:" in source and "\n  publish:" in source:
        package_source = source.split("\n  package:", 1)[1].split(
            "\n  publish:", 1
        )[0]
        if "contents: write" in package_source:
            failures.append(
                "macOS package job combines Apple credentials with contents: write"
            )
    else:
        failures.append("macOS release must isolate package and publish jobs")
    return failures


def validate_macos_packaging(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    builder = sources["builder"]
    signer = sources["signer"]
    notary = sources["notary"]
    dmg = sources["dmg"]
    entitlements = sources["entitlements"]
    required = {
        "builder clears inherited xattrs": (builder, 'xattr -cr "${OUTPUT_APP}"'),
        "builder signs nested SDL": (builder, 'codesign --force --sign - "${SDL2_BUNDLED_PATH}"'),
        "builder seals outer app": (builder, 'codesign --force --sign - "${OUTPUT_APP}"'),
        "builder verifies final bundle": (builder, "verify_gatekeeper_bundle.sh"),
        "signer enumerates nested dylibs": (signer, "find \"${FRAMEWORKS_DIR}\" -depth -type f -name '*.dylib'"),
        "signer requires Developer ID certificate type": (signer, '"Developer ID Application: "*'),
        "signer enables Hardened Runtime": (signer, "--options runtime --timestamp"),
        "signer delegates notarization": (signer, 'notarize_artifact.sh" "${APP_PATH}"'),
        "notary requires exact acceptance": (notary, '"${STATUS}" != "Accepted"'),
        "notary staples": (notary, 'xcrun stapler staple "${TARGET_PATH}"'),
        "notary validates staple": (notary, 'xcrun stapler validate "${TARGET_PATH}"'),
        "DMG verifies checksum": (dmg, 'hdiutil verify "${OUTPUT_DMG}"'),
        "DMG mounts read-only": (dmg, "hdiutil attach -readonly -nobrowse"),
        "DMG re-verifies packaged app": (dmg, 'verify_gatekeeper_bundle.sh" "${PACKAGED_APP}"'),
    }
    failures.extend(
        f"missing macOS packaging {label}: {needle!r}"
        for label, (source, needle) in required.items()
        if needle not in source
    )
    ordered = [
        builder.find("install_name_tool -change"),
        builder.find('codesign --force --sign - "${SDL2_BUNDLED_PATH}"'),
        builder.find('codesign --force --sign - "${OUTPUT_APP}"'),
        builder.find("verify_gatekeeper_bundle.sh"),
    ]
    if any(index < 0 for index in ordered) or ordered != sorted(ordered):
        failures.append(
            "macOS builder no longer mutates, signs nested code, seals, and "
            "verifies in that order"
        )
    for entitlement in (
        "com.apple.security.cs.disable-library-validation",
        "com.apple.security.cs.allow-unsigned-executable-memory",
    ):
        if entitlement in entitlements:
            failures.append(
                f"forbidden high-risk macOS entitlement: {entitlement}"
            )
    return failures


def main() -> int:
    source = WORKFLOW.read_text(encoding="utf-8")
    failures = validate(source)
    macos_source = MACOS_WORKFLOW.read_text(encoding="utf-8")
    failures.extend(validate_macos_release(macos_source))
    packaging_sources = {
        "builder": MACOS_BUILDER.read_text(encoding="utf-8"),
        "signer": MACOS_SIGNER.read_text(encoding="utf-8"),
        "notary": MACOS_NOTARY.read_text(encoding="utf-8"),
        "dmg": MACOS_DMG.read_text(encoding="utf-8"),
        "entitlements": MACOS_ENTITLEMENTS.read_text(encoding="utf-8"),
    }
    failures.extend(validate_macos_packaging(packaging_sources))
    if failures:
        raise AssertionError("CI contract drift:\n  " + "\n  ".join(failures))

    # A gate that cannot reject regressions is documentation, not a control.
    controls = {
        "trigger": source.replace("\n  pull_request:", "\n  removed:", 1),
        "matrix": source.replace("label: Linux WebGPU", "label: removed", 1),
        "compiler": source.replace("compiler: gcc", "compiler: removed"),
        "action pin": re.sub(
            r"(@)[0-9a-f]{40}", r"\1v4", source, count=1
        ),
        "ROM guard": source.replace("tools/check_no_rom.sh dist/web", "true", 1),
        "native SDK surface guard": source.replace(
            "tools/check_native_sdk_surface.py", "true", 1
        ),
        "third-party notice guard": source.replace(
            "tools/check_third_party_notices.py", "true", 1
        ),
        "image metadata guard": source.replace(
            "tools/check_image_metadata.py", "true", 1
        ),
    }
    escaped = [name for name, broken in controls.items() if not validate(broken)]
    if escaped:
        raise AssertionError(
            "CI positive controls unexpectedly passed: " + ", ".join(escaped)
        )

    macos_controls = {
        "read-only signing job": macos_source.replace(
            "\npermissions:\n  contents: read",
            "\npermissions:\n  contents: write",
            1,
        ),
        "certificate": macos_source.replace(
            "MACOS_CERTIFICATE_P12_BASE64", "REMOVED_CERTIFICATE"
        ),
        "notary API": macos_source.replace(
            "APPLE_API_PRIVATE_KEY_BASE64", "REMOVED_NOTARY_KEY"
        ),
        "app signing": macos_source.replace(
            "sign_and_notarize.sh", "removed-signing.sh", 1
        ),
        "DMG notarization": macos_source.replace(
            'notarize_artifact.sh "$DMG_PATH"', "true", 1
        ),
        "Gatekeeper verification": macos_source.replace(
            "--distribution dist/mdkr64.app", "dist/mdkr64.app", 1
        ),
        "publish isolation": macos_source.replace(
            "\n  publish:", "\n  removed-publish:", 1
        ),
    }
    macos_escaped = [
        name for name, broken in macos_controls.items()
        if not validate_macos_release(broken)
    ]
    if macos_escaped:
        raise AssertionError(
            "macOS CI positive controls unexpectedly passed: "
            + ", ".join(macos_escaped)
        )

    packaging_controls = {
        "Developer ID certificate type": {
            **packaging_sources,
            "signer": packaging_sources["signer"].replace(
                '"Developer ID Application: "*', '"Apple Development: "*', 1
            ),
        },
        "outer integrity seal": {
            **packaging_sources,
            "builder": packaging_sources["builder"].replace(
                'codesign --force --sign - "${OUTPUT_APP}"', "true", 1
            ),
        },
        "exact notary acceptance": {
            **packaging_sources,
            "notary": packaging_sources["notary"].replace(
                '"${STATUS}" != "Accepted"', '"${STATUS}" != "Anything"', 1
            ),
        },
        "mounted DMG verification": {
            **packaging_sources,
            "dmg": packaging_sources["dmg"].replace(
                "hdiutil attach -readonly -nobrowse", "true", 1
            ),
        },
        "library-validation protection": {
            **packaging_sources,
            "entitlements": packaging_sources["entitlements"].replace(
                "<dict>",
                "<dict><key>com.apple.security.cs.disable-library-validation</key>",
                1,
            ),
        },
    }
    packaging_escaped = [
        name for name, broken in packaging_controls.items()
        if not validate_macos_packaging(broken)
    ]
    if packaging_escaped:
        raise AssertionError(
            "macOS packaging positive controls unexpectedly passed: "
            + ", ".join(packaging_escaped)
        )

    action_count = len(PINNED_ACTION_RE.findall(source))
    macos_action_count = len(PINNED_ACTION_RE.findall(macos_source))
    print(
        "check_ci_contract: PASS — push/PR/manual policy, "
        "3-cell native matrix, sanitizer lane, linked wasm/save custody, "
        "ROM guards, fail-closed Developer ID/notarization/DMG release, and "
        f"{action_count + macos_action_count} immutable action pins"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
