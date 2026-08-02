#!/bin/bash
#
# run_oracle.sh -- SILENT BY CONSTRUCTION: ares runs with SDL_AUDIODRIVER=dummy
#   (no device is ever opened) plus Audio/Mute=true + Audio/Volume=0, so an
#   oracle run cannot emit sound. Our own binary is run headless, which returns
#   before its audio device opens; pass MDKR_AUDIO=0 as well (note MDKR_AUDIO=off
#   is a NO-OP -- the code tests for "0").
#
#   The pinned ares has NO "Audio/Driver" setting (only Device/Frequency/Latency/
#   Blocking/Dynamic/Mute/Volume/Balance are bound), so the driver cannot be
#   chosen on the command line -- SDL_AUDIODRIVER is the lever that actually
#   works. See the block comment at the ares invocation below.
# run_oracle.sh -- Run one oracle route on BOTH the real ROM (instrumented ares)
# and the native port, then compare the captured frames.
#
#   tools/run_oracle.sh <route> [--ares-bin PATH] [--rom PATH] [options]
#
# Outputs land under build/ares-oracle/<route>/{native,ares}/frames and a
# comparison report + montage under build/ares-oracle/<route>/compare/.
# Everything is git-ignored, ROM-derived, and local-only -- do not commit.
#
set -euo pipefail
cd "$(dirname "$0")/.."

ROUTE=""
ROM="${ROM:-baserom.us.v80.z64}"
NATIVE_BIN="${NATIVE_BIN:-./build/mdkr64}"
ARES_BIN_DEFAULT="build/ares-oracle/ares/build-oracle/desktop-ui/ares.app/Contents/MacOS/ares"
ARES_BIN="${ARES_BIN:-$ARES_BIN_DEFAULT}"
WORK_ROOT="${MDKR64_ARES_ORACLE_DIR:-$PWD/build/ares-oracle}"
ARES_TIMEOUT="${ARES_TIMEOUT:-300}"
KEEP_ALL_NATIVE=0
SKIP_NATIVE=0
SKIP_ARES=0
NATIVE_ARM=""

usage() {
    cat <<'USAGE'
Usage: tools/run_oracle.sh <route> [options]

Options:
  --ares-bin PATH    instrumented ares binary (default: build/ares-oracle/...)
  --rom PATH         DKR ROM (default: baserom.us.v80.z64)
  --native-bin PATH  native port binary (default: ./build/mdkr64)
  --native-arm NAME  select a declared state-route native arm
  --ares-timeout N   seconds to allow ares per run (default: 300)
  --keep-all-native  keep every native PPM (default: prune to marks+cadence)
  --skip-native      reuse existing native frames
  --skip-ares        reuse existing ares frames
  -h, --help         show this help

Captures are ROM-derived and local-only. Do not commit them.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ares-bin) ARES_BIN="$2"; shift 2 ;;
        --rom) ROM="$2"; shift 2 ;;
        --native-bin) NATIVE_BIN="$2"; shift 2 ;;
        --native-arm) NATIVE_ARM="$2"; shift 2 ;;
        --ares-timeout) ARES_TIMEOUT="$2"; shift 2 ;;
        --keep-all-native) KEEP_ALL_NATIVE=1; shift ;;
        --skip-native) SKIP_NATIVE=1; shift ;;
        --skip-ares) SKIP_ARES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "Unknown arg: $1" >&2; usage >&2; exit 2 ;;
        *) if [[ -z "$ROUTE" ]]; then ROUTE="$1"; else echo "Unexpected: $1" >&2; exit 2; fi; shift ;;
    esac
done

[[ -n "$ROUTE" ]] || { echo "FAIL: route required" >&2; usage >&2; exit 2; }
[[ -f "$ROM" ]] || { echo "FAIL: ROM not found: $ROM" >&2; exit 1; }

ROUTE_PY="python3 tools/dkr_oracle_route.py"
$ROUTE_PY validate "$ROUTE" >/dev/null

