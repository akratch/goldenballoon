#!/usr/bin/env bash
#
# check_release_ready.sh -- public source-release hygiene checks for mdkr64.
#
# Wraps the current-tree contamination guards and adds the assertions that a
# public source release must satisfy: no obvious ROM/media paths anywhere in git
# history, required provenance docs present, and no stale public claims about
# packaging or provenance.
#
# Every assertion is scoped to inventory this repository actually has. Concepts
# mdkr64 does not ship are deliberately NOT checked: there is no Docker build
# context, no Xcode/Swift app project, no PortMaster/GLES target, no community
# soundpack mechanism, and no IPL3 boot-font placeholder. The section comments
# below record what each assertion is pinned to.
#
# Usage: tools/ci/check_release_ready.sh [-h|--help]
#
set -euo pipefail

for arg in "$@"; do
  case "$arg" in
    -h|--help)
      sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
  esac
done

if root="$(git rev-parse --show-toplevel 2>/dev/null)"; then
  cd "$root"
  HAVE_GIT=1
else
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "${script_dir}/../.."
  HAVE_GIT=0
fi

fail=0
note() { printf '  \033[31m[VIOLATION]\033[0m %s\n' "$1"; fail=1; }
warn() { printf '  \033[33m[WARN]\033[0m %s\n' "$1"; }

public_file_list() {
  if [ "$HAVE_GIT" -eq 1 ]; then
    git ls-files
    return
  fi

  find . \
    \( -path './.git' -o -path './build' -o -path './build-*' -o -path './dist' -o -path './__pycache__' \) -prune \
    -o -type f -print \
    | sed 's#^\./##' \
    | sort
}

allowlist_values() {
  sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' "$1"
}

echo "== mdkr64 release readiness guard =="

chmod +x tools/check_no_rom.sh
tools/check_no_rom.sh

echo
echo "== Clean-room repository check =="
chmod +x tools/check_clean_room.sh
if tools/check_clean_room.sh; then
  :
else
  note "clean-room repository guard failed"
fi

echo
echo "== Git history filename audit =="
if [ "$HAVE_GIT" -eq 1 ]; then
  # Media is rejected unless its exact path is in one of the reviewed
  # first-party allowlists. The clean-room and metadata checks still inspect
  # the accepted current assets; this exemption is path-only, not content-only.
  asset_patterns="$(mktemp "${TMPDIR:-/tmp}/mdkr64-assets.XXXXXX")"
  trap 'rm -f "$asset_patterns"' EXIT
  {
    allowlist_values tools/public_tracked_asset_allowlist.txt
    allowlist_values tools/public_historical_asset_allowlist.txt
  } > "$asset_patterns"
  history_hits=$(git log --all --name-only --pretty=format: \
    | awk 'NF' \
    | sort -u \
    | grep -E '\.(z64|n64|v64|rom|bin|bmp|png|jpe?g|gif|webp|ico|icns|ppm|raw|wav|mp3|ogg|flac|m4a|aac|mp4|mov|m4v|mkv|avi|webm|jsonl|ctl|tbl|aifc|aiff|sbk|seq|cdata|dmg|zip|7z|tar|tgz|gz)$|(^|/)baserom|(^|/)[^/]+\.app(/|$)|mdkr.*eeprom|(^|/)screenshot_[^/]*\.(bmp|png|jpe?g|gif|webp|ppm|raw|jsonl|mp4|mov|m4v|webm)$' \
    | grep -Fvx -f "$asset_patterns" \
    || true)
  if [ -n "$history_hits" ]; then
    while IFS= read -r f; do note "ROM/media/build artifact path found in git history: $f"; done <<< "$history_hits"
  else
    echo "  OK -- no obvious ROM/media/build-artifact filenames found in git history."
  fi
else
  echo "  SKIP -- not a git checkout; archive contents were scanned by the contamination guard."
fi

echo
echo "== Public tracked-tree boundary =="
if [ "$HAVE_GIT" -eq 1 ]; then
  if python3 tools/check_public_surface.py --tree; then
    :
  else
    note "committed public tree contains private/high-risk material"
  fi
else
  echo "  SKIP -- not a git checkout; archive content is covered by the artifact guards."
fi

echo
echo "== Tracked build artifact hygiene =="
tracked_asset_patterns="$(mktemp "${TMPDIR:-/tmp}/mdkr64-tracked-assets.XXXXXX")"
trap 'rm -f "${asset_patterns:-}" "$tracked_asset_patterns"' EXIT
allowlist_values tools/public_tracked_asset_allowlist.txt > "$tracked_asset_patterns"

