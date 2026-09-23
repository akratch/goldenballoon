# Dependency security review — 2026-09-23

The previous lockfile reported six npm audit findings (four high, two moderate).
These represented two underlying advisories and their dependent packages, not
six independent runtime defects. All dependencies in this service manifest are
development dependencies; deployment-tool exposure still matters.

- Vitest 4.1.10 / mocker: update to 4.1.11 addresses
  [GHSA-82fw-gwwq-j7x9](https://github.com/advisories/GHSA-82fw-gwwq-j7x9).
  The advisory concerns redirect-mock file reads through development-server
  interfaces. This project uses `vitest run`, not a public mocker server.
- Miniflare's pinned Sharp 0.35.2: the scoped override to 0.35.4 addresses
  [GHSA-rgj7-g3m4-5g8c](https://github.com/advisories/GHSA-rgj7-g3m4-5g8c),
  covering vulnerable libheif dependencies. The audit propagates this finding
  through Miniflare, Wrangler and the Cloudflare Vitest pool. No production
  image-processing service was identified in this project's source.

Keep the override scoped to Miniflare. Remove it when the selected compatible
Miniflare version itself pins a patched Sharp, then regenerate the lockfile,
audit, and rerun the service checks. Do not accept npm audit's suggested major
downgrade of the Cloudflare test pool merely to reduce the advisory count.

Validation: clean `npm ci` with Node 22.20.0 / npm 10.8.2 reported zero findings.
`npm run check` passed TypeScript, 17 test files / 152 tests, and QR round-trip
decoding. No deployment was performed. These are local service checks, not proof
of production deployment or platform-wide absence of vulnerabilities. Audit
results are time-sensitive and must be refreshed before release.

Lockfile review also identified transitive refreshes beyond those two direct
remediations: Vite 8.2.1 to 8.3.0, Rolldown 1.2.3 to 1.2.10 and its platform
bindings, Oxc types, Nano ID, Picomatch and PostCSS. These are included in the
tested lockfile; the update is not claimed to change only Vitest and Sharp.
A subsequent Node 22.20.0 check again passed all 152 tests, TypeScript and QR
decoding. The latest local audit reported zero known vulnerabilities.
