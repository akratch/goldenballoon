#!/bin/bash
#
# prepare_ares_oracle.sh -- Build a local, instrumented ares for the mdkr64
# visual oracle.
#
# Clones ares into IGNORED local build space, applies a small focused patch that
# adds (a) an N64 controller-input injection hook that reads a scripted route
# (VI-frame -> button-mask/stick, standard N64 masks), (b) a frame-dump hook
# that writes P6 PPM frames at a cadence and/or marked frames, and (c) an
# opt-in US 1.1 numeric racer-state trace, then builds the desktop frontend.
#
# It does NOT vendor ares source and does NOT create redistributable ROM-derived
# captures. Everything lands under build/ares-oracle/ (git-ignored).
#
# Modeled on mgb64's tools/prepare_ares_movement_oracle_build.sh, but deliberately
# minimal: no RDP command tracing and no generic symbol system. Input/dumps are
# keyed on the presented-frame counter; the state lane uses eight explicitly
# version-locked US 1.1 globals plus documented object/racer offsets.
#
set -euo pipefail
cd "$(dirname "$0")/.."

# Pinned known-good ares commit (same as mgb64's oracle). Override ARES_REPO_URL
# to clone from a local mirror for a faster/offline checkout.
ARES_REPO_URL="${ARES_REPO_URL:-https://github.com/ares-emulator/ares.git}"
ARES_REF="${ARES_REF:-91b112279ab2ce89c5fc9bff5dbb81e29af51a68}"
WORK_DIR="${MDKR64_ARES_ORACLE_DIR:-$PWD/build/ares-oracle}"
JOBS="${JOBS:-}"
FORCE=0
NO_BUILD=0

usage() {
    cat <<'USAGE'
Usage: tools/prepare_ares_oracle.sh [options]

Options:
  --work-dir DIR   ignored local workspace (default: build/ares-oracle)
  --ares-ref REF   ares commit/tag/branch (default: pinned known-good commit)
  --jobs N         parallel build jobs (default: host CPU count)
  --no-build       clone/patch only
  --force          delete and recreate the local ares checkout
  -h, --help       show this help

Env:
  ARES_REPO_URL    ares git URL or local path to clone from
  MDKR64_ARES_ORACLE_DIR  override the workspace dir

The checkout and build are local-only. Do not commit ares source, ROM-derived
screenshots, saves, or generated captures.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --work-dir) WORK_DIR="$2"; shift 2 ;;
        --ares-ref) ARES_REF="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --no-build) NO_BUILD=1; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$JOBS" ]]; then
    if command -v sysctl >/dev/null 2>&1; then
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || true)"
    fi
    if [[ -z "$JOBS" ]] && command -v getconf >/dev/null 2>&1; then
        JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
    fi
    JOBS="${JOBS:-4}"
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "FAIL: --jobs must be a positive integer" >&2
    exit 2
fi

for required in git cmake python3; do
    if ! command -v "$required" >/dev/null 2>&1; then
        echo "FAIL: $required is required" >&2
        exit 2
    fi
done

WORK_DIR="$(python3 - "$WORK_DIR" <<'PY'
import os, sys
print(os.path.abspath(sys.argv[1]))
PY
)"
SRC_DIR="$WORK_DIR/ares"
BUILD_DIR="$SRC_DIR/build-oracle"

if [[ "$FORCE" -eq 1 && -e "$SRC_DIR" ]]; then
    rm -rf "$SRC_DIR"
fi

mkdir -p "$WORK_DIR"
if [[ ! -d "$SRC_DIR/.git" ]]; then
    echo "Cloning ares from $ARES_REPO_URL ..."
    git clone --depth 1 "$ARES_REPO_URL" "$SRC_DIR"
fi

# Track only the files this patch touches. If the checkout has other dirty
# paths, refuse (use --force). If the only dirty paths are ours, refresh in
# place (idempotent re-apply).
SKIP_ARES_SYNC=0
if ! git -C "$SRC_DIR" diff --quiet || ! git -C "$SRC_DIR" diff --cached --quiet; then
    if ! git -C "$SRC_DIR" diff --cached --quiet; then
        echo "FAIL: local ares checkout has staged changes: $SRC_DIR" >&2
        echo "Use --force to recreate it, or clean that checkout manually." >&2
        exit 1
    fi
    while IFS= read -r dirty_path; do
        case "$dirty_path" in
            ares/n64/ai/ai.cpp|\
            ares/n64/controller/gamepad/gamepad.cpp|\
            ares/n64/cpu/cpu.cpp|\
            ares/n64/n64.hpp|\
            ares/n64/vi/vi.cpp|\
            ares/ares/node/video/screen.cpp|\
            cmake/macos/compilerconfig.cmake|\
            desktop-ui/cmake/os-macos.cmake)
                ;;
            *)
                echo "FAIL: local ares checkout has uncommitted changes: $SRC_DIR" >&2
                echo "  dirty path: $dirty_path" >&2
                echo "Use --force to recreate it, or clean that checkout manually." >&2
                exit 1
                ;;
        esac
    done < <(git -C "$SRC_DIR" diff --name-only)
    SKIP_ARES_SYNC=1
    echo "Refreshing existing instrumented ares checkout: $SRC_DIR"
fi

if [[ "$SKIP_ARES_SYNC" -eq 0 ]]; then
    if [[ "$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null || true)" != "$ARES_REF" ]]; then
        git -C "$SRC_DIR" fetch origin "$ARES_REF" --depth 1 2>/dev/null || git -C "$SRC_DIR" fetch origin "$ARES_REF"
        git -C "$SRC_DIR" checkout --detach "$ARES_REF"
    fi
fi

# ---------------------------------------------------------------------------
# Apply the focused instrumentation patch.
# ---------------------------------------------------------------------------
python3 - "$SRC_DIR" <<'PY'
from pathlib import Path
import sys

src = Path(sys.argv[1])

