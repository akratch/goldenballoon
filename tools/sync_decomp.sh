#!/bin/bash
#
# sync_decomp.sh -- Pull upstream decomp progress into the port's vendored game/
# tree WITHOUT clobbering our NATIVE_PORT patches.
#
# The port vendored game/{src,include,libultra} from the DKR decomp and then
# layered many NATIVE_PORT-gated fixes on top (LP64 pointer sizing, asset
# byteswaps, probes, UB fixes...). A plain re-copy would destroy those. This
# script instead does a per-file 3-WAY MERGE:
#
#     base   = the decomp file as of the recorded baseline commit
#     theirs = the decomp file NOW (working tree, so uncommitted WIP counts)
#     ours   = the port's current game/ copy (base + our patches)
#
# Clean merges are applied in place; conflicts are left with markers and listed
# so a human resolves them. Nothing is committed automatically.
#
# Usage:
#   tools/sync_decomp.sh [--decomp PATH] [--baseline COMMIT] [--dry-run]
#
# After a successful sync: rebuild, run the validation suite (see tests/README.md
# and docs/DECOMP_SYNC.md), then commit and update .decomp-baseline.
#
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="$PWD"

# Resolved without ':?' so that --help stays reachable with no environment set up;
# the emptiness check below runs after argument parsing and still fails closed.
DECOMP="${DKR_DECOMP:-}"
BASELINE_FILE="$PORT/.decomp-baseline"
BASELINE=""
DRY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --decomp)   DECOMP="$2"; shift 2 ;;
        --baseline) BASELINE="$2"; shift 2 ;;
        --dry-run)  DRY=1; shift ;;
        -h|--help)  sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$DECOMP" ]] || { echo "FAIL: set DKR_DECOMP, or pass --decomp PATH, to point at your local Diddy-Kong-Racing decomp checkout" >&2; exit 1; }
[[ -d "$DECOMP/.git" ]] || { echo "FAIL: not a decomp checkout: $DECOMP" >&2; exit 1; }
if [[ -z "$BASELINE" ]]; then
    [[ -f "$BASELINE_FILE" ]] || { echo "FAIL: no $BASELINE_FILE; pass --baseline COMMIT" >&2; exit 1; }
    BASELINE="$(grep -oE '^[0-9a-f]{7,40}' "$BASELINE_FILE" | head -1)"
fi

echo "port    : $PORT"
echo "decomp  : $DECOMP"
echo "baseline: $BASELINE"
echo

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
CLEAN=(); CONFLICT=(); NEWFILE=(); SKIP=()

# Vendored subtrees: decomp path -> port path
map_port_path() {          # $1 = decomp-relative path (e.g. src/racer.c)
    case "$1" in
        src/*)      echo "game/${1}" ;;
        include/*)  echo "game/${1}" ;;
        libultra/*) echo "game/${1}" ;;
        *)          echo "" ;;
    esac
}

cd "$DECOMP"
# Every vendored file that differs between baseline and the decomp's CURRENT
# working tree (committed changes + uncommitted WIP).
CANDIDATES="$( { git diff --name-only "$BASELINE" -- src include libultra;
                 git diff --name-only "$BASELINE" HEAD -- src include libultra; } | sort -u )"

if [[ -z "$CANDIDATES" ]]; then
    echo "Up to date: no vendored decomp files changed since $BASELINE."
    exit 0
fi

for f in $CANDIDATES; do
    pp="$(map_port_path "$f")"
    [[ -n "$pp" ]] || continue
    ours="$PORT/$pp"
    if [[ ! -f "$ours" ]]; then
        NEWFILE+=("$f"); continue                      # decomp added a file we never vendored
    fi
    if [[ ! -f "$DECOMP/$f" ]]; then
        SKIP+=("$f (deleted upstream)"); continue
    fi
    safe="${f//\//_}"
    git show "$BASELINE:$f" > "$TMP/$safe.base" 2>/dev/null || { SKIP+=("$f (no baseline blob)"); continue; }
    cp "$DECOMP/$f" "$TMP/$safe.theirs"
    cp "$ours"      "$TMP/$safe.merged"
    if git merge-file -L ours -L base -L theirs \
           "$TMP/$safe.merged" "$TMP/$safe.base" "$TMP/$safe.theirs" >/dev/null 2>&1; then
        CLEAN+=("$f")
        [[ "$DRY" -eq 1 ]] || cp "$TMP/$safe.merged" "$ours"
    else
        CONFLICT+=("$f")
        [[ "$DRY" -eq 1 ]] || cp "$TMP/$safe.merged" "$ours"   # leave markers for the human
    fi
done

echo "clean merges   : ${#CLEAN[@]}";    for f in ${CLEAN[@]+"${CLEAN[@]}"};       do echo "    $f"; done
echo "CONFLICTS      : ${#CONFLICT[@]}"; for f in ${CONFLICT[@]+"${CONFLICT[@]}"};  do echo "    $f  <-- resolve markers"; done
echo "new upstream   : ${#NEWFILE[@]}";  for f in ${NEWFILE[@]+"${NEWFILE[@]}"};    do echo "    $f  <-- vendor manually if needed"; done
echo "skipped        : ${#SKIP[@]}";     for f in ${SKIP[@]+"${SKIP[@]}"};          do echo "    $f"; done
echo
if [[ "$DRY" -eq 1 ]]; then
    echo "(dry run -- nothing written)"
else
    echo "NEXT: resolve any conflicts, then:"
    echo "  cmake --build build -j"
    echo "  # muted+headless validation (see docs/DECOMP_SYNC.md)"
    echo "  echo '<new-decomp-commit>  # synced YYYY-MM-DD' > .decomp-baseline"
fi
[[ "${#CONFLICT[@]}" -eq 0 ]]