field() { $ROUTE_PY field "$ROUTE" "$1"; }
NATIVE_FRAMES="$(field native_frames)"
NATIVE_DUMP_EVERY="$(field native_dump_every)"
ARES_FRAMES="$(field ares_frames)"
ARES_DUMP_EVERY="$(field ares_dump_every)"
ARES_DUMP_START="$(field ares_dump_start)"
COMPARE_FRAMES="$(field compare_frames)"
STATE_TRACE="$(field state_trace)"
STATE_REQUIRE_FINISH="$(field state_require_finish)"
FORCED_TRACK="$(field forced_track)"
FORCED_TRACK_SOURCE="$(field forced_track_source)"
STATE_RACER_INDEX="$(field state_racer_index)"
STATE_MIN_COMMON_CLOCKS="$(field state_min_common_clocks)"
STATE_MIN_LAP="$(field state_min_lap)"
STATE_MIN_CHECKPOINT="$(field state_min_checkpoint)"
STATE_MAX_POSITION_P95="$(field state_max_position_p95)"
STATE_MIN_PROGRESS_AGREEMENT="$(field state_min_progress_agreement)"
NATIVE_SYNTH_FIELDS="$(field native_synth_fields)"
NATIVE_CADENCE="$(field native_cadence)"
ARM_SUFFIX=""
REFERENCE_REPLAY=0
REPLAY_ARES_FRAME_START=""
REPLAY_FIELD_START=""
REPLAY_INPUT_START=""
ARM_NAMES="$($ROUTE_PY native-arms "$ROUTE")"
if [[ -n "$ARM_NAMES" ]]; then
    if [[ -z "$NATIVE_ARM" ]]; then
        NATIVE_ARM="$(printf '%s\n' "$ARM_NAMES" | head -1)"
    fi
    if ! printf '%s\n' "$ARM_NAMES" | grep -Fxq "$NATIVE_ARM"; then
        echo "FAIL: route $ROUTE has no native arm '$NATIVE_ARM'" >&2
        echo "Available arms:" >&2
        while IFS= read -r arm_name; do
            printf '  %s\n' "$arm_name" >&2
        done <<<"$ARM_NAMES"
        exit 2
    fi
    NATIVE_FRAMES="$($ROUTE_PY arm-field "$ROUTE" "$NATIVE_ARM" frames)"
    NATIVE_CADENCE="$($ROUTE_PY arm-field "$ROUTE" "$NATIVE_ARM" cadence)"
    NATIVE_SYNTH_FIELDS="$($ROUTE_PY arm-field "$ROUTE" "$NATIVE_ARM" synth_fields)"
    REFERENCE_REPLAY="$($ROUTE_PY arm-field "$ROUTE" "$NATIVE_ARM" reference_replay)"
    if [[ "$REFERENCE_REPLAY" -eq 1 ]]; then
        REPLAY_ARES_FRAME_START="$($ROUTE_PY arm-field "$ROUTE" "$NATIVE_ARM" replay_ares_frame_start)"
        REPLAY_FIELD_START="$($ROUTE_PY arm-field "$ROUTE" "$NATIVE_ARM" replay_field_start)"
        REPLAY_INPUT_START="$($ROUTE_PY arm-field "$ROUTE" "$NATIVE_ARM" replay_input_start)"
    fi
    ARM_SUFFIX="_$NATIVE_ARM"
elif [[ -n "$NATIVE_ARM" ]]; then
    echo "FAIL: route $ROUTE does not declare native arms" >&2
    exit 2
fi

if [[ "$STATE_TRACE" -eq 1 ]]; then
    # The RDRAM symbols in the ares patch are explicitly VERSION_us_v80.
    # Refuse every other revision (including the otherwise-supported PAL 1.1)
    # before starting a long run; filename is not evidence, so identify from the
    # normalized header CRC pair and accept all three N64 dump byte orders.
    python3 - "$ROM" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
with path.open("rb") as handle:
    head = handle.read(0x18)
if len(head) < 0x18:
    raise SystemExit(f"FAIL: state oracle ROM is shorter than an N64 header: {path}")
if head[:4] == b"\x80\x37\x12\x40":
    pass
elif head[:4] == b"\x37\x80\x40\x12":
    head = bytes(
        byte
        for index in range(0, len(head), 2)
        for byte in (head[index + 1], head[index])
    )
elif head[:4] == b"\x40\x12\x37\x80":
    head = bytes(
        byte
        for index in range(0, len(head), 4)
        for byte in reversed(head[index:index + 4])
    )
else:
    raise SystemExit(f"FAIL: state oracle ROM has an unknown N64 byte order: {path}")
crc = tuple(int.from_bytes(head[offset:offset + 4], "big") for offset in (0x10, 0x14))
if crc != (0xE402430D, 0xD2FCFC9D):
    raise SystemExit(
        "FAIL: state oracle instrumentation is US 1.1 (us.v80)-specific; "
        f"got header CRC1/CRC2 {crc[0]:08X}/{crc[1]:08X}"
    )
PY
fi