# The oracle translation unit, appended to vi.cpp so it lives in the N64 core
# namespace (ares::Nintendo64) where n32/Node::Video::Screen are visible. A tiny
# forwarding shim in ares::Core::Video lets screen.cpp reach the N64 impl.
oracle_cpp = r'''#include <n64/n64.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ares::Nintendo64 {

namespace {

struct MdkrInputEvent {
  u64 start = 0;
  u64 end = 0;
  u16 buttons = 0;
  int stickX = 0;
  int stickY = 0;
};

struct MdkrOracleState {
  bool configured = false;
  bool hasInputScript = false;
  u64 videoFrame = 0;
  u64 frameLimit = 0;

  char dumpDir[1024] = {0};
  bool hasDumpDir = false;
  FILE* stateTrace = nullptr;
  /*
   * Audio lane. The tap is in AI::sample(), i.e. the exact s16 pair the ROM
   * DMA'd to the audio interface, BEFORE ares converts it to float, before the
   * decay/hold filter, and before any host resampler or output driver. That is
   * the real ROM's own synthesiser output at full scale -- the absolute-level
   * reference the port's software mixer has to be measured against.
   */
  FILE* audioDump = nullptr;
  std::mutex audioMutex;
  u64 audioSamples = 0;
  u32 audioRate = 0;
  char audioPath[1024] = {0};
  /*
   * Track-exit poke lane (issue #55). The Hot Top Volcano destination -1 exit
   * sits ~114 units below the drivable surface, so no scripted input route can
   * trip obj_loop_exit's latch on the real ROM. This lane holds the human
   * racer's OBJECT position at a fixed point for a presented-frame window and
   * zeroes its velocity, so the retail latch, door drive, fade, and the
   * MENU_UNUSED_8 routing all run authored code. Writes go through
   * CPU::writeDebug (cache-aware): writing backing RDRAM here can be undone by
   * a later dirty D-cache writeback, exactly the hazard the vehicle-RNG lane
   * documents on its read side. The optional trophyWorld field seeds
   * gTrophyRaceWorldId (and gTrophyRaceRound=2) once at window start so a
   * post-transition status line can witness whether retail preserves them
   * across the dead-end menu. From window start until exit a status line
   * (map/menu/mode/trophy globals) prints every 30 presented frames.
   */
  bool pokeEnabled = false;
  bool pokeAnnounced = false;
  bool pokeSeeded = false;
  u64 pokeStart = 0;
  u64 pokeEnd = 0;
  float pokeX = 0.0f;
  float pokeY = 0.0f;
  float pokeZ = 0.0f;
  int pokeTrophyWorld = -1;
  /*
   * RDRAM snapshot lane. At each listed presented frame the whole 8 MiB of
   * RDRAM is written to PREFIX_<frame>.bin through the CPU debug path, which is
   * coherent with dirty D-cache lines, so the texture memory a draw sampled
   * can be inspected offline (the Spaceport Alpha pulsing-ring parity work,
   * docs/ORACLE.md). ROM-derived and local-only, like every capture here.
   */
  char rdramPrefix[1024] = {0};
  u64 rdramFrames[16] = {0};
  u32 rdramFrameCount = 0;
  u64 dumpEvery = 0;
  u64 dumpStart = 0;
  u64 marks[512] = {0};
  u32 markCount = 0;
  u32 dumpedCount = 0;

  const void* firstGamepad = nullptr;
  u16 lastButtons = 0;
  int lastStickX = 0;
  int lastStickY = 0;
  u32 lastFramebuffer = 0;
  u64 framebufferSerial = 0;

  MdkrInputEvent events[1024];
  u32 eventCount = 0;

  /*
   * US 1.1 (VERSION_us_v80) state layout. The global address is independently
   * recorded in docs/ref/symbols/symbol_addrs.us.v80.txt; the structure offsets
   * are the matching 32-bit layouts documented in game/include/structs.h.
   * Keep this oracle version-specific instead of pretending that one address
   * applies to every retail revision.
   */
  static constexpr u32 gRacersAddress = 0x8011b464;
  static constexpr u32 gNumRacersAddress = 0x8011b470;
  static constexpr u32 gRaceStartTimerAddress = 0x8011dac0;
  static constexpr u32 gMapIdAddress = 0x801216e4;
  static constexpr u32 gVideoDeltaTimeAddress = 0x801268a9;
  static constexpr u32 sLogicUpdateRateAddress = 0x800dd974;
  static constexpr u32 gCurrentRngSeedAddress = 0x800dd9a4;
  static constexpr u32 gVideoLastFramebufferAddress = 0x80126878;
  /* Issue #55 poke/status lane, same US 1.1 symbol authority as above. */
  static constexpr u32 gCurrentMenuIdAddress = 0x800df9f0;
  static constexpr u32 gGameModeAddress = 0x80123a6c;
  static constexpr u32 gPlayableMapIdAddress = 0x80123a74;
  static constexpr u32 gLevelLoadLatchAddress = 0x80123a7c;  /* D_801234FC */
  static constexpr u32 gTrophyRaceWorldIdAddress = 0x800e1568;
  static constexpr u32 gTrophyRaceRoundAddress = 0x800e156c;
  static constexpr u32 gLevelSettingsAddress = 0x801217d0;

  static auto validPointer(u32 address, u32 bytes = 1) -> bool {
    if(address < 0x80000000 || address >= 0x80800000) return false;
    u32 physical = address & 0x1fffffff;
    return physical <= 0x00800000 && bytes <= 0x00800000 - physical;
  }

  static auto readU8(u32 address) -> u8 {
    return rdram.ram.read<Byte>(address & 0x1fffffff, RBusDevice::ARES_DEBUGGER);
  }

  static auto readU16(u32 address) -> u16 {
    return rdram.ram.read<Half>(address & 0x1fffffff, RBusDevice::ARES_DEBUGGER);
  }

  static auto readU32(u32 address) -> u32 {
    return rdram.ram.read<Word>(address & 0x1fffffff, RBusDevice::ARES_DEBUGGER);
  }

  static auto readF32(u32 address) -> float {
    u32 bits = readU32(address);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits));
    memcpy(&value, &bits, sizeof(value));
    return value;
  }

  /* CPU debug path: sign-extended KSEG0 vaddr, coherent with dirty D-cache
   * lines in both directions. The poke lane must use these, not rdram.ram. */
  static auto vaddrOf(u32 address) -> u64 {
    return (u64)(s64)(s32)address;
  }

  static auto readDebugWord(u32 address) -> u32 {
    return (u32)cpu.readDebug<Word>(vaddrOf(address));
  }

  static auto readDebugHalf(u32 address) -> u16 {
    return (u16)cpu.readDebug<Half>(vaddrOf(address));
  }

  static auto readDebugByte(u32 address) -> u8 {
    return (u8)cpu.readDebug<Byte>(vaddrOf(address));
  }

  static auto writeDebugWord(u32 address, u32 value) -> void {
    cpu.writeDebug<Word>(vaddrOf(address), value);
  }

  static int clampAxis(int v) {
    if(v < -80) return -80;
    if(v >  80) return  80;
    return v;
  }

  auto loadMarks(const char* csv) -> void {
    const char* p = csv;
    while(*p && markCount < 512) {
      while(*p == ',' || *p == ' ' || *p == '\t') p++;
      if(!*p) break;
      char* end = nullptr;
      unsigned long long v = strtoull(p, &end, 10);
      if(end == p) break;
      marks[markCount++] = v;
      p = end;
    }
  }

  auto loadInputScript(const char* path) -> void {
    FILE* f = fopen(path, "rb");
    if(!f) {
      fprintf(stderr, "mdkr64 oracle: cannot open input script %s\n", path);
      return;
    }
    hasInputScript = true;
    char line[512];
    while(fgets(line, sizeof(line), f) && eventCount < 1024) {
      char* c = line;
      while(*c == ' ' || *c == '\t') c++;
      if(*c == '\0' || *c == '\n' || *c == '#') continue;
      unsigned long long start = 0, len = 0;
      unsigned int btn = 0;
      int sx = 0, sy = 0;
      int n = sscanf(c, "%llu %llu %x %d %d", &start, &len, &btn, &sx, &sy);
      if(n < 3) {
        fprintf(stderr, "mdkr64 oracle: ignored malformed input line: %s", line);
        continue;
      }
      if(start == 0 || len == 0) continue;
      auto& e = events[eventCount++];
      e.start = start;
      e.end = start + len - 1;
      e.buttons = (u16)(btn & 0xffff);
      e.stickX = clampAxis(sx);
      e.stickY = clampAxis(sy);
    }
    fclose(f);
    fprintf(stderr, "mdkr64 oracle: loaded %u input event(s) from %s\n", eventCount, path);
  }

  auto configure() -> void {
    if(configured) return;
    configured = true;

    if(const char* lim = getenv("MDKR64_ARES_EXIT_AFTER_FRAMES")) {
      if(*lim) {
        char* end = nullptr;
        auto v = strtoull(lim, &end, 10);
        if(end && *end == 0) frameLimit = v;
      }
    }
    if(const char* d = getenv("MDKR64_ARES_DUMP_DIR")) {
      if(*d) { snprintf(dumpDir, sizeof(dumpDir), "%s", d); hasDumpDir = true; }
    }
    if(const char* ev = getenv("MDKR64_ARES_DUMP_EVERY")) {
      if(*ev) {
        char* end = nullptr;
        auto v = strtoull(ev, &end, 10);
        if(end && *end == 0) dumpEvery = v;
      }
    }
    if(const char* ds = getenv("MDKR64_ARES_DUMP_START")) {
      if(*ds) {
        char* end = nullptr;
        auto v = strtoull(ds, &end, 10);
        if(end && *end == 0) dumpStart = v;
      }
    }
    if(const char* mk = getenv("MDKR64_ARES_DUMP_MARKS")) {
      if(*mk) loadMarks(mk);
    }
    if(const char* s = getenv("MDKR64_ARES_INPUT_SCRIPT")) {
      if(*s) loadInputScript(s);
    }
    if(const char* path = getenv("MDKR64_ARES_STATE_TRACE")) {
      if(*path) {
        stateTrace = fopen(path, "wb");
        if(!stateTrace) {
          fprintf(stderr, "mdkr64 oracle: cannot open state trace %s\n", path);
        } else {
          fprintf(stateTrace,
            "frame,valid,map_id,slot,object,racer,x,y,z,x_velocity,y_velocity,z_velocity,"
            "forward_velocity,velocity,checkpoint,next_checkpoint,lap,count_lap,"
            "race_finished,finish_position,racer_index,player_index,vehicle,"
            "grounded_wheels,clock,race_start_timer,video_delta,logic_update_rate,"
            "rng_seed,framebuffer_serial,framebuffer,input_buttons,stick_x,stick_y\n");
        }
      }
    }
    if(const char* path = getenv("MDKR64_ARES_AUDIO_DUMP")) {
      if(*path) {
        audioDump = fopen(path, "wb");
        if(!audioDump) {
          fprintf(stderr, "mdkr64 oracle: cannot open audio dump %s\n", path);
        } else {
          snprintf(audioPath, sizeof(audioPath), "%s", path);
        }
      }
    }
    if(const char* poke = getenv("MDKR64_ARES_POKE_RACER_POS")) {
      if(*poke) {
        unsigned long long ps = 0, pe = 0;
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        int ptw = -1;
        int n = sscanf(poke, "%llu:%llu:%f:%f:%f:%d", &ps, &pe, &px, &py, &pz, &ptw);
        if(n >= 5 && pe >= ps) {
          pokeEnabled = true;
          pokeStart = ps;
          pokeEnd = pe;
          pokeX = px;
          pokeY = py;
          pokeZ = pz;
          pokeTrophyWorld = (n >= 6) ? ptw : -1;
          fprintf(stderr,
            "mdkr64 oracle: poke armed frames=[%llu,%llu] pos=(%g,%g,%g) trophyWorld=%d\n",
            ps, pe, px, py, pz, pokeTrophyWorld);
        } else {
          fprintf(stderr,
            "mdkr64 oracle: ignored malformed MDKR64_ARES_POKE_RACER_POS=%s\n", poke);
        }
      }
    }
    if(const char* rd = getenv("MDKR64_ARES_RDRAM_DUMP")) {
      /* FRAME[,FRAME...]:PREFIX */
      const char* colon = strchr(rd, ':');
      if(colon && colon[1]) {
        snprintf(rdramPrefix, sizeof(rdramPrefix), "%s", colon + 1);
        const char* cursor = rd;
        while(cursor < colon && rdramFrameCount < 16) {
          char* end = nullptr;
          auto v = strtoull(cursor, &end, 10);
          if(end == cursor) break;
          rdramFrames[rdramFrameCount++] = v;
          cursor = (*end == ',') ? end + 1 : end;
        }
      }
      if(rdramFrameCount == 0) {
        rdramPrefix[0] = 0;
        fprintf(stderr,
          "mdkr64 oracle: ignored malformed MDKR64_ARES_RDRAM_DUMP=%s\n", rd);
      } else {
        fprintf(stderr, "mdkr64 oracle: rdram dump armed frames=%u prefix=%s\n",
          rdramFrameCount, rdramPrefix);
      }
    }
    fprintf(stderr,
      "mdkr64 oracle: configured dumpDir=%s stateTrace=%s audioDump=%s every=%llu start=%llu marks=%u events=%u limit=%llu\n",
      hasDumpDir ? dumpDir : "(none)",
      stateTrace ? "on" : "off",
      audioDump ? audioPath : "(none)",
      (unsigned long long)dumpEvery, (unsigned long long)dumpStart,
      markCount, eventCount, (unsigned long long)frameLimit);
  }

  /*
   * One DMA'd stereo sample pair, little-endian s16 L,R -- the raw AI stream.
   * Only ACTIVE samples are written: ares' idle path exponentially decays the
   * held DAC value to emulate the analogue hold, which is an output-stage
   * artefact and not part of what the ROM's synthesiser produced. Writing it
   * would bias the very measurement this lane exists for.
   *
   * The sample rate is whatever the ROM programmed into AI_DACRATE (DKR asks
   * for 22050 Hz; the divider makes the true rate slightly different), so it is
   * recorded in a sidecar rather than assumed by the reader.
   */
  auto audioSample(s16 left, s16 right, double frequency) -> void {
    std::lock_guard<std::mutex> lock(audioMutex);
    configure();
    if(!audioDump) return;
    u8 bytes[4] = {
      (u8)((u16)left  & 0xff), (u8)(((u16)left  >> 8) & 0xff),
      (u8)((u16)right & 0xff), (u8)(((u16)right >> 8) & 0xff),
    };
    if(fwrite(bytes, 1, sizeof(bytes), audioDump) != sizeof(bytes)) {
      fprintf(stderr, "mdkr64 oracle: audio dump write failed\n");
      std::_Exit(2);
    }
    if(audioSamples == 0) {
      audioRate = (u32)(frequency + 0.5);
      char sidecar[1100];
      snprintf(sidecar, sizeof(sidecar), "%s.rate", audioPath);
      if(FILE* f = fopen(sidecar, "wb")) {
        fprintf(f, "%u\n", audioRate);
        fclose(f);
      }
      fprintf(stderr, "mdkr64 oracle: audio dump started, AI rate %u Hz\n", audioRate);
    }
    audioSamples++;
  }

  auto controllerRead(n32 data, const void* gamepad) -> n32 {
    configure();
    if(!hasInputScript) return data;
    /* PORT 1 ONLY. Gamepad::read() runs for EVERY connected controller and ares'
     * N64 has four ports configured, so a port-blind hook replayed the same
     * buttons on all four. In DKR that makes four players JOIN at PLAYER SELECT,
     * which changes the menu graph: charselect_confirm only takes the CAUTION
     * branch when gNumberOfActivePlayers == 1, so the real ROM skipped CAUTION
     * and ran ahead into track select while our port (one controller) showed
     * CAUTION -- scoring ~50% for two correctly-rendered but DIFFERENT screens.
     *
     * Discriminate on INSTANCE IDENTITY, not on the port name: Gamepad::port is
     * reassigned in the constructor to the *Pak* subport, so port->name() is
     * "Pak" for every controller and a name test silently matches nothing --
     * which disabled injection entirely, and ares then sat on the title screen
     * until the attract demo started. The SI polls the ports in order, so the
     * first gamepad to be read is port 1. */
    if(!firstGamepad) firstGamepad = gamepad;
    if(gamepad && gamepad != firstGamepad) return data;
    u16 buttons = 0;
    int sx = 0, sy = 0;
    for(u32 i = 0; i < eventCount; i++) {
      const auto& e = events[i];
      if(videoFrame >= e.start && videoFrame <= e.end) {
        buttons |= e.buttons;
        sx += e.stickX;
        sy += e.stickY;
      }
    }
    sx = clampAxis(sx);
    sy = clampAxis(sy);
    lastButtons = buttons;
    lastStickX = sx;
    lastStickY = sy;
    return ((u32)buttons << 16) | ((u32)(sx & 0xff) << 8) | (u32)(sy & 0xff);
  }

  auto shouldDump() const -> bool {
    if(!hasDumpDir) return false;
    u64 f = videoFrame;
    if(f < dumpStart) return false;
    for(u32 i = 0; i < markCount; i++) if(marks[i] == f) return true;
    if(dumpEvery && ((f - dumpStart) % dumpEvery) == 0) return true;
    return false;
  }

  auto writePpm(const u32* pixels, u32 pitch, u32 width, u32 height) -> void {
    char path[1200];
    snprintf(path, sizeof(path), "%s/frame_%05llu.ppm", dumpDir, (unsigned long long)videoFrame);
    FILE* out = fopen(path, "wb");
    if(!out) {
      fprintf(stderr, "mdkr64 oracle: cannot open %s\n", path);
      return;
    }
    fprintf(out, "P6\n%u %u\n255\n", width, height);
    for(u32 y = 0; y < height; y++) {
      for(u32 x = 0; x < width; x++) {
        u32 c = pixels[(u64)y * pitch + x];
        fputc((u8)((c >> 16) & 0xff), out);
        fputc((u8)((c >> 8) & 0xff), out);
        fputc((u8)(c & 0xff), out);
      }
    }
    fclose(out);
    dumpedCount++;
  }

  auto writeState() -> void {
    if(!stateTrace) return;

    u32 mapId = readU32(gMapIdAddress);
    u32 framebuffer = readU32(gVideoLastFramebufferAddress);
    if(framebuffer != 0 && framebuffer != lastFramebuffer) {
      lastFramebuffer = framebuffer;
      framebufferSerial++;
    }
    u32 racerArray = readU32(gRacersAddress);
    s32 racerCount = (s32)readU32(gNumRacersAddress);
    if(racerCount < 1 || racerCount > 10
        || !validPointer(racerArray, (u32)racerCount * 4)) {
      fprintf(stateTrace,
        "%llu,0,%d,-1,0x%08x,0x00000000,"
        "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
        "%d,%u,%d,%u,%llu,0x%08x,0x%04x,%d,%d\n",
        (unsigned long long)videoFrame, (s32)mapId, racerArray,
        (s32)readU32(gRaceStartTimerAddress),
        readU8(gVideoDeltaTimeAddress),
        (s32)readU32(sLogicUpdateRateAddress),
        readU32(gCurrentRngSeedAddress),
        (unsigned long long)framebufferSerial, framebuffer,
        lastButtons, lastStickX, lastStickY);
      fflush(stateTrace);
      return;
    }

    for(s32 slot = 0; slot < racerCount; slot++) {
      u32 object = readU32(racerArray + slot * 4);
      u32 racer = validPointer(object, 0x68) ? readU32(object + 0x64) : 0;
      bool valid = validPointer(object, 0x28) && validPointer(racer, 0x224);
      if(!valid) continue;

      s8 countLap = (s8)readU8(racer + 0x194);
      u32 clock = 0;
      u32 lapCount =
        countLap < 0 ? 0 : (countLap < 5 ? (u32)countLap + 1 : 5);
      for(u32 i = 0; i < lapCount; i++) {
        clock += readU32(racer + 0x128 + i * 4);
      }

      fprintf(stateTrace,
        "%llu,1,%d,%d,0x%08x,0x%08x,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
        "%.9g,%.9g,%d,%u,%u,%d,%d,%d,%u,%d,%d,%d,%u,%d,%u,%d,%u,%llu,"
        "0x%08x,0x%04x,%d,%d\n",
        (unsigned long long)videoFrame, (s32)mapId, slot, object, racer,
        readF32(object + 0x0c), readF32(object + 0x10), readF32(object + 0x14),
        readF32(object + 0x1c), readF32(object + 0x20), readF32(object + 0x24),
        readF32(racer + 0x08), readF32(racer + 0x2c),
        (s16)readU16(racer + 0x190), readU8(racer + 0x192),
        readU8(racer + 0x193), countLap, (s8)readU8(racer + 0x1d8),
        (s16)readU16(racer + 0x1ac), readU8(racer + 0x02),
        (s16)readU16(racer + 0x00), (s8)readU8(racer + 0x1d6),
        (s8)readU8(racer + 0x1e2), clock,
        (s32)readU32(gRaceStartTimerAddress),
        readU8(gVideoDeltaTimeAddress),
        (s32)readU32(sLogicUpdateRateAddress),
        readU32(gCurrentRngSeedAddress),
        (unsigned long long)framebufferSerial, framebuffer,
        lastButtons, lastStickX, lastStickY);
    }
    fflush(stateTrace);
  }

  auto pokeTick() -> void {
    if(!pokeEnabled || videoFrame < pokeStart) return;
    if((videoFrame - pokeStart) % 30 == 0) {
      fprintf(stderr,
        "mdkr64 oracle: poke status frame=%llu map=%d menu=%d mode=%d "
        "playable=%d trophyWorld=%d trophyRound=%d dest=%d loadLatch=%d\n",
        (unsigned long long)videoFrame,
        (s32)readDebugWord(gMapIdAddress),
        (s32)readDebugWord(gCurrentMenuIdAddress),
        (s32)readDebugWord(gGameModeAddress),
        (s32)readDebugWord(gPlayableMapIdAddress),
        (s32)readDebugWord(gTrophyRaceWorldIdAddress),
        (s32)readDebugWord(gTrophyRaceRoundAddress),
        (s32)(s8)readDebugByte(gLevelSettingsAddress + 2),
        (s32)readDebugWord(gLevelLoadLatchAddress));
      fflush(stderr);
    }
    if(videoFrame > pokeEnd) return;
    u32 racerArray = readDebugWord(gRacersAddress);
    s32 racerCount = (s32)readDebugWord(gNumRacersAddress);
    if(racerCount < 1 || racerCount > 10
        || !validPointer(racerArray, (u32)racerCount * 4)) return;
    for(s32 slot = 0; slot < racerCount; slot++) {
      u32 object = readDebugWord(racerArray + slot * 4);
      if(!validPointer(object, 0x68)) continue;
      u32 racer = readDebugWord(object + 0x64);
      if(!validPointer(racer, 0x224)) continue;
      if((s16)readDebugHalf(racer + 0x00) != 0) continue;  /* player one only */
      u32 bits = 0;
      memcpy(&bits, &pokeX, sizeof(bits));
      writeDebugWord(object + 0x0c, bits);
      memcpy(&bits, &pokeY, sizeof(bits));
      writeDebugWord(object + 0x10, bits);
      memcpy(&bits, &pokeZ, sizeof(bits));
      writeDebugWord(object + 0x14, bits);
      writeDebugWord(object + 0x1c, 0);
      writeDebugWord(object + 0x20, 0);
      writeDebugWord(object + 0x24, 0);
      if(!pokeAnnounced) {
        pokeAnnounced = true;
        fprintf(stderr,
          "mdkr64 oracle: poke engaged frame=%llu slot=%d object=0x%08x "
          "racer=0x%08x pos=(%g,%g,%g)\n",
          (unsigned long long)videoFrame, slot, object, racer,
          pokeX, pokeY, pokeZ);
        fflush(stderr);
      }
      if(pokeTrophyWorld >= 0 && !pokeSeeded) {
        pokeSeeded = true;
        writeDebugWord(gTrophyRaceWorldIdAddress, (u32)pokeTrophyWorld);
        writeDebugWord(gTrophyRaceRoundAddress, 2);
        fprintf(stderr,
          "mdkr64 oracle: poke seeded gTrophyRaceWorldId=%d gTrophyRaceRound=2\n",
          pokeTrophyWorld);
        fflush(stderr);
      }
      break;
    }
  }

  auto rdramTick() -> void {
    for(u32 i = 0; i < rdramFrameCount; i++) {
      if(rdramFrames[i] != videoFrame) continue;
      char path[1100];
      snprintf(path, sizeof(path), "%s_%llu.bin", rdramPrefix,
        (unsigned long long)videoFrame);
      FILE* out = fopen(path, "wb");
      if(!out) {
        fprintf(stderr, "mdkr64 oracle: cannot open rdram dump %s\n", path);
        continue;
      }
      static u8 image[0x00800000];
      for(u32 offset = 0; offset < sizeof(image); offset += 4) {
        u32 word = readDebugWord(0x80000000u + offset);
        image[offset + 0] = (u8)(word >> 24);
        image[offset + 1] = (u8)(word >> 16);
        image[offset + 2] = (u8)(word >> 8);
        image[offset + 3] = (u8)word;
      }
      bool ok = fwrite(image, 1, sizeof(image), out) == sizeof(image);
      ok = fclose(out) == 0 && ok;
      fprintf(stderr, "mdkr64 oracle: rdram dump frame=%llu bytes=%u %s %s\n",
        (unsigned long long)videoFrame, (u32)sizeof(image), ok ? "written" : "FAILED", path);
      fflush(stderr);
    }
  }

  // Called from screen.cpp on each present with the visible framebuffer.
  auto presentedDump(const u32* pixels, u32 pitch, u32 width, u32 height) -> void {
    configure();
    writeState();
    pokeTick();
    rdramTick();
    if(pixels && pitch && width && height) {
      if(shouldDump()) writePpm(pixels, pitch, width, height);
    }
    videoFrame++;
    if(frameLimit && videoFrame >= frameLimit) {
      // Presentation can run while the emulation thread delivers AI samples.
      // Freeze writes through process exit, flush successfully, then report the
      // exact stable count. A log printed before this boundary can undercount.
      std::lock_guard<std::mutex> lock(audioMutex);
      if(audioDump && fflush(audioDump) != 0) {
        fprintf(stderr, "mdkr64 oracle: audio dump flush failed\n");
        std::_Exit(2);
      }
      fprintf(stderr,
        "mdkr64 oracle: reached frame limit %llu (dumped %u frame(s), %llu audio sample-frame(s)), exiting\n",
        (unsigned long long)frameLimit, dumpedCount,
        (unsigned long long)audioSamples);
      if(stateTrace) fflush(stateTrace);
      fflush(stderr);
      std::_Exit(0);
    }
  }
};

auto oracleState() -> MdkrOracleState& {
  static MdkrOracleState state;
  return state;
}

}

auto mdkr64OracleControllerRead(n32 data, const void* gamepad) -> n32 {
  return oracleState().controllerRead(data, gamepad);
}

auto mdkr64OracleVideoFrameNow() -> u64 {
  return oracleState().videoFrame;
}

auto mdkr64OracleAudioSample(s16 left, s16 right, double frequency) -> void {
  oracleState().audioSample(left, right, frequency);
}

auto mdkr64OraclePresentedVideoDump(Node::Video::Screen screen, const u32* pixels, u32 pitch, u32 width, u32 height) -> void {
  (void)screen;
  oracleState().presentedDump(pixels, pitch, width, height);
}

}

namespace ares::Core::Video {

auto mdkr64OraclePresentedVideoDump(std::shared_ptr<Screen> screen, const u32* pixels, u32 pitch, u32 width, u32 height) -> void {
  ares::Nintendo64::mdkr64OraclePresentedVideoDump(screen, pixels, pitch, width, height);
}

}
'''

