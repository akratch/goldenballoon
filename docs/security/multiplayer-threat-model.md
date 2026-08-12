# Multiplayer threat model

Status: active security contract. Review on every protocol, origin, provider or
retention change. Local keyboard/gamepad play is outside the network trust
boundary and must remain available when every service is unavailable.

## Assets and boundaries

Protected assets are ROM/save bytes, controller authority, private-room
admission, gameplay integrity, account-free privacy, service availability and
the zero-spend ceiling. The launcher owns rooms, approval, seats and engine
lifetime. Game code consumes sanitized fixed-tick pad state and never sees URLs,
credentials, SDP, service SDKs or provider types. The controller page has no ROM,
save, wasm or game-asset route. Signaling is untrusted for gameplay authority.

## Abuse cases and mandatory controls

| Threat | Control | Executable evidence |
|---|---|---|
| QR screenshot/replay | 128-bit fragment capability, two-minute expiry, rotate on extend/reopen, revoke on sheet dismissal, pending cap, host phrase approval | Old capability fails after rotation/revocation; possession alone cannot control a seat |
| Match invite theft/replay | 128-bit fragment secret, ten-minute expiry, invite-only scope and leader-only generation-checked rotation; each join receives a distinct endpoint bearer | Old link/code, nonleader rotation and stale concurrent rotation fail without changing the room |
| Local source collision | Host-selected numbered seat, visible replacement label, generation-checked router lease; a reconnecting approved phone remains reserved and neutral | Keyboard/touch/gamepad cannot silently take over or share a phone-owned kart |
| Fallback-code guessing | Short TTL, purpose-isolated directory, pseudonymous requester rate limit, global admission ceiling, pending cap 8 and uniform rejection copy | Exhaustion never evicts approved controllers |
| Match command forgery/replay | 256-bit endpoint bearer contains 128-bit nonce + 128-bit room/role HMAC; Worker rejects forgery before reserve/object access, object stores a full purpose-HMAC digest and injects actor; exact revision, monotonic command id and eight-receipt window | Wrong/cross-room bearer consumes zero control units; cross-seat action, stale concurrent command and conflicting same-id replay cannot mutate |
| QR leaks in HTTP/referrer/log | Secret is URL fragment; remove with `history.replaceState` before network; `Referrer-Policy: no-referrer`; structured log allowlist | Capture corpus finds no capability |
| Pending-device flood | Eight pending, four approved, bounded body/SDP, per-room transition budget | Ninth pending gets typed refusal |
| Role/port forgery | Host assigns generation-checked `PadRouter` lease; state packets carry no port | Competing P1 claim and stale release reject |
| Stale/replayed input | DTLS peer binding, connection epoch, modulo sequence window, dedupe | Old epoch/current sequence reject |
| Malformed packet | 64-byte cap, atomic codec, strict version/type/flags/length/ranges/checksum | Every one-bit vector mutation rejects unchanged output |
| Held input on loss | Hidden/pagehide/channel loss sends neutral; host 250 ms timeout; overflow becomes neutral | Timeout/overflow each emit one neutral edge |
| Duplicate controller tab | Browser lock/broadcast lease, server connection generation, newest approved connection wins only with host-visible reclaim | Two publishers never share an epoch |
| Malicious SDP/control/socket body | Exact JSON schema, 64 KiB SDP and 16 KiB control caps, string/count/depth limits, no reflection; state-only MatchRoom sockets byte-count UTF-8 and binary messages against a 4 KiB cap before their mandatory close | 1 MiB request rejects before parse/storage; over-cap text and binary socket frames close with bounded code `4009` |
| Signaling MITM | HTTPS/HSTS plus DTLS fingerprints; 20-bit, two-compound-word pairing phrase derived from host/phone handshake transcript and verified by host approval | Substituted transcript changes phrase; both endpoints derive the same exact 20 bits |
| Native bootstrap impersonation | Originless native create requires exactly two version protocols plus one syntactically valid P-256 key; browser Origins are forbidden. Bootstrap is injected internally once, reconnect requires a room/role-bound 256-bit credential, and the object stores only its purpose-HMAC digest | Browser-origin, malformed-key, bootstrap-smuggling, wrong-room and forged reconnect negatives reject before room authority |
| Native TLS downgrade/trust-store drift | WSS requires certificate-chain and hostname verification against a dated, hash-pinned Mozilla extract; no insecure mode or implicit machine/Homebrew trust dependency. A hash-pinned MPL source patch keeps libdatachannel's Mbed TLS `VerifiedTlsTransport` active with that explicit CA on Windows instead of its upstream backend-agnostic fail-open branch. CA/library/patch updates require reviewed pin, notice and transport-vector changes | Build fails on archive/tag/patch/CA drift; clean MinGW compiles the patched branch; invalid chain/hostname cannot open the signaling socket |
| Peer IP disclosure | State clearly that direct WebRTC reveals peer network addresses; offer future relay-only privacy mode when capacity exists | Consent copy and mode telemetry contain no address |
| Relay observation | Direct WebRTC DTLS; any future WebSocket fallback adds reviewed end-to-end AEAD with transcript binding and erased keys | Relay fixture cannot decrypt or substitute frames |
| Quota/cost exhaustion | Separate pairing/control/relay reserves, fixed ceilings, kill switch, no billing method, fail closed with `service_budget_safe`; socket upgrades reserve their bounded message lifetime and each successful native host mutation/rotation reserves its additional fanout; one free `/api/`-only edge rule brakes single-IP floods before Worker invocation | Admitted work, repeated rotation and valid-credential floods cannot consume the internal close/control reserve; literal zero refuses first admission; static local routes do not match edge policy |
| Operations snapshot theft/abuse | Separate 256-bit-class bearer, same-origin check, constant-time comparison, absent-secret 404, no-store aggregate-only capacity/health schemas; unauthorized traffic never reads the budget object. Fixed operation buckets piggyback accepted writes, refuse mismatched v1 labels/weights and isolate legacy traffic without per-event labels or refusal-write amplification | Missing/wrong secret rejects; fixture payload contains no room, code, capability, credential, name or address canary; tracked units exactly reconstruct admitted units after restart/flood |
| Worker/Durable Object deploy skew | Versioned internal envelope; additive legacy+v1 readers; unknown version rejected before parse/storage; schema changes use expand/drain/emit/contract releases | 15-call source census, four boundary negatives, legacy direct-object and v1 full-Worker suites |
| Fabricated/incomplete $0 evidence | Exact size-bounded seven-day schema re-runs provider reconciliation and health weights, requires contiguous UTC dates, commit/deployment digests, local-play probes, closed incidents and daily/final GO; diagnostics never reflect input | Missing/skewed day, charge, partial tracking, open incident, false probe, schema canary and oversized-ledger negatives stop |
| XSS/supply chain | Dedicated origin, no user HTML, same-origin pinned assets, deny-default CSP, Trusted Types where supported, lockfile/license audit | Header and public-asset gates |
| Native callback/packet exhaustion | Transport callbacks enqueue only into bounded process-owned queues; signaling queue overflow fails the room, pad queue overflow revokes custody, state is capped at 64 bytes, MatchRoom inbound frames at 4 KiB and control/SDP at 16/64 KiB | Queue saturation and oversized payloads become typed failure/neutral rather than unbounded allocation, latency or held input |
| Service compromise | Store keyed credential digests, minimum room metadata, no packet/SDP history, ≤24 h room TTL enforced on request and by alarm | Storage snapshot/data map audit |

