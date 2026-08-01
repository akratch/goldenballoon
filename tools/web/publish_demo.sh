#!/bin/bash
#
# publish_demo.sh -- ONE-WAY publisher from this source repo into the public demo
# repo. Nothing else may ever write to the demo repo.
#
# WHY TWO REPOS: GitHub Pages from a private repo needs a paid plan, so a public
# demo requires a public repo. Rather than publishing the whole source tree, the
# demo repo carries ONLY built artifacts (wasm, loader, shell, brand assets) plus
# its own README/LICENSE/DISCLAIMER. No decompiled game code, no ROM, no assets.
#
# THE CONTRACT (see docs/DEMO_REPO.md):
#   1. One direction only. The demo repo is a publication target, never a source
#      of truth. Never hand-edit it -- this script overwrites its payload wholesale
#      and any local edit there is silently lost.
#   2. Every publish is provenance-stamped with the exact source commit, in three
#      places: dist/web/build-info.json, the page footer, and the demo commit
#      message. That stamp is the only link from the live site back to code.
#   3. Fail closed. A dirty source tree, a failed ROM guard, or an oversized wasm
#      aborts the publish rather than shipping something that corresponds to no
#      commit.
#
# Usage:
#   tools/web/publish_demo.sh --demo ../golden-balloon-demo            # stage + commit
#   tools/web/publish_demo.sh --demo ../golden-balloon-demo --push     # and push
#   tools/web/publish_demo.sh --demo DIR --allow-dirty                 # explicit override
#   tools/web/publish_demo.sh --demo DIR --no-build                    # reuse dist/web
#
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC_ROOT="$PWD"

DEMO=""
PUSH=0
ALLOW_DIRTY=0
DO_BUILD=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --demo)        DEMO="$2"; shift 2 ;;
        --push)        PUSH=1; shift ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        --no-build)    DO_BUILD=0; shift ;;
        -h|--help)     sed -n '2,27p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$DEMO" ]] || { echo "publish_demo: --demo DIR is required" >&2; exit 2; }

