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
- Browser and native derive those identity bytes from the same reviewed v1
  recipe: SHA-256 domain-separated clean release semver/source commit for the
  build id and source commit/rollback contract for gameplay. Dirty builds,
  malformed provenance and unsupported ROM revisions fail before room I/O;
  US/PAL changes only the locally SHA-verified ROM enum and authored cadence.
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
Every public create, link/code join, state, rotate and command body has one exact
key set and requires `application/json`. Compatibility itself has exactly the
five v1 fields; wrong media types and unknown or future-looking fields reject
rather than being silently ignored or persisted.

Create returns an unguessable room id, a 256-bit structured leader bearer, a fragment-only
128-bit invite secret and a six-digit fallback code. The object and code
directory retain only purpose-separated HMAC digests. Direct-link and code
directories are separate from Phone Party controller pairing. The current
leader may rotate both invitation forms with an expected invite generation;
the room changes them together, stale/concurrent rotations reject, and the old
link/code can no longer join even while its directory entry awaits expiry.

The shareable role URL is exactly the same-origin `/room/#match=<43 base64url>`
form. The role page validates one exact fragment field, erases its own fragment,
then performs a fragment-only same-origin redirect to the always-loaded launcher.
The launcher captures and erases that root fragment before consulting launcher
globals, release policy, model, ROM state or network configuration. It never
writes the capability to session/local storage. A disabled release destroys it
immediately; an enabled release may retain it only in closure memory for at most
ten minutes while a supported ROM is chosen. If either History API scrub fails,
the page navigates to its clean URL and abandons redemption. Invalid, expired,
disabled and scrub-failure paths retain local play and make no Match API request.

State responses contain a read-only reducer projection and at most 64 bounded
result records. State-socket messages and streamed HTTP response bodies share a
64 KiB launcher ceiling; the HTTP reader cancels before parsing once the bound
is crossed. The browser launcher validates every bound and relationship,
maps opaque endpoint identity to compact equality-only indices, and passes that
projection into the shared C view model. JavaScript performs no room transition
or Start decision, and service fields cannot elevate the local admission flag.
Publications must not regress revision, epoch, leader generation or invite
generation. Equal-revision publications must be byte-equivalent after
canonicalization. A nonempty control tail is contiguous, epoch-monotonic and
its final revision, epoch, leader, selected track and vehicle mask must equal
the enclosing lobby; the log cannot contradict the state it purports to explain.
Internal replay receipts, command high-water marks and non-cryptographic
fingerprints are removed from that projection; responses never contain
credentials, invite/code values, names, SDP or inputs.

HTTP mutation responses and state-socket publications can arrive out of order.
A structurally valid publication that is older in every changed monotonic
dimension is therefore an authenticated no-op; any mixture of regression and
advancement still rejects as equivocation. The invitation deadline is immutable
within one generation except for the terminal close transition, which destroys
custody. A delayed rotate response may contribute its invitation secret without
rolling public state back only when its generation is the exact generation
expected by that still-active rotation, is already the current public
generation and the accepted state still makes this endpoint the lobby leader.
An older generation is ignored; an uncorrelated, malformed or changed
same-generation secret and lost leader/phase custody fail closed. Once local
expiry destroys a generation's secret, replaying that response cannot extend
its deadline.

The absolute `inviteExpiresAt` remains authenticated service state and is never
compared directly with the display wall clock. A create/rotate response also
carries the actual positive remaining duration, capped by both the invitation
TTL and the room's own terminal deadline. The launcher converts that duration at
receipt into a local deadline and expires up to 5% (at most 30 seconds) early,
covering transport/timer delay without depending on clock synchronization.
Once elapsed, it destroys the link/code before rendering, removes every Share
surface and gives only the current leader a **New Invitation** action. Rotation
uses the room's current generation even though the old secret is no longer held.

The object uses one idempotent expiry alarm and hibernatable state sockets.
Those sockets are read-only: any client message closes them and every mutation
uses authenticated HTTP. Ephemeral peer setup uses a separate, independently
budgeted signaling socket whose exact protocol is frozen in
[Match signaling v1](match-signaling-v1.md). It cannot mutate this reducer or
carry gameplay/preflight data.
Every request also enforces the hard room deadline before authentication or
mutation, so a delayed alarm cannot extend retention or room authority.
Every accepted nonduplicate command persists the complete next state before it
is broadcast. Local workerd evidence reconstructs the object in Lobby, Loading,
Racing, Results and Closed; exact retries, conflicting replays, simultaneous
CAS commands, alarm deletion and hibernated-socket restoration pass.

Launcher subscriptions also carry a local generation. Closing, replacing or
leaving invalidates that generation before the adapter/socket is touched, so a
late state or close callback from the superseded subscription cannot mutate the
current room or schedule another reconnect. Invalid UTF-8/JSON, schema failure
or an oversized message closes only that subscription; Retry fetches current
authenticated state before creating a fresh one. Synchronous adapter or native
WebSocket construction/listener failure is contained at the same boundary: a
partially acquired handle is invalidated, the calm service recovery is shown,
and any automatic retry consumes the same finite reconnect budget.

## Still gated

- Native live adapter plus human lobby/preflight usability and assistive-device evidence.
- Live 2–4 endpoint peer channels, recovery, chaos/load and production deployment.

None of those may enable an online race before
[A3 rollback `GO`](../multiplayer/STATUS.md). The reducer can be tested now
because it has no authority over simulation or infrastructure.
