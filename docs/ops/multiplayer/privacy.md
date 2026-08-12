# Multiplayer privacy verification

Use with the normative [data map](../../privacy/multiplayer-data-map.md).

## Release review

- Diff every request/response/storage/log/metric field. A new field is denied
  until purpose, receiver, retention, logging and deletion are documented.
- Capture create/redeem/connect/approve/rotate/close and confirm fragments never
  enter HTTP, referrer, history, analytics or error reporting.
- Inspect Durable Object storage: keyed digests and minimum room state only;
  no bearer, phrase, SDP/ICE history, pad input or address metadata.
- Inspect static controller dependency graph/cache: same-origin pinned assets,
  no game engine/ROM asset, trackers, fonts, CDN scripts or embeds.
- Verify provider observability is off or allowlisted to aggregate fields and
  the provider's minimum network logs/retention are recorded.
- Read the capacity and health snapshots only through the secret-injected
  operations job. Confirm their exact versioned schemas have no room id, code, capability,
  credential, name, address or free-form label; confirm `no-store`, unauthorized
  denial, fixed operation buckets only, and the 32-day daily-shard deletion
  alarm. Require health tracking to become complete after the legacy drain.

## Deletion drill

1. Close a room and verify credentials/controllers become unusable immediately.
2. Confirm room alarm hard-deletes storage by 24 hours even without another
   request. Confirm fallback codes disappear at two minutes and requester rate
   buckets at ten minutes through directory alarm cleanup.
3. Verify no application log/metric can reconstruct a room or person.
4. Trigger an expired test budget shard alarm and verify its storage is empty.
5. Delete browser site data and confirm only local comfort preferences/ROM/save
   custody described by the main product are affected; there is no account or
   server profile to delete.

The [beta ledger](beta-ledger.md) stores only exact aggregates, source/deployment
digests and booleans. Its controlled experience canary discards raw timings,
room values, credentials and network metadata after calculating fixed counts
and p95 integers; it never observes production users or writes a service-side
metric. Reviewer identity and raw deployment ids stay in the access-controlled
signed evidence record; credentials/provider envelopes never enter either
artifact.

Any raw secret/name/address/SDP/input appearance is a stop-ship incident, not a
documentation exception.
