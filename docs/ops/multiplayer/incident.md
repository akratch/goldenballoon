# Multiplayer incident runbook

## First five minutes

1. Preserve local play: do not remove, gate or delay **Play here**.
2. Disable new affected admission with the narrowest kill switch; preserve
   authenticated close/control if safe.
3. Name severity, incident lead, UTC start, current/previous deployment ids and
   affected journey. Use aggregate evidence only.
4. If confidentiality or credential integrity may be affected, rotate invites,
   close rooms and invoke [rollback](rollback.md). Do not download room storage
   into an ad-hoc debugging workstation.
5. Publish player copy with one action: retry later or Play here. Never suggest
   repeated code attempts during rate/capacity incidents.

## Triage map

| Symptom | Inspect | Safe immediate action |
|---|---|---|
| Pair/create failures | weighted budget, provider quota, origin/secret binding | Stop admission; retain local/control. |
| QR works, direct channel fails | static/Worker protocol, ICE signaling bounds, browser versions | Rotate/retry once; do not enable an unreviewed relay. |
| Controller holds input | lifecycle neutral metrics, channel epoch, host timeout | Close affected room; disable Phone Party admission. |
| Phrase mismatch | static/Worker version, key/transcript substitution | Decline phone, rotate invite, investigate integrity. |
| Wrong seat/source | room lease generation, PadRouter ownership | Close room; do not manually reassign around stale state. |
| Elevated fallback guessing | directory rate buckets, distributed volume | Disable code entry while retaining QR if reviewed. |
| Edge rate-limit spike | Security Events aggregate, NAT/device cohort, Worker vs edge counters | Keep the `/api/`-only scope; raise threshold only with measured legitimate bursts, otherwise stop admission. |
| Unexpected usage/cost | provider usage and bindings vs capacity plus fixed reservation aggregate | Disable all new network admission; remove paid path. |
| Health tracking partial/invalid | legacy bucket, tracked/admitted units, Worker/object versions | Hold promotion; drain old Worker or roll back the compatible pair. |
| Desync/rollback spike | manifest cohorts, platform/cadence, correction depth | Disable online races only; preserve local/Phone Party. |

## Evidence handling

Allowed: deployment/build/protocol ids, bounded error/phase/transport buckets,
aggregate latency/loss/rollback distributions and redacted header names.
Forbidden: names, room ids, IP/address candidates, URLs/fragments, codes,
credentials/digests, SDP, packets, hashes tied to a room, ROM/save/snapshot/audio
or framebuffer data. If forbidden data appears, treat it as a privacy incident,
stop collection and follow the deletion drill.

Close only after the fixed path and its broken-direction control pass, quotas
reconcile to zero currency cost, rooms created during verification expire/delete,
player copy is removed, and a dated follow-up owns every systemic correction.