# --- n64.hpp: declare the hooks in the Interface class ----------------------
n64_hpp = src / "ares/n64/n64.hpp"
text = n64_hpp.read_text(encoding="utf-8")
if "#include <cstdlib>" not in text:
    text = text.replace("#include <float.h>\n",
                        "#include <float.h>\n#include <cstdio>\n#include <cstdlib>\n", 1)
declarations = []
if "mdkr64OracleControllerRead" not in text:
    declarations.append(
        "  auto mdkr64OracleControllerRead(n32 data, const void* gamepad) -> n32;\n")
if "mdkr64OracleAudioSample" not in text:
    declarations.append(
        "  auto mdkr64OracleAudioSample(s16 left, s16 right, double frequency) -> void;\n")
if "mdkr64OraclePresentedVideoDump" not in text:
    declarations.append(
        "  auto mdkr64OraclePresentedVideoDump(Node::Video::Screen screen, const u32* pixels, u32 pitch, u32 width, u32 height) -> void;\n")
if "mdkr64OracleVideoFrameNow" not in text:
    declarations.append(
        "  auto mdkr64OracleVideoFrameNow() -> u64;\n")
if declarations:
    anchor = "  auto option(string name, string value) -> bool;\n"
    if anchor not in text:
        raise SystemExit("FAIL: n64.hpp declaration anchor not found")
    text = text.replace(anchor, anchor + "".join(declarations), 1)
