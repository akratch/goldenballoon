# Phone Party signaling service

Implementation status: **local Worker/Durable Object implementation and tests
complete; production is not provisioned or deployed.** No production endpoint
or provider resource is required to run codec, router, controller-surface,
direct-WebRTC or service gates.

The service is deliberately narrow: create a room, redeem a controller, approve
it onto an explicit host-selected seat, revoke/rotate invites, and forward
bounded WebRTC signaling/control messages. Gameplay pad state takes
the peer-to-peer DataChannel and never passes through ordinary signaling.

Operational rules:

- Dedicated Party origin; static controller routes are served without invoking
  metered Worker code.
- One hibernatable room object, no polling timer and no stored SDP/input history.
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
  spend guard and never tear down already-approved local controls.
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

See [protocol v1](../../docs/ref/party-protocol-v1.md), the
[threat model](../../docs/security/multiplayer-threat-model.md) and the
[data map](../../docs/privacy/multiplayer-data-map.md) and the
[operational ledger](../../docs/multiplayer/STATUS.md). Production deployment
still requires a named owner, provisioned origin/HMAC secret, quota alarms,
verification and rollback evidence; a successful local dry-run is not a deploy.
