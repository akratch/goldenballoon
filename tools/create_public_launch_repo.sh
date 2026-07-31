#!/usr/bin/env bash
#
# create_public_launch_repo.sh -- build the fresh single-root launch repository
# from the current committed tree.
#
# This is the only supported way to produce the public repository: a brand-new
# repository whose entire content is one initial commit holding the launch
# tree. No commit, branch, tag, or ref of this repository is ever pushed to the
# public remote, and the public repository is never a filtered or rewritten
# copy of this history. The repository this script writes is the artefact that
# gets pushed; publishing is the single `git push` of its one commit to a fresh
# empty GitHub repository.
#
# Running it is non-destructive: it writes a separate repository under /tmp
# unless --out is provided, and touches no GitHub state.
#
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

out=""
message="Initial public source release"
smoke_archive=0
jobs="${MDKR64_BUILD_JOBS:-4}"
max_warnings=0
report=""
author_name="${MDKR_LAUNCH_AUTHOR_NAME:-MDKR64 Launch Builder}"
author_email="${MDKR_LAUNCH_AUTHOR_EMAIL:-mdkr64-launch@example.invalid}"
committer_name="${MDKR_LAUNCH_COMMITTER_NAME:-$author_name}"
committer_email="${MDKR_LAUNCH_COMMITTER_EMAIL:-$author_email}"
committer_name_explicit=0
committer_email_explicit=0

usage() {
  cat <<'USAGE'
Usage: tools/create_public_launch_repo.sh [--out DIR] [--message MESSAGE] [--smoke-archive]

Creates the fresh local git repository with a single root commit containing the
current HEAD tree, then runs public release/history guards inside that repo.

The output repository is the one that gets published. It does not include old
commits, hidden pull-request refs, local ignored files, internal documentation,
or generated assets.

Options:
  --out DIR             Output repository directory. Must be empty or absent.
  --message MESSAGE     Root commit message.
  --smoke-archive       Build and test the source archive from the clean repo.
  --jobs N              Parallel jobs for archive smoke (default: MDKR64_BUILD_JOBS or 4).
  --max-warnings N      Archive warning threshold (default: 0).
  --report PATH         Write a Markdown evidence report to PATH.
  --author-name NAME    Root commit author name (default: MDKR_LAUNCH_AUTHOR_NAME or placeholder).
  --author-email EMAIL  Root commit author email (use a GitHub noreply address for launch attribution).
  --committer-name NAME Root commit committer name (default: author name).
  --committer-email EMAIL
                        Root commit committer email (default: author email).
USAGE
}

require_non_negative_int() {
  local name="$1"
  local value="$2"
  case "$value" in
    ''|*[!0-9]*)
      echo "${name} must be a non-negative integer, got: ${value}" >&2
      exit 2
      ;;
  esac
}

require_positive_int() {
  local name="$1"
  local value="$2"
  require_non_negative_int "$name" "$value"
  if [ "$value" -eq 0 ]; then
    echo "${name} must be a positive integer, got: ${value}" >&2
    exit 2
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --out)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      out="$2"
      shift 2
      ;;
    --message)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      message="$2"
      shift 2
      ;;
    --smoke-archive)
      smoke_archive=1
      shift
      ;;
    --jobs)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      jobs="$2"
      shift 2
      ;;
    --max-warnings)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      max_warnings="$2"
      shift 2
      ;;
    --report)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      report="$2"
      shift 2
      ;;
    --author-name)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      author_name="$2"
      if [ "$committer_name_explicit" -eq 0 ]; then
        committer_name="$author_name"
      fi
      shift 2
      ;;
    --author-email)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      author_email="$2"
      if [ "$committer_email_explicit" -eq 0 ]; then
        committer_email="$author_email"
      fi
      shift 2
      ;;
    --committer-name)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      committer_name="$2"
      committer_name_explicit=1
      shift 2
      ;;
    --committer-email)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      committer_email="$2"
      committer_email_explicit=1
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

require_positive_int "--jobs" "$jobs"
require_non_negative_int "--max-warnings" "$max_warnings"
if [ -z "$author_name" ] || [ -z "$author_email" ] || [ -z "$committer_name" ] || [ -z "$committer_email" ]; then
  echo "Launch author/committer names and emails must be non-empty." >&2
  exit 2
fi
# Allowlist, not a denylist of specific personal addresses: the root launch
# commit's author/committer identity must be a GitHub noreply address or the
# placeholder domain, never a real personal inbox of any kind.
case "$author_email" in
  *@users.noreply.github.com|*@example.invalid) ;;
  *)
    echo "Refusing to create a public launch commit with a personal email address for the author; use a GitHub noreply address or the placeholder domain." >&2
    exit 1
    ;;
esac
case "$committer_email" in
  *@users.noreply.github.com|*@example.invalid) ;;
  *)
    echo "Refusing to create a public launch commit with a personal email address for the committer; use a GitHub noreply address or the placeholder domain." >&2
    exit 1
    ;;
