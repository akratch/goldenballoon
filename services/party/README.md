# Phone Party signaling service

Implementation status: **local Worker/Durable Object implementation complete;
the latest boundary fixtures await isolated-host execution and production is
not provisioned or deployed.** No production endpoint
or provider resource is required to run codec, router, controller-surface,
direct-WebRTC or service gates.

The service is deliberately narrow: create a room, redeem a controller, approve
it onto an explicit host-selected seat, revoke/rotate invites, and forward
bounded WebRTC signaling/control messages. Gameplay pad state takes
the peer-to-peer DataChannel and never passes through ordinary signaling.

Operational rules:

- Dedicated Party origin; static controller routes are served without invoking
  metered Worker code. `PARTY_ORIGIN` must be its exact canonical HTTPS origin
  (no trailing slash/path/query/fragment/userinfo); only loopback development
  may use HTTP. Invalid configuration refuses before routing or object access.
- One hibernatable room object, no polling timer and no stored SDP/input history.
- Every Durable Object remains eligible for idle hibernation: production source
  contains no standard WebSocket acceptance, socket event listeners, timers or
  outbound sockets. Provider reconciliation v2 measures the separate 13,000
  GB-s/day Free duration line; the v3 operated ledger rejects missing duration.
- Eight pending devices, four approved seats, two-minute invites and hard room
  expiry within 24 hours.
- Keyed credential digests, ephemeral ECDH comparison phrases, strict
  origin/schema/body limits and monotonically increasing transition ids.
- Fixed-size host/controller/endpoint credentials combine a 128-bit nonce with
  a 128-bit room/role HMAC. The Worker rejects forged or cross-room control
  before budget/object access; room storage retains only a full keyed digest.
- Separate pairing, control and optional relay reserves. Admission refuses with
  `service_budget_safe` before control/close capacity or the zero-spend ceiling
  can be consumed. Budget counters reserve worst-case external-operation units
  (including fallback-code collision loops), not one optimistic unit per API
  call; see the [capacity runbook](../../docs/ops/multiplayer/capacity.md).
- A separate secret-gated operations route exposes only the versioned daily
  capacity aggregate. Refusal is latched once per category, and each UTC-day
  shard is alarm-deleted after 32 days; no room/player/capability dimensions are
  stored or returned.
- Phone Party signaling sockets accept at most 120 messages per ten seconds and
  512 for their entire connection. Their admission reserves the worst-case
  Durable Object message-equivalent units; MatchRoom subscriptions are charged
  separately and reject client commands. MatchRoom byte-counts text as UTF-8
  and binary before applying its 4 KiB inbound frame cap, then closes every
  in-policy client message because commands belong on authenticated HTTP.
- Host WebRTC offers and ICE are delivered only to the exact authenticated
  controller attachment named by `to`; controllers never receive another
  phone's SDP/candidates and phone-originated identity is always injected from
  its socket attachment. The object reconstructs exact role-specific
  hello/offer/answer/ICE shapes, exact nested SDP and bounded declared ICE
  fields before relay; ambiguous JSON and unknown fields close without fanout.
  Browser host/phone reconnect is generation-guarded, capped at five attempts
  and resets only after 30 stable seconds.
- Match peer setup uses the separate `/api/match/{roomId}/signal` protocol.
  It injects the authenticated sender, targets one current endpoint generation,
  replaces duplicate sockets and forwards only exact P-256 hello, SDP, ICE and
  retire shapes without persistence. Its 60/10-second and 256-lifetime caps are
  reserved as 15 control units before upgrade. It cannot carry gameplay input
  or preflight consensus; see [Match signaling v1](../../docs/ref/match-signaling-v1.md).
- Native launchers create through one originless WebSocket upgrade at
  `/api/party/native-create`, offering exactly `gb-native-host-v1`,
  `gb-control-v1` and one 87-character P-256 public-key protocol. The Worker
  reserves pairing plus the full socket lifetime before it creates room state,
  injects the secret-bearing bootstrap exactly once into the authenticated host
  socket, and never accepts this route from a browser Origin. Reconnect offers
  the same two version protocols plus one room-bound `gb-host.<credential>`;
  ordinary callers cannot inject bootstrap headers.
- Native `approve`, `reject`, `remove`, `revoke` and `close` commands reserve
  two additional control units per successful mutation; invite rotation
  reserves ten for its bounded directory collision loop. Admission refusal is
  a recoverable `host_command_result`, so repeated commands cannot escape the
  spend guard and never tear down already-approved local controls. Each action
  has one exact field set; extra/wrong-action fields reject before reserve or
  authority mutation.
- Every Worker→Durable Object fetch uses internal protocol v1. Object readers
  retain the legacy-unversioned contract for old-Worker/new-object rollout and
  reject unknown versions before request parsing or storage; protocol changes
  follow the documented multi-release expand/drain/emit/contract sequence.
- No billing method is a deployment prerequisite. “Zero cost” means a hard
  spend guard and graceful local-play fallback, not unlimited free capacity.

The local service suite includes a deterministic 24,576-command reducer chaos
campaign. It mirrors every command through two independent copies and requires
valid bounded state, atomic rejection, exact retry/conflict behavior and parity
after simulated Durable Object reconstruction.

Expired fallback codes and pseudonymous requester rate buckets are removed by
the directory object's alarm. Closing the setup sheet revokes its invite while
preserving approved leases. Rotation is generation-checked, so a late rotate
cannot revive an invite after revocation. Invite rotation invalidates the old QR/code;
multiple friends may redeem one current invite, each receiving an independent
controller credential and requiring host approval.
Create and rotate responses carry the room object's actual positive remaining
invite lifetime (never a newly assumed TTL) plus the committed invite
generation. Browser clients bound each response to 16 KiB/ten seconds and use a
conservative receipt-relative countdown. One browser profile admits one
publisher for a scanned invite: Web Locks pair with an explicit broadcast
neutral/close acknowledgement, while the no-Web-Locks fallback stores only a
hashed 15-second tab lease, never the capability or controller credential.
Server-seat replacement remains a visible host action.
Revoke returns its exact committed transition and invite generation. The
browser clears QR/code/URL custody before the request and waits for that
generation before reopening; natural expiry performs the same clear. Room
storage remains authoritative if a hibernated socket disappears during
publication: send, close and attachment failures are contained per socket and
cannot turn a committed mutation into a retryable-looking HTTP failure.

See [protocol v1](../../docs/ref/party-protocol-v1.md), the
[threat model](../../docs/security/multiplayer-threat-model.md) and the
[data map](../../docs/privacy/multiplayer-data-map.md) and the
[operational ledger](../../docs/multiplayer/STATUS.md). Production deployment
still requires a named owner, provisioned origin/HMAC secret, quota alarms,
verification and rollback evidence; a successful local dry-run is not a deploy.

On an occupied workstation, validate only the non-executing type boundary with
`npm run typecheck`. `npm test`, `npm run check`, and direct Vitest startup fail
closed unless a human-confirmed isolated host supplies
`MDKR_DEDICATED_TEST_DESKTOP=1`.
