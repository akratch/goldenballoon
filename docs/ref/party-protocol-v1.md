# Phone Party protocol v1

Status: **frozen for Local Party M0**. The normative implementations are
[`party_protocol.c`](../../platform/party/party_protocol.c) and
[`party-protocol.js`](../../dist/web/party/party-protocol.js). Both consume the
same checked vector in [`tests/fixtures/party`](../../tests/fixtures/party).

This protocol lets a launcher treat a phone as one untrusted controller source.
It never carries ROM, save, framebuffer, audio, identity, room admission or SDP
data. Admission and seat ownership happen on the reliable control channel before
the host accepts these packets.

## Transport and channels

- `mdkr-pad-control-v1`: reliable and ordered. It carries version negotiation,
  approval, seat lease, connection epoch, ping/pong, leave, and typed errors.
- `mdkr-pad-state-v1`: unordered with `maxRetransmits: 0`. The newest state
  supersedes old datagrams, while bounded edge history rescues a quick tap.
- DTLS protects both WebRTC channels. A state channel is bound to one approved
  controller and one host-side lease; the packet cannot choose its port.
- Send immediately on every transition and repeat current state every 50 ms.
  Stop enqueueing stale state when `bufferedAmount` exceeds the host-advertised
  ceiling. The receiver emits neutral after 250 ms without a valid packet.

Signaling messages are transient and are never stored. The host creates the
offer and tags every offer and ICE candidate with one positive, monotonically
increasing `peerGeneration`; the controller echoes that exact generation in
its answer and candidates. Each side ignores a message for any other generation.
Host messages also carry one exact 22-character controller `to` id. The room
object resolves that id against authenticated socket attachments and sends the
message only to that controller; it never broadcasts one phone's SDP or ICE to
the others. Controller identity is overwritten from its authenticated socket
attachment before forwarding to the host.
An ICE restart may reuse a live generation. Closing either DataChannel retires
that generation and requires one newer offer, so delayed answers or candidates
cannot revive an abandoned peer. Signaling-socket loss alone does not release
input while both authenticated channels remain open.

The relay accepts only these exact transient envelopes: controller
`controller_hello`; host `webrtc_offer`/`webrtc_ice`; controller
`webrtc_answer`/`webrtc_ice`; and authenticated host `host_command`. Offer and
answer descriptions are exactly `{type,sdp}` with the expected role-specific
type and a 60 KiB UTF-8 SDP ceiling. ICE is exactly a required 4 KiB
`candidate` plus the declared optional `sdpMid`, `sdpMLineIndex` and
`usernameFragment` fields, each independently bounded. Generation is a
positive u32 and controller ids are exact 22-character base64url values.
Null, array, primitive, unknown-field and wrong-role messages close with
`invalid_signaling` before relay. The room rebuilds the forwarded object and
injects controller identity from the authenticated socket attachment, so a
phone-supplied id or diagnostic field cannot cross roles.

Native host commands likewise have one exact action shape: approve carries
controller and seat; reject/remove carry controller; rotate carries expected
invite generation; revoke/close carry no additional fields. Unknown fields or
actions produce a bounded `invalid_command` result without mutation or budget
reservation.

After the authenticated `controller_ready` hello, the host sends a bounded
`{type:"ping", protocol:1, nonce:u32}` on the reliable channel every five
seconds while no probe is outstanding. The controller echoes a bounded `pong`.
Fifteen seconds without the matching pong is a direct-channel failure: publish
neutral, retain the seat lease and recover through a fresh peer generation.

Browser host and phone signaling sockets own independent generation counters.
Messages, close events and timers from a replaced generation are no-ops. Each
side makes at most five consecutive/short-lived reconnect attempts using
300–4,800 ms backoff and resets only after 30 stable seconds. A synchronous
constructor/listener/send failure consumes the same sequence. Healthy direct
channels continue through signaling loss; without them, input is neutral and
the phone exposes an explicit **Try now** action after automatic exhaustion.

Public create, redeem, approve, reject, remove, rotate, revoke and close request
bodies use exact per-action key sets; the optional phone name is the only
declared optional field. JSON bodies require `application/json`; missing/wrong
media types and unknown fields reject before room authority. Room
create/redeem/rotate responses are exact JSON bounded to 16 KiB of decoded
UTF-8 and ten seconds. Create and rotate return the committed invite generation
and actual positive lifetime remaining at the room object; the launcher applies
a conservative receipt-relative countdown instead of assuming a fresh TTL.
Revoke returns exactly `{ok, transitionId, inviteGeneration}`. The launcher
removes the QR bitmap, code and URL from local/DOM custody before sending it,
then records only the exact next generation. Reopen waits for an in-flight
revoke before generation-checked rotation. A stale completion is room-bound and
cannot delay, annotate or mutate a replacement room. Natural expiry performs
the same local secret erasure while preserving approved leases.

