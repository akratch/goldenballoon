#!/usr/bin/env bash
# Deploy-time assertion that the reviewed edge rate-limit rule is actually
# live on the zone. The Worker-side per-address create throttle is the
# code-enforced availability control; this zone rule is the hand-applied edge
# layer in front of it, and a hand-applied rule is exactly the kind that
# silently is not there. This script asks the Cloudflare Rulesets API for the
# zone's http_ratelimit entry point and requires a rule matching
# free-rate-limit-rule.json: same ref, enabled, same expression, action and
# rate-limit parameters.
#
# Credentials come from the environment only:
#   CLOUDFLARE_API_TOKEN  least-privilege token able to read zone WAF rules
#   CLOUDFLARE_ZONE_ID    the zone serving PARTY_DOMAIN
#
# Exit codes:
#   0  the rule is live and matches the reviewed payload
#   1  the rule is missing, disabled, diverged, or the API call failed
#   2  credentials absent — an explicit refusal to claim success, so a
#      credential-less CI run can never report the rule as verified
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POLICY="$HERE/free-rate-limit-rule.json"
[[ -f "$POLICY" ]] || {
    echo "verify-edge-rate-limit: FAIL -- reviewed payload not found: $POLICY" >&2
    exit 1
}

if [[ -z "${CLOUDFLARE_API_TOKEN:-}" || -z "${CLOUDFLARE_ZONE_ID:-}" ]]; then
    echo "verify-edge-rate-limit: UNVERIFIED -- CLOUDFLARE_API_TOKEN and" >&2
    echo "  CLOUDFLARE_ZONE_ID are required to assert the zone rule exists." >&2
    echo "  Exiting 2: this is a refusal to claim success, not a pass." >&2
    exit 2
fi

RESPONSE="$(curl --silent --show-error --max-time 30 \
    --header "Authorization: Bearer $CLOUDFLARE_API_TOKEN" \
    "https://api.cloudflare.com/client/v4/zones/$CLOUDFLARE_ZONE_ID/rulesets/phases/http_ratelimit/entrypoint")" || {
    echo "verify-edge-rate-limit: FAIL -- the Cloudflare API was unreachable." >&2
    exit 1
}

RESPONSE="$RESPONSE" python3 - "$POLICY" <<'PY'
import json
import os
import sys

policy_path = sys.argv[1]
with open(policy_path, encoding="utf-8") as handle:
    wanted = json.load(handle)["rules"][0]

try:
    envelope = json.loads(os.environ["RESPONSE"])
except json.JSONDecodeError:
    print("verify-edge-rate-limit: FAIL -- the API response was not JSON.",
          file=sys.stderr)
    sys.exit(1)

if not envelope.get("success"):
    print("verify-edge-rate-limit: FAIL -- the API refused the request:",
          json.dumps(envelope.get("errors", []))[:512], file=sys.stderr)
    sys.exit(1)

rules = (envelope.get("result") or {}).get("rules") or []
live = [rule for rule in rules if rule.get("ref") == wanted["ref"]]
if not live:
    print("verify-edge-rate-limit: FAIL -- no rule with ref"
          f" {wanted['ref']!r} exists on the zone's http_ratelimit"
          " entry point. Apply services/party/ops/free-rate-limit-rule.json"
          " per docs/multiplayer/DEPLOY_PHONE_PARTY.md.", file=sys.stderr)
    sys.exit(1)

rule = live[0]
mismatches = []
if rule.get("enabled") is not True:
    mismatches.append("the rule is disabled")
for field in ("expression", "action"):
    if rule.get(field) != wanted[field]:
        mismatches.append(f"{field}: live {rule.get(field)!r}"
                          f" != reviewed {wanted[field]!r}")
live_limit = rule.get("ratelimit") or {}
wanted_limit = wanted["ratelimit"]
for field in ("period", "requests_per_period", "mitigation_timeout"):
    if live_limit.get(field) != wanted_limit[field]:
        mismatches.append(f"ratelimit.{field}: live {live_limit.get(field)!r}"
                          f" != reviewed {wanted_limit[field]!r}")
if sorted(live_limit.get("characteristics") or []) != \
        sorted(wanted_limit["characteristics"]):
    mismatches.append("ratelimit.characteristics diverged from the reviewed"
                      " payload")
if mismatches:
    print("verify-edge-rate-limit: FAIL -- the live rule diverges from the"
          " reviewed payload:", file=sys.stderr)
    for item in mismatches:
        print(f"  - {item}", file=sys.stderr)
    sys.exit(1)

print("verify-edge-rate-limit: PASS -- the reviewed /api/ rate-limit rule"
      f" ({wanted['ref']}) is live and enabled on the zone.")
PY
