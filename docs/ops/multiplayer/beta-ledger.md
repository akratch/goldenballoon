# Seven-day $0 beta ledger

This ledger is the final operated-cost evidence contract for a private beta.
The validator can prove that seven supplied days are internally coherent; it
cannot create hosted traffic, provider records, human review or a production
`GO`. Never populate it with fixture data and present that as launch evidence.

## Exact format

The ledger is one JSON object capped at 512 KiB:

```json
{
  "schemaVersion": 2,
  "window": {
    "startDay": "2026-08-01",
    "endDay": "2026-08-07",
    "timeZone": "UTC"
  },
  "days": [
    {
      "day": "2026-08-01",
      "commit": "40-or-64-lowercase-hex-commit-digest",
      "providerDeploymentDigest": "64-lowercase-hex-sha256",
      "internal": {},
      "provider": {},
      "health": {},
      "experience": {
        "schemaVersion": 1,
        "source": "synthetic_canary_v1",
        "matchCreate": {"attempts": 20, "successes": 20, "p95Ms": 900},
        "matchJoin": {"attempts": 20, "successes": 20, "p95Ms": 1100},
        "phoneDirect": {
          "attempts": 20,
          "successes": 19,
          "setupP95Ms": 3500,
          "inputRttP95Ms": 90
        },
        "decision": "GO"
      },
      "localPlayAvailable": true,
      "incidentOpen": false,
      "decision": "GO"
    }
  ],
  "finalDecision": "GO"
}
```

`days` must contain exactly seven entries in contiguous UTC order. Each
`internal`, `provider` and `health` value is the exact approved aggregate from
the capacity, provider normalization and observability runbooks. The deployment
field is SHA-256 of the provider deployment id; do not place a raw account id,
token, URL or provider envelope in this file. Every day must record the exact
source commit, a real service-blocked local-play probe, no open incident and a
signed daily `GO` decision.

`experience` is a synthetic canary aggregate, never user analytics. Each UTC
day runs exactly 20 clean-profile MatchRoom creates, 20 role-link joins and 20
Phone Party direct-connection/input-test journeys. Raw timings, room values,
addresses and credentials are discarded after computing the fixed fields. A
qualified day requires at least 19/20 successful match creates, 19/20 joins and
18/20 direct phone connections; create/join p95 must each be at most 2,500 ms,
direct setup p95 at most 8,000 ms and direct input-test RTT p95 at most 250 ms.
The canary has no server-side collector, metrics route, per-user sampling or
additional metrics write. Its ordinary admitted operations appear in the same
capacity/provider aggregates and must remain inside the daily stop line.
The controlled runner refuses non-HTTPS origins (except an explicit loopback
development mode), requires exactly 20 attempts for operated evidence, uses
fresh browser profiles for every direct-phone attempt, closes its synthetic
rooms, writes one new aggregate file without overwrite, and prints no origin,
room, credential or raw timing.

```bash
python3 tools/run_party_experience_canary.py \
  --origin https://party.example.invalid \
  --output experience-YYYY-MM-DD.json
```

Insert the resulting exact object as that day’s `experience`. A nonzero exit or
`STOP` output disqualifies the day. `--development --allow-http-loopback` exists
only for local smoke testing; a non-20-attempt result cannot pass this ledger.

The JSON carries bounded machine evidence only. Store actor/reviewer identity,
UTC capture times, command output, provider source links and the final signed
human decision in the access-controlled evidence record beside it—not as
free-form fields that weaken this schema.

## Daily collection

For each day:

1. Run the local-play probe with DNS/API blocked and record its boolean result.
2. Through approved secret injection, capture capacity and health once; never
   capture request headers.
3. Export the same UTC day’s normalized provider/billing aggregate.
4. While the initial capacity level is `normal`, run the fixed synthetic canary
   from controlled clean profiles. Aggregate the exact v1 experience object in
   memory, erase raw journey material and require its decision to be `GO`.
5. Run the [reconciliation gate](reconciliation.md); require `PASS` after the
   canary so its complete metered cost is included.
6. Require health `tracking: complete`, `legacy: 0`, and exact tracked/admitted
   units. Close any incident before a daily `GO`—do not erase the incident
   record merely to qualify the day.
7. Add the exact aggregates and digests, then rerun the ledger validator.

At 50–74% utilization reconciliation reports `watch`; it remains valid only
with the increased signed checkpoint cadence from the capacity runbook. Any
75% stop line, refusal latch, charge, billing path, partial tracking, a missing
or under-sampled experience lane, missed success/latency bound, unavailable
local play, open incident, missing date or `STOP` decision invalidates the
window. Restart the seven contiguous qualified days after correction.

## Validate

```bash
python3 tools/validate_party_beta_ledger.py --ledger beta-ledger.json
```

A pass reports dates and bounded counts only. Archive that pass line and the
ledger with the signed evidence record. A `STOP code=...` is not overridable by
editing utilization, removing a dimension, changing a date or converting a
decision. Keep admission disabled and follow incident/rollback handling.

The local adversarial contract is:

```bash
python3 tests/test_party_beta_ledger.py
```

It covers schema injection, missing/noncontiguous days, window skew, invalid
digests, local-play failure, open incidents, charged provider days, incomplete
legacy tracking, weighted-unit drift, experience schema/source/sample/success/
latency failures, boolean-as-integer confusion, oversized input and
secret-canary non-reflection. Passing it is not a seven-day canary.

`tests/test_party_experience_canary.py` separately proves origin restrictions,
nearest-rank p95 calculation, exact threshold decisions, aggregate output and
exclusive evidence-file creation. The real local smoke exercises production
HTTP, WebSocket and WebRTC paths but intentionally cannot count as hosted data.
