#!/usr/bin/env bash
# Stable CI entry point; the checked Python reader preserves arbitrary filenames.
set -euo pipefail
guard_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$guard_dir/check_clean_room.py" "$@"
