#!/usr/bin/env bash
# Deploy-time assertion that the controller page actually serves the reviewed
# security headers. dist/web/_headers is repository text that the static host
# interprets at deploy time; no Worker code path and no unit test notices when
# the live site stops sending it (a renamed file, a project migration, a
# provider change silently dropping the block), and /controller/ is exactly
# the surface a phone loads from a QR code. This script fetches the deployed
# /controller/ page and requires every header in the `_headers` block for
# `/controller/*` -- Content-Security-Policy first among them -- to arrive
# with the reviewed value, byte for byte.
#
# The origin comes from the environment only:
#   PARTY_DOMAIN  bare hostname serving the static pages and the /api/
#                 surface (tools/deploy_party.sh threads the same value);
#                 falls back to services/party/ops/production.env when unset.
#
# Exit codes:
#   0  every reviewed /controller/* header is live
#   1  the page responded but a header is missing or diverged
#   2  PARTY_DOMAIN unset or the origin unreachable -- an explicit refusal
#      to claim success, so a credential-less or not-yet-deployed run can
#      never report the headers as verified
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
HEADERS_FILE="$ROOT/dist/web/_headers"
[[ -f "$HEADERS_FILE" ]] || {
    echo "verify-controller-csp: FAIL -- reviewed header policy not found:" >&2
    echo "  $HEADERS_FILE" >&2
    exit 1
}

PARTY_DOMAIN="${PARTY_DOMAIN:-}"
if [[ -z "$PARTY_DOMAIN" && -f "$HERE/production.env" ]]; then
    # Same one-value resolution as tools/deploy_party.sh.
    PARTY_DOMAIN="$(sed -n 's/^[[:space:]]*PARTY_DOMAIN[[:space:]]*=[[:space:]]*//p' \
        "$HERE/production.env" | tail -1 | tr -d '"'"'"' \r')"
fi
if [[ -z "$PARTY_DOMAIN" ]]; then
    echo "verify-controller-csp: UNVERIFIED -- PARTY_DOMAIN is required" >&2
    echo "  (environment or services/party/ops/production.env) to assert the" >&2
    echo "  deployed controller page serves the reviewed CSP." >&2
    echo "  Exiting 2: this is a refusal to claim success, not a pass." >&2
    exit 2
fi

RESPONSE="$(curl --silent --show-error --max-time 30 \
    --dump-header - --output /dev/null \
    "https://$PARTY_DOMAIN/controller/")" || {
    echo "verify-controller-csp: UNVERIFIED -- https://$PARTY_DOMAIN/controller/" >&2
    echo "  was unreachable. Exiting 2: a not-yet-deployed or offline origin" >&2
    echo "  is a refusal to claim success, not a pass." >&2
    exit 2
}

RESPONSE="$RESPONSE" PARTY_DOMAIN="$PARTY_DOMAIN" \
    python3 - "$HEADERS_FILE" <<'PY'
import os
import sys

headers_path = sys.argv[1]
domain = os.environ["PARTY_DOMAIN"]

# The `/controller/*` block of dist/web/_headers: indented `Name: value`
# lines under the exact rule line, ended by the next un-indented line.
wanted: list[tuple[str, str]] = []
collecting = False
with open(headers_path, encoding="utf-8") as handle:
    for line in handle:
        if not line.strip():
            continue
        if not line[0].isspace():
            collecting = line.strip() == "/controller/*"
            continue
        if collecting:
            name, _, value = line.strip().partition(":")
            wanted.append((name.strip(), value.strip()))
if not wanted or "content-security-policy" not in {
        name.lower() for name, _ in wanted}:
    print("verify-controller-csp: FAIL -- dist/web/_headers no longer carries"
          " a /controller/* block with a Content-Security-Policy line.",
          file=sys.stderr)
    sys.exit(1)

lines = os.environ["RESPONSE"].replace("\r\n", "\n").strip().split("\n")
status_line = lines[0] if lines else ""
parts = status_line.split()
if len(parts) < 2 or parts[1] != "200":
    print("verify-controller-csp: FAIL -- expected HTTP 200 from"
          f" https://{domain}/controller/, got: {status_line!r}."
          " Something is live at the origin but it is not the deployed"
          " controller page.", file=sys.stderr)
    sys.exit(1)

live: dict[str, list[str]] = {}
for line in lines[1:]:
    name, separator, value = line.partition(":")
    if separator:
        live.setdefault(name.strip().lower(), []).append(value.strip())

problems = []
for name, value in wanted:
    arrived = live.get(name.lower(), [])
    if value in arrived:
        continue
    if arrived:
        problems.append(f"{name}: live {arrived!r} != reviewed {value!r}")
    else:
        problems.append(f"{name}: missing from the live response")
if problems:
    print("verify-controller-csp: FAIL -- the deployed /controller/ response"
          " diverges from the reviewed dist/web/_headers block:",
          file=sys.stderr)
    for item in problems:
        print(f"  - {item}", file=sys.stderr)
    sys.exit(1)

print("verify-controller-csp: PASS -- all"
      f" {len(wanted)} reviewed /controller/* headers (CSP included) are"
      f" live on https://{domain}/controller/.")
PY
