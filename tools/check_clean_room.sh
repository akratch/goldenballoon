#!/bin/bash
#
# check_clean_room.sh -- FAIL-CLOSED guard: the repository itself must be clean-room.
#
# tools/check_no_rom.sh proves a *shipped artifact* carries no ROM data. This script
# proves the complementary claim, the one DISCLAIMER.md and NOTICE.md both make:
# that no ROM or ROM-derived data is in the repository -- "not in the working tree
# and not in git history". A ROM committed and then deleted in a later commit is
# still in the clone forever, and no artifact scan would ever see it.
#
# Checks:
#   1. No ROM-extension file is TRACKED (all three N64 byte orders).
#   2. No ROM-extension file was EVER ADDED in any commit on any ref.
#   3. No blob anywhere in history carries an N64 ROM header at offset 0.
#   4. No blob in history is implausibly large for source (default 4 MiB) -- a
#      backstop for ROM-derived bulk (asset dumps, captures) under an innocent name.
#   5. No emulator source is vendored (the visual oracle patches ares out-of-tree).
#   6. .gitignore still covers ROMs in all three byte orders, saves, and captures.
#
# Usage:  tools/check_clean_room.sh
# Exit 0 = clean-room claim holds. Exit 1 = STOP, do not publish.
# -e is on so an unanticipated command failure aborts rather than being scored as
# a passing check. Every site below where a NON-ZERO status is the expected clean
# outcome (a grep that finds nothing, a reader that closes a git pipe early) is
# guarded with an explicit `|| true`; without those guards -e plus pipefail makes
# a clean repository exit 1 at check 2.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 1

fail=0
note() { printf '  %-4s %s\n' "$1" "$2"; }

# Honour TMPDIR so the scratch blob list lands on the platform's temp volume.
CR_BLOBS="${TMPDIR:-/tmp}/cr_blobs.$$"

echo "== 1. no ROM tracked in the working tree =="
if git ls-files | grep -iE '\.(z64|n64|v64)$'; then
    note FAIL "a ROM is tracked (above)"; fail=1
else
    note ok "no .z64/.n64/.v64 tracked"
fi

echo "== 2. no ROM ever added in any commit, on any ref =="
# No match is the PASSING outcome here, and pipefail propagates grep's exit 1.
ever=$(git log --all --diff-filter=A --name-only --pretty=format: \
       | grep -iE '\.(z64|n64|v64)$' | sort -u) || true
if [[ -n "$ever" ]]; then
    printf '%s\n' "$ever"
    note FAIL "a ROM was added in history (above)"; fail=1
else
    note ok "no ROM-extension path was ever added in history"
fi

echo "== 3. no N64 ROM header at offset 0 in any historical blob =="
# List every blob reachable from any ref, once, with its size.
git rev-list --objects --all | awk '{print $1}' | sort -u \
  | git cat-file --batch-check='%(objecttype) %(objectname) %(objectsize)' 2>/dev/null \
  | awk '$1=="blob" && $3>1024 {print $2, $3}' > "$CR_BLOBS" || true

hits=0
scanned=0
while read -r oid size; do
    scanned=$((scanned+1))
    # `head -c4` closes the pipe early, so git exits non-zero on every blob.
    magic=$(git cat-file blob "$oid" 2>/dev/null | head -c4 | od -An -tx1 | tr -d ' \n') || true
    case "$magic" in
        80371240|37804012|40123780)
            note FAIL "ROM header in blob $oid ($size bytes)"; hits=$((hits+1)) ;;
    esac
done < "$CR_BLOBS"
if [[ $hits -eq 0 ]]; then
    note ok "$scanned blobs >1KiB scanned, no ROM header in any byte order"
else
    fail=1
fi

echo "== 4. no implausibly large blob in history (ROM-derived bulk backstop) =="
limit=${CLEAN_ROOM_MAX_BLOB:-4194304}   # 4 MiB
big=0
while read -r oid size; do
    if [[ "$size" -gt "$limit" ]]; then
        # awk exits at the first match, closing the pipe under git rev-list.
        path=$(git rev-list --objects --all | awk -v o="$oid" '$1==o {print $2; exit}') || true
        note WARN "$((size/1024)) KiB  ${path:-$oid}"
        big=$((big+1))
    fi
done < "$CR_BLOBS"
rm -f "$CR_BLOBS"
if [[ $big -eq 0 ]]; then
    note ok "no blob exceeds $((limit/1024)) KiB"
else
    note note "$big blob(s) above the threshold -- confirm each is first-party or"
    note note "documented third-party in NOTICE.md (brand art and generated"
    note note "lookup tables legitimately exceed it). Not a failure by itself."
fi

echo "== 5. no emulator source vendored =="
if git ls-files | grep -iE '(^|/)(ares|mupen|parallel-rdp|angrylion|libretro)/' ; then
    note FAIL "emulator source appears to be vendored (above)"; fail=1
else
    note ok "no emulator tree tracked (the oracle patches ares out-of-tree)"
fi

echo "== 6. .gitignore covers ROMs, ROM ARCHIVES, saves and captures =="
# Archives are in this list because a ROM handed over as a .zip is still a ROM, and
# an archive is caught by NONE of the other gates: the byte-order patterns do not
# match it and check_no_rom.sh scans for an N64 header, which a compressed member
# does not present. Five DKR revisions once sat in the project root as .zip,
# untracked but NOT ignored -- one `git add -A` from being committed.
for pat in '*.z64' '*.n64' '*.v64' '*.zip' '*.7z' '*.rar' 'save/' '*.ppm'; do
    if grep -qxF "$pat" .gitignore; then
        note ok ".gitignore has $pat"
    else
        note FAIL ".gitignore is missing $pat"; fail=1
    fi
done

echo "== 7. nothing ROM-sized is visible to git =="
# Independent of the pattern list above: ask git what it would actually add. Any
# untracked file over 4 MiB that is not ignored is treated as a potential ROM or
# ROM archive, whatever it is named. This is the check that would have caught the
# five archives regardless of extension.
# xargs reports 123 when any child exits non-zero, which is every file UNDER the
# threshold -- i.e. the passing case.
big_untracked=$(git ls-files --others --exclude-standard -z 2>/dev/null \
    | xargs -0 -I{} sh -c 'test -f "{}" && test $(wc -c <"{}") -gt 4194304 && echo "{}"' 2>/dev/null) || true
if [[ -n "$big_untracked" ]]; then
    note FAIL "untracked file(s) over 4 MiB are NOT ignored -- a stray ROM or archive"
    printf '%s\n' "$big_untracked" | sed 's/^/       /'
    fail=1
else
    note ok "no un-ignored file over 4 MiB (a stray ROM/archive could not slip in)"
fi

echo
if [[ $fail -ne 0 ]]; then
    echo "check_clean_room: FAIL -- the clean-room claim does NOT hold. DO NOT PUBLISH." >&2
    exit 1
fi
echo "check_clean_room: PASS -- no ROM or ROM-derived data in the tree or in history."
