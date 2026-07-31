#!/usr/bin/env bash
#
# configure_github_launch_settings.sh -- apply or preview recommended GitHub
# repository settings for public maintenance.
#
# Default mode is a dry run. Pass --yes only after the printed operations have
# been reviewed.
#
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

repo=""
apply=0
skip_branch_protection=0
skip_security=0
actions_retention_days=14
enable_actions=0
required_checks=()
topics=(
  bring-your-own-rom
  decompilation
  game-preservation
  kart-racing
  n64
  native-port
  nintendo-64
  opengl
  reverse-engineering
  sdl2
  source-port
  webgpu
)
description="A decompilation and native source port of a 1997 Nintendo 64 kart racer, for research & preservation. Bring your own ROM - no copyrighted assets included."

usage() {
  cat <<'USAGE'
Usage: tools/configure_github_launch_settings.sh [options]

Dry-runs the recommended GitHub repository settings for public maintenance.
Pass --yes to actually apply settings.

Options:
  --repo OWNER/REPO       Repository to configure (default: gh repo view).
  --yes                   Apply changes. Without this, only print actions.
  --skip-branch-protection
                          Do not configure main branch protection.
  --skip-security         Do not attempt Dependabot/secret-scanning/private
                          vulnerability-reporting setup.
  --enable-actions        Enable hosted GitHub Actions. The default public repo
                          policy keeps Actions disabled and uses local
                          release_preflight evidence instead.
  --required-check NAME   Required status check for branch protection. Repeat
                          to add checks. Defaults to none because the public
                          repo policy uses local CI/preflight evidence.
  --actions-retention-days DAYS
                          Artifact/log retention for GitHub Actions. Defaults
                          to 14 and must be between 1 and 90 for public repos.
  -h, --help              Show this help.

This helper does not change repository visibility. Some security and
branch-protection endpoints may not be available for every repository/account;
the helper warns for those cases and tools/check_github_launch_ready.sh is the
public-surface verification gate.
USAGE
}

run_or_print() {
  if [ "$apply" -eq 1 ]; then
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
  else
    printf 'DRY-RUN:'
    printf ' %q' "$@"
    printf '\n'
  fi
}

run_or_warn() {
  if [ "$apply" -eq 1 ]; then
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    if "$@"; then
      :
    else
      echo "WARN: command failed; GitHub may not expose this setting for the current repository/account." >&2
    fi
  else
    printf 'DRY-RUN:'
    printf ' %q' "$@"
    printf '\n'
  fi
}

required_checks_overridden=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --repo)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      repo="$2"
      shift 2
      ;;
    --yes)
      apply=1
      shift
      ;;
    --skip-branch-protection)
      skip_branch_protection=1
      shift
      ;;
    --skip-security)
      skip_security=1
      shift
      ;;
    --enable-actions)
      enable_actions=1
      shift
      ;;
    --required-check)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      if [ "$required_checks_overridden" -eq 0 ]; then
        required_checks=()
        required_checks_overridden=1
      fi
      required_checks+=("$2")
      shift 2
      ;;
    --actions-retention-days)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      actions_retention_days="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if ! command -v gh >/dev/null 2>&1; then
  echo "Required command not found: gh" >&2
  exit 1
fi

if [ -z "$repo" ]; then
  repo="$(gh repo view --json nameWithOwner --jq '.nameWithOwner' 2>/dev/null || true)"
fi
if [ -z "$repo" ]; then
  echo "Could not resolve GitHub repository; pass --repo OWNER/REPO." >&2
  exit 2
fi

case "$actions_retention_days" in
  ''|*[!0-9]*)
    echo "--actions-retention-days must be an integer." >&2
    exit 2
    ;;
esac
if [ "$actions_retention_days" -lt 1 ] || [ "$actions_retention_days" -gt 90 ]; then
  echo "--actions-retention-days must be between 1 and 90 for a public repository." >&2
  exit 2
fi

if [ "$apply" -eq 0 ]; then
  echo "Dry run only. Pass --yes to modify GitHub."
else
  echo "Applying GitHub repository settings to ${repo}."
fi

topic_args=()
for topic in "${topics[@]}"; do
  topic_args+=(--add-topic "$topic")
done

run_or_print gh repo edit "$repo" \
  --description "$description" \
  --enable-issues \
  --enable-discussions \
  --enable-wiki=false \
  --delete-branch-on-merge \
  --allow-update-branch \
  --enable-squash-merge \
  --enable-merge-commit \
  --enable-rebase-merge \
  "${topic_args[@]}"

run_or_warn gh api -X PUT "repos/${repo}/actions/permissions/workflow" \
  -f default_workflow_permissions=read \
  -F can_approve_pull_request_reviews=false \
  --silent

run_or_warn gh api -X PUT "repos/${repo}/actions/permissions/artifact-and-log-retention" \
  -F "days=${actions_retention_days}" \
  --silent

if [ "$enable_actions" -eq 1 ]; then
  run_or_warn gh api -X PUT "repos/${repo}/actions/permissions" \
    -F enabled=true \
    -f allowed_actions=all \
    -F sha_pinning_required=true \
    --silent
else
  run_or_warn gh api -X PUT "repos/${repo}/actions/permissions" \
    -F enabled=false \
    --silent
fi

if [ "$skip_security" -eq 0 ]; then
  run_or_warn gh api -X PUT "repos/${repo}/vulnerability-alerts" --silent
  run_or_warn gh repo edit "$repo" --enable-secret-scanning
  run_or_warn gh repo edit "$repo" --enable-secret-scanning-push-protection
  run_or_warn gh api -X PUT "repos/${repo}/private-vulnerability-reporting" --silent
fi

if [ "$skip_branch_protection" -eq 0 ]; then
  protection_json="$(mktemp "${TMPDIR:-/tmp}/mdkr64-branch-protection.XXXXXX")"
  trap 'rm -f "$protection_json"' EXIT

  python3 - "$protection_json" "${required_checks[@]}" <<'PY'
import json
import sys

path = sys.argv[1]
checks = sys.argv[2:]
payload = {
    "required_status_checks": {
        "strict": True,
        "contexts": checks,
    } if checks else None,
    "enforce_admins": True,
    "required_pull_request_reviews": {
        "dismiss_stale_reviews": True,
        "require_code_owner_reviews": False,
        "required_approving_review_count": 1,
    },
    "restrictions": None,
    "required_conversation_resolution": True,
    "allow_force_pushes": False,
    "allow_deletions": False,
}
with open(path, "w", encoding="utf-8") as handle:
    json.dump(payload, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY

  if [ "$apply" -eq 0 ]; then
    echo "DRY-RUN: branch protection payload for repos/${repo}/branches/main/protection"
    sed 's/^/  /' "$protection_json"
    echo "DRY-RUN: gh api -X PUT repos/${repo}/branches/main/protection --input <branch-protection-json-above>"
  else
    run_or_warn gh api -X PUT "repos/${repo}/branches/main/protection" --input "$protection_json" --silent
  fi
fi

cat <<EOF

Next verification commands:
  NO_COLOR=1 tools/check_github_launch_ready.sh --repo ${repo}
  # For private mirrors or dry runs only:
  NO_COLOR=1 tools/check_github_launch_ready.sh --repo ${repo} --allow-private
EOF
