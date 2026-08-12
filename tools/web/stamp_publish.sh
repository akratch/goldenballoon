#!/bin/bash
#
# stamp_publish.sh -- stamp a staged copy of the web shell for publication.
#
# THIS IS THE ONLY PLACE THE STAMP IS APPLIED. There are two publish paths --
# tools/web/publish_demo.sh (push to the demo repo) and the web-demo GitHub
# Actions workflow (upload to Pages) -- and a page published by one of them with
# no stamp is a page a sticky cache can pin forever. Keeping the rewrite here,
# called by both, is what stops the two from drifting.
#
# It writes two things into index.html:
#
#   1. ?v=<commit> on every locally-referenced shell asset. The stamp changes
#      every publish, so index.html alone decides which stylesheet, which
#      rom-id/save-ui/shell script and which manifest run. mdkr64-shell.js
#      recovers the same stamp from its own script URL and propagates it to the
#      engine (mdkr64_web.js + .wasm via locateFile), the save-tools module and
#      the service worker -- so a stamped page can never mix builds.
#   2. data-build-stamp=<commit> on every role document and
#      data-build-version=<release version> on the launcher. The service worker
#      uses the former to keep a newly fetched document out of an older build's
#      offline cache; backups use the latter to record the product version that
#      wrote them. Unstamped local dev pages retain neither claim.
#
# The staged copy is rewritten IN PLACE; the tracked dist/web sources stay
# pristine. Fails closed: if any expected reference is missing after the
# rewrite, nothing gets published from a half-stamped page.
#
# Usage:
#   tools/web/stamp_publish.sh --dir DIR [--stamp SHORT] [--version VERSION]
#
set -euo pipefail
cd "$(dirname "$0")/../.."

DIR=""
STAMP=""
VERSION=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dir)     DIR="$2"; shift 2 ;;
        --stamp)   STAMP="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "stamp_publish: unknown arg: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$DIR" ]] || { echo "stamp_publish: --dir DIR is required" >&2; exit 2; }
INDEX="$DIR/index.html"
[[ -f "$INDEX" ]] || { echo "stamp_publish: no index.html in $DIR" >&2; exit 1; }
CONTROLLER="$DIR/controller/index.html"
ROOM="$DIR/room/index.html"
[[ -f "$CONTROLLER" ]] || { echo "stamp_publish: no controller/index.html in $DIR" >&2; exit 1; }
[[ -f "$ROOM" ]] || { echo "stamp_publish: no room/index.html in $DIR" >&2; exit 1; }

if [[ -z "$STAMP" ]]; then
    STAMP="$(git rev-parse --short HEAD 2>/dev/null || true)"
fi
if [[ -z "$VERSION" ]]; then
    VERSION="$(sed -n 's/^set(MDKR_VERSION "\([^"]*\)".*/\1/p' CMakeLists.txt | head -1)"
fi

# Both values end up inside an HTML attribute and inside a URL query. Refuse
# anything that is not the shape they are supposed to be rather than quoting it.
if [[ ! "$STAMP" =~ ^[0-9a-zA-Z._-]+$ ]]; then
    echo "stamp_publish: FAIL -- build stamp is missing or not a plain token: '$STAMP'" >&2
    echo "  Pass --stamp <short-commit> when the tree is not a git checkout." >&2
    exit 1
fi
if [[ ! "$VERSION" =~ ^[0-9a-zA-Z._+-]+$ ]]; then
    echo "stamp_publish: FAIL -- release version is missing or malformed: '$VERSION'" >&2
    echo "  Expected MDKR_VERSION from CMakeLists.txt, or an explicit --version." >&2
    exit 1
fi

perl -pi -e "
  s/(href=\"(?:style|input\\/touch-surface|party\\/party-host|online\\/online-room)\\.css)\"/\$1?v=$STAMP\"/g;
  s/(href=\"manifest\\.webmanifest)\"/\$1?v=$STAMP\"/;
  s/(src=\"(?:rom-id|mdkr-save-ui|mdkr64-shell|input\\/touch-surface|party\\/(?:party-protocol|party-sas|qrcodegen|party-host)|online\\/(?:online-control-config|online-room))\\.js)\"/\$1?v=$STAMP\"/g;
  s/^<html lang=\"en\">/<html lang=\"en\" data-build-version=\"$VERSION\" data-build-stamp=\"$STAMP\">/;
" "$INDEX"

perl -pi -e "
  s/(href=\"(?:\\.\\.\\/input\\/touch-surface|controller)\\.css)\"/\$1?v=$STAMP\"/g;
  s/(src=\"(?:\\.\\.\\/party\\/(?:party-protocol|party-sas)|\\.\\.\\/input\\/touch-surface|controller)\\.js)\"/\$1?v=$STAMP\"/g;
  s/^<html lang=\"en\">/<html lang=\"en\" data-build-stamp=\"$STAMP\">/;
" "$CONTROLLER"

perl -pi -e "
  s/(href=\"room-entry\\.css)\"/\$1?v=$STAMP\"/;
  s/(src=\"room-entry\\.js)\"/\$1?v=$STAMP\"/;
  s/^<html lang=\"en\">/<html lang=\"en\" data-build-stamp=\"$STAMP\">/;
" "$ROOM"

# Fail closed. A silently unmatched substitution publishes a page that looks
# fine and caches forever, which is the exact failure this script exists for.
missing=0
for ref in "style.css?v=$STAMP" "manifest.webmanifest?v=$STAMP" \
           "input/touch-surface.css?v=$STAMP" \
           "party/party-host.css?v=$STAMP" "online/online-room.css?v=$STAMP" \
           "rom-id.js?v=$STAMP" "mdkr-save-ui.js?v=$STAMP" \
           "input/touch-surface.js?v=$STAMP" \
           "party/party-protocol.js?v=$STAMP" "party/party-sas.js?v=$STAMP" \
           "party/qrcodegen.js?v=$STAMP" "party/party-host.js?v=$STAMP" \
           "online/online-control-config.js?v=$STAMP" \
           "online/online-room.js?v=$STAMP" "mdkr64-shell.js?v=$STAMP" \
           "data-build-version=\"$VERSION\"" "data-build-stamp=\"$STAMP\""; do
    if ! grep -Fq "$ref" "$INDEX"; then
        echo "stamp_publish: FAIL -- index.html is missing '$ref' after stamping." >&2
        missing=1
    fi
done
for ref in "../input/touch-surface.css?v=$STAMP" \
           "controller.css?v=$STAMP" \
           "../party/party-protocol.js?v=$STAMP" \
           "../party/party-sas.js?v=$STAMP" \
           "../input/touch-surface.js?v=$STAMP" "controller.js?v=$STAMP" \
           "data-build-stamp=\"$STAMP\""; do
    if ! grep -Fq "$ref" "$CONTROLLER"; then
        echo "stamp_publish: FAIL -- controller/index.html is missing '$ref' after stamping." >&2
        missing=1
    fi
done
for ref in "room-entry.css?v=$STAMP" "room-entry.js?v=$STAMP" \
           "data-build-stamp=\"$STAMP\""; do
    if ! grep -Fq "$ref" "$ROOM"; then
        echo "stamp_publish: FAIL -- room/index.html is missing '$ref' after stamping." >&2
        missing=1
    fi
done
if [[ "$missing" -ne 0 ]]; then
    echo "  The shell's markup changed shape; update this script before publishing." >&2
    exit 1
fi

echo ">> stamped $INDEX with ?v=$STAMP and data-build-version=$VERSION"
