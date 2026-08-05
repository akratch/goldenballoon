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
# In the private assembly checkout, the reachable-text audit uses the published
# public/main remote-tracking ref. In a normal public clone it audits HEAD. The
# exact candidate tree is audited separately in either checkout.
#
# Usage: tools/ci/check_release_ready.sh [--candidate [--web-dir DIR]] [-h|--help]
#
# Ordinary source-hygiene runs deliberately do not require a generated browser
# build. Candidate mode is the release/publish boundary: it requires the staged
# browser build to identify this exact commit and to have been built cleanly.
#
set -euo pipefail

candidate=0
web_dir="dist/web"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --candidate)
      candidate=1
      shift
      ;;
    --web-dir)
      [ "$#" -ge 2 ] || {
        echo "--web-dir requires a directory" >&2
        exit 2
      }
      web_dir="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [ "$candidate" -eq 0 ] && [ "$web_dir" != "dist/web" ]; then
  echo "--web-dir is only valid with --candidate" >&2
  exit 2
fi

if root="$(git rev-parse --show-toplevel 2>/dev/null)"; then
  cd "$root"
  HAVE_GIT=1
else
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "${script_dir}/../.."
  HAVE_GIT=0
fi

if [ "$candidate" -eq 1 ]; then
  echo
  echo "== Candidate web build provenance =="
  if [ "$HAVE_GIT" -ne 1 ]; then
    echo "candidate readiness requires a git checkout to resolve HEAD" >&2
    exit 1
  fi
  candidate_version="$(sed -nE 's/^[[:space:]]*set\(MDKR_VERSION "([^"]+)" CACHE STRING.*$/\1/p' CMakeLists.txt)"
  if [ "$(printf '%s\n' "$candidate_version" | awk 'NF' | wc -l | tr -d ' ')" -ne 1 ]; then
    echo "candidate readiness could not resolve one MDKR_VERSION from CMakeLists.txt" >&2
    exit 1
  fi
  python3 tools/ci/check_web_build_provenance.py \
    --web-dir "$web_dir" \
    --commit "$(git rev-parse HEAD)" \
    --version "$candidate_version"
fi

# `fail` counts violations rather than latching, so a section can report on its
# own findings alone. A section that tests the global counter directly starts
# lying as soon as any earlier section failed; sections bracket themselves with
# section_begin/section_clean instead.
fail=0
note() { printf '  \033[31m[VIOLATION]\033[0m %s\n' "$1"; fail=$((fail + 1)); }
warn() { printf '  \033[33m[WARN]\033[0m %s\n' "$1"; }

section_fail=0
section_begin() { section_fail="$fail"; }
section_clean() { [ "$fail" -eq "$section_fail" ]; }

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

# The mode bit is part of the tracked release surface, so assert it instead of
# repairing it: a guard that silently chmods its own dependencies cannot report
# that the checkout shipped them non-executable.
for guard_script in tools/check_no_rom.sh tools/check_clean_room.sh; do
  if [ ! -x "$guard_script" ]; then
    note "guard script is not executable: ${guard_script} (restore the mode bit: chmod +x ${guard_script})"
  fi
done

# The ROM guard scans dist/web (always tracked) plus any desktop package a
# release run has already produced in dist/. Packaged artifacts are the exact
# bytes a player downloads, so they must be scanned when they exist rather than
# only at package time.
rom_scan_targets=(dist/web)
while IFS= read -r desktop_artifact; do
  [ -n "$desktop_artifact" ] || continue
  rom_scan_targets+=("$desktop_artifact")
done < <(
  find dist -maxdepth 1 -type f \
    \( -name '*.dmg' -o -name '*.zip' -o -name '*.tar.gz' -o -name '*.tgz' -o -name '*.AppImage' \) \
    2>/dev/null | sort
)
echo "  scanning for ROM content: ${rom_scan_targets[*]}"
if [ "${#rom_scan_targets[@]}" -eq 1 ]; then
  echo "  note -- no built desktop package present in dist/; only dist/web was scanned."
fi
bash tools/check_no_rom.sh "${rom_scan_targets[@]}"

echo
echo "== Clean-room repository check =="
if bash tools/check_clean_room.sh; then
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
echo "== Release dependency tracking =="
if [ "$HAVE_GIT" -eq 1 ]; then
  # Every path below is consumed directly by CMake, a hosted release workflow,
  # or the release check runner. A local untracked file can make a dirty
  # checkout appear healthy while disappearing from git archive; fail that
  # condition explicitly instead of waiting for a clean-clone build to fail.
  for required_path in \
    docs/FPS_UNCAP_RELEASE_AUDIT.md \
    macos/Scripts/build_release_sdl2.sh \
    macos/Scripts/dmg_mount_cleanup.sh \
    macos/Scripts/release_sdl2_config.sh \
    macos/Scripts/run_launchservices_probe.py \
    macos/Scripts/stamp_macos_provenance.sh \
    macos/Scripts/verify_unsigned_dmg.sh \
    macos/Scripts/verify_unsigned_release.sh \
    platform/app/app_activation.h \
    platform/app/app_activation_mac.mm \
    platform/app/app_relaunch.cpp \
    platform/app/app_relaunch.h \
    platform/app/app_ui_policy.cpp \
    platform/app/app_ui_policy.h \
    platform/audio_sink_evidence.c \
    platform/audio_sink_evidence.h \
    platform/fast3d/gfx_webgpu_async_request.c \
    platform/fast3d/gfx_webgpu_async_request.h \
    platform/fast3d/gfx_webgpu_callback_latch.c \
    platform/fast3d/gfx_webgpu_callback_latch.h \
    platform/fast3d/gfx_webgpu_surface_policy.c \
    platform/fast3d/gfx_webgpu_surface_policy.h \
    platform/user_paths.c \
    platform/user_paths.h \
    platform/viewport_route_cache.c \
    platform/viewport_route_cache.h \
    tests/check_2p_human_binding.py \
    tests/check_app_adopted_pacing.py \
    tests/check_app_capture.py \
    tests/check_app_ui_input.py \
    tests/check_audio_sink_evidence.py \
    tests/check_audio_options_persistence.py \
    tests/check_authored_rng_compat.py \
    tests/check_camera_snapshot_coverage.py \
    tests/check_cli_version.cmake \
    tests/check_hud_render_authority.py \
    tests/check_release_ready_web_provenance.py \
    tests/check_restart_apply.py \
    tests/check_viewport_route_isolation.py \
    tests/check_weather_rng_order.py \
    tests/input_scripts/audio_options_persist_failure.txt \
    tests/input_scripts/audio_options_persist_success.txt \
    tests/input_scripts/magic_codes_accept_reject.txt \
    tests/input_scripts/race_2p_human_binding.txt \
    tests/input_scripts/race_3p_tt_camera.txt \
    tests/test_app_lifecycle.cpp \
    tests/test_app_ui_policy.cpp \
    tests/test_async_rom_validation.py \
    tests/test_audio_sink_evidence.c \
    tests/test_audio_sink_evidence_contract.py \
    tests/test_product_claim_boundaries.py \
    tests/test_user_paths.c \
    tests/test_viewport_route_cache.c \
    tests/test_webgpu_async_request.c \
    tests/test_webgpu_callback_latch.c \
    tests/test_webgpu_surface_policy.c \
    tools/check_windows_imports.sh \
    tools/ci/check_web_build_provenance.py; do
    if ! git ls-files --error-unmatch -- "$required_path" >/dev/null 2>&1; then
      note "release dependency is not tracked: $required_path"
    fi
  done
else
  echo "  SKIP -- archive contents have no git index; archive smoke resolves dependencies directly."
fi

echo
echo "== Public reachable-history text boundary =="
if [ "$HAVE_GIT" -eq 1 ]; then
  if python3 tools/check_public_surface.py --release-history; then
    :
  else
    note "reachable public git history contains private/high-risk file content"
  fi
else
  echo "  SKIP -- not a git checkout; archives have no reachable commit history."
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
section_begin
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
if section_clean; then
  echo "  OK -- required docs are present."
fi

echo
echo "== Repository metadata =="
section_begin
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
if section_clean; then
  echo "  OK -- required repository metadata is present."
fi

echo
echo "== Third-party provenance files =="
section_begin
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
if section_clean; then
  echo "  OK -- required third-party provenance files are present."
fi

echo
echo "== GitHub contributor scaffolding =="
section_begin
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
  .github/workflows/macos-release.yml \
  .github/workflows/web-demo.yml; do
  if [ ! -s "$f" ]; then
    note "missing or empty GitHub contributor scaffold: $f"
  fi
done
if section_clean; then
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

# The inverse of the assertion above: correctness.yml must KEEP its automatic
# lane. Read the trigger names out of its own `on:` block rather than pattern
# matching raw text -- an embedded-newline -F pattern splits into two patterns,
# one of them empty, and an empty fixed pattern matches every file.
correctness_triggers="$(
  python3 - <<'PY'
from pathlib import Path
import re

workflow = Path(".github/workflows/correctness.yml")
if not workflow.is_file():
    raise SystemExit

in_on = False
on_indent = 0
for line in workflow.read_text(encoding="utf-8").splitlines():
    clean = line.split("#", 1)[0].rstrip()
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
                for token in re.findall(r"[A-Za-z_]+", rest):
                    print(token)
            else:
                in_on = True
                on_indent = indent
        continue

    if text.startswith("-"):
        print(text[1:].strip().strip("'\""))
    else:
        print(text.split(":", 1)[0].strip().strip("'\""))
PY
)"
if [ ! -s .github/workflows/correctness.yml ]; then
  note "correctness.yml is missing or empty"
elif ! printf '%s\n' "$correctness_triggers" | grep -Fqx 'push'; then
  note "correctness.yml no longer has its expected automatic push trigger"
elif ! printf '%s\n' "$correctness_triggers" | grep -Fqx 'pull_request'; then
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
section_begin
for f in \
  tools/install_git_hooks.sh \
  tools/ci/check_high_risk_ignored_artifacts.sh \
  tools/ci/check_public_history_text.sh \
  tools/ci/check_public_push.sh \
  tools/check_public_surface.py \
  tools/make_public_source_archive.sh \
  tools/smoke_public_source_archive.sh \
  tools/check_github_launch_ready.sh \
  tools/check_windows_imports.sh \
  tools/package_windows_zip.sh \
  tools/package_linux_appimage.sh \
  tools/ci/ci_local.sh \
  tools/mingw_cross_check.sh \
  macos/Scripts/build_release_sdl2.sh \
  macos/Scripts/build_app_bundle.sh \
  macos/Scripts/build_universal.sh \
  macos/Scripts/create_dmg.sh \
  macos/Scripts/notarize_artifact.sh \
  macos/Scripts/run_launchservices_probe.py \
  macos/Scripts/sign_and_notarize.sh \
  macos/Scripts/stamp_macos_provenance.sh \
  macos/Scripts/verify_gatekeeper_bundle.sh \
  macos/Scripts/verify_asset_free.sh \
  macos/Scripts/verify_unsigned_release.sh \
  macos/Scripts/verify_unsigned_dmg.sh; do
  if [ ! -x "$f" ]; then
    note "missing or non-executable release helper script: $f"
  fi
done
if section_clean; then
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
