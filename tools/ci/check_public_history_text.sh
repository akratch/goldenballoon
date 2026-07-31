#!/usr/bin/env bash
#
# check_public_history_text.sh -- scan reachable git history for high-risk text.
#
# Publication model this guard is written against: the public repository is a
# brand-new repository whose entire content is one fresh initial commit holding
# the tree that tools/create_public_launch_repo.sh cuts. It is not a filtered,
# rewritten, or mirrored copy of this repository's history, so no commit,
# branch, tag, or reflog entry from here is reachable from it.
#
# What that leaves to check is the text that reached a tracked file's CONTENT,
# scanned with `git log -G<pattern>` over reachable history. In the public
# repository that history is the single initial commit, so the scan costs one
# tree's worth of diff; run here, against the source history the cut is taken
# from, it catches the same text before it can reach the tree being cut.
#
# Commit MESSAGES are out of scope. The public repository carries exactly one
# commit message and tools/create_public_launch_repo.sh supplies it verbatim
# (--message, default "Initial public source release"), so no message text
# survives the cut and there is nothing for a message scan to find. Message
# text in this repository's own history is never published.
#
# Scan scope: being export-ignored from `git archive` does NOT exempt a path
# from this guard's history scan. The only paths this guard skips are paths
# explicitly marked "history-scan-exempt" in .gitattributes, between
# "--- BEGIN history-scan-exempt ---" / "--- END history-scan-exempt ---"
# comment markers -- see the path-scope block below. .gitattributes defines no
# such marker block, so every export-ignored path is still content-scanned
# here. Adding a path to that marker block requires the same explicit, written
# sign-off .gitattributes itself would require; an incidental new
# export-ignore line must never silently widen this guard's blind spot.
#
set -euo pipefail

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "SKIP: not a git checkout; no reachable history to scan."
  exit 0
fi

cd "$(git rev-parse --show-toplevel)"

# The forbidden-text classes live in one shared data file so this guard and
# tools/check_github_launch_ready.sh can never drift apart on what counts as a
# leak. Each non-comment line is one regex alternative; they are joined with
# `|` into a single POSIX ERE for `git log -G`.
denylist='tools/public_text_denylist.txt'
if [ ! -f "$denylist" ]; then
  echo "FAIL: missing shared leak ruleset: $denylist" >&2
  exit 1
fi
pattern="$(awk 'NF && $0 !~ /^[[:space:]]*#/ { printf "%s%s", sep, $0; sep = "|" }' "$denylist")"
if [ -z "$pattern" ]; then
  echo "FAIL: shared leak ruleset defines no patterns: $denylist" >&2
  exit 1
fi
pattern="(${pattern})"

# Path scope. Files that carry these detection patterns, or the internal-only
# path literals, as part of their own job (not a leak) are excluded so they do
# not self-flag. The exclusions cover history as well as the current tree, so a
# file stays listed for as long as any reachable commit of it embeds the text:
#   - tools/public_text_denylist.txt (the shared ruleset itself).
#   - this script (embedded the patterns verbatim before they moved to the
#     shared ruleset).
#   - tools/check_public_history_paths.py (embeds the docs/superpowers,
#     docs/archive, PUBLIC_LAUNCH_BACKLOG.md, brand/gb1.png, brand/gb2.png
#     path literals as its own detection list).
#   - tools/create_public_launch_repo.sh (embeds the same path literals in
#     its belt-and-suspenders leaked-internal-docs spot-check).
#   - .gitattributes (documents the export-ignore rule for these same paths
#     in prose comments).
# tools/check_github_launch_ready.sh reads the shared ruleset instead of
# embedding it, and every reachable version of it splits each sensitive token
# across adjacent string literals, so it never needs to be on this list.
pathspecs=(
  .
  ':!tools/public_text_denylist.txt'
  ':!tools/ci/check_public_history_text.sh'
  ':!tools/check_public_history_paths.py'
  ':!tools/check_native_sdk_surface.py'
  ':!tools/ci/check_release_ready.sh'
  ':!tools/create_public_launch_repo.sh'
  ':!.gitattributes'
)
if [ -f .gitattributes ]; then
  while IFS= read -r pat; do
    [ -n "$pat" ] && pathspecs+=( ":(glob,exclude)${pat}" )
  done < <(awk '
    /^[[:space:]]*# --- BEGIN history-scan-exempt/ { active=1; next }
    /^[[:space:]]*# --- END history-scan-exempt/   { active=0; next }
    /^[[:space:]]*#/ { next }
    active && /(^|[[:space:]])export-ignore([[:space:]]|$)/ { print $1 }
  ' .gitattributes)
fi

limit="${GE007_HISTORY_TEXT_HIT_LIMIT:-80}"
if ! history_output="$(git log --all -G"$pattern" \
  --pretty='format:commit %H %s' --name-only -- \
  "${pathspecs[@]}" 2>&1)"; then
  printf '%s\n' "$history_output" >&2
  exit 1
fi

hits="$(printf '%s\n' "$history_output" | awk 'NF' | sed -n "1,${limit}p")"

if [ -n "$hits" ]; then
  printf '%s\n' "$hits"
  echo "FAIL: high-risk text found in reachable git history." >&2
  echo "Rewrite or remove the reachable history before public launch." >&2
  exit 1
fi

echo "PASS: no high-risk private/proprietary text found in reachable git history"
