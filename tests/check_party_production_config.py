#!/usr/bin/env python3
"""Hold the Party production Wrangler environment and its asset routing.

Two modes:

  * template mode (default) checks the tracked services/party/wrangler.jsonc.
    The single `party.example.invalid` placeholder is REQUIRED to still be
    there, because it is the fail-closed default that keeps an unconfigured
    checkout undeployable.
  * `--require-real-domain` checks a generated config that is about to be
    deployed. Any surviving placeholder/reserved host is a hard failure. This
    is the deploy-time gate tools/deploy_party.sh runs on the file it hands to
    `wrangler deploy`.

Both modes assert the production environment carries the same Durable Object
bindings, migrations and asset routing as the default (dev/test) config, and
that the controller and party-host static routes are reachable and unshadowed.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parent.parent
SERVICE = ROOT / "services/party"
PLACEHOLDER_HOST = "party.example.invalid"
# Reserved/special-use names that can never be a real deployed Party origin.
UNDEPLOYABLE_SUFFIXES = (".invalid", ".example", ".test", ".localhost",
                         ".local", ".workers.dev")
UNDEPLOYABLE_HOSTS = ("example.com", "example.net", "example.org", "localhost")


class CheckFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


def strip_jsonc(text: str) -> str:
    """Remove // and /* */ comments and trailing commas, string-aware.

    A naive comment strip would eat the `//` inside `https://…`, which is
    exactly the value this gate exists to police.
    """
    out: list[str] = []
    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        if character == '"':
            out.append(character)
            index += 1
            while index < length:
                out.append(text[index])
                if text[index] == "\\":
                    index += 1
                    if index < length:
                        out.append(text[index])
                        index += 1
                    continue
                if text[index] == '"':
                    index += 1
                    break
                index += 1
            continue
        if character == "/" and index + 1 < length and text[index + 1] == "/":
            while index < length and text[index] != "\n":
                index += 1
            continue
        if character == "/" and index + 1 < length and text[index + 1] == "*":
            index += 2
            while index + 1 < length and not (text[index] == "*" and
                                              text[index + 1] == "/"):
                index += 1
            index += 2
            continue
        out.append(character)
        index += 1
    stripped = "".join(out)
    result: list[str] = []
    index = 0
    length = len(stripped)
    while index < length:
        character = stripped[index]
        if character == '"':
            result.append(character)
            index += 1
            while index < length:
                result.append(stripped[index])
                if stripped[index] == "\\":
                    index += 1
                    if index < length:
                        result.append(stripped[index])
                        index += 1
                    continue
                if stripped[index] == '"':
                    index += 1
                    break
                index += 1
            continue
        if character == ",":
            look = index + 1
            while look < length and stripped[look] in " \t\r\n":
                look += 1
            if look < length and stripped[look] in "}]":
                index += 1
                continue
        result.append(character)
        index += 1
    return "".join(result)


def canonical_origin(value: Any) -> str:
    require(isinstance(value, str) and value, f"PARTY_ORIGIN is not a string: {value!r}")
    parts = urlsplit(value)
    require(parts.scheme == "https",
            f"PARTY_ORIGIN must be HTTPS for a deployed origin: {value}")
    require(parts.netloc and "@" not in parts.netloc and ":" not in parts.netloc,
            f"PARTY_ORIGIN must be a bare host with no port or credentials: {value}")
    require(parts.path == "" and not parts.query and not parts.fragment,
            f"PARTY_ORIGIN must have no path, query or fragment: {value}")
    require(value == f"https://{parts.netloc}",
            f"PARTY_ORIGIN must be exactly the canonical origin: {value}")
    require(not parts.netloc.endswith("."),
            f"PARTY_ORIGIN host must not be fully qualified with a trailing dot: {value}")
    return parts.netloc


def assert_asset_routing(assets: dict[str, Any], label: str) -> Path:
    require(assets.get("html_handling") == "auto-trailing-slash",
            f"{label} assets must resolve /controller -> /controller/ "
            f"(html_handling={assets.get('html_handling')!r})")
    require(assets.get("not_found_handling") == "404-page",
            f"{label} assets must serve the 404 page, not an SPA rewrite "
            f"(not_found_handling={assets.get('not_found_handling')!r})")
    require(assets.get("run_worker_first") == ["/api/*"],
            f"{label} must route only /api/* through the Worker; anything wider "
            f"shadows /controller/ and /party/: {assets.get('run_worker_first')!r}")
    directory = (SERVICE / str(assets.get("directory", ""))).resolve()
    require(directory == (ROOT / "dist/web").resolve(),
            f"{label} assets directory is not the reviewed dist/web: {directory}")
    return directory


def assert_static_surface(directory: Path) -> None:
    """The controller and party-host pages must actually exist and be whole."""
    for relative in ("index.html", "controller/index.html", "room/index.html",
                     "controller/controller.js", "controller/controller.css",
                     "party/party-host.js", "party/party-protocol.js",
                     "party/party-sas.js", "party/qrcodegen.js",
                     "party/party-host.css", "_headers"):
        require((directory / relative).is_file(),
                f"static asset route is missing {relative}")

    # Every local reference on the controller page must resolve on disk, or the
    # deployed /controller/ document loads a broken pad on a phone.
    controller = directory / "controller/index.html"
    text = controller.read_text(encoding="utf-8")
    references = [value.split('="', 1)[1].rstrip('"')
                  for value in re.findall(r'(?:href|src)="[^"]+"', text)]
    local = [value for value in references
             if not value.startswith(("#", "http:", "https:", "data:"))]
    require(local, "controller page references no local assets")
    for value in local:
        target = (controller.parent / value.split("?", 1)[0]).resolve()
        require(target.is_file(),
                f"controller page references a missing asset: {value}")
    require(any(value.endswith("controller.js") for value in local) and
            any(value.endswith("party-protocol.js") for value in local) and
            any(value.endswith("party-sas.js") for value in local),
            f"controller page no longer loads its pad scripts: {local}")

    # Nothing static may live under /api/, or an asset could shadow the Worker
    # surface (or be shadowed by it).
    require(not (directory / "api").exists(),
            "a static asset tree exists under /api/ and can shadow the Worker")

    headers = (directory / "_headers").read_text(encoding="utf-8")
    blocks: dict[str, str] = {}
    current = ""
    for line in headers.splitlines():
        if line.startswith("/"):
            current = line.strip()
            blocks[current] = ""
        elif current and line.strip():
            blocks[current] += line.strip() + "\n"
    require("/*" in blocks and "/controller/*" in blocks,
            f"_headers lost its root or controller block: {sorted(blocks)}")
    for name in ("/*", "/controller/*"):
        block = blocks[name]
        require("Content-Security-Policy:" in block and
                "default-src 'self'" in block and
                "frame-ancestors 'none'" in block,
                f"_headers {name} lost its content security policy")
        require("Permissions-Policy:" in block and "camera=()" in block and
                "microphone=()" in block,
                f"_headers {name} lost its Permissions-Policy")
        require("Strict-Transport-Security: max-age=" in block,
                f"_headers {name} lost HSTS; phones must stay on TLS")
        require("X-Content-Type-Options: nosniff" in block and
                "Referrer-Policy: no-referrer" in block,
                f"_headers {name} lost nosniff/no-referrer")
    require("Cache-Control: no-store" in blocks.get("/api/controller/*", ""),
            "_headers no longer marks controller credential responses no-store")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default=str(SERVICE / "wrangler.jsonc"),
                        help="Wrangler config to check")
    parser.add_argument("--require-real-domain", action="store_true",
                        help="deploy-time mode: fail closed on any placeholder")
    args = parser.parse_args()

    path = Path(args.config)
    require(path.is_file(), f"no Wrangler config at {path}")
    raw = path.read_text(encoding="utf-8")
    config = json.loads(strip_jsonc(raw))

    require(config.get("name") == "golden-balloon-party" and
            config.get("main") == "src/worker.ts",
            "default config lost its Worker identity")
    require(config.get("workers_dev") is False,
            "default config must not publish a workers.dev host")
    require(config.get("observability", {}).get("enabled") is False,
            "default config must keep observability off")
    default_assets = config.get("assets") or {}
    directory = assert_asset_routing(default_assets, "default config")
    assert_static_surface(directory)

    environments = config.get("env") or {}
    production = environments.get("production")
    require(isinstance(production, dict),
            "services/party/wrangler.jsonc has no env.production environment")

    require(production.get("workers_dev") is False,
            "production must not publish a workers.dev host")
    require(production.get("preview_urls") is False,
            "production must not publish preview URLs")
    require(production.get("observability", {}).get("enabled") is False,
            "production must keep observability off")
    require(production.get("limits") == config.get("limits"),
            "production CPU limit drifted from the reviewed default")
    require(production.get("name") == config.get("name"),
            "production Worker name drifted from the default config")

    # Wrangler does not inherit these into a named environment; they are
    # restated and must stay byte-identical to the reviewed defaults.
    require(production.get("durable_objects") == config.get("durable_objects"),
            "production Durable Object bindings differ from the default config")
    bindings = production["durable_objects"]["bindings"]
    require([item["name"] for item in bindings] ==
            ["PARTY_ROOMS", "PARTY_BUDGETS", "PARTY_CODES", "MATCH_ROOMS"],
            f"production binding census changed: {bindings}")
    require(production.get("migrations") == config.get("migrations") and
            [item["tag"] for item in production["migrations"]] ==
            ["v1", "v2", "v3"],
            "production migrations differ from the reviewed v1/v2/v3 ladder")
    require(production.get("assets") == default_assets,
            "production asset routing differs from the default config")
    assert_asset_routing(production["assets"], "production")

    routes = production.get("routes")
    require(isinstance(routes, list) and len(routes) == 1,
            f"production must bind exactly one custom-domain route: {routes}")
    route = routes[0]
    require(route.get("custom_domain") is True,
            "the production route must be a custom domain so TLS is provisioned "
            "for the host phones load the controller from")
    pattern = route.get("pattern")
    require(isinstance(pattern, str) and "/" not in pattern and
            "*" not in pattern and ":" not in pattern,
            f"the production route must be a bare hostname: {pattern!r}")

    variables = production.get("vars") or {}
    require(set(variables) == {"PARTY_ORIGIN", "MAX_ADMISSIONS_PER_DAY",
                               "CONTROL_RESERVE_PER_DAY"},
            f"production vars census changed: {sorted(variables)}")
    for name in ("MAX_ADMISSIONS_PER_DAY", "CONTROL_RESERVE_PER_DAY"):
        require(variables[name] == config["vars"][name],
                f"production {name} drifted from the reviewed capacity default")
    host = canonical_origin(variables["PARTY_ORIGIN"])
    require(host == pattern,
            f"PARTY_ORIGIN host {host!r} does not match the route {pattern!r}; "
            "the origin check would 403 every request")

    folded = host.casefold()
    undeployable = (folded == PLACEHOLDER_HOST or
                    folded in UNDEPLOYABLE_HOSTS or
                    folded.endswith(UNDEPLOYABLE_SUFFIXES))
    if args.require_real_domain:
        require(not undeployable,
                f"placeholder/reserved Party host is still present: {host} — set "
                "PARTY_DOMAIN in services/party/ops/production.env")
        require(PLACEHOLDER_HOST not in raw,
                f"{path} still contains the {PLACEHOLDER_HOST} placeholder")
        verdict = f"real domain {host}"
    else:
        require(folded == PLACEHOLDER_HOST,
                f"the tracked config must keep the fail-closed {PLACEHOLDER_HOST} "
                f"placeholder, found {host}")
        require(config["vars"]["PARTY_ORIGIN"] == variables["PARTY_ORIGIN"],
                "the default and production placeholder origins disagree")
        require(raw.count(PLACEHOLDER_HOST) == 4,
                "the placeholder host must appear exactly three times (comment, "
                f"default var, production route and production var): {raw.count(PLACEHOLDER_HOST)}")
        verdict = f"fail-closed placeholder {host}"

    print("check_party_production_config: PASS — env.production carries the "
          "custom-domain route, 4 DO bindings, v1/v2/v3 migrations, "
          f"workers_dev/observability off and {verdict}; /api/* alone runs the "
          "Worker and /controller/, /party/ and /room/ stay static")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CheckFailure as error:
        print(f"check_party_production_config: FAIL — {error}", file=sys.stderr)
        raise SystemExit(1)
