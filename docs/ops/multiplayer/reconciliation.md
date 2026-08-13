# $0 usage reconciliation

This gate reconciles the Party service's privacy-safe daily capacity snapshot
with a normalized provider aggregate. It is a release and daily-checkpoint
stop line, not proof that production exists. A local fixture pass proves the
contract; only a hosted export and a zero-currency billing record prove an
operated day.

## Safe inputs

Save the authenticated `GET /api/ops/capacity` response as `internal.json`.
Retrieve it only through the approved secret-injection job: never put the
bearer token in a command argument, shell history, evidence file or browser.
The exact accepted schema is:

```json
{
  "schemaVersion": 2,
  "day": "2026-08-12",
  "admitted": {"pairingUnits": 120, "controlUnits": 42},
  "refusalObserved": {"pairing": false, "control": false},
  "remaining": {"admissionUnits": 9880, "controlUnits": 19738},
  "admissionPercent": 1,
  "level": "normal"
}
```

Use approved provider tooling to normalize the same UTC day's service/account
aggregate as `provider.json`:

```json
{
  "schemaVersion": 1,
  "day": "2026-08-12",
  "plan": "free",
  "billing": {
    "methodAttached": false,
    "paidOveragesEnabled": false,
    "currencyCode": "USD",
    "chargeMicros": 0
  },
  "usage": {
    "workerRequests": 500,
    "durableObjectRequests": 450,
    "durableObjectDurationGbSeconds": 10,
    "rowsRead": 1000,
    "rowsWritten": 250,
    "storageBytes": 65536
  },
  "limits": {
    "workerRequests": 100000,
    "durableObjectRequests": 100000,
    "durableObjectDurationGbSeconds": 13000,
    "rowsRead": 5000000,
    "rowsWritten": 100000,
    "storageBytes": 5000000000
  }
}
```

Provider schema v2 adds `durableObjectDurationGbSeconds`. Normalize provider
duration upward to a whole GB-s; never round it down. This is a separate daily
Free-plan ceiling from Durable Object requests and is essential because an
object that cannot hibernate can exhaust duration while request volume remains
low. A v1 aggregate is intentionally rejected as incomplete evidence.

These files must contain aggregate numbers only. Never add an account id,
deployment token, request header, room/code/capability, address, player field,
raw log or provider response envelope. Unknown fields are rejected and input
values are never copied into failure output. Each input is capped at 64 KiB
before parsing.

The provider limits are reviewed Free-plan ceilings, not operator-tunable
claims. A lower provider-reported limit is accepted and tightens the gate. A
higher value stops as `unreviewed_provider_limit`; verify the official source
and change the reviewed ceiling in code before relying on it.

## Run the gate

```bash
python3 tools/reconcile_party_usage.py \
  --internal internal.json \
  --provider provider.json
```

A pass reports only the UTC day, aggregate units, highest provider utilization,
zero-charge state and `normal`/`watch`. Archive that single line with the two
approved aggregate files, actor, UTC time, commit and provider deployment id.
Delete working copies according to the evidence retention policy.

Pair this gate with the secret-gated
[reservation health aggregate](observability.md). Reconciliation proves Free
provider/billing headroom; health proves the internal admitted units have a
complete fixed operation shape. Neither substitutes for the other.

Stop promotion or new admission when any of these is true:

- the plan is not Free, a billing method or paid overage is enabled, or charge
  is nonzero;
- either internal refusal latch is set, or internal admission is at least 75%;
- any reviewed provider dimension is at least 75% or exceeds its reported
  limit;
- dates, schemas, arithmetic invariants or provider activity disagree.

Internal or provider utilization at 50% produces `watch`; it is still a pass,
but requires the signed checkpoint cadence in the capacity runbook. Every
failure is a bounded `STOP code=...` diagnostic. Do not override a stop by
editing an aggregate, increasing a limit, enabling billing, or omitting a
dimension. Disable new admission, preserve local play and follow the incident
and rollback runbooks.

## Executable contract proof

```bash
python3 tests/test_party_usage_reconciliation.py
```

The adversarial test covers unknown/private fields, billing and paid-overage
configuration, nonzero charge, stale arithmetic, refusal latches, clock skew,
provider-v1 downgrade, missing/boolean duration, provider exhaustion/activity
mismatch and secret-canary non-reflection. It
uses fixtures only; hosted reconciliation and the seven-day zero-currency
ledger remain operational evidence.