## Security invariants

- Never log or metric raw names, capabilities, fallback codes, credentials, IP
  addresses, SDP, DTLS fingerprints, input packets, ROM/save data or raw hashes.
- Names are plain text rendered with `textContent`, normalized and bounded; they
  never enter metric labels or protocol error text.
- Credential comparison is constant-time after keyed hashing. Keys come from
  the deployment secret store. The current schema uses drain-and-reissue key
  rotation; any future dual-key overlap requires an explicit versioned schema
  and review rather than an implicit fallback.
- Static routes never execute metered code. Credential responses use
  `Cache-Control: no-store`; controller HTML uses a restrictive CSP,
  `frame-ancestors 'none'`, nosniff, no-referrer and restrictive permissions.
- Unknown input fails closed without partial reducer/router mutation. Closing a
  room is idempotent and neutralizes controllers before releasing seats.
- No universal availability claim and no automatic paid upgrade. If free limits
  are unavailable, local play remains one click away with specific recovery.
- Durable code directories use purpose-separated HMAC keys and separate Party
  and Match shards. Neither room objects nor directory keys persist the raw
  six-digit code; storage-inspection tests enforce this boundary.
- Daily capacity shards store only bounded integer counters, twelve fixed
  reservation counts and two refusal latches, schedule deletion after 32 days,
  and expose them only through the separate operations bearer. Operation
  metrics reuse the accepted reservation write; a refusal flood writes each
  latch at most once and increments no metric.

## Review triggers

Threat-model sign-off is required before adding a provider, relay, account,
analytics field, third-party script, new credential role, longer retention,
protocol version, public matchmaking, native WebRTC library, CA bundle date or
cryptographic build configuration. Production launch also requires
dependency/SAST/secret scans, fuzzing, rate-limit load evidence, origin/header
capture, packaged-notice verification, incident runbook rehearsal and
credential rotation proof.
