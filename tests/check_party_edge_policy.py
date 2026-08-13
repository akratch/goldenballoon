#!/usr/bin/env python3
"""Validate the free edge-rate policy and its local-play isolation contract."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
POLICY = ROOT / "services/party/ops/free-rate-limit-rule.json"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    value = json.loads(POLICY.read_text(encoding="utf-8"))
    require(value.get("kind") == "zone" and value.get("phase") == "http_ratelimit",
            "edge policy is not a zone rate-limiting entry-point ruleset")
    rules = value.get("rules")
    require(isinstance(rules, list) and len(rules) == 1,
            "free-plan policy must consume exactly its one available rule")
    rule = rules[0]
    require(rule.get("enabled") is True and rule.get("action") == "block",
            "edge policy must be an enabled terminating block")
    require(rule.get("expression") ==
            'starts_with(http.request.uri.path, "/api/")',
            "edge policy must match only the dynamic /api/ prefix")
    limit = rule.get("ratelimit", {})
    require(limit == {"characteristics": ["cf.colo.id", "ip.src"],
                      "period": 10, "requests_per_period": 30,
                      "mitigation_timeout": 10},
            f"edge policy drifted from the reviewed free-plan shape: {limit}")

    dynamic = ["/api/party/create", "/api/match/join", "/api/ops/capacity",
               "/api/ops/health"]
    static = ["/", "/index.html", "/controller/", "/room/", "/sw.js",
              "/online/online-room-live-state.js",
              "/online/online-room-presenter.js", "/online/online-room.js"]
    matches = lambda path: path.startswith("/api/")
    require(all(matches(path) for path in dynamic),
            "a dynamic API route escaped the edge rule model")
    require(not any(matches(path) for path in static),
            "the edge rule can block launcher/local static recovery")
    serialized = json.dumps(value, sort_keys=True)
    require(not any(token in serialized.casefold() for token in
                    ("account_id", "zone_id", "api_token", "secret", "bearer")),
            "edge policy contains deployment identity or secret material")
    room_client = (ROOT / "dist/web/online/online-room.js").read_text(
        encoding="utf-8")
    require('response.status === 429 ? "rate_limited"' in room_client,
            "provider HTML 429 no longer maps to the typed capacity recovery")
    security = (ROOT / "services/party/src/security.ts").read_text(
        encoding="utf-8")
    worker = (ROOT / "services/party/src/worker.ts").read_text(
        encoding="utf-8")
    party_room = (ROOT / "services/party/src/party-room.ts").read_text(
        encoding="utf-8")
    require("export function validPartyOrigin" in security and
            'url.protocol === "https:"' in security and
            'url.protocol === "http:" && loopbackHostname' in security and
            "value === url.origin" in security and
            "if (!validPartyOrigin(env.PARTY_ORIGIN))" in worker and
            worker.index("if (!validPartyOrigin(env.PARTY_ORIGIN))") <
            worker.index("const url = new URL(request.url)") and
            "if (!validPartyOrigin(this.env.PARTY_ORIGIN))" in party_room,
            "Party origin must fail closed before routing or object access")

    print("check_party_edge_policy: PASS — one free /api-only 30/10s/IP rule; "
          "static local routes stay outside, origin is canonical TLS, and "
          "provider HTML 429 is typed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
