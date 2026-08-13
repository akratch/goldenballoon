# Match peer carrier v1 (transcript v2, envelope v2)

Status: local foundation; production online-race admission remains disabled.

The launcher owns this protocol. The game receives only already-authenticated,
canonical slot input through `MdkrMatchTransport`. Room state, peer identities,
keys, paths and recovery never enter `game/src`.

## Topology

A snapshot contains two to four unique endpoint ids, exact nonzero connection
generations and each endpoint's directed reachability observations. An edge is
usable only when both endpoints report it. A match topology is admissible only
when every pair has either a mutual direct edge or a mutual two-edge path.
Disconnected graphs and diameter-three chains are refused honestly.

Direct paths always win. When several one-hop paths exist, the intermediate
with the lowest authenticated endpoint id wins, independent of array or join
order. Route lookup requires the exact match epoch and both endpoint
generations. A changed connection therefore cannot inherit an old route.

The intermediate receives opaque source-to-recipient ciphertext. It may
forward the unchanged gameplay or preflight-fragment envelope once only when
the immediate DataChannel peer's separately authenticated endpoint id and
connection generation both match the header and the current deterministic
route names the local endpoint. A fixed 64-sequence window per directed pair and exact source/
destination generations drops duplicates, loops and old traffic before fanout.
A reconnect reuses the bounded direction slot but resets its replay window for
the newly derived generation-bound key. It has no gameplay key and cannot alter
or impersonate the source.

## Key schedule

Each peer pair performs ephemeral P-256 ECDH. For direction `source → target`,
HKDF-SHA-256 consumes:

- 32-byte ECDH secret as input key material;
- the 32-byte canonical room/roster/public-key transcript digest as salt;
- `golden-balloon-match-input-key-v1`, match epoch, source id/generation and
  target id/generation as binary HKDF info.

The reverse direction and every reconnect/epoch produce different keys. Native
uses pinned Mbed TLS 3.6 LTS and explicitly zeroizes retired key bytes. Browser
code uses non-extractable WebCrypto keys and retires their handles. The service
and any forwarder receive public keys and ciphertext, never the ECDH secret or
derived gameplay key. A room verification phrase must bind the same transcript
before production admission; that UI/handshake integration remains open.

### Key commitment round

The verification phrase is agreed over **two rounds**, so that no endpoint can
choose its key after seeing anyone else's.

**Round 1 — commit.** Before any public key is revealed, every endpoint
publishes

```
commitment = SHA-256( "golden-balloon-match-peer-commit-v1"
                      || u32 match_epoch
                      || u64 endpoint_id
                      || u32 connection_generation
                      || nonce[32]          // fresh random, per epoch
                      || public_key[65] )   // uncompressed P-256
```

The nonce hides the key, so a commitment reveals nothing; the epoch and the
authenticated identity are bound in, so a commitment cannot be lifted into
another room, epoch or generation. An all-zero nonce is refused.

**Round 2 — open.** Each endpoint reveals `(public_key, nonce)`. Every peer
recomputes the commitment and compares it in constant time. A mismatch fails
closed with no phrase produced.

Verification is **not** left to the caller: `mdkr_match_peer_transcript_digest`
and `digestMatchPeerTranscript` re-run round 2 for every entry and refuse to
emit a digest if any commitment does not open. A phrase derived from key
material that was never committed to is exactly what an active man in the middle
needs, so it cannot be produced by forgetting a check.

**Why the round exists.** Without it, a man in the middle presenting key `M1` to
A and `M2` to B may choose both *after* seeing A's and B's real keys, and can
search for `phrase(transcript_A) == phrase(transcript_B)`. That is a birthday
search over the 30-bit phrase space — about 2^15 work, measured at **19,231
keypairs per side in 0.15 s**. With the commitment the attacker must fix `M1`
and `M2` before the reveal, so each session is one independent 2^-30 guess:
**0 successes in 3,000,000 simulated sessions**. See §Evidence.

### Transcript

The canonical transcript is SHA-256 over the domain label
`golden-balloon-match-peer-transcript-v2`, the 128-bit room id,
protocol/build/gameplay compatibility, ROM/cadence, match epoch and two to four
entries sorted by numeric endpoint id. Each entry contains endpoint id,
connection generation, **its round-1 commitment** and its 65-byte uncompressed
P-256 public key — the phrase therefore covers the whole two-round handshake,
not only its outcome. Every endpoint replaces its own projected key with its
locally generated key before hashing; a service that substitutes that key
produces a different 30-bit, three-compound-word verification phrase. This
online-room phrase is deliberately stronger than Phone Party's separate
one-controller approval SAS, because a multi-peer room can perform more setup
retries. Native and browser share an exact digest/phrase vector, and entry order
cannot change it.

### Versioning and downgrade

The transcript domain carries the version (`…-transcript-v2`) and the envelope
header carries protocol version byte `2`. The two versions cannot interoperate
and cannot negotiate: a v1 transcript yields a different digest and therefore a
different key, and a v1 envelope is rejected by the header parser **before any
decryption is attempted**, surfacing as `INVALID` rather than an authentication
failure. There is no version negotiation step to downgrade.