ROUTE_DIR="$WORK_ROOT/$ROUTE"
NATIVE_DIR="$ROUTE_DIR/native/frames"
ARES_DIR="$ROUTE_DIR/ares/frames"
CMP_DIR="$ROUTE_DIR/compare"
NATIVE_INPUT="$ROUTE_DIR/native${ARM_SUFFIX}_input.txt"
ARES_INPUT="$ROUTE_DIR/ares_input.txt"
MARK_PAIRS="$ROUTE_DIR/mark_pairs.txt"
ARES_SETTINGS="$ROUTE_DIR/ares_settings.bml"   # persistent -> warms shader cache
ARES_SAVES="$ROUTE_DIR/ares_saves"             # persistent
NATIVE_SAVES="$ROUTE_DIR/native_saves"         # route-local; never touch user campaign data
ARES_STATE="$ROUTE_DIR/ares_state.csv"
NATIVE_LOG="$ROUTE_DIR/native${ARM_SUFFIX}.log"
NATIVE_VIDEO_CONFIG="$ROUTE_DIR/native_video.ini"
STATE_REPORT="$CMP_DIR/state_report_${ROUTE}${ARM_SUFFIX}.json"
REFERENCE_FIELDS="$ROUTE_DIR/native${ARM_SUFFIX}_fields.txt"
REFERENCE_BASE_INPUT="$ROUTE_DIR/native${ARM_SUFFIX}_base_input.txt"

mkdir -p "$ROUTE_DIR" "$CMP_DIR" "$ARES_SAVES" "$NATIVE_SAVES"

NATIVE_SCRIPT_ARGS=(native-script "$ROUTE")
if [[ -n "$NATIVE_ARM" ]]; then
    NATIVE_SCRIPT_ARGS+=(--arm "$NATIVE_ARM")
fi
if [[ "$REFERENCE_REPLAY" -eq 1 ]]; then
    if [[ "$SKIP_ARES" -ne 1 ]]; then
        echo "FAIL: reference_replay is a second-stage native arm; first refresh" >&2
        echo "the real-ROM trace, then rerun with --native-arm reference_replay --skip-ares" >&2
        exit 2
    fi
    [[ -f "$ARES_STATE" ]] || {
        echo "FAIL: reference replay requires existing ares trace: $ARES_STATE" >&2
        echo "Run this route once without --skip-ares first." >&2
        exit 1
    }
    $ROUTE_PY "${NATIVE_SCRIPT_ARGS[@]}" >"$REFERENCE_BASE_INPUT"
    python3 tools/oracle_reference_replay.py \
        --ares-trace "$ARES_STATE" \
        --base-native-input "$REFERENCE_BASE_INPUT" \
        --fields-out "$REFERENCE_FIELDS" \
        --input-out "$NATIVE_INPUT" \
        --ares-frame-start "$REPLAY_ARES_FRAME_START" \
        --field-start "$REPLAY_FIELD_START" \
        --input-start "$REPLAY_INPUT_START" \
        --racer-index "$STATE_RACER_INDEX"
else
    $ROUTE_PY "${NATIVE_SCRIPT_ARGS[@]}" >"$NATIVE_INPUT"
fi
$ROUTE_PY ares-script   "$ROUTE" >"$ARES_INPUT"
$ROUTE_PY mark-pairs    "$ROUTE" >"$MARK_PAIRS"

echo "route=$ROUTE  native_arm=${NATIVE_ARM:-default}  native_frames=$NATIVE_FRAMES  ares_frames=$ARES_FRAMES"
echo "marks:"; sed 's/^/  /' "$MARK_PAIRS"