n64_hpp.write_text(text, encoding="utf-8")

# --- gamepad.cpp: route the controller read through the oracle --------------
gamepad_cpp = src / "ares/n64/controller/gamepad/gamepad.cpp"
text = gamepad_cpp.read_text(encoding="utf-8")
if "mdkr64OracleControllerRead" not in text:
    anchor = "\n  return data;\n}\n\nauto Gamepad::getInodeChecksum"
    if anchor not in text:
        raise SystemExit("FAIL: gamepad.cpp read anchor not found")
    text = text.replace(anchor,
                        "\n  return mdkr64OracleControllerRead(data, (const void*)this);\n}\n\nauto Gamepad::getInodeChecksum", 1)
    gamepad_cpp.write_text(text, encoding="utf-8")

# --- ai.cpp: tap the DMA'd audio samples ------------------------------------
# The tap sits on the ACTIVE branch of AI::sample(), where `data` is the word
# the ROM itself DMA'd to the audio interface. Taking it here rather than at
# stream->frame() keeps the reference free of ares' float conversion, its
# analogue-hold decay on the idle branch, and every host resampler/driver --
# so the capture is the real ROM's synthesiser output at full s16 scale, and
# it is deterministic for a given input route.
ai_cpp = src / "ares/n64/ai/ai.cpp"
text = ai_cpp.read_text(encoding="utf-8")
if "mdkr64OracleAudioSample" not in text:
    anchor = ("    dac.left  = (s16)(data >> 16) / 32768.0;\n"
              "    dac.right = (s16)(data >>  0) / 32768.0;\n")
    if anchor not in text:
        raise SystemExit("FAIL: ai.cpp sample anchor not found")
    text = text.replace(
        anchor,
        anchor
        + "    mdkr64OracleAudioSample((s16)(data >> 16), (s16)(data >> 0), dac.frequency);\n",
        1)
    ai_cpp.write_text(text, encoding="utf-8")