while IFS= read -r allowed_asset; do
  if [ "$HAVE_GIT" -eq 1 ] && ! git ls-files --error-unmatch -- "$allowed_asset" >/dev/null 2>&1; then
    note "stale or untracked public-asset allowlist entry: $allowed_asset"
  fi
done < "$tracked_asset_patterns"

artifact_hits=$(public_file_list \
  | grep -E '\.(o|a|so|dylib|dll|exe|pyc|pyo|class|dSYM|app|dmg|zip|7z|tar|tgz|gz)$|(^|/)(__pycache__|build|build-[^/]*|dist)(/|$)' \
  | grep -Fvx -f "$tracked_asset_patterns" \
  || true)
if [ -n "$artifact_hits" ]; then
  while IFS= read -r f; do note "tracked build/generated artifact path: $f"; done <<< "$artifact_hits"
else
  echo "  OK -- no tracked build/generated artifact paths found."
fi

binary_hits=$(
  while IFS= read -r f; do
    [ -s "$f" ] || continue
    grep -Fqx -f "$tracked_asset_patterns" <<< "$f" && continue
    if ! grep -Iq . "$f" 2>/dev/null; then
      printf '%s\n' "$f"
    fi
  done < <(public_file_list)
)
if [ -n "$binary_hits" ]; then
  while IFS= read -r f; do note "tracked binary-looking file: $f"; done <<< "$binary_hits"
else
  echo "  OK -- no tracked binary-looking files found."
fi

echo
echo "== Required public-release docs =="
# The list below is exactly the docs this repo tracks at its root. Build
# instructions live in docs/DEVELOPER_HANDBOOK.md rather than a BUILDING.md.
# There is no standalone coding-style, instrumentation, or provenance-audit
# doc: those guardrails are enforced mechanically instead, by
# .clang-format/.editorconfig (asserted below) and the clean-room/no-ROM
# checks above.
for f in \
  LICENSE \
  README.md \
  DISCLAIMER.md \
  NOTICE.md \
  ROADMAP.md \
  CONTRIBUTING.md \
  CODE_OF_CONDUCT.md \
  SECURITY.md \
  RELEASE_NOTES.md \
  CHANGELOG.md \
  docs/STATUS.md \
  docs/DEVELOPER_HANDBOOK.md \
  docs/RELEASE_CHECKLIST.md; do
  if [ ! -s "$f" ]; then
    note "missing or empty release doc: $f"
  fi
done
if [ "$fail" -eq 0 ]; then
  echo "  OK -- required docs are present."
fi

echo
echo "== Repository metadata =="
# CMake is the only build system here, so no top-level Makefile is asserted,
# and there is no Docker build context (.dockerignore/Dockerfile) to require.
for f in \
  .gitattributes \
  .gitignore \
  .editorconfig \
  .clang-format \
  .githooks/pre-commit \
  .githooks/pre-push \
  CMakeLists.txt; do
  if [ ! -s "$f" ]; then
    note "missing or empty repository metadata file: $f"
  fi
done
if [ "$fail" -eq 0 ]; then
  echo "  OK -- required repository metadata is present."
fi

echo
echo "== Third-party provenance files =="
# mdkr64 vendors exactly two build-time dependencies that carry their own
# tracked license files; .gitattributes' linguist-vendored comment is the
# source of truth for that set. THIRD_PARTY.md is the per-path provenance
# table covering vendored and ported code rows as well as license files, and
# is enforced in detail by check_third_party_notices.py.
for f in \
  lib/glad/LICENSE \
  lib/glad/README.md \
  lib/sdl_gamecontrollerdb/LICENSE.txt \
  THIRD_PARTY.md; do
  if [ ! -s "$f" ]; then
    note "missing or empty third-party provenance file: $f"
  fi
done
if python3 tools/check_third_party_notices.py --repo-root .; then
  :
else
  note "third-party notice guard failed"
fi
if [ "$fail" -eq 0 ]; then
  echo "  OK -- required third-party provenance files are present."
fi

echo
echo "== GitHub contributor scaffolding =="
for f in \
  .github/CODEOWNERS \
  .github/dependabot.yml \
  .github/PULL_REQUEST_TEMPLATE.md \
  .github/ISSUE_TEMPLATE/config.yml \
  .github/ISSUE_TEMPLATE/bug_report.md \
  .github/ISSUE_TEMPLATE/build_failure.md \
  .github/ISSUE_TEMPLATE/parity_report.md \
  .github/ISSUE_TEMPLATE/platform_validation.md \
  .github/ISSUE_TEMPLATE/provenance_cleanup.md \
  .github/ISSUE_TEMPLATE/validation_task.md \
  .github/workflows/correctness.yml \
  .github/workflows/release.yml \
  .github/workflows/windows-validate.yml \
  .github/workflows/macos-release.yml; do
  if [ ! -s "$f" ]; then
    note "missing or empty GitHub contributor scaffold: $f"
  fi
