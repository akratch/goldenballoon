#!/usr/bin/env bash
# Scan the exact commits introduced by a push. Input is Git's pre-push stdin.
set -euo pipefail

remote_name="${1:-origin}"
zero=0000000000000000000000000000000000000000

while read -r local_ref local_oid remote_ref remote_oid; do
  [ -n "${local_ref:-}" ] || continue
  [ "$local_oid" != "$zero" ] || continue  # deleting a ref publishes no blob

  commit_oid="$(git rev-parse "${local_oid}^{commit}" 2>/dev/null || true)"
  if [ -z "$commit_oid" ]; then
    echo "FAIL: $local_ref does not resolve to a commit" >&2
    exit 1
  fi

  if [ "$(git cat-file -t "$local_oid")" = "tag" ]; then
    python3 tools/check_public_surface.py --tag "$local_oid"
  fi

  if [ "$remote_oid" = "$zero" ]; then
    # A new branch/tag normally shares the public remote's existing history.
    # Scan only commits not already reachable there; rescanning the historical
    # repository would make a future guard unable to prevent new leaks.
    git rev-list --reverse "$commit_oid" --not --remotes="$remote_name" \
      | python3 tools/check_public_surface.py --commits-stdin
  else
    remote_commit="$(git rev-parse "${remote_oid}^{commit}" 2>/dev/null || true)"
    [ -n "$remote_commit" ] || {
      echo "FAIL: remote ref $remote_ref does not resolve to a commit" >&2
      exit 1
    }
    python3 tools/check_public_surface.py --range "$remote_commit..$commit_oid"
  fi
done