# --- screen.cpp: dump each presented frame ----------------------------------
screen_cpp = src / "ares/ares/node/video/screen.cpp"
text = screen_cpp.read_text(encoding="utf-8")
if "mdkr64OraclePresentedVideoDump(std::shared_ptr<Screen>" not in text:
    anchor = "#include <memory>\n"
    if anchor not in text:
        raise SystemExit("FAIL: screen.cpp include anchor not found")
    text = text.replace(
        anchor,
        anchor
        + "\nauto mdkr64OraclePresentedVideoDump(std::shared_ptr<Screen> screen, const u32* pixels, u32 pitch, u32 width, u32 height) -> void;\n",
        1)
if "mdkr64OraclePresentedVideoDump(presentedScreen" not in text:
    anchor = "  platform->video(std::static_pointer_cast<Core::Video::Screen>(shared_from_this()), output + viewX + viewY * width, width * sizeof(u32), viewWidth, viewHeight);\n"
    if anchor not in text:
        raise SystemExit("FAIL: screen.cpp present anchor not found")
    text = text.replace(
        anchor,
        "  auto presentedScreen = std::static_pointer_cast<Core::Video::Screen>(shared_from_this());\n"
        "  mdkr64OraclePresentedVideoDump(presentedScreen, output + viewX + viewY * width, width, viewWidth, viewHeight);\n"
        "  platform->video(presentedScreen, output + viewX + viewY * width, width * sizeof(u32), viewWidth, viewHeight);\n",
        1)
