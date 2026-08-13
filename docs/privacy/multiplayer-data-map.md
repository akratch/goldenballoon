# Multiplayer data map

Data minimization is the default; “not collected” is an architectural boundary,
not a privacy-policy promise deferred to deployment.

| Field | Purpose/receiver | Retention | Logging |
|---|---|---|---|
| Invite capability | Redeem a pending controller or match endpoint | Raw value returned/held client-side only. `/controller/` scrubs its exact query-free fragment before configuration/probes and holds it only in closure memory; scrub failure abandons redemption, while explicit embedded/unsupported share or copy and duplicate-tab reclaim reconstruct the private URL only for that user gesture/new scrubbed navigation. The phone clears ordinary custody on redemption, terminal leave or page hide; only pre-redemption embedded/unsupported/duplicate recovery retains it. `/room/` crosses one same-origin redirect in the URL fragment, then the always-loaded launcher erases the URL and holds exact enabled-release handoff only in closure memory for ≤10 min—never web storage; disabled releases discard it immediately. A failed role/root URL scrub clean-navigates and abandons redemption. Joined Match custody ends at the conservative receipt-relative deadline, generation/leadership/phase loss or room close. Service retains only a purpose-HMAC digest until 2 min (controller), 10 min (match), rotation or room close | Never; even loopback-only test diagnostics record request field names, not values |
| Controller tab lease | Prevent two tabs in one browser profile from publishing the same scanned controller invitation | Document lifetime for Web Lock/broadcast ownership. The no-Web-Locks fallback uses one fixed origin-scoped localStorage record containing a SHA-256-derived 96-bit capability digest prefix, random tab id and authority expiry. Ownership expires 15 seconds after its last heartbeat; page hide/leave removes an owned record, and the next controller attempt removes or overwrites a crash-stale record (which may remain inert until that revisit or site-data clearing). Raw capability, room code, controller credential, device name and input never enter the channel or storage | Never |
| Structured credential / keyed digest | Client holds a 128-bit nonce + 128-bit room/role HMAC; Worker verifies it before metered control, room stores only a full purpose-HMAC digest | Credential lifetime; delete digest on room close | Never |
| Six-character code | Manual lookup to active invite | Raw value never stored; purpose-HMAC directory entry for 2 min (controller) or 10 min (match) | Never |
| Room id, phase, transition/invite generation | Route, rotate and recover room state | Active room, hard delete ≤24 h | Bounded phase counters only |
| Controller random id | Dedupe/reconnect within room | Room lifetime | Never raw |
| Optional device display name | Host seat recognition | Room lifetime; comfort copy may remain only on phone | Never |
| Seat, lease generation, connection epoch | Enforce input ownership | Room lifetime | Aggregate counts only |
| Protocol/capability versions | Compatibility | Connection/room lifetime | Bounded version counters |
| Match endpoint id, phase, selections, vote, revision/epoch | Authorize and recover private-room consensus | Active room, hard delete ≤24 h | Aggregate phase/error counts only |
| Peer public key, connection generation and reachability | Derive pairwise match keys and select a direct/one-hop path | Endpoint memory for one room epoch; public key may cross ephemeral signaling but is not stored | Never; no address, SDP or key dimension |
| Match peer envelope header | Route recipient-encrypted fixed input or reliable preflight fragment: authenticated payload type, epoch, source/destination/intermediate ids and generations, sequence | Endpoint memory inside a 64-sequence replay/forward window | Never |
| ECDH secret and direction-specific gameplay key | Source-authenticate and recipient-encrypt match input | Endpoint process only; native zeroizes on retirement, browser drops non-extractable handle on generation/epoch close | Never sent to service/forwarder; never logged |
| Match preflight attestation | Fixed 124-byte endpoint-to-endpoint agreement on descriptor/key transcript/canonical directed graph and local ROM/phrase/channel checks | Endpoint memory for one exact epoch and connection generation | Never centralized; no ROM hash/bytes, name, address, credential, SDP or input |
| Build/content compatibility bytes | Exact match admission; room members only | Active room, hard delete ≤24 h | Bounded version/result only; never raw diagnostics |
| Match command result tail | Retry/reconnect of launcher control state | At most 64 bounded results; room lifetime | Not separately logged |
| Match replay receipts/high-water fingerprints | Make exact retries idempotent and conflicting reuse fail closed; room object only | Eight receipts plus one high-water mark per endpoint; room lifetime; omitted from client state projection | Never |
| SDP/ICE | Establish a direct match or Phone Party connection; exact authenticated peer target and signaling service transiently | Forward only; no history/persistence | Never |
| Pairing transcript phrase | Human approval | Pending approval only | Never |
| Ephemeral P-256 pairing key | Host/phone derive the human comparison phrase | Public keys exist in active room/control state; private scalar exists only in endpoint process memory and is erased with that process/session | Never; private key is never sent to the service |
| Pad states/edges | Drive approved local seat; phone→host | In-memory eight-edge queue | Never |
| IP/network metadata | Network delivery by hosting/WebRTC providers | Provider minimum; not copied to app storage | Never in app logs/metrics |
| Error id/room phase/latency bucket | Reliability operations | Aggregate per documented window | Bounded ids/buckets only |
| Daily capacity aggregate | Enforce/reconcile the zero-spend stop line; authorized operator only | Pairing/control units, remaining units, refusal booleans and level; alarm-deleted 32 days after first mutation | Snapshot may be retained in the signed operations ledger; no identifiers or free-form labels |
| Daily reservation aggregate | Reconstruct admitted cost shape; authorized operator only | Thirteen fixed operation counts, including the separately weighted Match signaling socket, admitted/tracked units and legacy tracking state in the same 32-day budget shard | Snapshot may be retained in the signed operations ledger; no route, result, identifier or free-form label |
| Synthetic experience canary | Bound MatchRoom create/join and Phone Party direct setup/input RTT without observing users | Exactly 20 controlled attempts per daily lane; raw timings and ephemeral room/credential/network values discarded immediately after fixed aggregate calculation | Only attempts, successes and bounded p95 integers enter the signed beta ledger; no server metrics route or write |
| Seven-day beta ledger | Prove contiguous $0 qualification, synthetic experience bounds and local failure isolation | Seven exact aggregate days, including provider request/storage/whole-GB-s duration totals, commit/deployment digests and decision booleans; release-evidence retention | No raw deployment/account id, actor name, secret, route or free-form field; reviewer identity stays in access-controlled signed record |
| ROM, saves, game assets, audio, framebuffer | No multiplayer service purpose | Never sent/collected | Forbidden corpus |

Subject-facing controls are immediate room close/leave, optional-name deletion by
leaving, stored-ROM/save controls on the display, and browser site-data removal.
There is no account or cross-room profile in the zero-cost design. A remembered
controller stores only local comfort preferences and a revocable device hint on
the phone; it is not uploaded until a future, separately reviewed feature exists.
