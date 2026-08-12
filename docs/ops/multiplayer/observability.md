# Privacy-safe service observability

The Party service observes only what an operator needs to preserve the $0
boundary and diagnose broad traffic shape. It has no per-room event stream,
analytics identity, free-form labels or raw application logs. Provider
observability remains disabled unless a separately reviewed aggregate allowlist
is approved.

## Reservation aggregate

`GET /api/ops/health` uses the same independent `OPS_READ_TOKEN` and denial
behavior as the capacity snapshot: missing configuration is `404`, wrong or
missing bearer is `401`, and every response is `no-store`. Query it only from
the secret-injected operations job. The exact daily response contains:

- `reservationRequests`: successful budget reservations, not users, rooms or
  successful downstream gameplay actions;
- `reservations`: fixed integer buckets for MatchRoom/Phone Party creation,
  link/code joins, control, rotation and socket lifetime reservations;
- `admitted` and `tracked`: weighted pairing/control units from the budget and
  independently reconstructed from the fixed buckets;
- `tracking`: `complete`, `partial` or `invalid`; and the UTC `day`.

The fixed weights are:

| Bucket | Kind | Units |
|---|---|---:|
| `matchCreate`, `partyCreate` | pairing | 10 |
| `matchLinkJoin`, `partyLinkJoin` | pairing | 2 |
| `matchCodeJoin`, `partyCodeJoin` | pairing | 3 |
| `matchControl`, `partyControl` | control | 2 |
| `matchRotate`, `partyRotate` | control | 10 |
| `matchSocket` | control | 4 |
| `partySocket` | control | 28 |

The budget object increments these buckets inside its existing accepted
reservation write. Metrics therefore add no per-request object call or storage
write. Refused work never increments them; refusal remains a first-occurrence
boolean latch so abuse cannot create write amplification.

An old unversioned Worker is accepted into the `legacy` bucket during the
documented expand/drain compatibility window. Protocol v1 rejects a missing,
unknown or wrong-kind/weight operation. `partial` is expected only while legacy
traffic is draining. Require `legacy: 0`, `tracking: complete`, and exact
`admitted == tracked` before promotion. `invalid` is always a stop.

## Alarm policy

At each signed checkpoint, read capacity and health once, then run the provider
[reconciliation gate](reconciliation.md). Stop new admission when:

- capacity reports `freeze`/`closed` or either refusal latch;
- health is `invalid`, is unexpectedly `partial`, or its UTC day differs;
- provider/internal reconciliation stops;
- a socket or rotation bucket departs materially from the reviewed canary
  traffic mix; or
- provider requests grow without a corresponding aggregate reservation shape.

Traffic-mix thresholds require hosted baseline evidence; do not invent them
from local fixtures. Archive only the approved aggregate responses and bounded
reconciliation line. Never add a room id, code, credential, address, name,
route string, arbitrary error text, SDP, input or packet field to improve a
diagnosis.

## Deliberate limits

These buckets measure admission pressure and cost shape, not availability,
latency, active-room population or player experience. Production user
analytics remain intentionally absent. The seven-day beta ledger separately
defines a fixed 20-attempt synthetic canary for MatchRoom create/join latency
and Phone Party direct-connect/input RTT. It has an exact aggregate-only schema,
ordinary admission cost, no server collector or metrics write, bounded
thresholds and adversarial validation. Raw canary journeys are erased after
aggregation. Future online-race direct-graph metrics remain gated with that
transport; diagnose an individual user journey from typed state and
reproducible fixtures, never by creating a server-side person/room trail.

## Executable evidence

```bash
(cd services/party && npm run check)
python3 tests/check_party_capacity.py --shell-dir dist/web
```

The unit contract rejects malformed operation labels/weights, proves the
legacy compatibility bucket and exact weighted reconstruction, and the real
Worker gate proves complete aggregates across a persisted restart, exhausted
admission, forged credentials, authenticated control/socket traffic and the
literal-zero kill switch. Hosted alarm baselines and incident drills remain
operational evidence.