# ---- native run ------------------------------------------------------------
if [[ "$SKIP_NATIVE" -eq 0 ]]; then
    # Oracle input events are indexed to authored frame opportunities. Never
    # inherit a developer/player mdkr64.ini: a saved numeric/uncapped policy
    # changes opportunity numbering and can fire the route before its menu is
    # ready while leaving the retail arm untouched.
    rm -f "$NATIVE_VIDEO_CONFIG" "$NATIVE_VIDEO_CONFIG.tmp"
    if [[ "$COMPARE_FRAMES" -eq 1 ]]; then
        rm -rf "$NATIVE_DIR"; mkdir -p "$NATIVE_DIR"
    fi
    echo ">> native: $NATIVE_BIN --headless-frames $NATIVE_FRAMES"
    NATIVE_ARGS=(
        --headless-frames "$NATIVE_FRAMES"
        --input-script "$NATIVE_INPUT"
        --rom "$ROM"
    )
    if [[ "$COMPARE_FRAMES" -eq 1 ]]; then
        NATIVE_ARGS+=(--dump-frames "$NATIVE_DIR")
    fi
    NATIVE_ENV=(
        MDKR_AUDIO=0
        MDKR_SAVE_DIR="$NATIVE_SAVES"
        MDKR_VIDEO_CONFIG_PATH="$NATIVE_VIDEO_CONFIG"
        MDKR_PRESENT_RATE=original
        MDKR_PRESENT_SMOOTHING=off
        MDKR_SIMULATION_CADENCE="$NATIVE_CADENCE"
        MDKR_SYNTH_FIELDS="$NATIVE_SYNTH_FIELDS"
    )
    if [[ "$STATE_TRACE" -eq 1 ]]; then
        NATIVE_ENV+=(MDKR_TRACE=1 MDKR_ORACLE_STATE=1)
    fi
    if [[ "$REFERENCE_REPLAY" -eq 1 ]]; then
        NATIVE_ENV+=(MDKR_ORACLE_UPDATE_FIELDS="$REFERENCE_FIELDS")
    fi
    if [[ "$FORCED_TRACK" -ge 0 ]]; then
        NATIVE_ENV+=(MDKR_LOAD_TRACK="$FORCED_TRACK")
    fi
    env "${NATIVE_ENV[@]}" "$NATIVE_BIN" "${NATIVE_ARGS[@]}" \
        >"$NATIVE_LOG" 2>&1 || {
            echo "WARN: native exited non-zero; see $NATIVE_LOG" >&2; }
    if [[ "$COMPARE_FRAMES" -eq 1 ]]; then
        echo "   native dumped $(find "$NATIVE_DIR" -maxdepth 1 -type f -name 'frame_*.ppm' | wc -l | tr -d ' ') frame(s)"
    fi

    if [[ "$COMPARE_FRAMES" -eq 1 && "$KEEP_ALL_NATIVE" -eq 0 ]]; then
        # Keep only frames near marks + a coarse cadence; native dumps EVERY
        # frame (1280x960) so an unpruned route is multiple GB.
        KEEP_CSV="$($ROUTE_PY native-marks "$ROUTE")"
        python3 - "$NATIVE_DIR" "$KEEP_CSV" "${NATIVE_DUMP_EVERY:-0}" <<'PY'
import sys, os, re, glob
d, keep_csv, every = sys.argv[1], sys.argv[2], int(sys.argv[3] or 0)
keep = set()
for tok in keep_csv.split(","):
    tok = tok.strip()
    if tok:
        base = int(tok)
        for k in range(base-3, base+4):   # +-3 window around each mark
            keep.add(k)
rx = re.compile(r"frame_(\d+)\.ppm$")
removed = 0
for f in glob.glob(os.path.join(d, "frame_*.ppm")):
    m = rx.search(os.path.basename(f))
    if not m:
        continue
    n = int(m.group(1))
    if n in keep:
        continue
    if every and n % every == 0:
        continue
    os.remove(f); removed += 1
print(f"   pruned {removed} native frame(s); kept {len(glob.glob(os.path.join(d,'frame_*.ppm')))}")
PY
    fi
fi