done
if [ "$fail" -eq 0 ]; then
  echo "  OK -- required GitHub contributor scaffolding is present."
fi

echo
echo "== GitHub Actions trigger policy =="
# correctness.yml is DELIBERATELY hosted on push/pull_request: it is the
# ROM-free source/build gate (see that file's own header comment). Every other
# lane -- release, validation, demo-publish -- must stay manual-only, because
# those touch ROM-derived data or publish artifacts. So this asserts against
# every workflow file EXCEPT correctness.yml.
workflow_trigger_hits="$(
  python3 - <<'PY'
from pathlib import Path
import re

bad = {"push", "pull_request", "pull_request_target", "schedule"}
exempt = {"correctness.yml"}
workflow_dir = Path(".github/workflows")
if not workflow_dir.is_dir():
    raise SystemExit

def strip_comment(line: str) -> str:
    return line.split("#", 1)[0].rstrip()

for workflow in sorted(workflow_dir.glob("*.y*ml")):
    if workflow.name in exempt:
        continue
    lines = workflow.read_text(encoding="utf-8").splitlines()
    in_on = False
    on_indent = 0
    for number, line in enumerate(lines, 1):
        clean = strip_comment(line)
        if not clean.strip():
            continue
        indent = len(clean) - len(clean.lstrip(" "))
        text = clean.strip()

        if in_on and indent <= on_indent:
            in_on = False

        if not in_on:
            if text.startswith("on:"):
                rest = text[3:].strip()
                if rest:
                    tokens = re.findall(r"[A-Za-z_]+", rest)
                    if any(token in bad for token in tokens):
                        print(f"{workflow}:{number}:{text}")
                else:
                    in_on = True
                    on_indent = indent
            continue

        key = text.split(":", 1)[0].strip().strip("'\"")
        list_item = text[1:].strip().strip("'\"") if text.startswith("-") else ""
        if key in bad or list_item in bad:
            print(f"{workflow}:{number}:{text}")
PY
)"
if [ -n "$workflow_trigger_hits" ]; then
  while IFS= read -r hit; do
    note "GitHub Actions workflow has an automatic hosted trigger; release/validation/demo-publish lanes must be manual-only: $hit"
  done <<< "$workflow_trigger_hits"
else
  echo "  OK -- every workflow but correctness.yml is a manual-only (workflow_dispatch) lane."
fi

if [ ! -s .github/workflows/correctness.yml ] || ! grep -Fq $'\n  push:' .github/workflows/correctness.yml; then
  note "correctness.yml no longer has its expected automatic push trigger"
elif ! grep -Fq $'\n  pull_request:' .github/workflows/correctness.yml; then
  note "correctness.yml no longer has its expected automatic pull_request trigger"
else
  echo "  OK -- correctness.yml still runs automatically on push/pull_request, as documented in its header."
fi

