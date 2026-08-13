# $0 capacity and admission runbook

“Zero cost” means hard refusal before paid operation, not unlimited service.
Keyboard/gamepad/touch local play is never admitted or metered.

## Accounting model

`PartyBudget` counts conservative **external-operation units**, including its
own admission request. Current worst-case reservations are:

| Operation | Units | Rationale |
|---|---:|---|
| Create room | 10 | Budget + up to 8 fallback-code collision probes + initialize. |
| QR redeem | 2 | Budget + room mutation. |
| Code redeem | 3 | Budget + directory resolve + room mutation. |
| Approve/reject/remove/revoke/close | 2 | Budget + room mutation, for HTTP and each authenticated native host command. Invalid/stale native commands are rejected before admission. |
| Rotate invite | 10 | Budget + up to 8 hashed-code registrations + one room mutation, for HTTP and each authenticated native rotation. Registration happens first, so a collision cannot rotate the room to an unreachable code. |
| Create MatchRoom | 10 | Budget + up to 8 hashed-code collision probes + initialize. |
| Match link/code join | 2 / 3 | Budget + room mutation, with directory lookup for code. |
| Match state/command | 2 | Budget + one authenticated room operation. |
| Rotate MatchRoom invite | 10 | Budget + up to 8 hashed-code registrations + one leader-authenticated generation-CAS room mutation. |
| Phone Party signaling socket | 28 | Budget + room upgrade + the 26-request equivalent of at most 512 incoming signaling messages. A 120/10-second burst cap also applies. |
| Native Phone Party create + socket | 10 pairing + 28 control | Both reservations succeed before room creation, so a closed control line cannot leave an unreachable room. The one-use bootstrap and subsequent signaling share that single WSS connection; successful host mutations are then admitted per row above rather than assuming only one rotation. |
| MatchRoom state socket | 4 | Budget + room upgrade + one forbidden client message/close allowance. |
| Match peer signaling socket | 15 | Budget + room upgrade + thirteen 20-message request equivalents for the hard 256-message lifetime; a 60/10-second burst cap also applies. No gameplay input is relayed. |

Browser MatchRoom state, Phone Party host and phone-controller signaling do not
turn their socket rows into unbounded automatic retry loops. Each makes at most
five consecutive/short-lived reconnect attempts with backoff, and resets that
count only after 30 stable seconds. Construction/listener/send failures consume
the same sequence rather than opening an unmetered side path. Phone controls
with healthy direct DataChannels remain usable when signaling retry pauses;
otherwise **Try now** explicitly starts a fresh bounded sequence. MatchRoom
Retry refreshes authenticated state before another socket reservation.

Phone Party create/redeem/rotate HTTP reads also stop after ten seconds and 16
KiB of decompressed response data. The returned invite lifetime is measured
from the room object's absolute expiry after its response arrives, rather than
granting a fresh two minutes at the Worker boundary.

Reservations intentionally overcount successful first-attempt paths. The code's
`SAFE_EXTERNAL_OPERATIONS=20,000` is an internal safety ceiling, not a statement
of provider entitlement. Pairing units stop at configured 10,000 or when fewer
than 5,000 units remain for control. Control stops 100 units before the internal
ceiling. Hibernatable signaling traffic consumes additional provider quota, so
the internal ceiling must stay far below the current free allowance.
An authenticated native socket does not grant an unmetered control loop:
ordinary successful host mutations reserve two fresh units and each rotation
reserves ten. A refusal returns `service_budget_safe` on the established socket
without disconnecting controllers; invalid or stale UI races mutate nothing and
consume no command reservation.

`MAX_ADMISSIONS_PER_DAY=0` is the tested admission kill switch. It refuses new
Party and MatchRoom creation/join work while preserving the separately bounded
control path. Do not implement a kill switch through a falsy-default expression:
the literal zero must survive configuration parsing.

Provider limits can change. Before each production release, an owner verifies
the official pricing/quota page and records the date/link; lower current limits
replace these settings before deploy. Never raise a limit merely to clear an
alert.

