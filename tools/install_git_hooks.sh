#!/usr/bin/env bash
#
# Configure this checkout to use the tracked mdkr64 Git hooks.
#
# Git does not enable repository-provided hooks automatically after clone, so
# maintainers should run this once per checkout. The hooks run the ROM/data
# contamination and private-working-material guards before commit and push.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

chmod +x .githooks/pre-commit .githooks/pre-push tools/ci/check_public_push.sh
git config core.hooksPath .githooks

echo "Configured Git hooks: clean-room + public-surface checks via .githooks"
