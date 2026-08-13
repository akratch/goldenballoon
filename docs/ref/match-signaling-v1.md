# Match signaling v1

Status: implemented and tested locally; the publisher Online Room policy and
race admission remain disabled pending written A3 `GO`.

Match signaling is a distinct authenticated WebSocket at
`/api/match/{roomId}/signal`. It never changes lobby state and it is not the
state subscription at `/connect`. A client offers exactly
`gb-match-signal-v1` and `gb-match.{endpointCredential}`. Browser use is strict
same-origin; an originless native launcher is accepted only with that exact
protocol shape and the same room-bound credential validation. The credential
is a subprotocol, never a URL or query value.

## Connection generations

The room object assigns a nonzero, monotonically increasing 32-bit connection
generation per authenticated endpoint. A new socket replaces that endpoint's
old socket. It receives one `signal_welcome` containing its own identity and a
sorted, unique list of currently connected peers; the other signaling sockets
receive `peer_presence`. A close is announced only when no newer replacement
exists. The generation read, replacement check and absence broadcast share the
same object input gate as upgrade, preventing an old close callback from making
a fresh connection look absent. Because socket close is asynchronous, welcome
construction also collapses any transiently overlapping peer sockets to the
highest generation and presence fanout excludes every socket owned by the
announced endpoint. A queued message is re-authorized against the persisted current sender
generation immediately before relay, so replacement also revokes work already
waiting in the old socket's event queue.

Every client message names one other current room member and that peer's exact
connection generation. The object injects `fromEndpointId` and
`fromConnectionGeneration` from the authenticated socket attachment. A client
cannot claim either field. Delivery is to one exact endpoint/generation, never
broadcast. Missing or stale targets produce one bounded `peer_unavailable`
result to the sender. Leave, disconnect, room close and expiry close the
affected signaling sockets.

## Client messages

All messages are exact JSON objects with `protocolVersion: 1`, a nonzero
monotonic 32-bit `sequence`, `toEndpointId` and
`toConnectionGeneration`. Unknown or extra fields fail closed.

| Type | Additional fields | Purpose |
|---|---|---|
| `peer_hello` | canonical base64url 65-byte uncompressed P-256 `publicKey` | Build the pairwise key transcript |
| `webrtc_offer` / `webrtc_answer` | nonempty `sdp` | Establish one direct peer connection |
| `webrtc_ice` | `candidate`, nullable bounded `sdpMid`, `sdpMLineIndex`, `usernameFragment` | Trickle ICE without provider-shaped pass-through |
| `peer_end` | `reason: restart \| close` | Retire an exact peer generation |

The relay reconstructs a canonical outbound object and adds the authenticated
sender fields. It does not reflect arbitrary client JSON. Binary input,
malformed JSON, sender fields, self-targeting, stale/replayed sequences,
noncanonical keys and invalid types close with a bounded code.

## Bounds and $0 accounting

- Total signaling frame: 64 KiB UTF-8.
- SDP: 60 KiB; ICE candidate: 4 KiB; ICE metadata: 256 bytes each.
- 60 messages per 10 seconds and 256 messages per socket lifetime.
- At most one live signaling socket per each of four room endpoints.
- No signaling payload persistence, history, logs or telemetry.
- A `matchSignalSocket` reserves 15 control units before upgrade: budget call,
  object upgrade and thirteen 20-message request equivalents for the full
  256-message lifetime.

The fixed reservation may refuse with `service_budget_safe`; there is no paid
overflow. Gameplay input, preflight attestations, ROM/save data and result
state have no route through this socket. Pairwise DataChannels carry those
authenticated messages after setup, and local/Phone Party play remains the
immediate fallback.

## Launcher seam

`dist/web/online/match-signal-client.js` validates the same-origin service and
identity before constructing a socket, keeps the credential out of its URL,
verifies that the server selected the public version subprotocol, waits for a
valid welcome, retains peer-generation high-water marks across absence, rejects
stale target/source generations, refuses sends to an unknown peer generation and
nonmonotonic forwarded sequences, and sends no implicit retry or queued
gameplay data. It is dormant: `online-room.js` does not import it while the
publisher policy is disabled. The launcher will own retry/backoff, WebRTC
objects, phrase confirmation and local recovery after A3 approval; game code
continues to receive only sanitized fixed-tick input through the existing
session bridge.

The launcher, not game code, maps transport states to one calm recovery model:

| Adapter state | Player copy | Actions and preserved state |
|---|---|---|
| Connecting / waiting for a peer generation | “Securing direct connections · 2 of 3” | Keep room, seats and selections; **Cancel** is always available. |
| `peer_unavailable` or setup timeout | “Couldn’t connect everyone” | **Try Again** creates fresh generations; **Play Here** and **Leave Room** remain visible. |
| Replaced/stale generation or invalid server message | “Secure connection changed” | Retire keys/channels, reconnect, and compare a new phrase; never reuse Ready. |
| Direct graph ready | “Compare these words” | Show the same large phrase on every display and require explicit confirmation. |
| Signaling lost after direct channels are healthy | “Room updates reconnecting” | Keep healthy direct gameplay; do not claim the peer connection was lost. |
| DataChannel lost | “Player connection lost” | Neutralize that endpoint immediately and offer bounded reconnect/local recovery. |

Retry never clears character/vehicle/track choices and never dismisses the
room merely because WebRTC setup failed. Secret ids, generations, SDP and IPs
stay out of ordinary copy. A detail disclosure may show only bounded stage and
error enums. Reduced-motion and screen-reader views announce a state once, not
on every ICE candidate or retry tick.

Local tests cover exact schemas and UTF-8 limits, burst/lifetime admission,
authenticated sender injection, targeted delivery, stale generation, spoofing,
binary input, hibernation, replacement races, originless native negotiation,
overlapping-replacement welcome canonicalization, state-socket isolation and
the dormant browser adapter. Hosted NAT/channel and
physical UX evidence remain explicitly open.