# ---- gate 1: the source tree must correspond to a real commit ----------------
if [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
    if [[ "$ALLOW_DIRTY" -eq 0 ]]; then
        echo "publish_demo: FAIL -- source tree is dirty." >&2
        echo "  A published build must correspond to a commit, or nobody can tell what is live." >&2
        echo "  Commit first, or pass --allow-dirty if you really mean it." >&2
        git status --short --untracked-files=all | sed 's/^/    /' >&2
        exit 1
    fi
    echo ">> WARNING: publishing from a DIRTY tree (--allow-dirty)."
fi

SRC_SHA="$(git rev-parse HEAD)"
SRC_SHORT="$(git rev-parse --short HEAD)"
SRC_SUBJECT="$(git log -1 --pretty=%s)"

# ---- gate 2: build, which itself enforces the size budget + ROM guard --------
if [[ "$DO_BUILD" -eq 1 ]]; then
    echo ">> building web target"
    tools/web/build_web.sh
else
    echo ">> reusing existing dist/web (--no-build)"
    tools/check_no_rom.sh dist/web
fi

# ---- gate 3: the demo repo must be a git repo, and must not be this one ------
[[ -d "$DEMO" ]] || { echo "publish_demo: no such directory: $DEMO" >&2; exit 1; }
DEMO_ROOT="$(cd "$DEMO" && pwd)"
[[ "$DEMO_ROOT" != "$SRC_ROOT" ]] || { echo "publish_demo: --demo is the source repo" >&2; exit 1; }
case "$DEMO_ROOT" in
    "$SRC_ROOT"/*) echo "publish_demo: FAIL -- demo repo is nested inside the source repo" >&2; exit 1 ;;
esac
[[ -d "$DEMO_ROOT/.git" ]] || { echo "publish_demo: $DEMO_ROOT is not a git repo" >&2; exit 1; }

# ---- gate 4: refuse to clobber a demo repo containing source ----------------
# If someone ever copies the source tree in there, stop: publishing would then
# push decompiled game code to a public repo.
for forbidden in game platform baserom.us.v80.z64; do
    if [[ -e "$DEMO_ROOT/$forbidden" ]]; then
        echo "publish_demo: FAIL -- demo repo contains '$forbidden'." >&2
        echo "  The demo repo must hold built artifacts only. Refusing to publish." >&2
        exit 1
    fi
done

# ---- sync the payload -------------------------------------------------------
# Wholesale replace, so a file deleted here disappears there too. Only ever the
# built site; never source.
echo ">> syncing dist/web -> $DEMO_ROOT"
rm -rf "$DEMO_ROOT/dist_web_tmp"
mkdir -p "$DEMO_ROOT/dist_web_tmp"
cp -R dist/web/. "$DEMO_ROOT/dist_web_tmp/"

# The site lives at the demo repo ROOT so Pages serves it without a subpath.
#
# NOTHING in the demo repo is hand-maintained -- not even its README. Its
# governance files are version-controlled HERE, in tools/web/demo-repo/, and are
# re-synced on every publish. That is the point: there is exactly one source of
# truth, so the two repos cannot drift by someone editing the public one.
( cd "$DEMO_ROOT"
  find . -maxdepth 1 -mindepth 1 ! -name .git ! -name dist_web_tmp -exec rm -rf {} +
  cp -R dist_web_tmp/. .
  rm -rf dist_web_tmp )
cp -R tools/web/demo-repo/. "$DEMO_ROOT/"
touch "$DEMO_ROOT/.nojekyll"   # Pages must not run Jekyll over the wasm/js payload

# The demo README carries the live URL; keep the source LICENSE authoritative.
cp LICENSE "$DEMO_ROOT/LICENSE"

# ---- cache-bust the published shell assets ----------------------------------
# Stamp every locally-referenced asset URL with ?v=<source commit> IN THE
# PUBLISHED COPY ONLY (the tracked dist/web sources stay pristine). A sticky
# Safari cache can otherwise serve a stale shell against a fresh page — the
# stamp changes every publish, so index.html alone controls what runs. The
# shell recovers the same stamp from its own script URL and propagates it to
# the runtime-loaded engine (mdkr64_web.js + wasm via locateFile).
perl -pi -e "
  s/(href=\"style\\.css)\"/\$1?v=$SRC_SHORT\"/;
  s/(href=\"manifest\\.webmanifest)\"/\$1?v=$SRC_SHORT\"/;
  s/(src=\"(?:rom-id|mdkr-save-ui|mdkr64-shell)\\.js)\"/\$1?v=$SRC_SHORT\"/g;
" "$DEMO_ROOT/index.html"
echo ">> cache-busted shell asset URLs with ?v=$SRC_SHORT"

# ---- provenance in the demo repo -------------------------------------------
( cd "$DEMO_ROOT"
  tools_note="Published from the Golden Balloon source repo."
  git add -A
  if git diff --cached --quiet; then
      echo ">> demo repo already up to date with $SRC_SHORT - nothing to commit."
      exit 0
  fi
  git commit -q -F - <<COMMIT
web demo: source $SRC_SHORT

$tools_note
Source commit: $SRC_SHA
Source subject: $SRC_SUBJECT

Built artifacts only -- no decompiled game code, no ROM, no game assets. The
player supplies their own ROM, which is read client-side and never uploaded.
Verified by tools/check_no_rom.sh in the source repo before publishing.

DO NOT EDIT THIS REPO BY HAND. It is a publication target; the publisher
overwrites its payload wholesale and any local change is lost.
COMMIT
  echo ">> committed in demo repo: $(git rev-parse --short HEAD)" )

if [[ "$PUSH" -eq 1 ]]; then
    echo ">> pushing demo repo"
    ( cd "$DEMO_ROOT" && git push )
else
    echo ">> NOT pushed. Review, then:  (cd $DEMO_ROOT && git push)"
fi

echo
echo "published source $SRC_SHORT -> $DEMO_ROOT"
