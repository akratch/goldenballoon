#!/usr/bin/env bash
#
# verify_provenance.sh -- bind every release asset to the verified source commit.
# Fails CLOSED before any upload.
#
# Ported from mgb64's scripts/release/verify_provenance.sh (as tools/release/,
# mirroring mdkr64's tools/ top-level convention). It accepts only the exact
# `Golden-Balloon-<version>-<platform>-<arch>` release artifact family and
# preserves mdkr64's internal manifest schema tag.
#
# Usage:
#   tools/release/verify_provenance.sh --dist DIR --version VER --commit SHA \
#       [--out-checksums FILE] [--out-manifest FILE]
#
# For each release asset (dist/Golden-Balloon-<version>-*, excluding generated
# SHA256SUMS/manifest and the .provenance.json sidecars) requires a sidecar
# "<asset>.provenance.json" whose recorded sha256 == the file on disk, version ==
# VER, and commit == SHA. Rejects (nonzero) any missing sidecar, extra/renamed
# asset with no sidecar, orphan sidecar naming an absent asset, or any digest /
# version / commit mismatch. On success emits SHA256SUMS + a consolidated
# manifest.json for publication.
#
# Runs where python3 is available (maintainer macOS + the ctest host); the
# producer (stamp_provenance.sh) is python-free so it can run in the msys2 CI job.
set -euo pipefail

dist=""; version=""; commit=""; out_checksums=""; out_manifest=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dist) dist="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --commit) commit="$2"; shift 2 ;;
    --out-checksums) out_checksums="$2"; shift 2 ;;
    --out-manifest) out_manifest="$2"; shift 2 ;;
    -h|--help) echo "Usage: $0 --dist DIR --version VER --commit SHA [--out-checksums FILE] [--out-manifest FILE]"; exit 0 ;;
    *) echo "ERROR: unknown arg: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$dist" && -d "$dist" ]] || { echo "ERROR: --dist DIR required (got '$dist')." >&2; exit 2; }
[[ -n "$version" ]] || { echo "ERROR: --version VER required." >&2; exit 2; }
[[ -n "$commit" ]] || { echo "ERROR: --commit SHA required." >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 required to verify provenance sidecars." >&2; exit 2; }

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}';
  else echo ""; fi
}

fail=0
err() { printf 'FAIL: %s\n' "$1" >&2; fail=$((fail+1)); }

# Enumerate candidate release assets, excluding sidecars + generated outputs.
shopt -s nullglob
assets=()
macos_unsigned="Golden-Balloon-${version}-macos-arm64-unsigned.dmg"
macos_trusted="Golden-Balloon-${version}-macos-arm64-signed-notarized.dmg"
windows_zip="Golden-Balloon-${version}-windows-x64.zip"
linux_appimage="Golden-Balloon-${version}-linux-x86_64.AppImage"
linux_tarball="Golden-Balloon-${version}-linux-x86_64.tar.gz"
for f in "$dist"/Golden-Balloon-"$version"-*; do
  base="$(basename "$f")"
  case "$base" in
    *.provenance.json) continue ;;
    *.sha256) continue ;;
    Golden-Balloon-SHA256SUMS-*) continue ;;
    Golden-Balloon-manifest-*) continue ;;
  esac
  case "$base" in
    "$macos_unsigned"|"$macos_trusted"|"$windows_zip"|"$linux_appimage"|"$linux_tarball")
      assets+=("$f")
      ;;
    *) err "unsupported release artifact filename: $base" ;;
  esac
done
orphans=("$dist"/Golden-Balloon-"$version"-*.provenance.json)
shopt -u nullglob

[[ ${#assets[@]} -gt 0 ]] || { echo "ERROR: no release assets in $dist for version $version." >&2; exit 1; }

# Every sidecar must name an existing asset (catch orphan/stale sidecars).
if [[ ${#orphans[@]} -gt 0 ]]; then
  for s in "${orphans[@]}"; do
    named="${s%.provenance.json}"
    [[ -f "$named" ]] || err "orphan provenance sidecar names a missing asset: $(basename "$s")"
  done
fi

verified=()
for a in "${assets[@]}"; do
  base="$(basename "$a")"
  sidecar="${a}.provenance.json"
  if [[ ! -f "$sidecar" ]]; then
    err "asset has no provenance sidecar (missing/renamed/substituted): $base"
    continue
  fi
  actual="$(sha256_of "$a")"
  [[ -n "$actual" ]] || { echo "ERROR: no sha256 tool (sha256sum/shasum) available." >&2; exit 2; }
  if ASSET="$base" ACTUAL="$actual" WANT_VERSION="$version" WANT_COMMIT="$commit" \
     python3 - "$sidecar" <<'PY'
import json, os, sys
try:
    rec = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception as e:
    print(f"  unparseable sidecar: {e}", file=sys.stderr); sys.exit(1)
errs = []
for key, want in (("artifact", os.environ["ASSET"]),
                  ("sha256",   os.environ["ACTUAL"]),
                  ("version",  os.environ["WANT_VERSION"]),
                  ("commit",   os.environ["WANT_COMMIT"])):
    if rec.get(key) != want:
        errs.append(f"{key}={rec.get(key)!r} != {want!r}")
if rec.get("source_dirty") is not False:
    errs.append(f"source_dirty={rec.get('source_dirty')!r} != False")
for e in errs:
    print("  " + e, file=sys.stderr)
sys.exit(1 if errs else 0)
PY
  then
    verified+=("$a")
  else
    err "provenance mismatch for $base (see above)"
  fi
done

if [[ "$fail" -ne 0 ]]; then
  echo "verify_provenance: ${fail} provenance failure(s); refusing to release." >&2
  exit 1
fi

# All good -> emit checksums + a consolidated manifest for publication.
if [[ -n "$out_checksums" ]]; then
  : > "$out_checksums"
  for a in "${verified[@]}"; do
    printf '%s  %s\n' "$(sha256_of "$a")" "$(basename "$a")" >> "$out_checksums"
  done
  echo "wrote $out_checksums"
fi
if [[ -n "$out_manifest" ]]; then
  VERSION="$version" COMMIT="$commit" python3 - "$out_manifest" "${assets[@]}" <<'PY'
import json, os, sys
out = sys.argv[1]; assets = sys.argv[2:]
entries = [json.load(open(a + ".provenance.json", encoding="utf-8")) for a in assets]
entries.sort(key=lambda r: r.get("artifact", ""))
man = {"schema": "mdkr64-release-manifest/1",
       "version": os.environ["VERSION"],
       "commit": os.environ["COMMIT"],
       "assets": entries}
with open(out, "w", encoding="utf-8") as f:
    json.dump(man, f, indent=2, sort_keys=True); f.write("\n")
PY
  echo "wrote $out_manifest"
fi
echo "verify_provenance: ${#verified[@]} asset(s) bound to commit ${commit:0:12} (version $version)."
