# Multiplayer operations

These runbooks preserve two promises: local play survives every service failure,
and the hosted profile cannot silently become a bill. They are executable
checklists, not proof that production has been deployed. Record every run in the
matching `docs/evidence/multiplayer/` ledger with actor, UTC time, commit,
provider deployment id and command output.

| Runbook | Use it for |
|---|---|
| [deploy](deploy.md) | Provision, stage, verify and promote the Party service/static client. |
| [capacity](capacity.md) | Admission weights, reserves, alarms and the $0 stop line. |
| [observability](observability.md) | Bounded reservation health, privacy rules and alarm interpretation. |
| [reconciliation](reconciliation.md) | Compare privacy-safe internal/provider aggregates and enforce zero charge. |
| [beta ledger](beta-ledger.md) | Run the aggregate-only experience canary and validate seven contiguous hosted $0 evidence days. |
| [incident](incident.md) | Triage without exposing secrets or sacrificing local play. |
| [privacy](privacy.md) | Data-map verification, provider settings and deletion drills. |
| [rollback](rollback.md) | Disable admission, restore a compatible deployment and verify recovery. |

Production actions require an authorized human. Tests and dry-runs may be
automated; this repository never assumes credentials or deploy authority.
