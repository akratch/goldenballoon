# Party service deploy runbook

Status: ready for a named production owner; no production deployment is claimed.

**Owners start at [docs/multiplayer/DEPLOY_PHONE_PARTY.md](../../multiplayer/DEPLOY_PHONE_PARTY.md).**
That page is the three-command path — `wrangler login`, `tools/deploy_party.sh`,
`tools/verify_party_deploy.py` — and covers the one value to fill in, the two
secrets, the custom domain, the compiled launcher origin and rollback. This
page remains the authority for everything it does not automate: durable-data
compatibility, Worker/object version skew, the staged preview exercises, the
canary and promotion rules, and the reconciliation gates.

The one-command path implements stages 1-3 and 14's rule review, and reports
the same evidence:

- `tools/deploy_party.sh` records the commit and refuses a dirty tree, requires
  Node 22+ and the lockfile-pinned Wrangler, runs `npm ci && npm run check`,
  generates `services/party/wrangler.production.jsonc` from the tracked config
  by substituting the single `PARTY_DOMAIN`, runs the gates below, inspects the
  `--dry-run` binding census, requires both secret names to exist, deploys, and
  prints the verification command. `--dry-run` needs no account.
- `tests/check_party_production_config.py` holds `env.production`: the
  custom-domain route, `workers_dev`/`preview_urls`/observability off, the four
  Durable Object bindings and v1/v2/v3 migrations restated identically to the
  defaults, `/api/*` as the only `run_worker_first` prefix, and the controller
  and party-host static routes. `--require-real-domain` makes a surviving
  placeholder a hard deploy-time failure.
- `tools/verify_party_deploy.py --origin https://…` performs the phone-free
  post-deploy verification: headers, controller assets, room create, invite
  redeem, the WSS offer/answer/ICE round-trip with service-stamped controller
  identity, the fallback code, rotation invalidating the old link and code,
  origin refusal, static burst survival and the dist/web build match.
  `--self-test` runs all of it against a real `wrangler dev --local` Worker.

## Preconditions

- A dedicated custom Party origin serves both static controller assets and API,
  with no billing method/paid-overage path attached to the project.
- Node 22+ and the lockfile-pinned Wrangler are used. The account, zone and
  route are explicitly inspected before any mutation.
- `PARTY_HMAC_KEY` is at least 32 random bytes stored only in the provider secret
  store. It never appears in shell history, CI output, files or screenshots.
- `OPS_READ_TOKEN` is an independent random secret of at least 32 characters,
  injected only into the operations verification job. It is never a Wrangler
  plain-text var, browser value, command-line argument or substitute HMAC key.
- `PARTY_ORIGIN`, `MAX_ADMISSIONS_PER_DAY` and `CONTROL_RESERVE_PER_DAY` are
  reviewed against [capacity](capacity.md). `PARTY_ORIGIN` is exactly the
  canonical HTTPS origin with no trailing slash, path, query, fragment or
  credentials; HTTP is accepted only for loopback development. The placeholder
  `.invalid` origin is a deliberate fail-closed default.
- The zone's existing `http_ratelimit` entry-point ruleset has been exported and
  reviewed. Free plans allow one rule; do not overwrite an unrelated rule.
  `services/party/ops/free-rate-limit-rule.json` is the desired `/api/`-only
  payload and contains no account, zone or credential value.
- Current main has green CTest, Party service, browser direct-path, headers,
  dependency/license and source-boundary gates.
- `dist/web/online/online-control-config.js` is still disabled unless the
  evidence record contains written A3 `GO`, UX-03 sign-off and named security,
  privacy, capacity and operations approvals. Room-control activation and race
  admission are independent gates.

## Durable-data compatibility

- A first deployment may apply the additive `v2` Phone Party and `v3`
  MatchRoom migrations together because no production deployment is currently
  claimed. For any later upgrade, inventory live object versions first.
- The `v2` room reader converts a legacy raw Phone Party fallback code into its
  purpose-HMAC digest on first access. The old directory key is intentionally
  not dual-read. Disable new pairing, wait at least the full two-minute code
  TTL, then deploy; QR/direct invites and approved controllers use their own
  capabilities and continue under the compatibility matrix.
