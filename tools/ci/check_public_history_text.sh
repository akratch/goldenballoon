#!/usr/bin/env bash
# Audit reachable file history for high-risk text, including content committed
# and deleted before release. Pass a revision (default HEAD), not a range;
# new-commit metadata/ranges are enforced separately by the push/hosted gates.
set -euo pipefail

if [ "$#" -gt 1 ]; then
  echo "Usage: $0 [REVISION]" >&2
  exit 2
fi

exec python3 tools/check_public_surface.py --history "${1:-HEAD}"
