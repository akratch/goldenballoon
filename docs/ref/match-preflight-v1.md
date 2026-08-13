# Match preflight consensus v1

Status: pure local foundation; live binding and online-race admission remain
gated by the written A3 decision.

`platform/net/match_preflight.*` is the launcher-owned seam between a room in
Loading and a future engine launch. It owns no socket, clock, service request,
UI, ROM bytes or engine effect. `READY` means that a bounded set of
authenticated observations agree; it is deliberately not permission to start
the game.

## Frozen expectations

The launcher creates a new coordinator for one exact match epoch and connection
graph. It copies:

- the graph's two to four opaque endpoint ids and connection generations;
- SHA-256 of the canonical 148-byte `MdkrMatchLaunchDescriptorV1`;
- the canonical peer-key transcript digest used by the verification phrase;
- SHA-256 of the graph in ascending endpoint-id order, with every directed
  reachability bit remapped into that canonical order;
- the local endpoint id and exact connection generation.

The descriptor already binds build identity, gameplay digest, supported ROM
revision/cadence, track, rules, vehicle mask, input delay, RNG seed, canonical
slot owners and character/vehicle selection revisions. Each launcher must also
verify its local ROM against the known supported SHA-256 before it attests;
neither that ROM digest nor any ROM byte is sent to the service.

A changed epoch or connection generation constructs a new coordinator. Old
readiness can never cross that boundary.

## Authenticated reports

Each endpoint publishes a bounded attestation only over a carrier that has
already authenticated that endpoint id. Submission receives that authenticated
endpoint id and generation separately and requires the embedded claim to match;
report bytes are never their own identity evidence. The report repeats the exact epoch,
connection generation, descriptor digest and peer-key transcript digest, then
the canonical graph digest, then states three local checks:

1. the supported ROM was verified locally;
2. the room's three-compound-word phrase was confirmed by the people playing;
3. gameplay channels for the frozen graph are ready.

Native and browser share the fixed 124-byte `MPF1` encoding below. It is carried
only over a pairwise channel that already authenticated the endpoint id; it is
not a service command and has no unauthenticated fallback. All integers are
big-endian.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | `MPF1` |
| 4 | 1 | version `1` |
| 5 | 1 | ROM verified / phrase confirmed / channels ready bits |
| 6 | 2 | zero reserved |
| 8 | 4 | match epoch |
| 12 | 4 | connection generation |
| 16 | 4 | nonzero report sequence |
| 20 | 8 | authenticated endpoint id |
| 28 | 32 | launch-descriptor SHA-256 |
| 60 | 32 | peer-key transcript SHA-256 |
| 92 | 32 | canonical directed graph SHA-256 |

Decode requires exactly 124 bytes and rejects malformed control bytes, reserved
bits and zero identity/generation/sequence atomically. Digest mutations remain
well-formed reports so consensus can classify them as race-settings or secure-
connection/topology disagreement. The service cannot write reports or declare
consensus. Equivalent endpoint arrays hash identically, but any endpoint,
generation or directed-reachability change produces a different graph digest;
this prevents a signaling layer from equivocating about direct versus one-hop
routes while every peer still claims its own channels are ready.

Every endpoint report has a nonzero monotonic sequence. A higher sequence may
advance or withdraw a transient check. An exact retry is idempotent; reuse of a
sequence for changed content conflicts; older sequences, wrong epochs, stale
generations, authenticated-source mismatches, unknown endpoints and reserved
bits reject without mutation.

### Reliable carrier fragmentation

The report does not fit one 64-byte peer payload. The launcher therefore sends
it as exactly three payload-type-`1` envelopes on the reliable ordered control
path; gameplay continues to use payload type `0`. Each plaintext fragment is:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | nonzero report sequence |
| 4 | 1 | fragment index `0..2` |
| 5 | 1 | fragment count `3` |
| 6 | 58 | report bytes; the final fragment uses 8 and zero-pads 50 |

The AEAD envelope supplies authenticated source/destination identity,
connection generations, route, epoch and its own globally unique transport
sequence. Reassembly accepts fragments in any order, treats an exact duplicate
as idempotent, rejects a changed same-sequence/index fragment, rejects stale
reports, and atomically lets a newer report replace an incomplete older one.
Every reassembler is initialized for one exact authenticated source-to-recipient
key direction, and every fragment submission carries the matching decrypted
envelope context and payload type. The completed report's epoch, endpoint id
and connection generation must match that direction before output is published;
cross-peer fragment splicing and forged attribution therefore fail atomically.
The embedded report sequence must also match every fragment header. A one-hop
forwarder still sees only three opaque fixed envelopes; the signaling service
has no preflight route.

## Deterministic status and UX

Evaluation always returns one typed state, the lowest opaque endpoint id that
currently owns the issue, and received/required progress. The launcher maps the
state to a stable recovery action without exposing ids to players:

| State | Player-facing meaning | Primary recovery |
|---|---|---|
| Descriptor mismatch | “Race settings changed” | Return everyone to the same retained room settings and freeze a new epoch. |
| Transcript mismatch | “Secure connection changed” | Tear down the affected peer generation, reconnect, and compare the new phrase. |
| Graph mismatch | “Connection routes changed” | Retire Ready, exchange one fresh authenticated graph, and retry secure connections. |
| Route unavailable | “Couldn’t connect everyone” | Retry secure connections; offer Cancel immediately. Never imply that a free relay exists. |
| Waiting for peers | “Waiting for players · 2 of 3” | Keep room and selections; allow Cancel/Leave. |
| ROM unverified | “Verify your supported game copy” | Open the local ROM picker/check; upload nothing. |
| Verify phrase | “Compare these words” | Show the same large, copyable phrase on every game screen; require explicit confirmation. |
| Channels not ready | “Finishing secure connection” | Retry only the carrier, preserving room state. |
| Ready | “Everyone is ready” | Continue only when the separate product admission gate is enabled after A3 `GO`. |

Descriptor, transcript and graph disagreement outrank transient waiting states so the UI
does not spin when retry cannot help. Route failure is evaluated before missing
reports because an inadmissible graph cannot become ready through more waiting.
For every failure, the launcher retains local and Phone Party play as an
immediate escape hatch.

## Security and recovery invariants

- No display name, IP address, credential, SDP, input, ROM hash or ROM byte is
  retained by this component.
- The endpoint id shown in diagnostics is equality/routing data, not UI copy or
  telemetry.
- Readiness withdrawal is accepted only at a higher authenticated sequence and
  blocks launch immediately.
- Fragment state is per authenticated peer direction; neither a fragment nor a
  completed report may claim identity from its plaintext bytes.
- Invalid internal state fails closed as `INVALID`.
- All initialization and rejection paths leave caller-owned output unchanged.
- Consensus cannot bypass the publisher admission policy, A3 decision, engine
  manifest validation, rollback transport, or loaded-ROM validation.

Strict native/browser tests share one exact `MPF1` vector and cover three-
fragment reorder/duplicate/conflict/stale/padding/replacement behavior,
cross-source splicing and forged-attribution rejection, staged
success, progress, deterministic issue ownership, disconnected topology,
descriptor/transcript/graph disagreement, reordered-graph equivalence, stale
identity, idempotence, conflict, readiness
withdrawal, corrupted state and fail-atomic negative paths. Real WebRTC
binding, browser UI projection, disconnect timing and human phrase usability
remain explicit post-gate work.