- Rotating `PARTY_HMAC_KEY` invalidates invite, code and endpoint/host/controller
  proofs. Treat it as a security incident or planned room-drain operation:
  disable admission, wait for room retention or explicitly close rooms, rotate,
  then issue entirely new links and credentials. Never run two keys implicitly.

## Worker/object version skew

Cloudflare documents that a new Worker can call a Durable Object still running
old code for seconds to minutes, and that stored state is not versioned or
rolled back with Worker code. The current internal contract is therefore
explicit v1: every one of the 16 Worker→object calls sends
`x-mdkr-internal-api: 1`; all four object classes accept both v1 and the frozen
legacy-unversioned request, while rejecting any unknown version before parsing
or storage access. New Worker→old object works because the old object ignores
the additive header; old Worker→new object works because legacy remains
accepted. See Cloudflare's [known-issue guidance](https://developers.cloudflare.com/durable-objects/platform/known-issues/#code-updates),
[version/deployment model](https://developers.cloudflare.com/workers/versions-and-deployments/),
and [rollback limits](https://developers.cloudflare.com/workers/versions-and-deployments/rollbacks/#bindings).

For any future internal v2, use separate releases:

1. Expand readers to accept legacy, v1 and v2; continue emitting/writing v1.
2. Disable new admission and wait the full 24-hour room lifetime so every old
   Worker/object/socket can drain. Prove current rooms close and new code can
   reconstruct every old stored schema.
3. In a later release, emit v2 while readers and rollback code still accept
   both v1 and v2. New stored fields are additive; do not change a stored schema
   discriminator or delete an old field.
4. Contract only in a separately approved release after the rollback window and
   stored-data retention have elapsed. Keeping the legacy reader indefinitely
   is preferred to a risky cleanup.

Do not use a percentage gradual deployment for this v1 full-stack Worker. The
browser cannot reliably attach version affinity to the initial HTML and all
bootstrap asset requests, and Durable Objects still have one-version-at-a-time
assignment. Promote one immutable version only after preview and compatibility
proof. Never combine a class lifecycle migration, internal protocol change,
stored-schema contraction, static-client protocol change or quota-policy change
in one release. Cloudflare also forbids rollback across a Durable Object
lifecycle change; this is why additive class migrations are isolated.

## Stage

1. Record commit, clean/dirty state, Node/Wrangler versions and current deployed
   version. Never deploy an unexplained dirty worktree.
2. Run
   `(cd services/party && npm ci && npm run check)`.
3. Run pinned Wrangler `deploy --dry-run`; archive bundle size and bindings.
   Only `PARTY_ROOMS`, `PARTY_BUDGETS`, `PARTY_CODES`, `MATCH_ROOMS` and
   documented vars/secrets are allowed. The static-assets manifest must point
   at the reviewed `dist/web` release and route only `/api/*` through the Worker.
   Confirm neither secret is present in the bundle, manifest or dry-run output.
   Run `python3 tests/check_party_internal_api.py`; require the 16-call census,
   v1 envelope and four pre-storage rejection guards. The service suite's
   ordinary direct-object tests are the legacy-old-Worker arm; full Worker tests
   are the v1-new-Worker arm; unknown-version tests are the fail-closed arm.
   Run `python3 tests/check_party_edge_policy.py` and archive its PASS line.
4. Publish static assets to a non-production preview. Capture response headers
   for `/`, `/controller/`, controller scripts and all credential responses.
   Confirm the publisher policy is disabled, opening Online Room makes no
   `/api/` request and `mdkr-online-tools` remains unloaded.
5. Exercise create → two redeems of one invite → phrase compare → approve →
   direct channel → reconnect → rotate → old QR/code rejection → close.
6. Exercise wrong origin, oversize JSON/SDP, invalid public key, ninth pending,
   fifth approval, fallback guessing, stale epoch and quota-safe refusal.
7. With online admission still disabled, exercise MatchRoom create → link join
   → code join → selections/vote/ready → load/cancel → load/race/results/rematch
   → close. Evict the object in each phase and verify exact state restoration;
   inspect storage for absence of raw code, invite and endpoint credentials.
8. Rotate a MatchRoom invitation: old link/code, nonleader and stale generation
   fail; new link/code both join and room membership remains intact.
9. Inspect an authenticated MatchRoom state response: reducer receipts,
   command high-water marks and fingerprints must also be absent. Submit a
   non-Join command with compatibility bytes and require `invalid_command`;
   this prevents alternate replay encodings across C and TypeScript.
10. Run the browser activation gate. It must prove clean-build/local-ROM
    derivation, exact NTSC/PAL revision/cadence mapping, one-time model loading,
    same-origin enforcement and dirty-build refusal without contacting `/api/`.
11. Only after every activation precondition is signed, create a clean canary
    commit that changes the static policy to `enabled: true`. Do not enable it
    through remote configuration, query strings or service state. Publish the
    static canary before changing any service admission policy.
12. Through the secret-injected operations job, read `/api/ops/capacity` once.
    Require the documented schema, `no-store`, no identity/capability fields,
    and reconciliation with the staged actions. Missing configuration must be
    `404`; missing/wrong credentials must be `401` and must not query a budget
    object. Export the same UTC day's normalized provider usage/billing
    aggregate and run the [strict $0 reconciliation gate](reconciliation.md).
    Require `PASS` before continuing. Archive only approved aggregates and the
    bounded pass line, never request headers, tokens or raw provider envelopes.
    Read `/api/ops/health` once in the same checkpoint; require exact health
    schema v2, the same UTC day, `tracking: complete`, `legacy: 0`, all thirteen
    fixed buckets, and exact tracked/admitted units.
13. Run the browser publish-skew gate. Require an old active/new waiting worker,
    isolated build caches, complete old-build offline fallback after new HTML
    has been observed, and new-build activation only after the old document is
    gone. Never add `skipWaiting()` or `clients.claim()` to accelerate rollout.
14. In preview, apply or reconcile the reviewed zone rate-limit payload through
    the Rulesets API with a least-privilege `Zone WAF Write` deployment secret.
    Never paste the token into the JSON, shell history or evidence. Send 30
    `/api/` requests from the test client in 10 seconds and require normal
    responses; the next request must be an edge `429` without a Worker capacity
    increment. Root/controller/room/static requests must remain 200. Wait the
    10-second mitigation, require recovery, inspect shared-NAT false positives,
    then promote the identical rule. If the zone already uses its one free rule,
    stop for an explicit owner decision—do not delete or merge it implicitly.

## Promote and verify

Promote one immutable version. Do not combine a Worker migration, static-client
protocol change and quota-policy change unless the compatibility matrix proves
both old/new pairings. Verify from two networks and physical iOS/Android:

- current QR and fallback code pair; previous ones fail;
- exact 20-bit phrase matches; a substituted key changes it;
- pad state is direct and service logs/storage contain no SDP/input/name/secret;
- signaling loss does not stop an established direct controller;
- local Play here works with DNS/API blocked;
- admission/refusal counters and control reserve are within the signed budget.

Keep the previous compatible deployment addressable until the observation
window closes. A static page must never cache a credential response.

For beta promotion, retain the canary cohort and complete the strict
[seven-day $0 ledger](beta-ledger.md). Require its validator `PASS` plus the
separate signed human `GO`; a local fixture pass or seven noncontiguous days is
not promotion evidence.

To roll back control UX, publish the immutable build whose static policy is
disabled, verify the shared-C module is no longer requested on a clean profile,
then disable new MatchRoom admission. Do not wait for Worker propagation before
restoring the local-only launcher surface.

## Stop conditions

Immediately disable new admission and enter the rollback runbook for secret or
name leakage, unexpected bindings/log fields, paid billing attachment, missing
control reserve, origin bypass, old-invite acceptance, seat spoofing, non-neutral
disconnect, sustained error-rate regression or incompatible static/Worker mix.
