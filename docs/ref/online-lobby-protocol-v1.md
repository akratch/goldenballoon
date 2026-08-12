# Online lobby reducer protocol v1

Status: **pure launcher core and local MatchRoom persistence implemented;
production transport/admission gated.**
`platform/online/lobby_core.*` is the canonical reducer and
`tests/test_online_lobby_core.c` is its executable contract. It deliberately
cannot start or mutate the game engine. `SessionCore` owns product navigation;
this reducer owns private-room membership and pre-race consensus; a future
launcher adapter maps accepted phases between them.

## Ownership boundary

```text
launcher UI ── command ──> OnlineLobbyCore ── accepted state ──> SessionCore
     │                           │                                  │
     └── service adapter         └── no sockets/clocks/UI/game      └── engine effects
```

The engine sees only a frozen `MdkrMatchManifestV1`, canonical pad samples and
the rollback driver after the launch barrier. It never knows room codes,
WebSockets, leader leases, display names, matchmaking providers or retries.
This is the enforced answer to “keep matchmaking in the launcher.”

Protocol v1 admits only `MDKR_MATCH_RULES_STANDARD_RACE`. Manifest validation
rejects every other rules value, and the engine independently compares the
manifest track, the loaded ROM catalog's standard-race type and the authored
regional cadence before tick one. Battles, challenges, bosses, hubs and
cutscenes are explicit future protocol work; they cannot enter rollback merely
because a numeric track id was syntactically valid.

## State and commands

The room holds at most four opaque endpoints and four canonical seats. The
reducer supports up to two seats per endpoint so mixed couch/online can be
earned later without changing the room state shape; the initial online UI may
still restrict admission to one seat per endpoint.

Phases are monotonic within a round:

```text
Lobby → Loading → Racing → Results → Lobby (rematch)
                                   └→ Closed
```

Commands are `Join`, `Leave`, `Disconnect`, `Reconnect`, `Set ready`,
`Set vote`, `Set character`, `Set vehicle`, `Begin loading`, `Acknowledge
loaded`, `Begin race`, `Publish results`, `Rematch`, `Transfer leader` and
`Close`.

- Join compares protocol, 16-byte build identity, 32-byte gameplay/content
  digest, ROM revision and authored cadence byte-for-byte before mutation.
- Ready is endpoint-scoped; votes are seat-scoped. A seat can only vote through
  its owning endpoint.
- Character and vehicle choices are seat-scoped. Characters must be unique;
  every accepted choice increments a seat revision and clears the owner's Ready
  state. Ready cannot become true until every owned seat has both choices.
- Begin loading carries the winning track's independently ROM-derived legal
  vehicle mask. Every selected vehicle must be legal or the transition rejects
  atomically. The mask is frozen into the manifest and revalidated against the
  loaded ROM before tick zero.
- At least two connected, ready endpoints and one vote are required to load.
  Highest vote count wins; ties use room id and next match epoch, so every
  replica chooses the same track without wall clock or provider randomness.
- Every connected endpoint must acknowledge the same loading phase before the
  leader may begin the race.
- Disconnect preserves seats and clears ready/loaded. Reconnect preserves room
  identity but requires fresh readiness. Mid-race consequences remain gated on
  the rollback/disconnect policy; this reducer does not counterfeit AI takeover.
- Leader transfer is explicit and increments `leader_generation`; leader leave
  elects the lowest connected opaque endpoint. A service lease timer will
  issue that command—it will not embed clock reads in the reducer.

## Concurrency and replay safety

Every command carries exact protocol version, expected room revision
(compare-and-swap), nonzero opaque actor id and actor-scoped nonzero command id.
Rejected commands are byte-atomic. Accepted commands increment revision once.

An eight-entry room receipt window and per-member high-water/fingerprint make
an exact network retry return duplicate success without a second mutation;
reusing the id for a different payload fails closed. The room receipt survives
member removal, so retrying `Leave` is safe. No command contains a display name,
IP address, credential, SDP, pad sample or game asset.

## MatchRoom service binding

`services/party/src/match/` mirrors the bounded protocol in a synchronous,
fail-atomic TypeScript reducer and a separate SQLite Durable Object. External
commands never choose their actor: the Worker hashes a 256-bit endpoint bearer,
whose 128-bit random nonce and 128-bit truncated HMAC bind it to the exact room
and endpoint role. Forged/cross-room bearers reject before control-budget or
object access. The object resolves the full token's purpose-HMAC digest to one
opaque endpoint id, then injects the
actor before dispatch. The client supplies protocol, command id, expected
revision, typed action, bounded value/target and (for Join only) compatibility.

Create returns an unguessable room id, a 256-bit structured leader bearer, a fragment-only
128-bit invite secret and a six-digit fallback code. The object and code
directory retain only purpose-separated HMAC digests. Direct-link and code
directories are separate from Phone Party controller pairing. The current
leader may rotate both invitation forms with an expected invite generation;
the room changes them together, stale/concurrent rotations reject, and the old
link/code can no longer join even while its directory entry awaits expiry.
State responses contain a read-only reducer projection and at most 64 bounded
result records. The browser launcher validates every bound and relationship,
maps opaque endpoint identity to compact equality-only indices, and passes that
projection into the shared C view model. JavaScript performs no room transition
or Start decision, and service fields cannot elevate the local admission flag.
Internal replay receipts, command high-water marks and non-cryptographic
fingerprints are removed from that projection; responses never contain
credentials, invite/code values, names, SDP or inputs.

The object uses one idempotent expiry alarm and hibernatable state sockets.
Every request also enforces the hard room deadline before authentication or
mutation, so a delayed alarm cannot extend retention or room authority.
Every accepted nonduplicate command persists the complete next state before it
is broadcast. Local workerd evidence reconstructs the object in Lobby, Loading,
Racing, Results and Closed; exact retries, conflicting replays, simultaneous
CAS commands, alarm deletion and hibernated-socket restoration pass.

## Still gated

- Native live adapter plus human lobby/preflight usability and assistive-device evidence.
- Peer transport, recovery, chaos/load and production deployment.

None of those may enable an online race before
[A3 rollback `GO`](../multiplayer/STATUS.md). The reducer can be tested now
because it has no authority over simulation or infrastructure.
