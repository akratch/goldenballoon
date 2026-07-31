#!/usr/bin/env bash
#
# stamp_provenance.sh -- write a commit-bound provenance sidecar for one release
# asset.
#
# Ported from mgb64's scripts/release/stamp_provenance.sh (as tools/release/,
# mirroring mdkr64's tools/ top-level convention -- this repo has no
# top-level scripts/). Logic is unchanged except the artifact naming
# convention (mdkr64-<platform>-<version> instead of mgb64-<platform>-...)
# and the schema tag.
#
# Usage: tools/release/stamp_provenance.sh <asset-path> <version>
#
# Emits "<asset-path>.provenance.json" recording the asset's sha256 and the
# source commit + builder/run identity it was produced from. In GitHub Actions
# the commit + run identity come from the GITHUB_* env; locally they fall back to
# the current git HEAD and a "local-<os>" builder. verify_provenance.sh consumes
# these sidecars and fails a release CLOSED on any missing/stale/mismatched one.
#
# python-free by design: the sidecar is a flat JSON of controlled values (hex
# digests, a version/commit string, numeric run ids, a workflow name) written
# with printf so this can run under the MINGW64 msys2 shell (no python there).
set -euo pipefail

asset="${1:?usage: stamp_provenance.sh <asset-path> <version>}"
version="${2:?usage: stamp_provenance.sh <asset-path> <version>}"
[[ -f "$asset" ]] || { echo "ERROR: asset not found: $asset" >&2; exit 1; }

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}';
  else echo ""; fi
}
# Minimal JSON string escaping: drop backslashes and double-quotes so a controlled
# value can never break the flat JSON we printf below.
json_str() { printf '%s' "$1" | tr -d '\\"'; }

digest="$(sha256_of "$asset")"
[[ -n "$digest" ]] || { echo "ERROR: no sha256 tool (sha256sum/shasum) to stamp $asset." >&2; exit 1; }

commit="${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}"
[[ "$commit" != "unknown" ]] || { echo "ERROR: cannot resolve source commit for $asset." >&2; exit 1; }

if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
  builder="github-actions"
else
  builder="local-$(uname -s | tr '[:upper:]' '[:lower:]')"
fi
run_id="${GITHUB_RUN_ID:-local}"
run_number="${GITHUB_RUN_NUMBER:-local}"
workflow="${GITHUB_WORKFLOW:-local}"

base="$(basename "$asset")"
# Derive the platform label from mdkr64-<platform>-<version>.<ext>.
platform="$(printf '%s\n' "$base" | sed -E 's/^mdkr64-([A-Za-z0-9]+)-.*/\1/')"

out="${asset}.provenance.json"
cat > "$out" <<EOF
{
  "schema": "mdkr64-provenance/1",
  "artifact": "$(json_str "$base")",
  "builder": "$(json_str "$builder")",
  "commit": "$(json_str "$commit")",
  "platform": "$(json_str "$platform")",
  "run_id": "$(json_str "$run_id")",
  "run_number": "$(json_str "$run_number")",
  "sha256": "$(json_str "$digest")",
  "version": "$(json_str "$version")",
  "workflow": "$(json_str "$workflow")"
}
EOF
echo "stamped $out (commit ${commit:0:12}, sha256 ${digest:0:12}...)"
