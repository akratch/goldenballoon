# Match launch descriptor v1

Status: **implemented through launcher, persistent session, engine runtime and
direct race load; evidence review remains.** The executable authority is
`platform/net/match_launch_descriptor.*`, the pure
`platform/online/match_launch_builder.*` adapter, plus
`platform/session/session_bridge.*`, `platform/app/session_runtime.*` and their
unit tests.

The descriptor freezes exactly the deterministic race choices that exist after
the launcher lobby reaches its Loading barrier. Matchmaking, votes, guest names,
controller sources, connection state and retry metadata stay out of the engine.

## Canonical 148-byte encoding

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `MLD1` |
| 4 | 1 | descriptor version `1` |
| 5 | 3 | zero reserved bytes |
| 8 | 112 | canonical `MdkrMatchManifestV1` bytes |
| 120 | 24 | four six-byte seat selections |
| 144 | 4 | big-endian FNV-1a-32 checksum over bytes 0–143 |

Each seat selection is big-endian `selection_revision:u32`, then
`character_id:u8` and `vehicle_id:u8`. Occupied slots are contiguous in stable
lobby seat-array order. Every unused tail slot is exactly
`revision=0, character=0xff, vehicle=0xff`.

Validation requires:

- the embedded match manifest is valid;
- every occupied selection has a nonzero revision, one of ten supported player
  characters and one of the three player vehicles;
- characters are unique and every chosen vehicle is present in the manifest's
  ROM-derived legal-vehicle mask;
- the Loading lobby and manifest agree on epoch, track, vehicle mask and slot
  count before an output byte is changed;
- any byte mutation fails checksum/decode atomically.

The descriptor never contains display names or source credentials. Stable
opaque slot owners remain in the embedded manifest; endpoint-local viewport and
physical-controller mappings remain in the session launch envelope.

## Session and engine handoff

`MdkrSessionLaunchV3` carries this whole descriptor plus endpoint-local seat and
viewport masks. Both `MdkrSessionBridge` and `SessionRuntime` validate and
copy-own it; mutation of the launcher source after admission cannot change the
engine loan. Duplicate characters, illegal vehicles, non-neutral envelope
bytes and wrong epochs reject without partial state. The laboratory
`MdkrSessionLaunchV2` still carries only the manifest and deliberately exposes
no descriptor, so it cannot masquerade as a selection-complete launch.

The copy-owned descriptor is published through the engine-lifetime runtime.
The exact retail race-load seam direct-loads its track, and racer initialization
reads the canonical character/vehicle arrays. Before authored tick zero, engine
admission revalidates the manifest track, race type, authored cadence and legal
vehicle capability mask against the loaded ROM; malformed selections fail at
descriptor admission. The real-ROM gate proves this without `MDKR_LOAD_TRACK`,
and the four-process V3 convergence gate proves identical match identity with
different endpoint-local controller/view maps.

No room, matchmaking, invite, vote, retry or display-name state crosses this
seam. Production online race admission remains disabled until the separate A3
`GO`; completing the descriptor path does not waive that release gate.
