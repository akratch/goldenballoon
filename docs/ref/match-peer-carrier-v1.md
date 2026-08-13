# Match peer carrier v1

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

The canonical transcript is SHA-256 over a domain label, the 128-bit room id,
protocol/build/gameplay compatibility, ROM/cadence, match epoch and two to four
entries sorted by numeric endpoint id. Each entry contains endpoint id,
connection generation and its 65-byte uncompressed P-256 public key. Every
endpoint replaces its own projected key with its locally generated key before
hashing; a service that substitutes that key therefore produces a different
30-bit, three-compound-word verification phrase. This online-room phrase is
deliberately stronger than Phone Party's separate one-controller approval SAS,
because a multi-peer room can perform more setup retries. Native and browser share an
exact digest/phrase vector, and entry order cannot change it.

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
lowest-id entry that sorting previously could have skipped. Real browser/native
WebRTC, signaling-loss and reconnect matrices remain required before ON-02 can
close.

The pure [preflight consensus gate](match-preflight-v1.md) consumes a frozen
graph and this transcript digest. Its 124-byte report is sequence-bound into
three fixed payload-type-1 fragments, so a one-hop path can forward it without
reading it or using the signaling service. It can report agreement and
actionable local recovery, but cannot create a channel or admit an online race.