Baseline verified **2026-08-12**: Workers Free and SQLite Durable Objects each
publish 100,000 requests/day; Durable Objects also publish 5,000,000 rows
read/day, 100,000 rows written/day, 5 GB stored and 13,000 GB-s duration/day,
with free-tier operations
failing after a limit and daily counters resetting at 00:00 UTC. Incoming
Durable Object WebSocket messages use a 20:1 compute-request ratio; alarms and
storage deletion are metered. Static-asset requests remain free/unlimited when
they do not invoke the Worker. Primary references:
[Workers limits](https://developers.cloudflare.com/workers/platform/limits/),
[Durable Objects pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/),
[static-assets billing](https://developers.cloudflare.com/workers/static-assets/billing-and-limitations/).
These are planning inputs, not an entitlement or availability guarantee.
Use the Hibernation WebSocket API exclusively: accepted inbound sockets may
hibernate without disconnecting and idle eligible objects do not accrue
duration. A standard `accept()` socket, timer, pending I/O or outbound socket
can prevent hibernation and is a release-blocking cost regression. The service
suite inventories every Durable Object source for those constructs and requires
`acceptWebSocket` plus serialized attachments on both socket-owning objects.

## Free edge guard

The reviewed zone ruleset payload is
`services/party/ops/free-rate-limit-rule.json`. It consumes the Free plan's one
rate-limiting rule and terminates requests from one IP above 30 `/api/` requests
per 10 seconds for 10 seconds. It uses only the Free-plan Path expression and IP
counting shape. Cloudflare executes `http_ratelimit` as a terminating security
phase before later request handling, so blocked traffic does not invoke this
Worker. Static `/`, `/controller/`, `/room/`, service-worker and asset paths do
not match and remain available. See [rate-limit availability](https://developers.cloudflare.com/waf/rate-limiting-rules/#availability)
and [security phase ordering](https://developers.cloudflare.com/waf/feature-interoperability/#execution-order).

This is a coarse abuse brake, not a quota guarantee. It can false-positive a
large shared-NAT party and cannot stop distributed clients below 30/10 seconds.
The launcher maps a provider-generated non-JSON `429` to the same calm typed
capacity recovery, with **Choose ROM** and **Try Again**. Never broaden the rule
to static paths, lower it without measured legitimate burst evidence, or claim
that it reserves provider-level Worker requests for authenticated control.

Each UTC-day budget shard schedules its own deletion 32 days after its first
mutation. This retention alarm is part of the cost and privacy boundary: do not
turn the snapshot into a room history, event stream or per-player ledger.

## Operator snapshot

`GET /api/ops/capacity` returns one versioned, daily aggregate: admitted
pairing/control units, remaining admission/control units, whether either stop
line has refused at least once, utilization percent, and `normal`, `watch`,
`freeze` or `closed` level. It contains no room id, code, capability,
credential, name, address, route or game data. Responses are always
`Cache-Control: no-store`.

Provision `OPS_READ_TOKEN` as a separate random secret of at least 32
characters. Never place it in `wrangler.jsonc`, reuse `PARTY_HMAC_KEY`, pass it
as a command-line argument, print it, or expose it to browser code. If the
secret is absent or too short, the route returns `404` and no data. A wrong or
missing bearer receives `401` without touching the
daily Durable Object. Query through approved secret-injection tooling at most
once per operational checkpoint; every authenticated read itself consumes one
Worker and one Durable Object request.

The two refusal booleans are durable latches, not counters. The first refusal
in each category writes the latch; an abuse flood cannot amplify storage writes
by increasing a refusal count. Rotate the read token immediately if an
authenticated snapshot appears outside the operations job.

`GET /api/ops/health` is the companion exact-schema
[reservation aggregate](observability.md). It reconstructs pairing/control
units from fixed operation buckets without another write. Require complete
tracking after the old-Worker drain; it does not replace the stop-line snapshot
or provider reconciliation.

## Alarms

The snapshot maps 50/75/90% to watch/freeze/closed. Check before promotion and
at the signed daily checkpoint; escalate on any control refusal, unusual
code-guess rate, signaling message close `4008`, or provider throttle. Metrics
contain only bounded operation/error/phase/path buckets—no room id, name, IP,
credential, code, SDP or input.

At 75%, freeze nonessential previews/load tests. At 90%, disable new pairing and
online admission while retaining close/control. At a provider throttle, show
`service_budget_safe`, keep local visible, and do not retry-loop. The response
must never offer or activate a paid tier.

## Daily verification

Record the authenticated aggregate snapshot, provider-reported usage and a
literal currency charge of zero, then run the strict
[reconciliation gate](reconciliation.md). Archive its bounded pass line; a
stop disables new admission and enters incident handling. Read the companion
health aggregate once and require `tracking: complete`, `legacy: 0` and exact
tracked/admitted units. Peak active rooms/controllers and signaling-message
buckets remain intentionally excluded. The beta ledger's fixed synthetic
canary owns bounded MatchRoom latency and Phone Party direct-connect/input RTT
evidence without a server-side analytics path; its ordinary admitted traffic
must be visible in this same snapshot. A mismatch between internal
and provider counters is an incident; investigate before raising admission.
During a private-beta canary, append each qualified day to the strict
[seven-day ledger](beta-ledger.md); fixture or local-Worker days never count.

## Local executable proof

```bash
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_BROWSER_TESTS_ALLOWED=1 python3 tests/check_party_capacity.py --shell-dir dist/web
python3 tests/test_party_usage_reconciliation.py
```

This starts the production Worker/static-assets shape with fresh local Durable
Objects. One MatchRoom plus one Phone Party consume the fixture admission line;
a 64-request mixed flood and new joins then receive only the bounded
`service_budget_safe`/`no-store` response. Existing authenticated state,
mutation and both close paths retain their control reserve. The Wrangler process
is stopped and restarted against the same disk after the flood; its refusal
latch, closed admission level, MatchRoom state and later Phone Party close all
reconstruct. Root, controller and room documents keep their security headers,
the disabled static policy
remains present, and a real browser exposes accessible **Choose ROM** and
**Try Again** recovery. HTTP state/mutation/close accounts for 10 control units;
both authenticated socket upgrade shapes are also charged, bringing the
secret-gated snapshot to exactly 20 admission and 42 control units (including
the post-restart authenticated state read). The test rejects identifiers or
capabilities in that schema. A separate 64-request forged MatchRoom/Phone Party
control flood receives only `unauthorized`, leaves control at zero and never
touches room objects; authentic post-restart controls then consume the expected
units. A second environment proves the literal-zero kill
switch and its closed snapshot independently. This is local workerd evidence,
not the required hosted provider-usage or seven-day currency ledger. The
reconciliation test separately proves exact schemas, internal arithmetic,
reviewed Free ceilings, zero-billing stop lines and non-reflective diagnostics.

```bash
python3 tests/check_party_edge_policy.py
```

This verifies the exact one-rule Free-plan shape, threshold, absence of
deployment identifiers/secrets, `/api/` coverage, static-route exclusion and
typed handling of a provider HTML `429`. Hosted rule installation, false-
positive observation and Security Events evidence remain provisioning work.
