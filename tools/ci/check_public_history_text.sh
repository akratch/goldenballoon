#!/usr/bin/env bash
# Compatibility entry point: the public repository keeps ordinary history now.
# Scan the committed tree by default, or pass one REVISION_RANGE to scan every
# newly introduced commit and its metadata.
set -euo pipefail

if [ "$#" -gt 1 ]; then
  echo "Usage: $0 [REVISION_RANGE]" >&2
  exit 2
fi

if [ "$#" -eq 1 ]; then
  exec python3 tools/check_public_surface.py --range "$1"
fi
exec python3 tools/check_public_surface.py --tree