workflow_action_hits="$(
  if [ -d .github/workflows ]; then
    while IFS= read -r workflow; do
      line_no=0
      while IFS= read -r line || [ -n "$line" ]; do
        line_no=$((line_no + 1))
        no_comment="${line%%#*}"
        ref="$(printf '%s\n' "$no_comment" | sed -nE 's/^[[:space:]]*(-[[:space:]]*)?uses:[[:space:]]*([^[:space:]]+).*/\2/p')"
        [ -n "$ref" ] || continue
        case "$ref" in
          ./*|../*) continue ;;
        esac
        if [[ "$ref" != *@* ]]; then
          printf '%s:%s:%s\n' "$workflow" "$line_no" "$ref"
          continue
        fi
        action_ref="${ref##*@}"
        if [[ ! "$action_ref" =~ ^[0-9A-Fa-f]{40}$ ]]; then
          printf '%s:%s:%s\n' "$workflow" "$line_no" "$ref"
        fi
      done < "$workflow"
    done < <(find .github/workflows -type f \( -name '*.yml' -o -name '*.yaml' \) | sort)
  fi
)"
if [ -n "$workflow_action_hits" ]; then
  while IFS= read -r hit; do
    note "GitHub Actions workflow reference is not pinned to a full commit SHA: $hit"
  done <<< "$workflow_action_hits"
else
  echo "  OK -- GitHub Actions workflow references are pinned to full commit SHAs."
fi

echo
echo "== Public documentation links =="
if python3 tools/check_markdown_links.py --repo-root .; then
  :
else
  note "Markdown link guard failed"
fi

echo
echo "== Public shell tool syntax =="
if python3 tools/check_shell_syntax.py --repo-root .; then
  :
else
  note "shell syntax guard failed"
fi

echo
echo "== Release helper scripts =="
for f in \
  tools/install_git_hooks.sh \
  tools/ci/check_high_risk_ignored_artifacts.sh \
  tools/ci/check_public_history_text.sh \
  tools/ci/check_public_push.sh \
  tools/check_public_surface.py \
  tools/make_public_source_archive.sh \
  tools/smoke_public_source_archive.sh \
  tools/check_github_launch_ready.sh \
  tools/package_windows_zip.sh \
  tools/package_linux_appimage.sh \
  tools/ci/ci_local.sh \
  tools/mingw_cross_check.sh \
  macos/Scripts/build_app_bundle.sh \
  macos/Scripts/build_universal.sh \
  macos/Scripts/create_dmg.sh \
  macos/Scripts/notarize_artifact.sh \
  macos/Scripts/sign_and_notarize.sh \
  macos/Scripts/verify_gatekeeper_bundle.sh \
  macos/Scripts/verify_asset_free.sh; do
  if [ ! -x "$f" ]; then
    note "missing or non-executable release helper script: $f"
  fi
done
if [ "$fail" -eq 0 ]; then
  echo "  OK -- release helper scripts are present and executable."
fi

echo
echo "== Native SDK surface =="
# Enforces NOTICE.md's "Nintendo 64 SDK headers" contract: no native CMake
# source or glob reaches into game/libultra/, and no SGI/proprietary legend
# text appears anywhere under game/. There is no exception list -- every hit
# fails closed.
if python3 tools/check_native_sdk_surface.py --repo-root .; then
  :
else
  note "native SDK surface guard failed"
fi

echo
echo "== Public claim alignment =="
overbroad_provenance_claim_hits=$(grep -RInE \
  'from[- ]scratch.{0,80}(decompilation|port)|fully[ -]clean[- ]room|clean[- ]room.{0,80}(decompilation|port|project|repository)' \
  README.md RELEASE_NOTES.md NOTICE.md DISCLAIMER.md docs/STATUS.md 2>/dev/null || true)
if [ -n "$overbroad_provenance_claim_hits" ]; then
  while IFS= read -r hit; do note "overbroad provenance/clean-room public claim: $hit"; done <<< "$overbroad_provenance_claim_hits"
else
  echo "  OK -- no overbroad from-scratch/clean-room public claims found."
fi

macos_claim_hits=$(grep -RInE \
  'Prebuilt distributables are[[:space:]]+\*\*unsigned\*\*|signed/notarized distributables are available' \
  README.md RELEASE_NOTES.md docs/*.md macos/README.md 2>/dev/null || true)
if [ -n "$macos_claim_hits" ]; then
  while IFS= read -r hit; do note "stale signed/prebuilt macOS public claim: $hit"; done <<< "$macos_claim_hits"
else
  echo "  OK -- no stale signed/prebuilt macOS release claims found in public docs."
fi

macos_workflow_claim_hits=$(grep -RInE \
  'handle the full release pipeline|Requires the following repository secrets|skip_notarize|Build macOS Artifacts|Upload library artifact' \
  .github/workflows/macos-release.yml macos/README.md 2>/dev/null || true)
if [ -n "$macos_workflow_claim_hits" ]; then
  while IFS= read -r hit; do note "stale macOS workflow/distribution claim: $hit"; done <<< "$macos_workflow_claim_hits"
else
  echo "  OK -- no stale macOS workflow/distribution claims found."
fi

packaging_placeholder_hits=$(grep -RInE \
  'PLACEHOLDER_SHA256|github\.com/mdkr64/mdkr64\b' \
  .github README.md RELEASE_NOTES.md docs/*.md macos/README.md 2>/dev/null || true)
if [ -n "$packaging_placeholder_hits" ]; then
  while IFS= read -r hit; do note "stale or placeholder packaging metadata: $hit"; done <<< "$packaging_placeholder_hits"
else
  echo "  OK -- no placeholder packaged-release metadata found."
fi

# Proprietary SDK notice text is enforced above by
# tools/check_native_sdk_surface.py (which knows the game/include/ allowlist
# and fails on any hit outside it); no separate ad-hoc scan is needed here.

if [ "$fail" -ne 0 ]; then
  echo
  echo "Release readiness guard FAILED."
  exit 1
fi

echo
echo "Release readiness guard passed."