screen_cpp.write_text(text, encoding="utf-8")

# --- vi.cpp: append the oracle translation unit -----------------------------
vi_cpp = src / "ares/n64/vi/vi.cpp"
text = vi_cpp.read_text(encoding="utf-8")
marker = "struct MdkrOracleState"
if marker in text:
    # Re-apply idempotently: strip any previously appended block first.
    start = text.rfind("#include <n64/n64.hpp>", 0, text.find(marker))
    if start >= 0:
        text = text[:start].rstrip() + "\n"
vi_cpp.write_text(text.rstrip() + "\n\n" + oracle_cpp.rstrip() + "\n", encoding="utf-8")

# --- cpu.cpp: retarget one real one-player race load ------------------------
cpu_cpp = src / "ares/n64/cpu/cpu.cpp"
text = cpu_cpp.read_text(encoding="utf-8")
if "mdkr64OracleMaybeForceTrack" not in text:
    anchor = "auto CPU::instruction() -> bool {\n"
    if anchor not in text:
        raise SystemExit("FAIL: cpu.cpp instruction anchor not found")
    helper = r'''static auto mdkr64OracleMaybeForceTrack(CPU& cpu) -> void {
  static bool configured = false;
  static s32 sourceTrack = -1;
  static s32 targetTrack = -1;
  /* Optional third field: rewrite only the Nth matching load (1-based), so a
   * route can race the source track normally first. 0 rewrites every one. */
  static s32 occurrence = 0;
  static s32 matches = 0;
  if(!configured) {
    configured = true;
    if(const char* value = getenv("MDKR64_ARES_FORCE_TRACK")) {
      int fields = sscanf(value, "%d:%d:%d", &sourceTrack, &targetTrack, &occurrence);
      if(fields < 2 || occurrence < 0) {
        sourceTrack = targetTrack = -1;
        occurrence = 0;
        fprintf(stderr,
          "mdkr64 oracle: ignored malformed MDKR64_ARES_FORCE_TRACK=%s\n",
          value);
      }
    }
  }
  /*
   * US 1.1 level_load entry (PAL 1.1 shares the address,
   * docs/ref/symbols/symbol_addrs.pal.v80.txt). The route selects Ancient Lake normally, then this
   * rewrites only a one-player gameplay call (a1 == 0). Menu backgrounds and
   * track previews use negative player counts and remain byte-for-byte retail.
   */
  if(targetTrack >= 0 && (u32)cpu.ipu.pc == 0x8006b490
      && cpu.ipu.r[4].s32 == sourceTrack && cpu.ipu.r[5].s32 == 0) {
    matches++;
    if(occurrence == 0 || matches == occurrence) {
      fprintf(stderr, "mdkr64 oracle: forced retail race load %d -> %d\n",
        sourceTrack, targetTrack);
      cpu.ipu.r[4].u64 = (s64)targetTrack;
    } else {
      fprintf(stderr, "mdkr64 oracle: retail race load %d left as is (match %d of %d)\n",
        sourceTrack, matches, occurrence);
    }
  }
}

'''
    text = text.replace(
        anchor,
        helper + anchor + "  mdkr64OracleMaybeForceTrack(*this);\n",
        1,
    )