The controller invitation is exactly
`/controller/#<base64url-capability>` on the static origin, with no query or
alternate path. The production origin is one canonical HTTPS origin; plain HTTP
is accepted only on a loopback hostname for local development. Both the Worker
and browser reject a noncanonical, credential-bearing, cross-origin or insecure
production base before redemption or Durable Object work. The controller
captures and erases the fragment before reading
fixture/configuration globals, probing browser features or making network calls;
malformed routes fall back to code entry. It retains the capability only in the
page closure, never web storage. If History API erasure fails, it clean-navigates
to `/controller/` and abandons redemption. Embedded/unsupported-browser
**Share private link** and **Copy private link** gestures may reconstruct the
exact private URL directly into the system share sheet or clipboard, and
**Use this tab instead** may use that same exact fragment for one same-tab
navigation after the prior tab acknowledges takeover, publishes neutral, closes
its direct/signaling transports and releases the browser lock. The newly loaded
page immediately scrubs the fragment again. Web Locks are paired with a
same-origin `BroadcastChannel`; rejection fails closed and never falls through
to a competing lease system. Where Web Locks are absent, a broadcast election
plus a 15-second heartbeat stores only a 96-bit capability digest prefix,
random tab id and expiry in one origin-scoped record—not the capability or
controller credential. Authority expires after 15 seconds even if a crash-stale
record remains until the next controller attempt overwrites it. If both
coordination mechanisms are unavailable, takeover refuses. If no capability
remains, browser recovery shares/copies the
public `/controller/` page and explicitly requires the current code. Neither
recovery copies the incidental already-clean `location.href`.
Protocol-version recovery performs an actual static-client reload rather than
labelling code entry as refresh. `pagehide` releases input, transports and tab
ownership; a back-forward-cache restore reloads one clean capability-free
controller document rather than reviving dead controls. A frozen document
publishes neutral before its timers are suspended; resume re-arms only an
otherwise-connected controller, discards all pre-suspension edge history and
publishes a fresh neutral sample before accepting input. Controller redemption
requests are aborted on `pagehide`, and both tab acquisition and response
completion recheck lifecycle state before using a capability or rendering a
credential-derived state. The launcher host likewise
erases QR/code/URL custody, closes peers and neutralizes remote pads on
`pagehide`; a persisted restore restarts its countdown/socket from retained
room authority without resurrecting an invitation. Create/rotate requests are
page-bound and aborted on dismiss, end or navigation; an operation generation
also rejects a response that crossed that boundary, so a late response cannot
repopulate a secret even when network cancellation arrived too late.

Because the object publishes public state before the Worker returns the secret,
the rotation response is correlated to the request's captured predecessor. It
may attach the secret when current authority is exactly that predecessor or its
already-published successor; any older, newer or unrelated generation rejects.
Every public generation change erases any displayed secret before merging the
state, including changes made by another authenticated host socket. Thus QR,
code and URL custody can never silently cross into a generation they did not
create, even while the local host is waiting for its own correlated response.
Host room and controller state publications are exact, monotonic and immutable:
a lower transition is stale, an identical transition is an idempotent no-op and
different content at the same transition closes the poisoned socket.
After authoritative storage commits, socket publication is best effort and
exception-contained per peer. A stale/broken hibernated socket is retired with
a typed close; it cannot turn the committed HTTP command into a failure or
prevent delivery to another peer. Deserialized socket attachments are validated
before they can select a signaling recipient.
Authenticated native host mutations share the Durable Object's full input gate
with HTTP mutations across budget, directory and storage awaits. The native
tail additionally orders multiple host sockets; no transport can persist a
stale predecessor over the other's approval, revoke, close or rotation.

An approved phone is removed only by the launcher-owned `remove` command for
its exact controller id. The room commits `Closed`, clears the numbered seat,
publishes the host projection without that controller, sends the target phone
one exact terminal `controller_state`, then closes only that phone's signaling
socket with `seat_reclaimed`. Other controllers remain connected. A rejected
pending phone follows the same targeted terminal sequence with
`approval_rejected`. Either the terminal state or its typed close independently
stops phone retry and releases direct input; losing one frame cannot turn a
revoked credential into a reconnect loop. Removal never adds gameplay state to
the room service.

## Pad-state binary layout

All multibyte integers are unsigned network byte order. The fixed packet is 24
bytes; each history edge adds five bytes; 64 bytes is the absolute v1 maximum.

| Offset | Bytes | Field | Validation |
|---:|---:|---|---|
| 0 | 2 | magic | ASCII `GB` |
| 2 | 1 | version | `1` |
| 3 | 1 | type | `1` (`STATE`) |
| 4 | 1 | flags | `PRESENT=1`, `NEUTRAL=2`, `HAS_EDGES=4`; no other bits |
| 5 | 4 | connection sequence | Exact epoch established on the control channel |
| 9 | 4 | sample sequence | Monotonic modulo 2³²; half-range comparison |
| 13 | 4 | sender monotonic ms | Diagnostics only; never used as authority time |
| 17 | 2 | buttons | N64 bits `0xff3f`; reserved bits reject the packet |
| 19 | 1 | stick X | Signed `[-80,80]` |
| 20 | 1 | stick Y | Signed `[-80,80]` |
| 21 | 1 | edge count | `0..8`; must agree with `HAS_EDGES` and total length |
| 22 | 5n | edge records | Sequence delta, buttons, signed X, signed Y |
| 22+5n | 2 | CRC-16/CCITT-FALSE | Initial `0xffff`, polynomial `0x1021` |