# ---- ares run --------------------------------------------------------------
if [[ "$SKIP_ARES" -eq 0 ]]; then
    [[ -x "$ARES_BIN" ]] || { echo "FAIL: ares binary not executable: $ARES_BIN" >&2; \
        echo "Build it with tools/prepare_ares_oracle.sh" >&2; exit 1; }
    if [[ "$COMPARE_FRAMES" -eq 1 ]]; then
        rm -rf "$ARES_DIR"; mkdir -p "$ARES_DIR"
    fi
    if [[ "$STATE_TRACE" -eq 1 ]]; then
        rm -f "$ARES_STATE"
    fi
    ARES_MARKS_CSV="$($ROUTE_PY ares-marks "$ROUTE")"
    echo ">> ares: exit@$((ARES_FRAMES+5)) dump_every=$ARES_DUMP_EVERY marks=$ARES_MARKS_CSV (timeout ${ARES_TIMEOUT}s)"

    # SILENCE, in two independent layers (see the header comment):
    #  1. SDL_AUDIODRIVER=dummy -- ares' audio driver defaults to "SDL"
    #     (desktop-ui/settings/settings.cpp:46) and there is NO "Audio/Driver"
    #     setting bound in this pinned ares, so the driver CANNOT be selected on
    #     the command line. SDL's own dummy backend is the structural guarantee:
    #     it opens no device at all.
    #  2. Audio/Mute + Audio/Volume=0 -- these keys ARE bound, and are kept as
    #     belt-and-braces in case a future ares defaults to a non-SDL driver.
    # Do NOT re-add "--setting Audio/Driver=None": the key does not exist, and an
    # unknown key makes ares print "Invalid setting" and RETURN before loading the
    # ROM (desktop-ui.cpp:142) -- the run silently produces zero frames.
    ARES_ENV=(
        SDL_AUDIODRIVER=dummy
        MDKR64_ARES_INPUT_SCRIPT="$ARES_INPUT"
        MDKR64_ARES_EXIT_AFTER_FRAMES="$((ARES_FRAMES+5))"
    )
    if [[ "$COMPARE_FRAMES" -eq 1 ]]; then
        ARES_ENV+=(
            MDKR64_ARES_DUMP_DIR="$ARES_DIR"
            MDKR64_ARES_DUMP_EVERY="$ARES_DUMP_EVERY"
            MDKR64_ARES_DUMP_START="$ARES_DUMP_START"
            MDKR64_ARES_DUMP_MARKS="$ARES_MARKS_CSV"
        )
    fi
    if [[ "$STATE_TRACE" -eq 1 ]]; then
        ARES_ENV+=(MDKR64_ARES_STATE_TRACE="$ARES_STATE")
    fi
    if [[ "$FORCED_TRACK" -ge 0 ]]; then
        ARES_ENV+=(MDKR64_ARES_FORCE_TRACK="$FORCED_TRACK_SOURCE:$FORCED_TRACK")
    fi
    env "${ARES_ENV[@]}" "$ARES_BIN" --kiosk \
        --settings-file "$ARES_SETTINGS" \
        --setting Audio/Mute=true \
        --setting Audio/Volume=0 \
        --setting Input/Defocus=Allow \
        --setting General/AutoSaveMemory=false \
        --setting Paths/Saves="$ARES_SAVES/" \
        --setting Boot/Fast=true \
        --system "Nintendo 64" \
        --no-file-prompt \
        "$ROM" >"$ROUTE_DIR/ares.log" 2>&1 &
    APID=$!
    SECS=0
    while kill -0 "$APID" 2>/dev/null; do
        sleep 3; SECS=$((SECS+3))
        if [[ "$SECS" -ge "$ARES_TIMEOUT" ]]; then
            echo "   ares timeout at ${SECS}s -- terminating"
            kill "$APID" 2>/dev/null || true; sleep 2
            kill -9 "$APID" 2>/dev/null || true
            break
        fi
    done
    wait "$APID" 2>/dev/null || true
    if [[ "$COMPARE_FRAMES" -eq 1 ]]; then
        echo "   ares dumped $(find "$ARES_DIR" -maxdepth 1 -type f -name 'frame_*.ppm' | wc -l | tr -d ' ') frame(s) in ~${SECS}s"
    else
        echo "   ares completed in ~${SECS}s"
    fi
    if [[ "$COMPARE_FRAMES" -eq 1 ]] && ! ls "$ARES_DIR"/frame_*.ppm >/dev/null 2>&1; then
        echo "WARN: ares produced no frames; tail of ares.log:" >&2
        tail -8 "$ROUTE_DIR/ares.log" | sed 's/^/  /' >&2
    fi
fi

# ---- compare ---------------------------------------------------------------
if [[ "$COMPARE_FRAMES" -eq 1 ]]; then
    python3 tools/compare_frames.py \
        --route "$ROUTE" \
        --native-dir "$NATIVE_DIR" \
        --ares-dir "$ARES_DIR" \
        --mark-pairs "$MARK_PAIRS" \
        --out-dir "$CMP_DIR"
fi
if [[ "$STATE_TRACE" -eq 1 ]]; then
    STATE_ROUTE_LABEL="$ROUTE"
    if [[ -n "$NATIVE_ARM" ]]; then
        STATE_ROUTE_LABEL="$ROUTE/$NATIVE_ARM"
    fi
    STATE_COMPARE_ARGS=(
        --route "$STATE_ROUTE_LABEL"
        --native-log "$NATIVE_LOG"
        --ares-trace "$ARES_STATE"
        --out "$STATE_REPORT"
        --racer-index "$STATE_RACER_INDEX"
        --min-common-clocks "$STATE_MIN_COMMON_CLOCKS"
        --min-lap "$STATE_MIN_LAP"
        --min-checkpoint "$STATE_MIN_CHECKPOINT"
        --max-position-p95 "$STATE_MAX_POSITION_P95"
        --min-progress-agreement "$STATE_MIN_PROGRESS_AGREEMENT"
    )
    if [[ "$STATE_REQUIRE_FINISH" -eq 1 ]]; then
        STATE_COMPARE_ARGS+=(--require-finish)
    fi
    python3 tools/compare_oracle_state.py \
        "${STATE_COMPARE_ARGS[@]}"
fi