if "mdkr64OracleTraceVehicleRng" not in text:
    anchor = "auto CPU::instruction() -> bool {\n"
    if anchor not in text:
        raise SystemExit("FAIL: cpu.cpp vehicle-RNG anchor not found")
    helper = r'''static auto mdkr64OracleTraceVehicleRng(CPU& cpu) -> void {
  static bool configured = false;
  static FILE* trace = nullptr;
  static u64 ordinal = 0;
  if(!configured) {
    configured = true;
    if(const char* path = getenv("MDKR64_ARES_VEHICLE_RNG_TRACE")) {
      if(*path) {
        trace = fopen(path, "wb");
        if(!trace) {
          fprintf(stderr,
            "mdkr64 oracle: cannot open vehicle RNG trace %s\n", path);
        } else {
          fprintf(trace,
            "ordinal,pc,ra,seed_before,min,max,racer,player,vehicle\n");
        }
      }
    }
  }
  if(!trace || (u32)cpu.ipu.pc != 0x8006fb8c) return;

  /*
   * US 1.1 mathRnd/rand_range entry. A return address inside
   * racer_sound_car proves that the retail car-audio routine itself consumes
   * the shared authored RNG stream. gSoundRacerObj identifies whether the
   * caller is an ordinary player car or a special vehicle.
   */
  u32 ra = (u32)cpu.ipu.r[31].u64;
  if(ra < 0x80005254 || ra >= 0x80005d08) return;
  /* Read through the CPU's debug path so dirty KSEG0 D-cache lines are
   * authoritative. Reading backing RDRAM here can report the prior seed. */
  u32 seed = (u32)cpu.readDebug<Word>(0xffff'ffff'800d'd9a4ull);
  u32 racer = (u32)cpu.readDebug<Word>(0xffff'ffff'8011'a1bcull);
  s32 player = -32768;
  s32 vehicle = -128;
  if((racer & 0x1fffffff) < 0x00800000) {
    u32 address = racer & 0x1fffffff;
    player = (s16)cpu.readDebug<Half>(0xffff'ffff'8000'0000ull + address);
    vehicle = (s8)cpu.readDebug<Byte>(
      0xffff'ffff'8000'0000ull + address + 0x1d7);
  }
  fprintf(trace, "%llu,0x%08x,0x%08x,%u,%d,%d,0x%08x,%d,%d\n",
    (unsigned long long)++ordinal, (u32)cpu.ipu.pc, ra, seed,
    cpu.ipu.r[4].s32, cpu.ipu.r[5].s32, racer, player, vehicle);
  fflush(trace);
}

'''
    text = text.replace(
        anchor,
        helper + anchor + "  mdkr64OracleTraceVehicleRng(*this);\n",
        1,
    )
