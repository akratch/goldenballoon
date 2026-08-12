# Multiplayer session protocol v1

Status: implemented foundation contract. This document describes the C
authority in `platform/session/session_types.h`; numeric values in that header
are wire-stable for v1.

## Ownership

`MdkrSessionCore` is a synchronous, allocation-free reducer compiled into native
and wasm. A view reads `MdkrSessionState`, dispatches a generation-checked
`MdkrSessionCommand`, performs returned `MdkrSessionEffect` values through a
platform adapter, then reports resulting engine/room/connectivity state through
another command. Rendering state never performs an effect.

The dimensions are deliberately independent:

| Dimension | Values |
|---|---|
| Intent | none, local, private online |
| Scene | home, local setup, join, lobby, loading, race chrome, results, recovery |
| Engine | stopped, booting, ready, racing, finished, failed |
| Connectivity | offline, connecting, direct, forwarded, relayed, degraded, lost |
| Room | none, open, preflight, selecting, loading, countdown, racing, results, closed |
| Overlay | closed, controls, connection, leave confirmation |
| Update | none, waiting, required |

Invalid combinations fail atomically. In particular, online intent can never set
`engine_pause_requested`; opening online race chrome captures navigation without
emitting a pause effect.

## Generation and epoch rules

- Every command carries `protocol_version` and the exact state generation it
  observed. A stale command returns `STALE_COMMAND` without changing state or
  consuming an effect id.
- Every accepted command advances generation. Returned effects carry the
  committed generation, match epoch and a nonzero monotonic effect id.
- `session_id` belongs to the launcher-owned runtime and survives race, results,
  rematch and Return Home.
- A first race and every rematch increment `match_epoch`. Match inputs/results
  carrying a different epoch are rejected at the bridge.

## Engine bridge

`session_bridge.h` is the only launcher/game handoff. It accepts a fixed-size
`MdkrSessionLaunchV2`, complete canonical `MdkrInputSet` values and strict
engine phase changes; it emits bounded events and one validated
`MdkrMatchResultV1`. The launch envelope contains the one canonical
`platform/net/match_manifest.h` wire manifest plus endpoint-local seat and
viewport masks. Local input ownership and rendering are deliberately outside
the manifest: peers with different local seats/views—including a no-render
verifier—retain byte-identical synchronized match identity and digest. A view
may target only a locally owned seat; a local seat need not have a view.
This endpoint envelope is version 2 because v1's byte was reserved; v1 is
rejected rather than silently reinterpreted as a no-render request. The
enclosed session and match protocol versions remain v1.

The persistent native `SessionRuntime` owns the bridge above the blocking
engine call. Online boot fails closed unless the canonical manifest epoch is
exactly the session's current match epoch. The launcher installs a copied,
immutable `MdkrNetRoster` after all engine admission checks and clears it only
after all engine workers have joined; the game never borrows or mutates
launcher/session memory. Rematch must freeze a fresh envelope for the next
epoch and cannot silently reuse the previous match identity.

The bridge rejects:

- wrong launch version/size, invalid canonical manifest, unsupported 25/30 Hz cadence or ROM revision;
- zero/too-many players, invalid local/viewport masks, remote-only viewports and
  nonzero reserved fields;
- non-neutral absent/inactive pads, stick values outside authored `[-80, 80]`,
  inconsistent presence masks and non-increasing ticks;
- invalid engine phase transitions, duplicate finishing slots, wrong result
  epoch/player count and event-queue overflow.

Rejection occurs before mutation. There is no second session-specific struct
named `MdkrMatchManifestV1`; launcher, roster and rollback consume the same
contract. The event queue is fixed at 16 entries and
applies backpressure rather than overwriting evidence. Applying a new manifest
is allowed only while the bridge engine is stopped and atomically starts a new
event epoch. The higher launcher runtime additionally binds it to the exact
upcoming/current session epoch.

## Source boundary

`tests/check_multiplayer_boundaries.py` rejects service/provider vocabulary in
`game/src` and platform dependencies, allocation, environment/time access or
sockets in `session_core.c`. Its built-in synthetic violations are the mutation
control for the check itself.

## Evidence commands

```sh
cmake --preset dev
cmake --build build --target mdkr_session_core_test mdkr_session_bridge_test
ctest --test-dir build --output-on-failure \
  -R '^(session_core|session_bridge|multiplayer_boundaries)$'
```
