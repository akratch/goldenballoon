#!/usr/bin/env bash
#
# prepare_github_launch_evidence.sh -- collect GitHub-side launch blocker
# evidence for support tickets and repository replacement decisions.
#
# The report is written outside the repository by default. It may include stale
# remote commit SHAs from hidden pull-request refs, so do not commit the output.
#
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

repo=""
out=""

usage() {
  cat <<'USAGE'
Usage: tools/prepare_github_launch_evidence.sh [--repo OWNER/REPO] [--out PATH]

Collects a local Markdown report with:
  - current branch/head and origin/main state
  - local reachable-history provenance blockers
  - stale hidden refs/pull/* refs outside current branch history
  - latest hosted CI run/job/annotation summary, if any, for context only
  - full tools/check_github_launch_ready.sh --allow-private transcript

The report is intended for GitHub Support tickets or launch/replacement
decisions. It is not safe to commit because it can contain stale hidden-ref SHAs.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --repo)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      repo="$2"
      shift 2
      ;;
    --out)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      out="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

for cmd in gh git; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Required command not found: $cmd" >&2
    exit 1
  fi
done

if [ -z "$repo" ]; then
  repo="$(gh repo view --json nameWithOwner --jq '.nameWithOwner' 2>/dev/null || true)"
fi
if [ -z "$repo" ]; then
  echo "Could not resolve GitHub repository; pass --repo OWNER/REPO." >&2
  exit 2
fi

head_sha="$(git rev-parse HEAD)"
short_sha="$(git rev-parse --short=12 HEAD)"
branch="$(git branch --show-current)"
origin_main_sha="$(git rev-parse origin/main 2>/dev/null || true)"
generated_at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

if [ -z "$out" ]; then
  out="${TMPDIR:-/tmp}/mdkr64-github-launch-evidence-${short_sha}.md"
fi

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/mdkr64-launch-evidence.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT

reachable="$tmpdir/reachable.txt"
pull_refs="$tmpdir/pull_refs.tsv"
stale_refs="$tmpdir/stale_refs.tsv"
launch_output="$tmpdir/launch_check.txt"
history_paths_output="$tmpdir/history_paths.txt"
jobs_output="$tmpdir/jobs.txt"
annotations_output="$tmpdir/annotations.txt"

git rev-list HEAD > "$reachable"

remote_url="$(gh repo view "$repo" --json sshUrl --jq '.sshUrl // ""' 2>/dev/null || true)"
if [ -z "$remote_url" ]; then
  remote_url="https://github.com/${repo}.git"
fi

if ! git ls-remote "$remote_url" 'refs/pull/*' > "$pull_refs"; then
  echo "Could not read pull-request refs from $remote_url" >&2
  exit 1
fi

: > "$stale_refs"
while IFS=$'\t' read -r sha ref; do
  [ -n "$sha" ] || continue
  if ! grep -Fqx "$sha" "$reachable"; then
    printf '%s\t%s\n' "$ref" "$sha" >> "$stale_refs"
  fi
done < "$pull_refs"

set +e
tools/check_github_launch_ready.sh --repo "$repo" --allow-private > "$launch_output" 2>&1
launch_status=$?
python3 tools/check_public_history_paths.py --repo-root . > "$history_paths_output" 2>&1
history_paths_status=$?
set -e

run_id="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 \
  --json databaseId --jq '.[0].databaseId // ""' 2>/dev/null || true)"
run_sha=""
run_status=""
run_conclusion=""
run_url=""
if [ -n "$run_id" ]; then
  run_sha="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 \
    --json headSha --jq '.[0].headSha // ""' 2>/dev/null || true)"
  run_status="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 \
    --json status --jq '.[0].status // ""' 2>/dev/null || true)"
  run_conclusion="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 \
    --json conclusion --jq '.[0].conclusion // ""' 2>/dev/null || true)"
  run_url="$(gh run list --repo "$repo" --workflow CI --branch main --limit 1 \
    --json url --jq '.[0].url // ""' 2>/dev/null || true)"

  gh api "repos/${repo}/actions/runs/${run_id}/jobs" \
    --jq '.jobs[] | "- \(.name): status=\(.status) conclusion=\(.conclusion) runner_id=\(.runner_id // 0) steps=\((.steps // []) | length) id=\(.id)"' \
    > "$jobs_output" 2>/dev/null || true

  job_ids="$(gh api "repos/${repo}/actions/runs/${run_id}/jobs" \
    --jq '.jobs[].id' 2>/dev/null || true)"
  : > "$annotations_output"
  if [ -n "$job_ids" ]; then
    while IFS= read -r job_id; do
      [ -n "$job_id" ] || continue
      gh api "repos/${repo}/check-runs/${job_id}/annotations" \
        --jq '.[] | "- " + .message' >> "$annotations_output" 2>/dev/null || true
    done <<< "$job_ids"
  fi
fi

{
  printf '# MDKR64 GitHub Launch Evidence\n\n'
  printf '%s\n' "- Generated: \`${generated_at}\`"
  printf '%s\n' "- Repository: \`${repo}\`"
  printf '%s\n' "- Remote used for pull refs: \`${remote_url}\`"
  printf '%s\n' "- Current branch: \`${branch}\`"
  printf '%s\n' "- Current HEAD: \`${head_sha}\`"
  if [ -n "$origin_main_sha" ]; then
    printf '%s\n' "- Local \`origin/main\`: \`${origin_main_sha}\`"
  else
  printf '%s\n' "- Local \`origin/main\`: unavailable"
  fi
  printf '%s\n\n' "- Launch check exit status: \`${launch_status}\`"
  printf '%s\n\n' "- Local history-provenance check exit status: \`${history_paths_status}\`"

  printf '## Support Request Summary\n\n'
  printf 'Please purge the hidden pull-request refs listed below, any closed PR diff caches that keep them reachable, and any unreachable commit objects that are not reachable from current `main`.\n\n'
  printf 'Current public branch head to preserve: `%s`.\n\n' "$head_sha"
  printf 'Important: this support request can only address hidden GitHub refs and caches. If the local reachable-history provenance section below fails, preserving the current `main` history is not sufficient for public launch; publish from a fresh single-root launch repository or complete an approved history rewrite as well.\n\n'

  printf '## Stale Hidden Pull-Request Refs\n\n'
  if [ -s "$stale_refs" ]; then
    printf '| Ref | SHA |\n'
    printf '| --- | --- |\n'
    while IFS=$'\t' read -r ref sha; do
      printf '| `%s` | `%s` |\n' "$ref" "$sha"
    done < "$stale_refs"
  else
    printf 'No stale `refs/pull/*` refs were found outside current git history.\n'
  fi
  printf '\n'

  printf '## Local Reachable-History Provenance\n\n'
  printf '```text\n'
  cat "$history_paths_output"
  printf '```\n\n'

  printf '## Latest Hosted CI Run (Informational)\n\n'
  if [ -n "$run_id" ]; then
    printf '%s\n' "- Run id: \`${run_id}\`"
    printf '%s\n' "- Run URL: ${run_url}"
    printf '%s\n' "- Head SHA: \`${run_sha}\`"
    printf '%s\n' "- Status: \`${run_status:-unknown}\`"
    printf '%s\n\n' "- Conclusion: \`${run_conclusion:-unknown}\`"
    if [ -s "$jobs_output" ]; then
      printf '### Jobs\n\n'
      cat "$jobs_output"
      printf '\n\n'
    fi
    if [ -s "$annotations_output" ]; then
      printf '### Annotations\n\n'
      cat "$annotations_output"
      printf '\n\n'
    fi
  else
    printf 'No hosted CI run was found.\n\n'
  fi

  printf '## Full Launch Readiness Transcript\n\n'
  printf '```text\n'
  cat "$launch_output"
  printf '```\n'
} > "$out"

echo "Wrote launch evidence report: $out"
if [ -s "$stale_refs" ]; then
  echo "Stale hidden pull-request refs found: $(wc -l < "$stale_refs" | tr -d ' ')"
else
  echo "No stale hidden pull-request refs found."
fi
exit 0