if "mdkr64OracleTraceMenuResult" not in text:
    anchor = "auto CPU::instruction() -> bool {\n"
    if anchor not in text:
        raise SystemExit("FAIL: cpu.cpp menu-result anchor not found")
    helper = r'''static auto mdkr64OracleTraceMenuResult(CPU& cpu) -> void {
  static bool configured = false;
  static FILE* trace = nullptr;
  static u32 prevPC = 0;
  static s32 lastMenuId = -0x7fffffff;
  static u64 ordinal = 0;
  /*
   * Issue #55 witness: the exact value the retail US 1.1 menu_loop returns to
   * mode_menu, per call. The C for MENU_UNUSED_8 (id 8) has no switch case and
   * returns `ret` UNINITIALIZED (authored UB), so the value cannot be read
   * from source -- only measured. Address ranges come from
   * docs/ref/symbols/symbol_addrs.us.v80.txt: menu_loop 0x800819F4 (next
   * function menu_number_render 0x80082054), its only caller mode_menu
   * 0x8006DF38 (next function load_level_for_menu 0x8006E528). menu_loop's
   * callees return INTO menu_loop and interrupts land in neither range, so an
   * instruction boundary that steps from inside menu_loop directly to inside
   * mode_menu is menu_loop's return, and v0 is the routed value. Rows are
   * kept for every menuId==8 return plus each menuId change for context.
   */
  constexpr u32 menuLoopBegin = 0x800819f4, menuLoopEnd = 0x80082054;
  constexpr u32 modeMenuBegin = 0x8006df38, modeMenuEnd = 0x8006e528;
  if(!configured) {
    configured = true;
    if(const char* path = getenv("MDKR64_ARES_MENU_RESULT_TRACE")) {
      if(*path) {
        trace = fopen(path, "wb");
        if(!trace) {
          fprintf(stderr,
            "mdkr64 oracle: cannot open menu result trace %s\n", path);
        } else {
          fprintf(trace,
            "ordinal,frame,ret,menu_id,game_mode,playable_map,"
            "trophy_world,trophy_round,level_settings2\n");
        }
      }
    }
  }
  if(!trace) return;
  u32 pc = (u32)cpu.ipu.pc;
  bool wasInMenuLoop = prevPC >= menuLoopBegin && prevPC < menuLoopEnd;
  prevPC = pc;
  if(!wasInMenuLoop || pc < modeMenuBegin || pc >= modeMenuEnd) return;
  u32 ret = (u32)cpu.ipu.r[2].u64;  /* v0 = menu_loop's return value */
  s32 menuId = (s32)cpu.readDebug<Word>(0xffff'ffff'800d'f9f0ull);
  ++ordinal;
  if(menuId != lastMenuId || menuId == 8) {
    lastMenuId = menuId;
    fprintf(trace, "%llu,%llu,0x%08x,%d,%d,%d,%d,%d,%d\n",
      (unsigned long long)ordinal,
      (unsigned long long)mdkr64OracleVideoFrameNow(),
      ret, menuId,
      (s32)cpu.readDebug<Word>(0xffff'ffff'8012'3a6cull),   /* gGameMode */
      (s32)cpu.readDebug<Word>(0xffff'ffff'8012'3a74ull),   /* gPlayableMapId */
      (s32)cpu.readDebug<Word>(0xffff'ffff'800e'1568ull),   /* gTrophyRaceWorldId */
      (s32)cpu.readDebug<Word>(0xffff'ffff'800e'156cull),   /* gTrophyRaceRound */
      (s32)(s8)cpu.readDebug<Byte>(0xffff'ffff'8012'17d2ull) /* gLevelSettings[2] */);
    fflush(trace);
  }
}

'''
    text = text.replace(
        anchor,
        helper + anchor + "  mdkr64OracleTraceMenuResult(*this);\n",
        1,
    )
cpu_cpp.write_text(text, encoding="utf-8")

# --- macOS build fixes (same as mgb64's oracle build) -----------------------
compilerconfig = src / "cmake/macos/compilerconfig.cmake"
if compilerconfig.exists():
    text = compilerconfig.read_text(encoding="utf-8")
    if "--show-sdk-version" not in text:
        needle = """  execute_process(
    COMMAND xcrun --sdk macosx --show-sdk-platform-version
    OUTPUT_VARIABLE ares_macos_current_sdk
    RESULT_VARIABLE result
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
"""
        replacement = needle + """  if(NOT result EQUAL 0)
    execute_process(
      COMMAND xcrun --sdk macosx --show-sdk-version
      OUTPUT_VARIABLE ares_macos_current_sdk
      RESULT_VARIABLE result
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  endif()
"""
        text = text.replace(needle, replacement, 1)
        compilerconfig.write_text(text, encoding="utf-8")

macos_ui = src / "desktop-ui/cmake/os-macos.cmake"
if macos_ui.exists():
    text = macos_ui.read_text(encoding="utf-8")
    text = text.replace("if(ACTOOL_PROGRAM)\n",
                        'if(ACTOOL_PROGRAM AND NOT "$ENV{ARES_CLT_BUILD}" STREQUAL "1")\n', 1)
    macos_ui.write_text(text, encoding="utf-8")

print("Applied mdkr64 oracle instrumentation.")
PY

if [[ "$NO_BUILD" -eq 1 ]]; then
    echo "Prepared instrumented ares checkout: $SRC_DIR"
    exit 0
fi

ARES_CLT_BUILD=1 cmake -S "$SRC_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target desktop-ui --parallel "$JOBS"

if [[ -x "$BUILD_DIR/desktop-ui/ares.app/Contents/MacOS/ares" ]]; then
    ARES_BIN="$BUILD_DIR/desktop-ui/ares.app/Contents/MacOS/ares"
elif [[ -x "$BUILD_DIR/desktop-ui/ares" ]]; then
    ARES_BIN="$BUILD_DIR/desktop-ui/ares"
else
    echo "FAIL: built desktop-ui target, but could not find the ares executable" >&2
    exit 1
fi

echo ""
echo "PASS: instrumented ares binary:"
echo "  $ARES_BIN"
echo ""
echo "Run a route on both emulator + native with:"
echo "  tools/run_oracle.sh boot_to_title --ares-bin \"$ARES_BIN\""
