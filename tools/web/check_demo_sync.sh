#!/bin/bash
#
# check_demo_sync.sh -- is the published web demo behind this source repo, and does
# it matter?
#
# The two-repo split (see docs/DEMO_REPO.md) buys a public Pages demo without a
# public source repo, but it introduces the classic failure mode: the live site
# quietly rots while the source moves on, and nobody notices because nothing is
# broken -- it is just old. This is the ratchet against that.
#
# It reads the provenance stamp the publisher wrote (build-info.json), compares it
# to HEAD here, and -- the part that matters -- reports whether any of the
# intervening commits actually touched something the web build ships. A doc-only
# commit does not warrant a republish; a renderer commit does.
#
# Usage:
#   tools/web/check_demo_sync.sh --demo ../golden-balloon-demo
#   tools/web/check_demo_sync.sh --demo DIR --quiet     # exit code only
#
# Exit 0 = in sync, or behind only by commits that cannot affect the build.
# Exit 1 = stale in a way that matters (republish), or provenance unreadable.
set -euo pipefail
cd "$(dirname "$0")/../.."

DEMO=""
QUIET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --demo)  DEMO="$2"; shift 2 ;;
        --quiet) QUIET=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$DEMO" ]] || { echo "check_demo_sync: --demo DIR is required" >&2; exit 2; }

say() { [[ "$QUIET" -eq 1 ]] || echo "$@"; }

INFO="$DEMO/build-info.json"
if [[ ! -f "$INFO" ]]; then
    echo "check_demo_sync: FAIL -- no build-info.json in $DEMO." >&2
    echo "  Either it was never published by tools/web/publish_demo.sh, or someone" >&2
    echo "  edited the demo repo by hand. Republish." >&2
    exit 1
fi

PUB_SHA="$(sed -n 's/.*"source_commit"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$INFO")"
PUB_DIRTY="$(sed -n 's/.*"source_dirty"[[:space:]]*:[[:space:]]*\([a-z]*\).*/\1/p' "$INFO")"
PUB_AT="$(sed -n 's/.*"built_utc"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$INFO")"
HEAD_SHA="$(git rev-parse HEAD)"

say "published: ${PUB_SHA:0:12}  (built $PUB_AT, dirty=$PUB_DIRTY)"
say "source:    ${HEAD_SHA:0:12}"

[[ -n "$PUB_SHA" ]] || { echo "check_demo_sync: FAIL -- unreadable source_commit" >&2; exit 1; }

if [[ "$PUB_DIRTY" == "true" ]]; then
    echo "check_demo_sync: FAIL -- the live build was made from a DIRTY tree, so it" >&2
    echo "  corresponds to no commit. Republish from a clean checkout." >&2
    exit 1
fi

if [[ "$PUB_SHA" == "$HEAD_SHA" ]]; then
    say "check_demo_sync: PASS -- live demo is exactly HEAD."
    exit 0
fi

if ! git cat-file -e "$PUB_SHA^{commit}" 2>/dev/null; then
    echo "check_demo_sync: FAIL -- published commit $PUB_SHA is not in this repo" >&2
    echo "  (history rewritten, or published from a different clone)." >&2
    exit 1
fi

BEHIND="$(git rev-list --count "$PUB_SHA".."$HEAD_SHA" 2>/dev/null || echo 0)"
say "behind by $BEHIND commit(s)"

# Which paths can actually change what the demo ships?
RELEVANT="$(git diff --name-only "$PUB_SHA".."$HEAD_SHA" -- \
    platform game lib cmake CMakeLists.txt dist/web tools/web 2>/dev/null || true)"

if [[ -z "$RELEVANT" ]]; then
    say "check_demo_sync: PASS -- $BEHIND commit(s) behind, but none touch the build"
    say "  (docs/tests only). A republish would produce the same site."
    exit 0
fi

say ""
say "check_demo_sync: STALE -- these changes are not live yet:"
say "$(echo "$RELEVANT" | sed 's/^/    /')"
say ""
say "  Republish:  tools/web/publish_demo.sh --demo $DEMO --push"
exit 1