## Envelope

AES-256-GCM seals exactly one fixed 64-byte payload. Authenticated payload type
`0` is a redundant input bundle; type `1` is one reliable preflight fragment.
Both types consume the same monotonically unique per-key sequence space. All
integers are big-endian.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | `MPE1` |
| 4 | 1 | version `1` |
| 5 | 1 | forward count: `0` or `1` |
| 6 | 1 | payload type: input `0` or preflight fragment `1` |
| 7 | 1 | zero reserved |
| 8 | 4 | match epoch |
| 12 | 8 | source endpoint id |
| 20 | 4 | source connection generation |
| 24 | 8 | destination endpoint id |
| 32 | 4 | destination connection generation |
| 36 | 8 | intermediate endpoint id, or zero for direct |
| 44 | 8 | nonzero per-key sequence |
| 52 | 64 | ciphertext |
| 116 | 16 | GCM authentication tag |

The 52-byte header is authenticated additional data. The 96-bit nonce is the
match epoch followed by the sequence. Sequence reuse under one key is forbidden;
the public sealing call does not accept a sequence. One direction-bound seal
window, initialized exactly once for each freshly derived sending key, owns a
counter beginning at one across both payload types. It advances only after a
successful seal, never moves backwards after a route change, and fails closed
after sealing `UINT64_MAX` once. Native initialization requires an all-zero
window and refuses an active or exhausted window. Browser creation binds one
read-only window to the exact direction retained by one derived key object and
refuses a mismatch or second window; its internal
busy latch prevents two concurrent asynchronous WebCrypto calls from observing
the same nonce. A failed provider call releases that latch without publishing
output or consuming the sequence. Reconnect derives a new generation-bound key
and creates a new zeroed/window object; reconnect code must never retain or
reinitialize the old key/window pair. The key changes with epoch, direction or either
connection generation. The recipient supplies the complete source-to-recipient
direction expected for the selected key and rejects a header that claims a
different source, recipient, epoch or generation before decryption. This keeps
key selection and claimed header identity from becoming a confused-deputy
boundary. It then authenticates before changing replay state or publishing
plaintext. Wrong source, recipient, generation, epoch, authentication and replay
are distinct local errors but never centralized diagnostics dimensions.

Preflight reassembly consumes the authenticated envelope context alongside each
plaintext fragment and binds its state to one exact source-to-recipient key
direction. It then requires the completed report's endpoint, generation and
epoch to match that direction. Decryption alone is therefore insufficient to
misattribute another member or splice fragments from separate peer channels.

Native and browser tests share one exact HKDF/envelope vector, prove monotonic
sender allocation under reordered delivery, fail-atomic invalid/provider state,
browser concurrent-seal exclusion and native counter exhaustion, and reject a peer
sealing under its valid key while claiming another source identity/generation,
and mutate every header (including payload type), ciphertext and tag byte. Topology tests cover canonical padding-free
copies, 2–4 endpoints, stable
one-hop selection, asymmetric observations, diameter-three refusal, stale
epochs/generations, stale-channel generation impersonation, malicious forwarding,
generation-reset replay windows,
duplicates and direct-path replacement. Preflight carrier tests additionally
cover cross-direction splicing and forged report attribution. Transcript negatives include an invalid
lowest-id entry that sorting previously could have skipped, and sweep **every
byte of a commitment and of the opening nonce** the way the envelope sweep
covers all 132 envelope bytes: each single-byte change must refuse to produce a
phrase. Substituting another peer's otherwise valid key while keeping the old
commitment — the grinding attack itself — is refused, as is a degenerate
all-zero nonce and a commitment lifted to a different epoch or generation.
Both implementations also refuse a v1 envelope before decryption. Real browser/native
WebRTC, signaling-loss and reconnect matrices remain required before ON-02 can
close.

### Evidence: phrase grinding, before and after

Measured against the shipped digest/commitment implementation (the fast search
loop is validated to reproduce the module's exact pinned digest and phrase
before it runs):

| attacker model | result |
|---|---|
| v1 semantics — MITM picks its keys *after* seeing both honest keys, re-rolling freely | collision found after **19,231 keypairs per side, 0.15 s** |
| v2 committed round — MITM must fix both keys before the reveal | **0 successes in 3,000,000 full sessions** (35.1 s); 0.0028 expected by chance at 2^-30 |

The bound is therefore what a 30-bit phrase should be worth: one independent
guess per session, detectable because the humans compare and abort, rather than
a seconds-long offline search. The commitment does not widen the phrase; it
removes the attacker's ability to search it.

The pure [preflight consensus gate](match-preflight-v1.md) consumes a frozen
graph and this transcript digest. Its 124-byte report is sequence-bound into
three fixed payload-type-1 fragments, so a one-hop path can forward it without
reading it or using the signaling service. It can report agreement and
actionable local recovery, but cannot create a channel or admit an online race.
