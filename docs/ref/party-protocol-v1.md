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
| Open/Racing | setup dismissed | same | Revoke displayed invite; preserve approved leases |
| Open | host starts | Racing | Revoke invite; preserve approved leases |
| Racing | safe join enabled | Racing | Mint a scoped, two-minute invite |
| Open/Racing | host closes | Closed | Neutralize/revoke every lease and credential |
| Any live | TTL/budget exhausted | Closed | Typed close reason; no automatic paid fallback |

Controller admission is `Redeemed → Awaiting approval → Approved → Leased →
Connected`. Reject, expiry, host close, duplicate-tab loss and explicit leave go
to terminal `Closed`. Host approval names the exact numbered seat; an invalid or
already phone-leased seat fails without mutation. The launcher visibly labels
when that choice replaces keyboard, touch or a gamepad. Only a free,
generation-checked router seat can move to `Leased`; only a matching protocol
hello can move to `Connected`. Reconnect rotates the connection sequence without
changing the seat generation. While reconnecting, its lease remains reserved
and publishes neutral, so a local source cannot silently take it over. A stale
credential, epoch or lease can only be rejected—it cannot revive an earlier state.

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

## Compatibility

No silent downgrade exists. A v1 host rejects unknown versions/types before any
state mutation and sends `protocol_update_required` over the reliable channel.
A new optional control capability may be ignored only when its declaration says
so; changing pad bytes, timing, flags or authority rules requires v2.