An edge sequence delta is `current sample sequence - edge sequence`. It is in
`[1,127]`. Records are oldest first, so deltas are strictly decreasing, such as
`4,2,1`; duplicates and ambiguous half-range history are impossible. Each edge
is a complete present-controller sample. Current `NEUTRAL` and absent states
must contain zero buttons and centered sticks; absence also requires `NEUTRAL`.

CRC detects accidental corruption and malformed app payloads. It is not a MAC;
the authenticated DTLS channel and approved lease provide peer binding.

## Atomic receiver rules

1. Copy the received bytes into a bounded local buffer.
2. Validate total length, magic, version, type, flags, count, checksum, every
   edge delta, button mask, stick range and neutral invariant.
3. Leave caller output byte-for-byte untouched after any failure.
4. Require the exact reliable-channel connection epoch. A reconnect is an
   explicit rebind that first publishes neutral.
5. Reject duplicate/stale current sequences. Dedupe recovered edges against the
   last accepted sequence, then publish remaining edges oldest first.
6. If nine state mutations would enter the eight-entry receiver queue, replace
   the entire queue with one absent-neutral transition. Never trade latency for
   completeness or preserve a potentially held accelerator.
7. At 250 ms silence, enqueue one absent-neutral transition and latch the
   timeout until a newer valid packet arrives.

## Control state machines

Room phases are monotonic except that an open invite may rotate without changing
phase. Unknown phase/event pairs fail closed.

| Current | Event | Next | Required effect |
|---|---|---|---|
| Creating | room created | Open | Mint two-minute invite |
| Open | invite rotate | Open | Revoke old capability before returning new one |
| Open | setup dismissed | Open | Revoke displayed invite; preserve approved leases |
| Open | host starts | Open | Revoke displayed invite; preserve approved leases; keep gameplay state launcher-owned |
| Open | in-game setup opened | Open | Mint a fresh, scoped two-minute invite |
| Open | host closes | Closed | Neutralize/revoke every lease and credential |
| Any live | TTL/budget exhausted | Closed | Typed close reason; no automatic paid fallback |

Controller admission is `Redeemed → Awaiting approval → Approved → Leased →
Connected`. Reject, expiry, host close and explicit leave go to terminal
`Closed`; an approved/connected phone also reaches `Closed` through
an explicit, confirmed host removal. Host approval names the exact numbered
seat; an invalid or already phone-leased seat fails without mutation. The launcher visibly labels
when that choice replaces keyboard, touch or a gamepad. Only a free,
generation-checked router seat can move to `Leased`; only a matching protocol
hello can move to `Connected`. Reconnect rotates the connection sequence without
changing the seat generation. While reconnecting, its lease remains reserved
and publishes neutral, so a local source cannot silently take it over. A stale
credential, epoch or lease can only be rejected—it cannot revive an earlier state.
Removal publishes neutral custody, retires the host peer when the authoritative
room state arrives, closes the removed signaling socket and makes the seat
available without disturbing another lease.

A duplicate-tab takeover is local publisher custody, not silent server-seat
transfer. The old tab neutralizes and closes before the new tab can redeem. The
display then shows the new pending phone and requires the host to remove/replace
the old lease and approve the new one visibly; an unacknowledged takeover fails
closed and leaves the original controller in charge.

Host and controller credentials retain the fixed 43-character base64url wire
shape but are not opaque random strings: 16 random bytes are followed by a
16-byte truncated HMAC over purpose, room id and nonce. The Worker verifies that
binding before charging control reserve or calling a room object. Room storage
still keeps only a purpose-HMAC digest of the full credential. Cross-role,
cross-room, malformed and forged tokens therefore consume neither control units
nor object requests; a stolen authentic token retains only its original room/
role authority and remains an incident requiring room close.

## Close reasons and recovery

Stable ids are `invite_expired`, `invite_rotated`, `room_full`, `pending_full`,
`approval_rejected`, `host_closed`, `room_expired`, `protocol_update_required`,
`duplicate_controller`, `seat_reclaimed`, `rate_limited`, `service_budget_safe`,
and `transport_lost`. UI copy is platform-local; wire ids are never displayed
raw. Only update-required and budget-safe states suppress automatic retry.
Every automatic retry sequence is finite regardless of reason; this sentence
describes semantic recovery, not permission for an unbounded socket loop.

## Compatibility

No silent downgrade exists. A v1 host rejects unknown versions/types before any
state mutation and sends `protocol_update_required` over the reliable channel.
A new optional control capability may be ignored only when its declaration says
so; changing pad bytes, timing, flags or authority rules requires v2.
