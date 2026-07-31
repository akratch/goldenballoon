#!/usr/bin/env bash
#
# Configure this checkout to use the tracked mdkr64 Git hooks.
#
# Git does not enable repository-provided hooks automatically after clone, so
# maintainers should run this once per checkout. The hooks run the ROM/data
# contamination guard before commit and push.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

chmod +x .githooks/pre-commit .githooks/pre-push
git config core.hooksPath .githooks

echo "Configured Git hooks: core.hooksPath=.githooks"