esac

dirty="$(git status --porcelain --untracked-files=all)"
if [ -n "$dirty" ]; then
  echo "Refusing to create a launch repo from a dirty checkout:" >&2
  printf '%s\n' "$dirty" >&2
  exit 1
fi
source_head="$(git rev-parse HEAD)"
source_tree="$(git rev-parse HEAD^{tree})"

if [ -z "$out" ]; then
  out="$(mktemp -d "${TMPDIR:-/tmp}/mdkr64-public-launch.XXXXXX")/repo"
fi

if [ -e "$out" ] && [ -n "$(find "$out" -mindepth 1 -maxdepth 1 2>/dev/null | sed -n '1p')" ]; then
  echo "Output directory already exists and is not empty: $out" >&2
  exit 1
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/mdkr64-public-launch-objects.XXXXXX")"
bare="$tmp/remote.git"
archive_info="$tmp/archive-info.tsv"
git init --bare "$bare" >/dev/null

# Build the public launch tree: HEAD's tree minus every path marked
# `export-ignore` in .gitattributes -- the same internal planning/working docs
# that `git archive` omits from the public source archive. This keeps the fresh
# launch repository in lockstep with the public archive: internal docs never
# enter the public repository.
launch_index="$tmp/launch-index"
GIT_INDEX_FILE="$launch_index" git read-tree "HEAD^{tree}"
launch_ignore_list="$tmp/launch-export-ignore.txt"
git ls-files | git check-attr --stdin export-ignore \
  | sed -n 's/: export-ignore: set$//p' > "$launch_ignore_list"
if [ -s "$launch_ignore_list" ]; then
  GIT_INDEX_FILE="$launch_index" git rm --cached --quiet --ignore-unmatch \
    --pathspec-from-file="$launch_ignore_list" >/dev/null
fi

# The internal documentation set is removed unconditionally, not only while
# .gitattributes happens to mark it export-ignore: docs/archive/** (dated
# working documents), docs/superpowers/** (internal planning workspace) and
# docs/SDK_INVENTORY.md are outside the public docs boundary regardless of any
# attribute, and an edited-away export-ignore line must not republish them.
GIT_INDEX_FILE="$launch_index" git rm --cached --quiet --ignore-unmatch -r -- \
  'docs/archive' \
  'docs/superpowers' \
  'docs/SDK_INVENTORY.md' >/dev/null

launch_tree_filtered="$(GIT_INDEX_FILE="$launch_index" git write-tree)"

launch_commit="$(
  GIT_AUTHOR_NAME="$author_name" \
  GIT_AUTHOR_EMAIL="$author_email" \
  GIT_COMMITTER_NAME="$committer_name" \
  GIT_COMMITTER_EMAIL="$committer_email" \
  git commit-tree "$launch_tree_filtered" -m "$message"
)"
git push --quiet "$bare" "${launch_commit}:refs/heads/main"
git --git-dir="$bare" symbolic-ref HEAD refs/heads/main
if [ -n "$(git ls-remote "$bare" 'refs/pull/*' 'refs/tags/*')" ]; then
  echo "Temporary launch remote unexpectedly advertises pull-request or tag refs." >&2
  git ls-remote "$bare" 'refs/pull/*' 'refs/tags/*' >&2
  exit 1
fi
git clone --quiet "$bare" "$out"

