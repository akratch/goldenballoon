#!/usr/bin/env python3
"""Assert that a packaged build actually ships the Phone Party host.

This is a static, GPU-free inspection of the shipped executable. It answers
the one thing a release lane can prove headlessly without a launcher seam:
that the real WSS/WebRTC party transport and the host's fail-closed pairing
policy were compiled and linked into the artifact players will download, and
(when asked) that the artifact carries the production service origin it was
supposed to be built with.

It is deliberately NOT a claim that the launcher draws a QR code, that a
bootstrap listener is up, or that a phone can join. Those need either a
launcher env seam (see the party-ui-notes request) or a real device.

Usage:
    check_party_binary_surface.py <executable-or-.app> [--expect-origin URL]
                                 [--allow-stub]
    check_party_binary_surface.py --self-test

``--self-test`` takes no artifact: it proves this validator actually fails on a
build that dropped the party host, the transport, or the expected origin, so a
release lane cannot pass it vacuously.
"""

from __future__ import annotations

import argparse
import io
import pathlib
import plistlib
import sys
import tempfile

# Present only when platform/party/libdatachannel_party_transport.cpp is in
# the link: the signaling paths the native host dials on the Party service.
REAL_TRANSPORT_MARKERS = (
    b"/api/party/native-create",
    b"/api/party/",
    b"/connect",
)

# Present whenever platform/party/native_party_host.cpp is in the link: the
# host's secure-origin refusal and the invite surface it drives.
HOST_MARKERS = (
    b"Phone controllers require the configured secure Party service.",
    b"Scan to add a phone controller.",
    b"The invite is closed. Connected phones keep their seats.",
)


def resolve_executable(target: pathlib.Path) -> pathlib.Path:
    """Accept a bare executable or a macOS .app bundle."""
    if target.is_file():
        return target
    if target.is_dir() and target.suffix == ".app":
        info = target / "Contents" / "Info.plist"
        if info.is_file():
            with info.open("rb") as handle:
                name = plistlib.load(handle).get("CFBundleExecutable")
            if name:
                candidate = target / "Contents" / "MacOS" / name
                if candidate.is_file():
                    return candidate
        macos = target / "Contents" / "MacOS"
        binaries = sorted(p for p in macos.glob("*") if p.is_file())
        if len(binaries) == 1:
            return binaries[0]
        raise SystemExit(
            f"could not resolve a single main executable inside {target}")
    raise SystemExit(f"not an executable or .app bundle: {target}")


def inspect(
    executable: pathlib.Path,
    data: bytes,
    expect_origin: str | None,
    allow_stub: bool,
) -> str:
    """Return the detected transport flavour, or raise SystemExit."""
    missing = [marker for marker in HOST_MARKERS if marker not in data]
    if missing:
        for marker in missing:
            print(f"missing party host marker: {marker!r}", file=sys.stderr)
        raise SystemExit(
            f"{executable} does not link the native Phone Party host")

    transport_missing = [
        marker for marker in REAL_TRANSPORT_MARKERS if marker not in data]
    if transport_missing and not allow_stub:
        for marker in transport_missing:
            print(f"missing party transport marker: {marker!r}",
                  file=sys.stderr)
        raise SystemExit(
            f"{executable} was built without the native WSS/WebRTC party "
            "transport (MDKR_NATIVE_PHONE_PARTY=OFF?)")

    if expect_origin:
        if not expect_origin.startswith("https://"):
            raise SystemExit("--expect-origin must begin with https://")
        if expect_origin.encode() not in data:
            raise SystemExit(
                f"{executable} does not embed the expected party origin "
                f"{expect_origin}")
        print(f"party origin embedded: {expect_origin}")

    return "stub" if transport_missing else "native-wss-webrtc"


def self_test() -> int:
    """Prove each rejection path actually rejects."""
    origin = "https://party.self-test.invalid"
    complete = (b"\x00padding" + b"".join(HOST_MARKERS) +
                b"".join(REAL_TRANSPORT_MARKERS) + origin.encode() +
                b"trailing\x00")

    def expect_reject(label: str, data: bytes, **kwargs: object) -> None:
        saved = sys.stderr
        sys.stderr = io.StringIO()  # the rejection diagnostics are expected
        try:
            inspect(pathlib.Path("<self-test>"), data,
                    kwargs.get("expect_origin"),  # type: ignore[arg-type]
                    bool(kwargs.get("allow_stub")))
        except SystemExit:
            return
        finally:
            sys.stderr = saved
        raise SystemExit(f"self-test: {label} was accepted but must not be")

    flavour = inspect(pathlib.Path("<self-test>"), complete, origin, False)
    if flavour != "native-wss-webrtc":
        raise SystemExit("self-test: complete fixture misdetected as " + flavour)

    for marker in HOST_MARKERS:
        expect_reject(f"host marker {marker!r} dropped",
                      complete.replace(marker, b""))
    for marker in REAL_TRANSPORT_MARKERS:
        expect_reject(f"transport marker {marker!r} dropped",
                      complete.replace(marker, b""))
    expect_reject("wrong origin", complete,
                  expect_origin="https://party.wrong.invalid")
    expect_reject("insecure expected origin", complete,
                  expect_origin="http://party.self-test.invalid")

    # A stub build is refused by default and accepted only when asked.
    stub = complete
    for marker in REAL_TRANSPORT_MARKERS:
        stub = stub.replace(marker, b"")
    expect_reject("stub transport without --allow-stub", stub)
    if inspect(pathlib.Path("<self-test>"), stub, None, True) != "stub":
        raise SystemExit("self-test: stub build was not reported as a stub")

    # The .app resolver must find the bundle's main executable.
    with tempfile.TemporaryDirectory() as tmp:
        bundle = pathlib.Path(tmp) / "SelfTest.app"
        (bundle / "Contents" / "MacOS").mkdir(parents=True)
        (bundle / "Contents" / "MacOS" / "selftest").write_bytes(complete)
        with (bundle / "Contents" / "Info.plist").open("wb") as handle:
            plistlib.dump({"CFBundleExecutable": "selftest"}, handle)
        resolved = resolve_executable(bundle)
        if resolved.read_bytes() != complete:
            raise SystemExit("self-test: .app resolver found the wrong file")

    print("party binary surface validator self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("target", type=pathlib.Path, nargs="?")
    parser.add_argument(
        "--expect-origin",
        help="HTTPS origin that must be compiled into the artifact "
             "(MDKR_PARTY_ORIGIN)")
    parser.add_argument(
        "--allow-stub", action="store_true",
        help="accept a build without the native WebRTC transport "
             "(MDKR_NATIVE_PHONE_PARTY=OFF)")
    parser.add_argument(
        "--self-test", action="store_true",
        help="validate this checker against synthetic fixtures and exit")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.target is None:
        parser.error("an executable or .app bundle is required")

    executable = resolve_executable(args.target)
    transport = inspect(
        executable, executable.read_bytes(), args.expect_origin,
        args.allow_stub)
    print(f"party host linked: {executable} transport={transport} "
          f"markers={len(HOST_MARKERS) + len(REAL_TRANSPORT_MARKERS)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
