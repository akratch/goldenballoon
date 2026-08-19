#!/usr/bin/env python3
"""Pin the configure-time canonical-origin gate on MDKR_PARTY_ORIGIN.

CMakeLists.txt refuses to configure unless MDKR_PARTY_ORIGIN is empty or one
canonical HTTPS origin: `https://` + lowercase host + at most one explicit
port, with no path, query, fragment, userinfo or trailing slash. The value is
compiled into the launcher, interpolated into invite URLs and compared
byte-for-byte against the service's PARTY_ORIGIN, so anything else is a
misconfigured artifact that no compiler or unit test would otherwise notice
until a phone failed to pair.

This check replays the gate's own regexes -- extracted from the CMake source,
not reimplemented -- over an accept/reject matrix, so weakening any of them
is a suite failure, matching tests/check_ci_contract.py's regex-over-source
house style. It also requires the runtime twin
(mdkr_party_canonical_https_origin) to stay wired in the native host, so the
two layers cannot drift apart silently. A self-test proves the matrix still
rejects a weakened gate.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

REFUSED = [
    "http://party.example.invalid",
    "https:/party.example.invalid",
    "HTTPS://party.example.invalid",
    "https://party.example.invalid/",
    "https://party.example.invalid/controller",
    "https://party.example.invalid?admin=1",
    "https://party.example.invalid#fragment",
    "https://user@party.example.invalid",
    "https://party.example.invalid:@evil.example",
    "https://party.example.invalid:8443@evil.example",
    "https://Party.Example.Invalid",
    "https://party.example.invalid.",
    "https://party..example.invalid",
    "https://-party.example.invalid",
    "https://party.example.invalid-",
    "https://party.-example.invalid",
    "https://party.example.invalid:",
    "https://party.example.invalid:0",
    "https://party.example.invalid:08443",
    "https://party.example.invalid:65536",
    "https://party.example.invalid:999999999999999999",
    "https://party.example.invalid:8443:8443",
]
ACCEPTED = [
    "",
    "https://party.example.invalid",
    "https://party.example.invalid:8443",
    "https://party.example.invalid:65535",
    "https://a",
    "https://xn--bcher-kva.example",
    "https://192.0.2.7",
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def gate_block(cmake: str) -> str:
    start = cmake.find("if(MDKR_PARTY_ORIGIN)")
    require(start >= 0, "CMakeLists.txt lost the if(MDKR_PARTY_ORIGIN) gate")
    depth = 0
    for match in re.finditer(r"\b(if|endif)\s*\(", cmake[start:]):
        if match.group(1) == "if":
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                return cmake[start:start + match.end()]
    raise AssertionError("MDKR_PARTY_ORIGIN gate has no matching endif()")


def cmake_patterns(block: str) -> list[str]:
    quoted = re.findall(r'MATCHES\s*\n?\s*"((?:[^"\\]|\\.)*)"', block)
    # CMake string escapes: the file spells a regex backslash as `\\`.
    return [value.replace("\\\\", "\\") for value in quoted]


class Gate:
    """The configure-time decision, driven by the extracted regexes."""

    def __init__(self, cmake: str) -> None:
        block = gate_block(cmake)
        patterns = cmake_patterns(block)

        def one(predicate, name: str) -> str:
            found = [value for value in patterns if predicate(value)]
            require(len(found) == 1,
                    f"the gate no longer carries exactly one {name} regex:"
                    f" {found!r}")
            return found[0]

        self.scheme = one(lambda value: value == "^https://", "scheme")
        self.userinfo = one(lambda value: value == "@", "userinfo")
        self.pathish = one(lambda value: value == "[/?#]",
                           "path/query/fragment")
        self.long_port = one(lambda value: value.startswith(":[0-9]"),
                             "six-digit port")
        self.port_value = one(lambda value: value == ":([0-9]+)$",
                              "port capture")
        self.canonical = one(lambda value: value.startswith("^https://["),
                             "canonical host")
        self.labels = one(lambda value: "\\.\\." in value, "host label")
        require("GREATER 65535" in block,
                "the gate no longer bounds the port at 65535")
        require("FATAL_ERROR" in block,
                "the gate no longer fails the configure")

    def admits(self, origin: str) -> bool:
        if origin == "":
            return True
        if not re.search(self.scheme, origin):
            return False
        authority = re.sub("^https://", "", origin)
        if re.search(self.userinfo, authority):
            return False
        if re.search(self.pathish, authority):
            return False
        if re.search(self.long_port, authority):
            return False
        port = re.search(self.port_value, origin)
        if port and int(port.group(1)) > 65535:
            return False
        if not re.search(self.canonical, origin):
            return False
        return not re.search(self.labels, authority)


def run(cmake: str) -> None:
    gate = Gate(cmake)
    for origin in REFUSED:
        require(not gate.admits(origin),
                f"the configure gate admits a non-canonical origin: {origin!r}")
    for origin in ACCEPTED:
        require(gate.admits(origin),
                f"the configure gate refuses a canonical origin: {origin!r}")


def native_twin_wired() -> None:
    header = (ROOT / "platform/party/native_party_host.h").read_text(
        encoding="utf-8")
    host = (ROOT / "platform/party/native_party_host.cpp").read_text(
        encoding="utf-8")
    require("inline bool mdkr_party_canonical_https_origin" in header,
            "the runtime canonical-origin twin left native_party_host.h")
    require("!mdkr_party_canonical_https_origin(serviceOrigin)" in host,
            "MdkrNativePartyHost::open no longer applies the canonical"
            " origin gate")


def self_test(cmake: str) -> None:
    """Each named weakening must fail the matrix, or this check pins nothing."""
    weakened = cmake.replace('MATCHES "[/?#]"', 'MATCHES "[?#]"')
    require(weakened != cmake, "self-test could not weaken the path guard")
    try:
        run(weakened)
    except AssertionError:
        pass
    else:
        raise AssertionError(
            "self-test: a gate that admits a trailing slash passed")

    headless = cmake.replace("if(MDKR_PARTY_ORIGIN)", "if(_MDKR_NEVER)", 1)
    try:
        run(headless)
    except AssertionError:
        pass
    else:
        raise AssertionError("self-test: a CMakeLists without the gate passed")


def main() -> int:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    run(cmake)
    native_twin_wired()
    self_test(cmake)
    print("party cmake origin gate: canonical-origin matrix and native twin"
          " verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