(
  cd "$out"
  launch_head="$(git rev-parse HEAD)"
  launch_tree="$(git rev-parse HEAD^{tree})"
  commit_count="$(git rev-list --all --count)"
  parent_count="$(git rev-list --parents -n 1 HEAD | awk '{ print NF - 1 }')"
  launch_status="$(git status --porcelain --untracked-files=all)"

  if [ "$launch_head" != "$launch_commit" ]; then
    echo "Launch repository HEAD mismatch: got $launch_head, expected $launch_commit" >&2
    exit 1
  fi
  if [ "$launch_tree" != "$launch_tree_filtered" ]; then
    echo "Launch repository tree mismatch: got $launch_tree, expected export-ignore-filtered tree $launch_tree_filtered (HEAD $source_head minus export-ignore paths)" >&2
    exit 1
  fi
  # Belt-and-suspenders spot-check on the filtered tree itself, on top of the
  # export-ignore filtering and the unconditional removals above. Each path
  # below exists in this repository and is internal-only: docs/superpowers/**
  # and docs/archive/** are working-documentation trees, docs/SDK_INVENTORY.md
  # and PUBLIC_LAUNCH_BACKLOG.md are internal inventories, and brand/gb1.png /
  # brand/gb2.png are deleted brand assets that must not resurface.
  leaked_internal="$(git ls-files -- \
    ':(glob)docs/superpowers/**' \
    ':(glob)docs/archive/**' \
    'docs/SDK_INVENTORY.md' \
    'PUBLIC_LAUNCH_BACKLOG.md' \
    'brand/gb1.png' \
    'brand/gb2.png')"
  if [ -n "$leaked_internal" ]; then
    echo "Launch repository unexpectedly contains internal (export-ignore) docs:" >&2
    printf '%s\n' "$leaked_internal" >&2
    exit 1
  fi
  if [ "$commit_count" -ne 1 ]; then
    echo "Launch repository must contain exactly one reachable commit, got $commit_count" >&2
    exit 1
  fi
  if [ "$parent_count" -ne 0 ]; then
    echo "Launch repository HEAD must be a root commit, got $parent_count parent(s)" >&2
    exit 1
  fi
  if [ -n "$launch_status" ]; then
    echo "Launch repository checkout is dirty:" >&2
    printf '%s\n' "$launch_status" >&2
    exit 1
  fi

  echo "== Clean launch repository invariants =="
  echo "  OK -- launch tree matches the export-ignore-filtered HEAD tree ($launch_tree_filtered)."
  echo "  OK -- launch repository contains no internal documentation."
  echo "  OK -- launch history contains exactly one root commit."
  echo "  OK -- launch checkout is clean."

  # The guards below must pass inside the generated repository, not only in the
  # source checkout: they are the evidence that the tree actually being
  # published is clean-room, link-correct, syntactically sound, and free of
  # launch-blocking provenance paths.
  ./tools/check_clean_room.sh
  python3 tools/check_markdown_links.py --repo-root .
  python3 tools/check_shell_syntax.py --repo-root .
  python3 tools/check_public_history_paths.py --repo-root .

  if [ "$smoke_archive" -eq 1 ]; then
    echo
    echo "== Clean launch source archive smoke =="
    ./tools/make_public_source_archive.sh --force
    archive="dist/mdkr64-$(git rev-parse --short=12 HEAD).tar.gz"
    ./tools/smoke_public_source_archive.sh "$archive" --jobs "$jobs" --max-warnings "$max_warnings"
    archive_sha="$(shasum -a 256 "$archive" | awk '{ print $1 }')"
    printf '%s\t%s\n' "$archive" "$archive_sha" > "$archive_info"
  fi
)

if [ -z "$report" ]; then
  report="$(dirname "$out")/mdkr64-clean-launch-report.md"
fi
mkdir -p "$(dirname "$report")"

archive_path=""
archive_sha=""
if [ -s "$archive_info" ]; then
  IFS=$'\t' read -r archive_path archive_sha < "$archive_info"
fi

{
  printf '# MDKR64 Clean Launch Repository Evidence\n\n'
  printf '%s\n' "- Generated: \`$(date -u '+%Y-%m-%dT%H:%M:%SZ')\`"
  printf '%s\n' "- Source repository: \`$(pwd)\`"
  printf '%s\n' "- Source HEAD: \`${source_head}\`"
  printf '%s\n' "- Source tree: \`${source_tree}\`"
  printf '%s\n' "- Launch repository: \`${out}\`"
  printf '%s\n' "- Launch HEAD: \`$(git -C "$out" rev-parse HEAD)\`"
  printf '%s\n' "- Launch tree: \`$(git -C "$out" rev-parse HEAD^{tree})\`"
  printf '%s\n' "- Launch author: \`${author_name} <${author_email}>\`"
  printf '%s\n' "- Launch committer: \`${committer_name} <${committer_email}>\`"
  printf '%s\n' "- Launch reachable commits: \`$(git -C "$out" rev-list --all --count)\`"
  printf '%s\n' "- Launch HEAD parent count: \`$(git -C "$out" rev-list --parents -n 1 HEAD | awk '{ print NF - 1 }')\`"
  printf '%s\n' "- Launch working tree status: \`clean\`"
  if [ -n "$archive_path" ]; then
    printf '%s\n' "- Source archive: \`${out}/${archive_path}\`"
    printf '%s\n' "- Source archive SHA-256: \`${archive_sha}\`"
  else
    printf '%s\n' "- Source archive smoke: not requested"
  fi
  printf '\n## Publish Flow\n\n'
  printf '%s\n' "Publishing is this and only this: push this repository's single commit to a fresh, empty GitHub repository, then verify what the remote advertises."
  printf '\n```sh\n'
  printf '%s\n' "cd \"$out\""
  printf '%s\n' "git remote add launch-clean <new-remote>"
  printf '%s\n' "git push launch-clean HEAD:refs/heads/main"
  printf '%s\n' "git ls-remote launch-clean 'refs/heads/*' 'refs/tags/*' 'refs/pull/*'"
  printf '%s\n' "tools/check_github_launch_ready.sh --repo <owner>/<repo> --allow-private"
  printf '```\n'
} > "$report"

cat <<EOF

Created clean launch repository:
  $out

Launch HEAD:
  $(git -C "$out" rev-parse HEAD)

Source HEAD used:
  $source_head

Evidence report:
  $report

Verify remote refs before publishing from this repository:
  git -C "$out" ls-remote <new-remote> 'refs/heads/*' 'refs/tags/*' 'refs/pull/*'
EOF
