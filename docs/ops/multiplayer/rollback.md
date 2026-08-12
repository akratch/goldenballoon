# Multiplayer service rollback runbook

Rollback is version compatibility plus verification, not merely selecting an
older Worker.

1. Republish the compatible static launcher with
   `online-control-config.js` disabled, then disable new pairing/online
   admission; leave local Play here visible. Preserve
   close/control only if the incident does not compromise those credentials.
2. Record current static build, Worker version, schema migration tags, secret
   version and aggregate active-room count. Do not copy room payloads.
3. Select the last version compatible with the currently served static protocol
   and Durable Object schema. SQLite class migrations are forward-only; never
   pretend a code rollback removes stored schema.
4. Promote the compatible prior Worker/static pair or keep admission disabled.
   Do not mix versions opportunistically.
5. Rotate the HMAC secret and close existing rooms when credential integrity is
   implicated. Rotation invalidates sessions by design; explain that phones
   must scan again.
6. Run wrong-origin/body, create/redeem/approve/direct/neutral/rotate/close and
   old-secret rejection checks. Verify local play with service blocked.
7. Re-enable a small admission cohort only after the
   [strict $0 reconciliation gate](reconciliation.md) passes and provider
   usage, reserve, privacy fields and error rates agree. Expand deliberately.

If no compatible prior version exists, remaining disabled is the correct $0,
privacy-safe outcome. Never restore service through an unreviewed paid relay,
looser origin, longer credential, disabled rate limit or raw diagnostic logging.
Any rollback, open incident, charged day, partial health tracking or failed
local-play probe invalidates the current beta qualification window; begin a new
contiguous [seven-day ledger](beta-ledger.md) after recovery.
