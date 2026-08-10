/**
 * platform_sdl_min.c — the SDL2 host layer: window/context lifecycle, controller
 * and keyboard input, content-pack discovery, the cooperative frame boundary,
 * and VI pacing.
 *
 * Responsibilities:
 *   - Create the window for whichever backend is active (GL 3.3 core via glad,
 *     or a Metal view / browser canvas for WebGPU) and tear it down cleanly,
 *     including the explicitly selected GL diagnostic path. WebGPU failures
 *     never switch renderers inside the live process.
 *   - Open game controllers, load SDL mappings, and drive the deterministic
 *     input-script fixtures the regression checks replay.
 *   - Scan the player's mods/ directory and hold the pack registry the renderer's
 *     texture override store borrows, including the Tab key that switches those
 *     overrides off and back on mid-race.
 *   - Own the frame boundary. The collapsed single-threaded game loop blocks in
 *     osRecvMesg on the video queue; that block calls in here to poll input,
 *     present, and advance the headless frame counter.
 *   - Own VI retrace pacing: measure the wall-clock interval between presents
 *     and return the updateRate the game needs to hold constant wall-clock
 *     speed, honouring the configured one- or two-field floor.
 *   - Publish the [PACE]/[PVEH] probes the pacing and race-progress checks
 *     assert on, under MDKR_TRACE.
 *
 * Invariant: exactly one window and one backend context exist at a time, and
 * every SDL handle created here is released in platform_sdl_shutdown().
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdint.h>
#include <limits.h>
#include <time.h>
#ifndef __EMSCRIPTEN__
#ifdef _WIN32
/* Ahead of SDL_syswm.h, which pulls the same header in without these guards.
 * The pacer's Windows wait (pace_sleep_until) needs the waitable-timer API. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <glad/glad.h>          /* GL loader — desktop only; web is WebGPU-only */
#include <SDL_syswm.h>
#else
#include <emscripten/emscripten.h>
#include <emscripten/em_js.h>
#include <emscripten/html5.h>
/* Block (via Asyncify) until the browser's next animation frame. This is the
 * web build's ONLY per-frame suspend point — the requestAnimationFrame wake is
 * display-synced (unlike emscripten_sleep's setTimeout, which browsers clamp),
 * so the frame pacer lands on vsync edges and the WebGPU canvas is presented
 * when this yields. On the narrow ASYNCIFY_ADD spine (osRecvMesg reaches it via
 * direct calls). Models mgb64's platformWaitAnimationFrame. */
EM_ASYNC_JS(void, platformWaitAnimationFrame, (void), {
    while (document.visibilityState === 'hidden') {
        await new Promise((resolve) => {
            const visible = () => {
                if (document.visibilityState !== 'hidden') {
                    document.removeEventListener('visibilitychange', visible);
                    resolve();
                }
            };
            document.addEventListener('visibilitychange', visible);
        });
    }
    const timestamp = await new Promise(
        (resolve) => requestAnimationFrame(resolve));
    /* The browser regression shell exposes this bounded array only under its
     * inert CDP test bridge. Capture the actual compositor opportunities here,
     * at the production rAF boundary, so cadence failures can distinguish a
     * slow/occluded host from scheduler or renderer work. */
    if (Array.isArray(globalThis.__mdkrActualRafDeltas)) {
        const prior = Number(globalThis.__mdkrActualRafTimestamp);
        if (Number.isFinite(prior)) {
            globalThis.__mdkrActualRafDeltas.push(timestamp - prior);
            if (globalThis.__mdkrActualRafDeltas.length > 12000) {
                globalThis.__mdkrActualRafDeltas.shift();
            }
        }
        globalThis.__mdkrActualRafTimestamp = timestamp;
    }
});
EM_ASYNC_JS(double, platformWaitSyntheticAnimationFrame, (void), {
    return await globalThis.__mdkrWaitAnimationFrame();
});
EM_JS(int, platformAnimationFrameClockSynthetic, (void), {
    return globalThis.__mdkrSyntheticAnimationFrameClock === true ? 1 : 0;
});
EM_JS(double, platformAnimationFrameClockDeltaNs, (void), {
    const value = Number(globalThis.__mdkrLastAnimationFrameDeltaNs);
    return Number.isFinite(value) && value > 0 ? value : 0;
});
static uint64_t s_browserFrameNowNs;
static uint64_t pace_host_ns(void);

static void browser_record_animation_frame(double timestamp_ms) {
    (void)timestamp_ms;
    if (platformAnimationFrameClockSynthetic()) {
        const double supplied_ns = platformAnimationFrameClockDeltaNs();
        const uint64_t delta_ns =
            supplied_ns > 0.0 && supplied_ns <= (double)UINT64_MAX
                ? (uint64_t)supplied_ns : 0u;
        if (UINT64_MAX - s_browserFrameNowNs < delta_ns) {
            s_browserFrameNowNs = UINT64_MAX;
        } else {
            s_browserFrameNowNs += delta_ns;
        }
    } else {
        /* Ordinary play keeps counting render/host work between callbacks;
         * only the injected schedule owns a frozen per-opportunity clock. */
        s_browserFrameNowNs = 0u;
    }
}

static uint64_t browser_wait_animation_frame(void) {
    if (platformAnimationFrameClockSynthetic()) {
        browser_record_animation_frame(platformWaitSyntheticAnimationFrame());
    } else {
        platformWaitAnimationFrame();
        s_browserFrameNowNs = 0u;
    }
    return pace_host_ns();
}
#endif
#include <SDL.h>

uint64_t platform_perf_monotonic_ns(void) {
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    const uint64_t counter = SDL_GetPerformanceCounter();
    const uint64_t seconds = frequency != 0U ? counter / frequency : 0U;
    const uint64_t remainder = frequency != 0U ? counter % frequency : 0U;

    if (frequency == 0U || seconds > UINT64_MAX / UINT64_C(1000000000)) {
        return 0U;
    }
    return seconds * UINT64_C(1000000000) +
           remainder * UINT64_C(1000000000) / frequency;
}
#include "platform_os.h"
#include "fs_utf8.h"
#include "app/app_brand.h"
#include "user_paths.h"
#include "pacing_policy.h"
#include "present_sched.h"
#include "input_latency_census.h"
#include "gameplay_event_trace.h"
#include "a11y_race.h"
#include "input_tick_queue.h"
#include "input_consumption_trace.h"
#include "controller_mapping.h"
#include "video_config.h"
#include "mod_registry.h"
#include "mod_texture_store.h"
#include "mdkr_bounds.h"
#include "gfx_ptr.h"
#ifdef MDKR_APP
#include "host_window.h"          /* app-shell window/device handoff */
#include "app_overlay_hooks.h"    /* in-game overlay event/render hooks */
#endif
#include "presentation_snapshot.h" /* live policy apply retires the staged pair */
#include "fast3d/gfx_pc_dkr.h"   /* gfx_dkr_texload_line_swapped (headless report) */
#include "fast3d/gfx_font_outline.h" /* gfx_font_outline_clipped_texels */
#include "fast3d/gfx_level_lighting.h"
#include "fast3d/gfx_shadow_cascade.h"
#include "fast3d/gfx_shadow_frame.h"
#include "fast3d/gfx_uniforms.h"
#ifndef __EMSCRIPTEN__
#include "fast3d/gfx_opengl.h"
#endif
#ifdef MDKR_WEBGPU_BACKEND
#include "fast3d/gfx_webgpu.h"
#endif

int g_headlessFrames = -1;   /* -1 = run forever; >=0 = exit after N frames */
int g_headlessTicks = -1;    /* -1 = frame budget; >=0 = exit after N ticks */
int g_frameCounter   = 0;
uint64_t g_surfaceFrameCounter = 0;
const char *g_dumpFramesDir = NULL;  /* --dump-frames DIR (P6 PPM per frame) */
static int s_exitRequested = 0;
static int s_exitCode = 0;

void platform_request_exit(int code) {
    /*
     * A later fatal request may strengthen an already-requested clean exit,
     * but a later success must never erase a failure.
     */
    if (!s_exitRequested || code != 0) {
        s_exitCode = code;
    }
    s_exitRequested = 1;
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (typeof Module !== 'undefined') {
            Module.__mdkrExitRequested = true;
            Module.__mdkrExitCode = $0;
        }
    }, s_exitCode);
#endif
}

int platform_exit_requested(void) {
    return s_exitRequested;
}

int platform_exit_code(void) {
    return s_exitCode;
}

/* ---- MDKR_TRACE boot diagnostics ---------------------------------------- */
int mdkr_trace_level(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MDKR_TRACE");
        cached = (e && e[0]) ? atoi(e) : 0;
#ifdef __EMSCRIPTEN__
        /* No process env in the browser; let the shell/harness set the trace
         * level via a JS global (Module.__mdkrTrace) before callMain. */
        if (cached == 0) {
            cached = EM_ASM_INT({ return (typeof Module !== 'undefined' && Module.__mdkrTrace) | 0; });
        }
#endif
        if (cached < 0) cached = 0;
    }
    return cached;
}
int mdkr_trace_enabled(void) {
    return mdkr_trace_level() > 0;
}
int mdkr_resource_trace_enabled(void) {
    static int cached = -1;
    if (mdkr_trace_enabled()) {
        return 1;
    }
    if (cached < 0) {
        const char *value = getenv("MDKR_RESOURCE_STATS");
        cached = value != NULL && value[0] == '1' && value[1] == '\0';
    }
    return cached;
}
void mdkr_trace(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[TRACE] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static SDL_Window   *s_window = NULL;
static SDL_GLContext s_glctx  = NULL;
static int           s_glReady = 0;
static int           s_sdlReady = 0;
static int           s_renderBackend = -1;
static int           s_renderBackendUnavailable = 0;
static int           s_surfaceRenderElided;
static int           s_surfaceResumeRebasePending;
static int           s_testMinimizeStart = -2;
static int           s_testMinimizeEnd = -2;
static int           s_testForcedMinimized;
#if defined(MDKR_WEBGPU_BACKEND) && defined(__APPLE__)
static SDL_MetalView s_metalView = NULL;   /* CAMetalLayer host for the WGPUSurface */
#endif
#ifdef MDKR_APP
/* The app shell owns the window; the engine must not destroy it at shutdown or
 * the launcher's ImGui context is left pointing at freed memory. */
static int           s_windowAdopted = 0;
#endif

/* ---- Render backend selection (M4.5) ------------------------------------ *
 * Resolved once from MDKR_RENDERER (webgpu|gl|metal), default WebGPU. Validated
 * against the backends actually compiled in: webgpu needs MDKR_WEBGPU_BACKEND;
 * metal is not built in mdkr64 (gfx_metal.mm is Obj-C++, excluded), so that
 * spelling selects WebGPU's Metal-backed implementation. GL remains available
 * only as an explicit diagnostic selection. A WebGPU-disabled build is GL-only
 * by construction, but an explicit WebGPU request still fails closed. The
 * choice drives both the
 * window/context kind (platform_sdl_init) and the &gfx_*_api gfx_init receives
 * (main_pc), so it must be a single cached source of truth. */
int mdkr_render_backend(void) {
    if (s_renderBackend >= 0) return s_renderBackend;
    const char *e = getenv("MDKR_RENDERER");
#ifdef MDKR_WEBGPU_BACKEND
    int want = MDKR_BACKEND_WEBGPU;   /* visually qualified native default */
#else
    int want = MDKR_BACKEND_GL;       /* this binary was built GL-only */
#endif
    if (e && e[0]) {
        if      (!strcasecmp(e, "gl") || !strcasecmp(e, "opengl")) want = MDKR_BACKEND_GL;
        else if (!strcasecmp(e, "webgpu") || !strcasecmp(e, "wgpu")) want = MDKR_BACKEND_WEBGPU;
        else if (!strcasecmp(e, "metal")) want = MDKR_BACKEND_METAL;
        else {
            fprintf(stderr, "[mdkr64] MDKR_RENDERER='%s' unrecognized "
                            "(webgpu|gl|metal); using the compiled default.\n", e);
#ifdef MDKR_WEBGPU_BACKEND
            want = MDKR_BACKEND_WEBGPU;
#else
            want = MDKR_BACKEND_GL;
#endif
        }
    }
    if (want == MDKR_BACKEND_METAL) {
        /* Native Metal backend (gfx_metal.mm) is not compiled in mdkr64. */
        fprintf(stderr, "[mdkr64] Metal backend not available in mdkr64; "
                        "using WebGPU (Metal-backed via wgpu-native).\n");
        want = MDKR_BACKEND_WEBGPU;
    }
#ifndef MDKR_WEBGPU_BACKEND
    if (want == MDKR_BACKEND_WEBGPU) {
        fprintf(stderr, "[mdkr64] WebGPU was explicitly selected, but this "
                        "build is OpenGL-only (MDKR_WEBGPU_BACKEND=OFF).\n");
        s_renderBackendUnavailable = 1;
    }
#endif
    s_renderBackend = want;
    return s_renderBackend;
}

int mdkr_render_backend_available(void) {
    (void)mdkr_render_backend();
    return !s_renderBackendUnavailable;
}

const char *mdkr_render_backend_name(void) {
    switch (mdkr_render_backend()) {
        case MDKR_BACKEND_WEBGPU: return "webgpu";
        case MDKR_BACKEND_METAL:  return "metal";
        default:                  return "gl";
    }
}

/* ======================================================================== *
 *  Input — SDL keyboard / game controller / scripted -> OSContPad state.
 *
 *  The event pump captures host transitions on every presentation opportunity.
 *  platform_input_commit_tick publishes one bounded queue sample per fixed
 *  simulation ticket; presents never directly replace DKR-visible pad state.
 *
 *  N64 controller button bits — MUST match game/include/PR/os_cont.h CONT_*.
 * ======================================================================== */
#define N64_A       MDKR_N64_A
#define N64_B       MDKR_N64_B
#define N64_Z       MDKR_N64_Z       /* Z-trigger (CONT_G) */
#define N64_START   MDKR_N64_START
#define N64_DU      MDKR_N64_DU      /* D-pad up   (CONT_UP)    */
#define N64_DD      MDKR_N64_DD      /* D-pad down (CONT_DOWN)  */
#define N64_DL      MDKR_N64_DL      /* D-pad left  (CONT_LEFT) */
#define N64_DR      MDKR_N64_DR      /* D-pad right (CONT_RIGHT)*/
#define N64_L       MDKR_N64_L       /* L shoulder (CONT_L) */
#define N64_R       MDKR_N64_R       /* R shoulder (CONT_R) */
#define N64_CU      MDKR_N64_CU      /* C-up    (CONT_E) */
#define N64_CD      MDKR_N64_CD      /* C-down  (CONT_D) */
#define N64_CL      MDKR_N64_CL      /* C-left  (CONT_C) */
#define N64_CR      MDKR_N64_CR      /* C-right (CONT_F) */

#define DKR_MAXPADS 4
#define STICK_FULL  80        /* menu deflection (spec: full ±80) */

struct pad_state {
    unsigned int buttons;
    int stick_x, stick_y;
    int present;
};
static struct pad_state s_pads[DKR_MAXPADS];
static MdkrInputTickQueue s_inputQueue;
static int s_inputQueueReady;

struct controller_source_state {
    Uint8 buttons[SDL_CONTROLLER_BUTTON_MAX];
    Sint16 axes[SDL_CONTROLLER_AXIS_MAX];
};
static struct controller_source_state s_controllerSource[DKR_MAXPADS];
static Uint8 s_keyboardDown[SDL_NUM_SCANCODES];
static int s_gameInputSuppressed;
static int s_testScriptOnlyInput = -1;
#ifdef __EMSCRIPTEN__
static struct pad_state s_browserTouchSource;
#endif

static void input_capture_live(uint64_t target_tick);
/* The pacer's host clock, defined with the rest of the pacing state below. The
 * input latency census timestamps against it so every term in the budget is on
 * the same clock the pacer itself schedules on. */
static uint64_t pace_host_ns(void);

static SDL_GameController *s_gc[DKR_MAXPADS] = { NULL, NULL, NULL, NULL };
/* DKR's current motor request per port. Kept separate from the user mute/profile
 * so a live preference change can stop or refresh an already-running effect. */
static Uint8 s_rumbleRequested[DKR_MAXPADS] = { 0, 0, 0, 0 };
/* -1 means unprobed.  Capability discovery is cached for the lifetime of the
 * opened controller because SDL's compatibility probe is itself a zero-power
 * rumble command on older releases. Reissuing that command from osPfsIsPlug()
 * can stop an effect which DKR still considers active. */
static int8_t s_rumbleSupported[DKR_MAXPADS] = { -1, -1, -1, -1 };
static int s_quitRequested = 0;

#ifdef __EMSCRIPTEN__
/*
 * Emscripten's SDL controller layer has not implemented rumble consistently
 * across SDL port revisions. Use the browser's standard Gamepad actuator as a
 * fallback, addressed by SDL's joystick instance id (which is the browser
 * Gamepad.index in Emscripten SDL2). Returning false is harmless: libultra then
 * reports no Rumble Pak instead of claiming feedback that never happens.
 */
EM_JS(int, browser_gamepad_rumble_supported, (int instanceId), {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    const pad = pads && pads[instanceId];
    return !!(pad && (pad.vibrationActuator ||
        (pad.hapticActuators && pad.hapticActuators.length)));
});
EM_JS(int, browser_gamepad_rumble, (int instanceId, int strength), {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    const pad = pads && pads[instanceId];
    if (!pad) return 0;
    const actuator = pad.vibrationActuator ||
        (pad.hapticActuators && pad.hapticActuators[0]);
    if (!actuator) return 0;
    try {
        if (strength <= 0) {
            if (typeof actuator.reset === "function") actuator.reset();
            else if (typeof actuator.playEffect === "function") {
                actuator.playEffect(actuator.type || "dual-rumble", {
                    duration: 0, startDelay: 0,
                    strongMagnitude: 0, weakMagnitude: 0
                });
            }
            return 1;
        }
        const magnitude = Math.max(0, Math.min(1, strength / 65535));
        if (typeof actuator.playEffect === "function") {
            actuator.playEffect(actuator.type || "dual-rumble", {
                duration: 60000, startDelay: 0,
                strongMagnitude: magnitude, weakMagnitude: magnitude
            }).catch(() => {});
            return 1;
        }
        if (typeof actuator.pulse === "function") {
            actuator.pulse(magnitude, 60000).catch(() => {});
            return 1;
        }
    } catch (_) {
        return 0;
    }
    return 0;
});

/*
 * Touch input is published by the browser shell as plain Module state and
 * sampled here at the same boundary as SDL keyboard/gamepad input. Calling an
 * exported wasm function directly from a pointer callback would re-enter the
 * module while Asyncify has main() suspended at requestAnimationFrame. Sampling
 * avoids that re-entrancy entirely and also makes one coherent pad snapshot per
 * game frame.
 */
EM_JS(void, browser_touch_pad_read,
      (unsigned int *buttons, int *stickX, int *stickY, int *enabled), {
    const pad = (typeof Module !== "undefined") && Module.__mdkrTouchPad;
    const active = pad && pad.enabled !== false;
    HEAPU32[buttons >> 2] = active ? (Number(pad.buttons) >>> 0) : 0;
    HEAP32[stickX >> 2] = active ? (Number(pad.stickX) | 0) : 0;
    HEAP32[stickY >> 2] = active ? (Number(pad.stickY) | 0) : 0;
    HEAP32[enabled >> 2] = active ? 1 : 0;
});
EM_JS(int, browser_touch_pad_pop,
      (unsigned int *buttons, int *stickX, int *stickY, int *enabled), {
    const pad = (typeof Module !== "undefined") && Module.__mdkrTouchPad;
    const events = pad && Array.isArray(pad.events) ? pad.events : null;
    if (!events || events.length === 0) return 0;
    const event = events.shift();
    const active = event && event.enabled !== false;
    HEAPU32[buttons >> 2] = active ? (Number(event.buttons) >>> 0) : 0;
    HEAP32[stickX >> 2] = active ? (Number(event.stickX) | 0) : 0;
    HEAP32[stickY >> 2] = active ? (Number(event.stickY) | 0) : 0;
    HEAP32[enabled >> 2] = active ? 1 : 0;
    return 1;
});
#endif

/* ---- Scripted input (--input-script FILE) ------------------------------- *
 * Lines: `frame TOKEN[+TOKEN...] [holdFrames] [P1..P4]`. Injected into the named
 * controller port for [frame, frame+hold) authoritative ticks; the port field is
 * optional and defaults to P1, so every pre-existing single-pad script parses
 * unchanged. Blank lines and `#` comments ignored. Directional tokens drive the
 * analog stick (DKR menus read the stick, not the D-pad, for navigation) and
 * also set the matching D-pad bit.
 *
 * Why a port field: two-player split-screen needs input on controller 1 as well
 * as 0 — a player JOINS at PLAYER SELECT by pressing A on their OWN pad
 * (charselect_new_player(), menu.c), so there is no way to reach
 * gNumberOfActivePlayers == 2 by injecting into port 0 only. The game side
 * already reads all four pads (osContGetReadData in stubs_dkr.c fills
 * MAXCONTROLLERS entries from platform_pad_buttons(i)); only the script harness
 * was single-port. */
struct script_entry { int frame; int hold; unsigned int buttons; int stick_x, stick_y; int port; };
#define MAX_SCRIPT 512
static struct script_entry s_script[MAX_SCRIPT];
static int s_scriptCount = 0;
static unsigned int s_scriptPresentMask = 0;

static unsigned int script_token_to_bit(const char *tok, int *sx, int *sy) {
    /* Buttons */
    if (!strcasecmp(tok, "A"))          return N64_A;
    if (!strcasecmp(tok, "B"))          return N64_B;
    if (!strcasecmp(tok, "Z"))          return N64_Z;
    if (!strcasecmp(tok, "START"))      return N64_START;
    if (!strcasecmp(tok, "L"))          return N64_L;
    if (!strcasecmp(tok, "R"))          return N64_R;
    if (!strcasecmp(tok, "CUP")   || !strcasecmp(tok, "C_UP"))    return N64_CU;
    if (!strcasecmp(tok, "CDOWN") || !strcasecmp(tok, "C_DOWN"))  return N64_CD;
    if (!strcasecmp(tok, "CLEFT") || !strcasecmp(tok, "C_LEFT"))  return N64_CL;
    if (!strcasecmp(tok, "CRIGHT")|| !strcasecmp(tok, "C_RIGHT")) return N64_CR;
    /* Directions: stick + D-pad bit (N64 stick_y +up / -down). */
    if (!strcasecmp(tok, "UP")    || !strcasecmp(tok, "DPAD_UP")    || !strcasecmp(tok, "DUP"))    { if (sy) *sy =  STICK_FULL; return N64_DU; }
    if (!strcasecmp(tok, "DOWN")  || !strcasecmp(tok, "DPAD_DOWN")  || !strcasecmp(tok, "DDOWN"))  { if (sy) *sy = -STICK_FULL; return N64_DD; }
    if (!strcasecmp(tok, "LEFT")  || !strcasecmp(tok, "DPAD_LEFT")  || !strcasecmp(tok, "DLEFT"))  { if (sx) *sx = -STICK_FULL; return N64_DL; }
    if (!strcasecmp(tok, "RIGHT") || !strcasecmp(tok, "DPAD_RIGHT") || !strcasecmp(tok, "DRIGHT")) { if (sx) *sx =  STICK_FULL; return N64_DR; }
    fprintf(stderr, "[input-script] unknown token '%s'\n", tok);
    return 0;
}

int platform_input_load_script(const char *path) {
    FILE *f = mdkr_fopen_utf8(path, "r");
    if (!f) { fprintf(stderr, "[input-script] cannot open %s\n", path); return -1; }
    char line[256];
    s_scriptCount = 0;
    s_scriptPresentMask = 0;
    while (fgets(line, sizeof(line), f) && s_scriptCount < MAX_SCRIPT) {
        /* strip comment */
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        /* first token = frame number */
        char *save = NULL;
        char *ftok = strtok_r(line, " \t\r\n", &save);
        if (!ftok) continue;
        int frame = atoi(ftok);
        char *btok = strtok_r(NULL, " \t\r\n", &save);
        if (!btok) continue;
        struct script_entry e = { frame, 3, 0, 0, 0, 0 };
        /* button field may be TOKEN+TOKEN+... */
        char bcopy[128];
        snprintf(bcopy, sizeof(bcopy), "%s", btok);
        char *bsave = NULL;
        for (char *p = strtok_r(bcopy, "+", &bsave); p; p = strtok_r(NULL, "+", &bsave)) {
            e.buttons |= script_token_to_bit(p, &e.stick_x, &e.stick_y);
        }
        /* optional hold-frame count */
        char *htok = strtok_r(NULL, " \t\r\n", &save);
        if (htok) { int h = atoi(htok); if (h > 0) e.hold = h; }
        /* optional controller port: `P1`..`P4` (1-based, as authored). Anything
         * else is a hard error rather than a silent default — a typo'd port on a
         * split-screen route would otherwise send player 2's input to player 1
         * and quietly produce a one-player run. */
        char *ptok = strtok_r(NULL, " \t\r\n", &save);
        if (ptok) {
            if ((ptok[0] == 'P' || ptok[0] == 'p') && ptok[1] >= '1' && ptok[1] <= '4' && ptok[2] == '\0') {
                e.port = ptok[1] - '1';
            } else {
                fprintf(stderr, "[input-script] bad port field '%s' (expected P1..P4) in %s\n", ptok, path);
                fclose(f);
                return -1;
            }
        }
        s_scriptPresentMask |= 1u << e.port;
        s_script[s_scriptCount++] = e;
    }
    fclose(f);
    printf("[input-script] loaded %d entries from %s\n", s_scriptCount, path);
    return 0;
}

/* Apply every entry addressed to `port` onto that port's pad state. */
static void script_apply(struct pad_state *pads, int frame) {
    for (int i = 0; i < s_scriptCount; i++) {
        struct script_entry *e = &s_script[i];
        if (frame >= e->frame && frame < e->frame + e->hold) {
            struct pad_state *p = &pads[e->port];
            p->buttons |= e->buttons;
            if (e->stick_x) p->stick_x = e->stick_x;
            if (e->stick_y) p->stick_y = e->stick_y;
        }
    }
}

/* The vendored OpenGL backend (gfx_opengl.c) reads this to query the drawable
 * size (SDL_GL_GetDrawableSize) for HiDPI framebuffer sizing. Point it at our
 * window once created; NULL is handled by the backend. */
SDL_Window *g_sdlWindow = NULL;

#define DEFAULT_WIN_W 640
#define DEFAULT_WIN_H 480
static int s_initialWindowWidth = DEFAULT_WIN_W;
static int s_initialWindowHeight = DEFAULT_WIN_H;

void platform_sdl_set_initial_size(int w, int h) {
    /* Called before platform_sdl_init(). Keep the bounds broad enough for
     * ultrawide/HiDPI testing while rejecting typo-sized allocations. */
    if (w >= 160 && h >= 120 && w <= 16384 && h <= 16384) {
        s_initialWindowWidth = w;
        s_initialWindowHeight = h;
    }
}

#ifndef __EMSCRIPTEN__
/* Bring up a complete GL window/context/loader tuple. A partial tuple is torn
 * down and reported as failure: game code must never run against an inert
 * renderer. Desktop only — the web build is WebGPU-only. */
#define GL_FRAME_IN_FLIGHT_MAX 2u
static int s_glSwapEffective = 1;
static GLsync s_glPresentFences[GL_FRAME_IN_FLIGHT_MAX];
static unsigned s_glFenceHead;
static unsigned s_glFencesInFlight;
static unsigned s_glFenceHighWater;
static uint64_t s_glFrameSubmissions;
static uint64_t s_glFrameCompletions;
static uint64_t s_glBackpressureWaits;
static uint64_t s_glBackpressurePolls;
static uint64_t s_glBackpressureFailures;
static uint64_t s_glBackpressureWaitNs;
static uint64_t s_glFirstSubmitNs;
static uint64_t s_glLastSubmitNs;

static void sdl_gl_backpressure_reset_stats(void) {
    memset(s_glPresentFences, 0, sizeof(s_glPresentFences));
    s_glFenceHead = 0u;
    s_glFencesInFlight = 0u;
    s_glFenceHighWater = 0u;
    s_glFrameSubmissions = 0u;
    s_glFrameCompletions = 0u;
    s_glBackpressureWaits = 0u;
    s_glBackpressurePolls = 0u;
    s_glBackpressureFailures = 0u;
    s_glBackpressureWaitNs = 0u;
    s_glFirstSubmitNs = 0u;
    s_glLastSubmitNs = 0u;
}

static uint64_t sdl_gl_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

static bool sdl_gl_retire_oldest(bool wait) {
    GLsync fence;
    GLenum status;
    unsigned attempts = 0u;
    uint64_t started = 0u;

    if (s_glFencesInFlight == 0u) {
        return true;
    }
    fence = s_glPresentFences[s_glFenceHead];
    if (fence == NULL) {
        s_glFenceHead = (s_glFenceHead + 1u) % GL_FRAME_IN_FLIGHT_MAX;
        s_glFencesInFlight--;
        s_glBackpressureFailures++;
        return false;
    }
    if (wait) {
        started = sdl_gl_monotonic_ns();
        s_glBackpressureWaits++;
    }
    do {
        status = glClientWaitSync(
            fence, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
            wait ? UINT64_C(100000000) : 0u);
        s_glBackpressurePolls++;
        attempts++;
    } while (wait && status == GL_TIMEOUT_EXPIRED && attempts < 20u);
    if (wait) {
        s_glBackpressureWaitNs += sdl_gl_monotonic_ns() - started;
    }
    if (status == GL_ALREADY_SIGNALED ||
        status == GL_CONDITION_SATISFIED) {
        glDeleteSync(fence);
        s_glPresentFences[s_glFenceHead] = NULL;
        s_glFenceHead = (s_glFenceHead + 1u) % GL_FRAME_IN_FLIGHT_MAX;
        s_glFencesInFlight--;
        s_glFrameCompletions++;
        return true;
    }
    if (!wait && status == GL_TIMEOUT_EXPIRED) {
        return false;
    }

    fprintf(stderr,
            "[gl] GPU completion fence failed (status=0x%x attempts=%u)\n",
            (unsigned)status, attempts);
    s_glBackpressureFailures++;
    glDeleteSync(fence);
    s_glPresentFences[s_glFenceHead] = NULL;
    s_glFenceHead = (s_glFenceHead + 1u) % GL_FRAME_IN_FLIGHT_MAX;
    s_glFencesInFlight--;
    platform_request_exit(EXIT_FAILURE);
    return false;
}

static void sdl_gl_backpressure_after_swap(void) {
    uint64_t now;
    unsigned slot;

    now = sdl_gl_monotonic_ns();
    if (s_glFirstSubmitNs == 0u) {
        s_glFirstSubmitNs = now;
    }
    s_glLastSubmitNs = now;
    s_glFrameSubmissions++;
    if (s_glSwapEffective > 0) {
        return; /* FIFO swap itself is the queue bound. */
    }

    if (glFenceSync == NULL || glClientWaitSync == NULL ||
        glDeleteSync == NULL) {
        /* A 3.3 core context should always expose sync objects. glFinish is a
         * conservative bounded fallback instead of silently allowing queue
         * growth on an unusual loader/driver. */
        glFinish();
        s_glFrameCompletions++;
        s_glBackpressureWaits++;
        s_glBackpressureFailures++;
        return;
    }
    while (s_glFencesInFlight > 0u &&
           sdl_gl_retire_oldest(false)) {
        /* Retire every completion already observable without blocking. */
    }
    if (s_glFencesInFlight >= GL_FRAME_IN_FLIGHT_MAX) {
        (void)sdl_gl_retire_oldest(true);
    }
    slot = (s_glFenceHead + s_glFencesInFlight) % GL_FRAME_IN_FLIGHT_MAX;
    s_glPresentFences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (s_glPresentFences[slot] == NULL) {
        glFinish();
        s_glFrameCompletions++;
        s_glBackpressureWaits++;
        s_glBackpressureFailures++;
        return;
    }
    s_glFencesInFlight++;
    if (s_glFencesInFlight > s_glFenceHighWater) {
        s_glFenceHighWater = s_glFencesInFlight;
    }
    glFlush();
    if (s_glFencesInFlight >= GL_FRAME_IN_FLIGHT_MAX) {
        (void)sdl_gl_retire_oldest(true);
    }
}

static void sdl_gl_backpressure_shutdown(void) {
    uint64_t rate_millihz = 0u;
    while (s_glFencesInFlight > 0u) {
        (void)sdl_gl_retire_oldest(true);
    }
    if (s_glFrameSubmissions > 1u &&
        s_glLastSubmitNs > s_glFirstSubmitNs) {
        const uint64_t elapsed = s_glLastSubmitNs - s_glFirstSubmitNs;
        const uint64_t intervals = s_glFrameSubmissions - 1u;
        if (intervals <= UINT64_MAX / UINT64_C(1000000000000)) {
            rate_millihz = intervals * UINT64_C(1000000000000) / elapsed;
        }
    }
    fprintf(stderr,
            "[GL-BACKPRESSURE] cap=%u opportunities=%d submitted=%llu "
            "held=%llu completed=%llu "
            "inflight=%u highwater=%u waits=%llu polls=%llu failures=%llu "
            "waitns=%llu rateMilliHz=%llu effectiveSwap=%d swapBound=%d\n",
            GL_FRAME_IN_FLIGHT_MAX,
            g_frameCounter,
            (unsigned long long)s_glFrameSubmissions,
            (unsigned long long)(
                (uint64_t)g_frameCounter > s_glFrameSubmissions
                    ? (uint64_t)g_frameCounter - s_glFrameSubmissions : 0u),
            (unsigned long long)s_glFrameCompletions,
            s_glFencesInFlight, s_glFenceHighWater,
            (unsigned long long)s_glBackpressureWaits,
            (unsigned long long)s_glBackpressurePolls,
            (unsigned long long)s_glBackpressureFailures,
            (unsigned long long)s_glBackpressureWaitNs,
            (unsigned long long)rate_millihz,
            s_glSwapEffective, s_glSwapEffective > 0 ? 1 : 0);
}

/*
 * The GL diagnostic backend's mirror of the WebGPU present-mode policy. GL has
 * no mailbox, so a policy that wants the latest image still swaps on the
 * vblank: the deadline grid keeps the requested cap, and above the refresh the
 * swap becomes the ceiling. Only the tearing opt-in leaves the vblank, and it
 * prefers adaptive sync, which tears on a late frame and holds otherwise.
 *
 * Automation keeps its uncapped swap: a finite --headless-frames budget is a
 * throughput measurement, not an image a player looks at.
 */
static void sdl_apply_gl_present_policy(void) {
    const bool automation = g_headlessFrames >= 0;
    const bool tearing = !automation && present_sched_allow_tearing();
    const int requested = automation ? 0 : (tearing ? -1 : 1);
    int result;
    int effective;

    sdl_gl_backpressure_reset_stats();
    result = SDL_GL_SetSwapInterval(requested);
    if (result != 0 && tearing) {
        result = SDL_GL_SetSwapInterval(0);
    }
    effective = SDL_GL_GetSwapInterval();
    s_glSwapEffective = effective;

    fprintf(stderr,
            "[PRESENT-MODE] backend=gl policy=%s rate=%u tearing=%d "
            "requestedSwap=%d effectiveSwap=%d supported=%d\n",
            present_sched_present_policy_name(),
            present_sched_present_rate(), tearing ? 1 : 0, requested, effective,
            result == 0 && (effective == requested ||
                            (tearing && effective == 0)) ? 1 : 0);
}

static int sdl_init_gl(Uint32 base_flags) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    s_window = SDL_CreateWindow(MDKR_BRAND_NAME " — OpenGL diagnostics",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                s_initialWindowWidth, s_initialWindowHeight,
                                base_flags | SDL_WINDOW_OPENGL);
    if (!s_window) {
        fprintf(stderr, "[SDL] CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }
    g_sdlWindow = s_window;   /* expose to the fast3d GL backend */
    s_glctx = SDL_GL_CreateContext(s_window);
    if (!s_glctx) {
        fprintf(stderr, "[SDL] GL context failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_window);
        s_window = NULL;
        g_sdlWindow = NULL;
        return -1;
    }
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "[SDL] glad load failed\n");
        SDL_GL_DeleteContext(s_glctx);
        s_glctx = NULL;
        SDL_DestroyWindow(s_window);
        s_window = NULL;
        g_sdlWindow = NULL;
        return -1;
    }
    sdl_apply_gl_present_policy();
    s_glReady = 1;
    printf("[SDL] GL ready: %s / %s\n",
           (const char *)glGetString(GL_VERSION),
           (const char *)glGetString(GL_RENDERER));
    return 0;
}
#endif /* !__EMSCRIPTEN__ */

static int sdl_should_hide_window(void) {
    /*
     * Surface-present integration needs a bounded frame count without making
     * the native swapchain genuinely hidden (Metal then has no drawable).
     * This test-only opt-out leaves --headless-frames' deterministic pacing
     * and automatic exit intact while exposing the window for those cases.
     */
    const char *visible_headless = getenv("MDKR_TEST_VISIBLE_HEADLESS");
    if (visible_headless != NULL && strcmp(visible_headless, "1") == 0) {
        return 0;
    }
    return g_headlessFrames >= 0 || getenv("MDKR64_HIDDEN") != NULL;
}

static int sdl_should_start_fullscreen(void) {
#ifdef __EMSCRIPTEN__
    return 0;
#else
    const MdkrVideoConfig *config = mdkr_video_config_current();
    return !sdl_should_hide_window() && config != NULL &&
        strcmp(config->values[MDKR_WINDOW_MODE].text, "fullscreen") == 0;
#endif
}

int platform_sdl_surface_presentable(void) {
#ifdef __EMSCRIPTEN__
    return 1;
#else
    if (s_window == NULL) {
        return 0;
    }
    const Uint32 flags = SDL_GetWindowFlags(s_window);
    return (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
#endif
}

int platform_sdl_init(void) {
    s_surfaceRenderElided = 0;
    s_surfaceResumeRebasePending = 0;
    s_testMinimizeStart = -2;
    s_testMinimizeEnd = -2;
    s_testForcedMinimized = 0;
    present_sched_set_surface_elided(false);
    if (!mdkr_render_backend_available()) {
        fprintf(stderr,
                "[SDL] requested renderer is unavailable; stopping without "
                "an automatic fallback.\n");
        return -1;
    }
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 &&
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[SDL] init failed: %s\n", SDL_GetError());
        return -1;
    }
    s_sdlReady = 1;

#ifdef MDKR_APP
    /*
     * App-shell handoff: when the launcher already created the window (and, on
     * WebGPU, the device/surface), adopt them instead of creating a second one.
     * This is what makes Play continue in the SAME window rather than closing
     * the launcher and opening a new one.
     *
     * Nothing is adopted when no host is registered — every automation/CLI
     * invocation goes through mdkr64_headless_main WITHOUT a host window, so
     * that path still creates its own window exactly as before.
     */
    if (platformHasHostWindow()) {
        s_window = (SDL_Window *)platformHostWindow();
        g_sdlWindow = s_window;
        s_windowAdopted = 1;
        s_glctx = (SDL_GLContext)platformHostGLContext();
        if (s_glctx != NULL) {
            /* GL host: make the borrowed context current and load GLAD here,
             * exactly as sdl_init_gl() does for an engine-owned context.
             *
             * On macOS the app shell deliberately uses the system OpenGL
             * declarations and therefore does not initialize GLAD.  Assuming
             * otherwise left every function used only by the presentation
             * pacer (glFenceSync/glClientWaitSync/glFinish) NULL.  FIFO hid
             * that until Video.FrameLimit=uncapped selected the explicit GPU
             * queue bound, which then called the NULL glFinish fallback.
             * Treat a borrowed context as a complete context/loader tuple too:
             * gameplay must never start with a partially live GL API. */
            if (SDL_GL_MakeCurrent(s_window, s_glctx) != 0) {
                fprintf(stderr, "[SDL] adopted GL context could not be made current: %s\n",
                        SDL_GetError());
                s_glctx = NULL;
                return -1;
            }
            if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
                fprintf(stderr, "[SDL] glad load failed for adopted GL context\n");
                s_glctx = NULL;
                return -1;
            }
            sdl_apply_gl_present_policy();
            s_glReady = 1;
            printf("[SDL] adopted the app shell's GL window\n");
        } else {
            /* WebGPU host: gfx_webgpu.c's wgpu_init takes the device/surface
             * from platformHostWgpu*() and skips its own bring-up, so no Metal
             * view is created here — the shell owns it. */
            printf("[SDL] adopted the app shell's WebGPU window\n");
        }
        return 0;
    }
#endif

    Uint32 base_flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    if (sdl_should_hide_window()) {
        base_flags |= SDL_WINDOW_HIDDEN;
    } else if (sdl_should_start_fullscreen()) {
        /* Desktop fullscreen preserves the monitor's current mode while
         * covering it exactly. BORDERLESS is explicit so Windows cannot leave
         * ordinary overlapped-window decorations around the adopted surface. */
        base_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS;
    }

#ifdef __EMSCRIPTEN__
    /* Browser: the WebGPU surface is created from the "#canvas" selector by the
     * vendored backend (gfx_webgpu_compat.h), NOT from an SDL window handle, so
     * this SDL window exists only to route keyboard/mouse/gamepad events from the
     * page. It must bind to the SAME canvas WebGPU renders into — emscripten's
     * SDL2 uses Module.canvas (the shell sets it to #canvas). No GL, no Metal. */
    {
        int canvasWidth = 0, canvasHeight = 0;
        if (emscripten_get_canvas_element_size("#canvas", &canvasWidth, &canvasHeight) ==
                EMSCRIPTEN_RESULT_SUCCESS &&
            canvasWidth >= 160 && canvasHeight >= 120) {
            s_initialWindowWidth = canvasWidth;
            s_initialWindowHeight = canvasHeight;
        }
    }
    s_window = SDL_CreateWindow(MDKR_BRAND_NAME, SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                s_initialWindowWidth, s_initialWindowHeight,
                                base_flags);
    if (s_window == NULL) {
        fprintf(stderr, "[SDL] Emscripten window creation failed: %s\n",
                SDL_GetError());
        return -1;
    }
    g_sdlWindow = s_window;
    printf("[SDL] Window created (Emscripten / WebGPU via #canvas)\n");
    return 0;   /* device/surface bring-up happens in gfx_init -> wgpu_init */
#else /* !__EMSCRIPTEN__ */

#ifdef MDKR_WEBGPU_BACKEND
    if (mdkr_render_backend() == MDKR_BACKEND_WEBGPU) {
        /* WebGPU renders through wgpu-native, which on macOS wraps a CAMetalLayer
         * (no GL context). Create a Metal window + view; gfx_webgpu.c pulls the
         * layer via platformGetMetalLayer() during wgpu_init. On non-macOS the
         * surface comes straight from the native window handle (no Metal view). */
        Uint32 flags = base_flags;
#ifdef __APPLE__
        flags |= SDL_WINDOW_METAL;
#endif
        const char *forceWindowFailure = getenv("MDKR_TEST_WEBGPU_WINDOW_FAIL");
        if (forceWindowFailure != NULL && forceWindowFailure[0] == '1') {
            /* Test-only fault injection for fail-closed window creation. A real
             * SDL failure reaches the same branch; no-op unless explicitly set. */
            s_window = NULL;
            fprintf(stderr, "[SDL] WebGPU window failure forced for fail-closed test.\n");
        } else {
            s_window = SDL_CreateWindow(MDKR_BRAND_NAME " — WebGPU",
                                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        s_initialWindowWidth, s_initialWindowHeight,
                                        flags);
        }
        if (!s_window) {
            const char *reason =
                (forceWindowFailure != NULL && forceWindowFailure[0] == '1')
                    ? "forced by MDKR_TEST_WEBGPU_WINDOW_FAIL"
                    : SDL_GetError();
            fprintf(stderr,
                    "[SDL] WebGPU window creation failed: %s; stopping without "
                    "an automatic GL fallback. Set MDKR_RENDERER=gl explicitly "
                    "for diagnostics.\n",
                    reason);
            return -1;
        }
        g_sdlWindow = s_window;
#ifdef __APPLE__
        s_metalView = SDL_Metal_CreateView(s_window);
        if (!s_metalView) {
            fprintf(stderr,
                    "[SDL] Metal view creation failed: %s; stopping without "
                    "an automatic GL fallback. Set MDKR_RENDERER=gl explicitly "
                    "for diagnostics.\n",
                    SDL_GetError());
            SDL_DestroyWindow(s_window);
            s_window = NULL; g_sdlWindow = NULL;
            return -1;
        }
#endif
        if (getenv("MDKR_TEST_VISIBLE_HEADLESS") != NULL) {
            SDL_ShowWindow(s_window);
            SDL_RaiseWindow(s_window);
            SDL_PumpEvents();
        }
        printf("[SDL] Window created (WebGPU / wgpu-native)\n");
        return 0;   /* device/surface bring-up happens in gfx_init -> wgpu_init */
    }
#endif /* MDKR_WEBGPU_BACKEND */

    return sdl_init_gl(base_flags);
#endif /* __EMSCRIPTEN__ */
}

/* ---- Native window handles for the WebGPU backend ----------------------- */
void *platformGetSdlWindow(void) { return (void *)s_window; }

void *platformGetMetalLayer(void) {
#if defined(MDKR_WEBGPU_BACKEND) && defined(__APPLE__)
    return s_metalView ? SDL_Metal_GetLayer(s_metalView) : NULL;
#else
    return NULL;
#endif
}

#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
enum MdkrWebGpuWindowSystem platformWebGpuWindowInfo(
    void *sdl_window, void **out_display, void **out_window,
    unsigned long long *out_id) {
    SDL_SysWMinfo info;
    if (out_display != NULL) *out_display = NULL;
    if (out_window != NULL) *out_window = NULL;
    if (out_id != NULL) *out_id = 0;
    if (sdl_window == NULL) {
        fprintf(stderr, "[webgpu] SDL native-window query received NULL\n");
        return MDKR_WGPU_WINDOW_UNKNOWN;
    }
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo((SDL_Window *) sdl_window, &info)) {
        fprintf(stderr, "[webgpu] SDL native-window query failed: %s\n",
                SDL_GetError());
        return MDKR_WGPU_WINDOW_UNKNOWN;
    }
    switch (info.subsystem) {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
        case SDL_SYSWM_WINDOWS:
            if (out_display != NULL) {
                *out_display = (void *)info.info.win.hinstance;
            }
            if (out_window != NULL) {
                *out_window = (void *)info.info.win.window;
            }
            return MDKR_WGPU_WINDOW_WIN32;
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
        case SDL_SYSWM_X11:
            if (out_display != NULL) {
                *out_display = (void *)info.info.x11.display;
            }
            if (out_id != NULL) {
                *out_id = (unsigned long long)info.info.x11.window;
            }
            return MDKR_WGPU_WINDOW_X11;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            if (out_display != NULL) {
                *out_display = (void *)info.info.wl.display;
            }
            if (out_window != NULL) {
                *out_window = (void *)info.info.wl.surface;
            }
            return MDKR_WGPU_WINDOW_WAYLAND;
#endif
        default:
            fprintf(stderr,
                    "[webgpu] SDL window subsystem %d has no WebGPU surface "
                    "adapter\n",
                    (int)info.subsystem);
            return MDKR_WGPU_WINDOW_UNKNOWN;
    }
}
#endif

/* Drawable (framebuffer) size in physical pixels — HiDPI aware. Falls back to
 * the logical window size, then to the default 640x480. Used to size the
 * renderer (gfx_set_dimensions). */
void platform_sdl_drawable_size(int *w, int *h) {
    int dw = s_initialWindowWidth, dh = s_initialWindowHeight;
#ifdef __EMSCRIPTEN__
    /*
     * SDL_GetWindowSize reports the logical SDL window created at boot and can
     * remain 640x480 after JavaScript resizes the WebGPU canvas. Query the canvas
     * backing store directly: this is the physical-pixel size configured by the
     * shell after DPR and GPU-budget clamping.
     */
    if (emscripten_get_canvas_element_size("#canvas", &dw, &dh) != EMSCRIPTEN_RESULT_SUCCESS) {
        dw = s_initialWindowWidth;
        dh = s_initialWindowHeight;
    }
#else
    if (s_window) {
        if (s_glReady) {
            SDL_GL_GetDrawableSize(s_window, &dw, &dh);
#if defined(MDKR_WEBGPU_BACKEND) && defined(__APPLE__)
        /*
         * Metal-backed WebGPU window. Test the window's own flag rather than
         * s_metalView: under the app shell the view belongs to the launcher, so
         * s_metalView is NULL here even though this IS a Metal window, and
         * falling through to the logical-size branch would feed the renderer a
         * half-resolution drawable on every Retina display.
         */
        } else if (s_metalView ||
                   (SDL_GetWindowFlags(s_window) & SDL_WINDOW_METAL)) {
            SDL_Metal_GetDrawableSize(s_window, &dw, &dh);
#endif
        } else {
#if SDL_VERSION_ATLEAST(2, 26, 0)
            /*
             * A WebGPU surface is configured in physical pixels. On scaled
             * Wayland/X11 desktops SDL_GetWindowSize may report logical units,
             * producing a low-resolution or suboptimal swapchain after resize
             * and fullscreen. SDL 2.26 added the backend-neutral pixel query;
             * retain the logical fallback for older supported SDL2 headers.
             */
            SDL_GetWindowSizeInPixels(s_window, &dw, &dh);
#else
            SDL_GetWindowSize(s_window, &dw, &dh);
#endif
        }
    }
#endif
    if (dw <= 0) dw = s_initialWindowWidth;
    if (dh <= 0) dh = s_initialWindowHeight;
    if (w) *w = dw;
    if (h) *h = dh;
}

void platform_sdl_sync_drawable_size(void) {
    int width, height;
    platform_sdl_drawable_size(&width, &height);
    gfx_set_dimensions((uint32_t)width, (uint32_t)height);
}

/* Capture the last completed frame to DIR/frame_%04d.ppm as binary P6.
 * Backend readback returns rows bottom-up; PPM is top-down, so rows are flipped
 * on write. No external deps. */
static int s_dumpFrom = -2;
static int s_dumpEvery = 1;

static void platform_frame_dump_filter_init(void) {
    if (s_dumpFrom != -2) return;
    const char *e = getenv("MDKR_DUMP_FROM");
    const char *n = getenv("MDKR_DUMP_EVERY");
    s_dumpFrom = (e && e[0]) ? atoi(e) : -1;
    if (n && n[0]) s_dumpEvery = atoi(n);
    if (s_dumpEvery < 1) s_dumpEvery = 1;
}

int platform_frame_dump_due(void) {
    if (!g_dumpFramesDir) return 0;
    platform_frame_dump_filter_init();
    if (s_dumpFrom >= 0 && g_frameCounter < s_dumpFrom) return 0;
    if (s_dumpEvery > 1) {
        const int base = s_dumpFrom > 0 ? s_dumpFrom : 0;
        if (((g_frameCounter - base) % s_dumpEvery) != 0) return 0;
    }
    return 1;
}

int platform_frame_dump_prepare_due(void) {
    enum { DUMP_ADMISSION_PREROLL = 8 };
    int distance;

    if (!g_dumpFramesDir) return 0;
    platform_frame_dump_filter_init();
    if (s_dumpFrom >= 0 && g_frameCounter < s_dumpFrom) {
        distance = s_dumpFrom - g_frameCounter;
        return distance <= DUMP_ADMISSION_PREROLL;
    }
    if (s_dumpEvery <= 1) return 1;
    {
        const int base = s_dumpFrom > 0 ? s_dumpFrom : 0;
        const int phase = (g_frameCounter - base) % s_dumpEvery;
        distance = phase == 0 ? 0 : s_dumpEvery - phase;
    }
    return distance <= DUMP_ADMISSION_PREROLL;
}

static void platform_dump_frame(void) {
    if (!platform_frame_dump_due()) return;
#ifdef MDKR_WEBGPU_BACKEND
    /* WebGPU renders the scene offscreen and reads it back through the rendering
     * API (works even for a hidden window — no drawable needed). GL copies its
     * retained completed-frame image below. Both return bottom-left-origin RGB,
     * so the PPM row-flip is shared. */
    int is_webgpu = (mdkr_render_backend() == MDKR_BACKEND_WEBGPU) && !s_glReady && s_window;
#else
    int is_webgpu = 0;
#endif
    if (!s_glReady && !is_webgpu) {
        return;
    }
    /*
     * Capture the extent of the frame that was actually rendered, not a fresh
     * SDL size sample. platform_frame_sync() deliberately publishes resize
     * events after this dump; sampling SDL here can therefore describe the
     * next frame while readback still contains the previous one. WebGPU may
     * additionally debounce its committed output target for a few frames.
     */
    uint32_t capture_width = 0, capture_height = 0;
    if (!gfx_get_capture_dimensions(&capture_width, &capture_height) ||
        capture_width > INT_MAX || capture_height > INT_MAX) {
        return;
    }
    int w = (int)capture_width;
    int h = (int)capture_height;
    size_t rowBytes = (size_t) w * 3u;
    unsigned char *pix = (unsigned char *) malloc(rowBytes * (size_t) h);
    if (!pix) {
        return;
    }
#ifdef MDKR_WEBGPU_BACKEND
    if (is_webgpu) {
        /* gfx_read_framebuffer_rgb (gfx_pc_dkr.c) delegates to the active
         * backend's read_framebuffer_rgb. On failure, leave the (uninitialised)
         * buffer unwritten — skip the frame rather than dump garbage. */
        extern int gfx_read_framebuffer_rgb(int x, int y, int width, int height,
                                            unsigned char *rgb_out);
        if (!gfx_read_framebuffer_rgb(0, 0, w, h, pix)) {
            free(pix);
            return;
        }
    } else
#endif
    {
#ifndef __EMSCRIPTEN__
        /*
         * Never sample GL_BACK here. A VI present can occur without a new DKR
         * graphics task, after swap has made that buffer undefined/older. The
         * backend stashes the last completed composited frame before its swap.
         */
        if (!gfx_opengl_copy_captured_frame(w, h, pix)) {
            free(pix);
            return;
        }
#else
        /* No GL on web; the WebGPU readback path above is the only one. Frame
         * dumping is gated off in the shipped web build regardless. */
        free(pix);
        return;
#endif
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/frame_%04d.ppm", g_dumpFramesDir, g_frameCounter);
    FILE *f = mdkr_fopen_utf8(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) {
            fwrite(pix + (size_t) y * rowBytes, 1, rowBytes, f);
        }
        fclose(f);
    }
    free(pix);
}

/* ---- Content packs (see platform_os.h) ---------------------------------- *
 *
 * The registry is a plain value owned here for the life of the process, because
 * the texture store BORROWS it (mod_texture_store.h) and must not outlive it.
 * Storing it beside the store's own init/shutdown pair is what keeps that
 * lifetime a local fact rather than an ordering rule spread across files.
 *
 * s_contentPacksActive is the answer to "did the player install anything?", and
 * it is deliberately NOT mdkr_mod_texture_store_active(): that one goes false
 * the moment the player switches overrides off, which is exactly when the
 * toggle still has to work to switch them back on.
 */
static MdkrModRegistry s_contentPacks;
static int s_contentPacksActive;   /* enabled packs the scan actually kept */
static int s_contentPacksScanned;  /* init has run at least once this process */

/* One entry of Content.PackDisabled. The list is comma-separated, entries are
 * trimmed of surrounding blanks, and the comparison is ASCII case-insensitive
 * for the same reason the registry's tie-break is: a player who types a pack's
 * name back with different capitalisation means the same pack, and the answer
 * must not depend on their locale. */
int platform_content_pack_name_disabled(const char *list, const char *name) {
    size_t name_length;

    if (list == NULL || list[0] == '\0' || name == NULL || name[0] == '\0') {
        return 0;
    }
    name_length = strlen(name);
    while (*list != '\0') {
        const char *entry_end;
        size_t entry_length;

        while (*list == ',' || *list == ' ' || *list == '\t') list++;
        entry_end = strchr(list, ',');
        if (entry_end == NULL) entry_end = list + strlen(list);
        entry_length = (size_t)(entry_end - list);
        while (entry_length > 0 &&
               (list[entry_length - 1] == ' ' ||
                list[entry_length - 1] == '\t')) {
            entry_length--;
        }
        if (entry_length == name_length) {
            size_t index = 0;
            while (index < entry_length) {
                unsigned char a = (unsigned char)list[index];
                unsigned char b = (unsigned char)name[index];
                if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
                if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
                if (a != b) break;
                index++;
            }
            if (index == entry_length) return 1;
        }
        list = entry_end;
        if (*list == ',') list++;
    }
    return 0;
}

void platform_content_packs_init(void) {
    const MdkrVideoConfig *config = mdkr_video_config_current();
    const char *disabled_list =
        config != NULL ? config->values[MDKR_CONTENT_PACK_DISABLED].text : "";
    const int packs_enabled =
        config == NULL ||
        config->values[MDKR_CONTENT_PACKS_ENABLED].number != 0.0f;
    char mods_dir[MDKR_MOD_PATH_MAX];
    int registry_skipped;
    int player_disabled = 0;
    int authored_off = 0;
    int count;
    int index;

    s_contentPacksActive = 0;
    s_contentPacksScanned = 1;
    if (!mdkr_user_mods_directory(mods_dir, sizeof mods_dir)) {
        /* Only reachable when the packaged preference directory could not be
         * prepared, which mdkr_user_paths_init() has already reported. Content
         * packs are not worth a second complaint about the same failure. */
        mdkr_mod_registry_shutdown(&s_contentPacks);
        mdkr_mod_texture_store_init(NULL);
        mdkr_mod_texture_set_enabled(packs_enabled);
        return;
    }
    (void)mdkr_mod_registry_init(&s_contentPacks, mods_dir);

    /*
     * Content.PackDisabled is applied HERE, to the registry, rather than in the
     * texture store. Three reasons, in order of weight:
     *
     *  - It is a statement about which packs are installed, not about textures.
     *    Clearing an entry's `enabled` bit is the same lever a pack's own
     *    pack.ini pulls, so one rule governs both and mdkr_mod_registry_resolve()
     *    honours it for every asset kind a later milestone adds -- a store-side
     *    filter would cover textures and silently miss the rest.
     *  - The store's public surface has no name in it. It is keyed by content
     *    digest by design, and threading pack names through it would give the
     *    hot lookup path a string compare it exists to avoid.
     *  - A disabled pack has to appear in the summary below with a reason, and
     *    the summary is written from the registry.
     */
    count = mdkr_mod_registry_count(&s_contentPacks);
    for (index = 0; index < count; index++) {
        MdkrModEntry *entry = &s_contentPacks.entries[index];
        if (!entry->manifest.enabled) {
            authored_off++;
        } else if (platform_content_pack_name_disabled(
                       disabled_list, entry->manifest.name)) {
            entry->manifest.enabled = 0;
            player_disabled++;
        } else {
            s_contentPacksActive++;
        }
    }

    mdkr_mod_texture_store_init(&s_contentPacks);
    mdkr_mod_texture_set_enabled(packs_enabled);

    registry_skipped = mdkr_mod_registry_skipped(&s_contentPacks);
    if (count == 0 && registry_skipped == 0) {
        /* The overwhelmingly common install. Nothing was asked for and nothing
         * happened, so say nothing: a line here would be in every log forever. */
        return;
    }

    /* A pack that is quietly ignored looks exactly like a pack that loaded and
     * did nothing, so every pack the scan saw is accounted for on one of the
     * lines below, with a reason a player can act on. On stderr, which is where
     * mod_texture_store.c already reports an unusable pack texture: all of a
     * player's [MODS] evidence has to survive the same redirection.  */
    fprintf(stderr, "[MODS] %d pack(s) active, %d skipped%s\n",
            s_contentPacksActive,
            registry_skipped + player_disabled + authored_off,
            packs_enabled ? "" : "; overrides are switched off in settings");
    for (index = 0; index < count; index++) {
        const MdkrModEntry *entry = mdkr_mod_registry_entry(&s_contentPacks,
                                                            index);
        if (entry == NULL) continue;
        if (entry->manifest.enabled) {
            /* Only `name` is mandatory in a pack.ini, so the optional fields
             * are appended rather than formatted in: a pack that declared
             * neither must not print as "Name  by  (priority 100)". */
            char credit[MDKR_MOD_VERSION_MAX + MDKR_MOD_AUTHOR_MAX + 8];
            credit[0] = '\0';
            snprintf(credit, sizeof credit, "%s%s%s%s",
                     entry->manifest.version[0] != '\0' ? " " : "",
                     entry->manifest.version,
                     entry->manifest.author[0] != '\0' ? " by " : "",
                     entry->manifest.author);
            fprintf(stderr, "[MODS]   active: %s%s (priority %d)\n",
                    entry->manifest.name, credit, entry->manifest.priority);
        } else if (platform_content_pack_name_disabled(
                       disabled_list, entry->manifest.name)) {
            fprintf(stderr,
                    "[MODS]   skipped: %s - listed in Content.PackDisabled\n",
                    entry->manifest.name);
        } else {
            fprintf(stderr,
                    "[MODS]   skipped: %s - its pack.ini sets enabled = 0\n",
                    entry->manifest.name);
        }
    }
    for (index = 0; index < registry_skipped; index++) {
        /* mod_registry.h publishes an accessor for the reason but not for the
         * name it belongs to; the skip table itself is public, so read it. */
        fprintf(stderr, "[MODS]   skipped: %s - %s\n",
                s_contentPacks.skip_name[index],
                mdkr_mod_registry_skip_reason(&s_contentPacks, index));
    }
}

const MdkrModRegistry *platform_content_packs_registry(void) {
    /* The launcher draws Settings before anything calls init. Scanning here is
     * the same scan, at the same path, reading the same two config keys -- and
     * the engine's own call re-runs it, so the value the renderer binds to is
     * still produced after the launcher -> engine handoff has resolved the
     * config, never inherited from whatever the settings panel happened to see
     * first. */
    if (!s_contentPacksScanned) platform_content_packs_init();
    return &s_contentPacks;
}

void platform_content_packs_shutdown(void) {
    /* Store first: it borrows the registry, so unbinding it before the registry
     * is cleared is the only order in which no lookup can see a dead scan. */
    mdkr_mod_texture_store_shutdown();
    mdkr_mod_registry_shutdown(&s_contentPacks);
    s_contentPacksActive = 0;
    /* Deliberately not clearing s_contentPacksScanned: a settings panel drawn
     * during shutdown must read the empty registry it was just handed, not
     * rescan the disk on the way out. */
}

void platform_content_packs_toggle(void) {
    if (s_contentPacksActive == 0) return;
    mdkr_mod_texture_set_enabled(!mdkr_mod_texture_enabled());
    fprintf(stderr, "[MODS] content pack textures %s\n",
            mdkr_mod_texture_enabled() ? "on" : "off");
}

/* ---- Game controller open/close ----------------------------------------- */
/* The channel a live SDL instance id is bound to, or -1. Defined below with the
 * rest of the per-event routing; declared here because binding a device is what
 * has to know whether it is already bound. */
static int gc_port_for_instance(SDL_JoystickID which);

/*
 * Binding one device to one channel, IDEMPOTENTLY.
 *
 * This runs from two places that legitimately see the same device: the startup
 * enumeration in platform_input_init(), and SDL_CONTROLLERDEVICEADDED. Those
 * two are NOT alternatives. A pad that is already plugged in when the game
 * launches is enumerated by SDL_Init, which queues its device-added event; the
 * enumeration loop then binds the pad while that event is still sitting in the
 * queue, and the first input pump delivers it. SDL_GameControllerOpen() is
 * reference-counted and hands back the SAME controller for a device that is
 * already open, so without the guard below every boot-time pad claimed a second
 * channel: DKR saw a controller plugged into P2 that no one was holding, the
 * next real pad to join was pushed to P3, and rumble addressed at the phantom
 * channel buzzed the first player's pad.
 *
 * The instance id is the identity that matters -- device indices renumber on
 * every add and remove -- and the pointer comparison after the open is the
 * backstop for a host that cannot report one.
 */
static void gc_try_open(int deviceIndex) {
    SDL_JoystickID instance;
    if (!SDL_IsGameController(deviceIndex)) return;
    instance = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    if (instance >= 0 && gc_port_for_instance(instance) >= 0) return;
    for (int i = 0; i < DKR_MAXPADS; i++) {
        if (!s_gc[i]) {
            s_gc[i] = SDL_GameControllerOpen(deviceIndex);
            if (s_gc[i]) {
                for (int j = 0; j < DKR_MAXPADS; j++) {
                    if (j != i && s_gc[j] == s_gc[i]) {
                        /* Already bound to channel j; release this reference
                         * and leave the binding where it was. */
                        SDL_GameControllerClose(s_gc[i]);
                        s_gc[i] = NULL;
                        return;
                    }
                }
                s_rumbleSupported[i] = -1;
                s_rumbleRequested[i] = 0;
                memset(&s_controllerSource[i], 0,
                       sizeof(s_controllerSource[i]));
                printf("[SDL] gamepad P%d: %s\n", i + 1,
                       SDL_GameControllerName(s_gc[i]));
            }
            return;
        }
    }
    /* DKR has four ports and no notion of a fifth. Say so once per device
     * rather than leaving a plugged-in pad silently inert. */
    printf("[SDL] gamepad ignored: all %d controller channels are in use\n",
           DKR_MAXPADS);
}
static void gc_close_instance(SDL_JoystickID which) {
    for (int i = 0; i < DKR_MAXPADS; i++) {
        if (s_gc[i]) {
            SDL_Joystick *j = SDL_GameControllerGetJoystick(s_gc[i]);
            if (j && SDL_JoystickInstanceID(j) == which) {
                /* Stop an unbounded host-side effect before releasing its
                 * device. A disconnect while DKR's PWM is in its "on" phase
                 * must not leave the browser/native driver vibrating. */
                (void)platform_pad_rumble(i, 0);
                SDL_GameControllerClose(s_gc[i]);
                s_gc[i] = NULL;
                s_rumbleSupported[i] = -1;
                s_rumbleRequested[i] = 0;
                memset(&s_controllerSource[i], 0,
                       sizeof(s_controllerSource[i]));
                printf("[SDL] gamepad P%d disconnected\n", i + 1);
                return;
            }
        }
    }
}

/* ---- MDKR_TEST_PAD_HOTPLUG ----------------------------------------------- *
 *
 * The headless seam for controller hotplug, in the same spirit as
 * MDKR_TEST_SETTINGS_TOGGLE above: an automated run cannot unplug a pad, but
 * from SDL's device-added/removed event onward a virtual joystick and a
 * physical one are the same thing. SDL_JoystickAttachVirtual() creates a device
 * the game controller layer enumerates, maps, opens, reads and removes exactly
 * like hardware, so the whole channel-assignment path below is exercised
 * without any pad being plugged in.
 *
 * FORMAT: <op>=<args>@<tick>[,<op>=<args>@<tick>]... e.g.
 *   MDKR_TEST_PAD_HOTPLUG=attach=1@0,attach=2@90,hold=2:left@120,detach=2@180
 * Each entry fires once, at the first input pump at or after its tick.
 *
 *   attach=ID          add a virtual pad; ID (1..8) is the fixture's handle
 *   detach=ID          remove it, whatever channel it landed in
 *   hold=ID:POSE       pose in {neutral,left,right,up,down,a,start}
 *   rumble=PORT:0|1    call platform_pad_rumble() on a DKR channel directly
 *
 * TICK 0 MEANS BOOT, and it is not a synonym for "very early". Entries at tick
 * 0 fire from platform_input_init() BEFORE its startup enumeration, so the
 * device exists while SDL_Init's own device-added event is still queued. That
 * is the exact ordering a pad plugged in before launch produces, and it is not
 * reachable from the pump: by then the queue has already drained.
 *
 * Test-only and inert unless set. When the arm is set, every published channel
 * is traced once per authoritative tick from platform_input_commit_tick(), so a
 * check reads what DKR reads rather than what SDL was told. */
#if !defined(__EMSCRIPTEN__) && SDL_VERSION_ATLEAST(2, 0, 14)
#define MDKR_TEST_HOTPLUG_VIRTUAL 1
#else
#define MDKR_TEST_HOTPLUG_VIRTUAL 0
#endif

#define MDKR_TEST_HOTPLUG_MAX 32
#define MDKR_TEST_HOTPLUG_IDS 8
#define MDKR_TEST_HOTPLUG_POSE_MAX 16

enum {
    MDKR_HOTPLUG_ATTACH,
    MDKR_HOTPLUG_DETACH,
    MDKR_HOTPLUG_HOLD,
    MDKR_HOTPLUG_RUMBLE
};

typedef struct MdkrTestHotplug {
    uint64_t tick;
    int op;
    int id;      /* fixture handle for attach/detach/hold; DKR port for rumble */
    int value;   /* rumble on/off */
    char pose[MDKR_TEST_HOTPLUG_POSE_MAX];
    int fired;
} MdkrTestHotplug;

static int s_hotplugState = -1;   /* -1 unparsed, 0 disarmed, 1 armed */
static MdkrTestHotplug s_hotplug[MDKR_TEST_HOTPLUG_MAX];
static int s_hotplugCount;
/* Fixture handle -> SDL instance id, so a fixture never has to know SDL's
 * device indices (which renumber on every add and remove). Index 0 is unused
 * so the fixture's handles read as 1-based, like the P1..P4 channels do. */
static SDL_JoystickID s_hotplugInstance[MDKR_TEST_HOTPLUG_IDS + 1];

static const char *hotplug_op_name(int op) {
    switch (op) {
        case MDKR_HOTPLUG_ATTACH: return "attach";
        case MDKR_HOTPLUG_DETACH: return "detach";
        case MDKR_HOTPLUG_HOLD:   return "hold";
        default:                  return "rumble";
    }
}

static void hotplug_report(uint64_t tick, int op, int id, int port,
                           int instance, int result) {
    fprintf(stderr,
            "[PAD-HOTPLUG] tick=%llu op=%s id=%d port=%d instance=%d "
            "result=%d\n",
            (unsigned long long)tick, hotplug_op_name(op), id, port, instance,
            result);
    fflush(stderr);
}

#if MDKR_TEST_HOTPLUG_VIRTUAL
/* SDL device indices renumber whenever anything is added or removed, so the
 * only stable handle across a schedule is the instance id. */
static int hotplug_device_index(SDL_JoystickID instance) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_JoystickGetDeviceInstanceID(i) == instance) return i;
    }
    return -1;
}

/* Poses are written in the game's vocabulary, not SDL's: the assertions are
 * about what reaches a DKR channel, so a fixture should not have to restate
 * SDL's axis numbering to hold a direction. Every axis and button the pose
 * vocabulary covers is rewritten on each call, so a pose replaces the previous
 * one rather than accumulating with it. */
static int hotplug_pose_apply(SDL_Joystick *joystick, const char *pose) {
    int stick_x = 0;
    int stick_y = 0;
    Uint8 button_a = 0;
    Uint8 button_start = 0;
    if (strcmp(pose, "left") == 0)         stick_x = -32000;
    else if (strcmp(pose, "right") == 0)   stick_x = 32000;
    else if (strcmp(pose, "up") == 0)      stick_y = -32000;
    else if (strcmp(pose, "down") == 0)    stick_y = 32000;
    else if (strcmp(pose, "a") == 0)       button_a = 1;
    else if (strcmp(pose, "start") == 0)   button_start = 1;
    else if (strcmp(pose, "neutral") != 0) return -1;
    if (SDL_JoystickSetVirtualAxis(
            joystick, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)stick_x) != 0 ||
        SDL_JoystickSetVirtualAxis(
            joystick, SDL_CONTROLLER_AXIS_LEFTY, (Sint16)stick_y) != 0 ||
        SDL_JoystickSetVirtualButton(
            joystick, SDL_CONTROLLER_BUTTON_A, button_a) != 0 ||
        SDL_JoystickSetVirtualButton(
            joystick, SDL_CONTROLLER_BUTTON_START, button_start) != 0) {
        return -1;
    }
    return 0;
}
#endif

static void hotplug_fire(const MdkrTestHotplug *entry, uint64_t now) {
    int instance = -1;
    int port = -1;
    int result = -1;

    if (entry->op == MDKR_HOTPLUG_RUMBLE) {
        /* Deliberately not filtered by presence: the point of the arm is that
         * DKR's own Rumble Pak path may address a channel whose pad has just
         * gone away, and that this is a no-op rather than a fault. */
        port = entry->id;
        result = platform_pad_rumble(port, entry->value);
        hotplug_report(now, entry->op, -1, port, -1, result);
        return;
    }
#if MDKR_TEST_HOTPLUG_VIRTUAL
    if (entry->id >= 1 && entry->id <= MDKR_TEST_HOTPLUG_IDS) {
        switch (entry->op) {
            case MDKR_HOTPLUG_ATTACH: {
                const int device = SDL_JoystickAttachVirtual(
                    SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                    SDL_CONTROLLER_BUTTON_MAX, 1);
                if (device >= 0) {
                    instance = (int)SDL_JoystickGetDeviceInstanceID(device);
                    s_hotplugInstance[entry->id] = (SDL_JoystickID)instance;
                    result = 0;
                } else {
                    fprintf(stderr, "[PAD-HOTPLUG] attach failed: %s\n",
                            SDL_GetError());
                }
                break;
            }
            case MDKR_HOTPLUG_DETACH: {
                instance = (int)s_hotplugInstance[entry->id];
                if (instance >= 0) {
                    const int device =
                        hotplug_device_index((SDL_JoystickID)instance);
                    port = gc_port_for_instance((SDL_JoystickID)instance);
                    if (device >= 0) {
                        result = SDL_JoystickDetachVirtual(device);
                    }
                    s_hotplugInstance[entry->id] = -1;
                }
                break;
            }
            default: {
                SDL_Joystick *joystick = NULL;
                instance = (int)s_hotplugInstance[entry->id];
                if (instance >= 0) {
                    port = gc_port_for_instance((SDL_JoystickID)instance);
                    joystick =
                        SDL_JoystickFromInstanceID((SDL_JoystickID)instance);
                }
                if (joystick != NULL) {
                    result = hotplug_pose_apply(joystick, entry->pose);
                }
                break;
            }
        }
    }
#else
    /* No virtual joystick support in this SDL: say so in the trace rather than
     * letting a check read a missing row as a passing assertion. */
    result = -2;
#endif
    hotplug_report(now, entry->op, entry->id, port, instance, result);
}

static void pad_hotplug_lazy_init(void) {
    const char *value;
    const char *cursor;

    if (s_hotplugState >= 0) {
        return;
    }
    s_hotplugState = 0;
    for (int i = 0; i <= MDKR_TEST_HOTPLUG_IDS; i++) {
        s_hotplugInstance[i] = -1;
    }
    value = getenv("MDKR_TEST_PAD_HOTPLUG");
    if (value == NULL || value[0] == '\0') {
        return;
    }
    cursor = value;
    while (*cursor != '\0' && s_hotplugCount < MDKR_TEST_HOTPLUG_MAX) {
        const char *end = strchr(cursor, ',');
        const char *entry_end = end != NULL ? end : cursor + strlen(cursor);
        const char *eq = memchr(cursor, '=', (size_t)(entry_end - cursor));
        const char *at = NULL;
        const char *colon = NULL;
        MdkrTestHotplug entry;
        char *tail = NULL;
        size_t name_len;
        int parsed = 0;

        for (const char *scan = eq != NULL ? eq + 1 : cursor;
             scan < entry_end; scan++) {
            if (*scan == '@') at = scan;
            else if (*scan == ':' && colon == NULL) colon = scan;
        }
        memset(&entry, 0, sizeof(entry));
        name_len = eq != NULL ? (size_t)(eq - cursor) : 0u;
        if (eq != NULL && at != NULL && at > eq) {
            if (name_len == 6 && memcmp(cursor, "attach", 6) == 0) {
                entry.op = MDKR_HOTPLUG_ATTACH;
                parsed = 1;
            } else if (name_len == 6 && memcmp(cursor, "detach", 6) == 0) {
                entry.op = MDKR_HOTPLUG_DETACH;
                parsed = 1;
            } else if (name_len == 4 && memcmp(cursor, "hold", 4) == 0) {
                entry.op = MDKR_HOTPLUG_HOLD;
                parsed = colon != NULL && colon < at &&
                         (size_t)(at - colon - 1) < sizeof(entry.pose);
                if (parsed) {
                    memcpy(entry.pose, colon + 1, (size_t)(at - colon - 1));
                }
            } else if (name_len == 6 && memcmp(cursor, "rumble", 6) == 0) {
                entry.op = MDKR_HOTPLUG_RUMBLE;
                parsed = colon != NULL && colon < at;
                if (parsed) {
                    entry.value = atoi(colon + 1);
                }
            }
            if (parsed) {
                entry.id = atoi(eq + 1);
                entry.tick = strtoull(at + 1, &tail, 10);
                parsed = tail == entry_end;
            }
        }
        if (!parsed) {
            fprintf(stderr, "[PAD-HOTPLUG] entry \"%.*s\" unrecognized "
                            "(<op>=<args>@<tick>); ignored\n",
                    (int)(entry_end - cursor), cursor);
            cursor = end != NULL ? end + 1 : entry_end;
            continue;
        }
        s_hotplug[s_hotplugCount++] = entry;
        s_hotplugState = 1;
        cursor = end != NULL ? end + 1 : entry_end;
    }
}

/* Fires every entry the clock has reached, in schedule order so a fixture can
 * attach several pads at one tick and know which channel each one claims. */
static void pad_hotplug_run(uint64_t now) {
    for (int i = 0; i < s_hotplugCount; i++) {
        if (s_hotplug[i].fired || now < s_hotplug[i].tick) {
            continue;
        }
        s_hotplug[i].fired = 1;
        hotplug_fire(&s_hotplug[i], now);
    }
}

static void pad_hotplug_poll(void) {
    pad_hotplug_lazy_init();
    if (s_hotplugState != 1) {
        return;
    }
    pad_hotplug_run(present_sched_ticks());
}

/* The boot half of the arm: see the tick-0 note above. */
static void pad_hotplug_boot(void) {
    pad_hotplug_lazy_init();
    if (s_hotplugState != 1) {
        return;
    }
    pad_hotplug_run(0u);
}

/* One row per DKR channel per authoritative tick, read through the same
 * accessors osContGetReadData uses, so the trace cannot agree with SDL while
 * disagreeing with the game. `instance` and `attached` describe the SDL
 * controller the channel currently owns: a channel still holding a detached
 * device reports attached=0 rather than simply looking idle. */
static void pad_hotplug_trace_channels(void) {
    if (s_hotplugState != 1) {
        return;
    }
    for (int port = 0; port < DKR_MAXPADS; port++) {
        int stick_x = 0;
        int stick_y = 0;
        int instance = -1;
        int attached = 0;
        if (s_gc[port] != NULL) {
            SDL_Joystick *joystick = SDL_GameControllerGetJoystick(s_gc[port]);
            if (joystick != NULL) {
                instance = (int)SDL_JoystickInstanceID(joystick);
            }
            attached = SDL_GameControllerGetAttached(s_gc[port]) ? 1 : 0;
        }
        platform_pad_stick(port, &stick_x, &stick_y);
        fprintf(stderr,
                "[PAD-CHANNEL] tick=%d port=%d present=%d buttons=0x%04x "
                "sx=%d sy=%d instance=%d attached=%d\n",
                g_simTickCounter, port, platform_pad_present(port),
                platform_pad_buttons(port), stick_x, stick_y, instance,
                attached);
    }
    fflush(stderr);
}

void platform_input_init(void) {
    MdkrInputSample initial[MDKR_INPUT_PORTS];
    char packaged_mapping_path[4096];
    char executable_mapping_path[4096];
    char *executable_base = NULL;
    const char *paths[4];
    size_t path_count = 0u;
    /* Layered on top of SDL's built-in DB (last write wins), so a curated entry
     * can override a stale built-in mapping. Packaged Resources are immutable
     * and addressed absolutely; source-tree/CWD fallbacks remain for developer
     * builds. Non-fatal if every candidate is absent. */
    if (mdkr_user_paths_is_packaged() &&
        mdkr_user_resource_path(
            "gamecontrollerdb.txt", packaged_mapping_path,
            sizeof(packaged_mapping_path))) {
        paths[path_count++] = packaged_mapping_path;
    }
    /* Portable Windows/Linux packages place the curated database beside the
     * executable.  Resolve that location independently of the caller's CWD;
     * relative ROM arguments deliberately continue to use the caller's CWD. */
    executable_base = SDL_GetBasePath();
    if (executable_base != NULL) {
        int written = snprintf(
            executable_mapping_path, sizeof(executable_mapping_path),
            "%sgamecontrollerdb.txt", executable_base);
        if (written > 0 && (size_t)written < sizeof(executable_mapping_path)) {
            paths[path_count++] = executable_mapping_path;
        }
    }
    paths[path_count++] = "lib/sdl_gamecontrollerdb/gamecontrollerdb.txt";
    paths[path_count++] = "gamecontrollerdb.txt";
    memset(initial, 0, sizeof(initial));
    initial[0].present = true;
    mdkr_input_tick_queue_init(&s_inputQueue, initial);
    s_inputQueueReady = 1;
    memset(s_keyboardDown, 0, sizeof(s_keyboardDown));
    if (s_sdlReady) {
        const Uint8 *keyboard = SDL_GetKeyboardState(NULL);
        if (keyboard != NULL) {
            memcpy(s_keyboardDown, keyboard, sizeof(s_keyboardDown));
        }
    }
    for (size_t i = 0; i < path_count; i++) {
        int n = SDL_GameControllerAddMappingsFromFile(paths[i]);
        if (n > 0) {
            printf("[SDL] loaded %d controller mappings from %s\n", n, paths[i]);
            break;
        }
    }
    if (executable_base != NULL) {
        SDL_free(executable_base);
    }
    /* Before the enumeration below, so a fixture's boot pads are indistinguish-
     * able from hardware SDL_Init already saw. Inert unless the arm is set. */
    pad_hotplug_boot();
    for (int i = 0; i < SDL_NumJoysticks(); i++) gc_try_open(i);
    for (int i = 0; i < DKR_MAXPADS; i++) {
        s_pads[i].buttons = 0;
        s_pads[i].stick_x = 0;
        s_pads[i].stick_y = 0;
        s_pads[i].present = i == 0 || s_gc[i] != NULL;
    }
    input_capture_live(1u);
}

static int gc_port_for_instance(SDL_JoystickID which) {
    int port;
    for (port = 0; port < DKR_MAXPADS; port++) {
        SDL_Joystick *joystick;
        if (s_gc[port] == NULL) {
            continue;
        }
        joystick = SDL_GameControllerGetJoystick(s_gc[port]);
        if (joystick != NULL && SDL_JoystickInstanceID(joystick) == which) {
            return port;
        }
    }
    return -1;
}

/* Translate one maintained SDL controller source into an N64 pad sample. */
static void gc_read(
    const struct controller_source_state *source, struct pad_state *p) {
    MdkrControllerDigitalState normalized = {{0}};
    struct {
        SDL_GameControllerButton button;
        MdkrControllerSource normalized_source;
    } map[] = {
        { SDL_CONTROLLER_BUTTON_A, MDKR_CONTROLLER_SOURCE_A },
        { SDL_CONTROLLER_BUTTON_B, MDKR_CONTROLLER_SOURCE_B },
        { SDL_CONTROLLER_BUTTON_X, MDKR_CONTROLLER_SOURCE_X },
        { SDL_CONTROLLER_BUTTON_Y, MDKR_CONTROLLER_SOURCE_Y },
        { SDL_CONTROLLER_BUTTON_START, MDKR_CONTROLLER_SOURCE_START },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK,
          MDKR_CONTROLLER_SOURCE_LEFT_STICK },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK,
          MDKR_CONTROLLER_SOURCE_RIGHT_STICK },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
          MDKR_CONTROLLER_SOURCE_LEFT_SHOULDER },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
          MDKR_CONTROLLER_SOURCE_RIGHT_SHOULDER },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, MDKR_CONTROLLER_SOURCE_DPAD_UP },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, MDKR_CONTROLLER_SOURCE_DPAD_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, MDKR_CONTROLLER_SOURCE_DPAD_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, MDKR_CONTROLLER_SOURCE_DPAD_RIGHT },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        normalized.active[map[i].normalized_source] =
            source->buttons[map[i].button] != 0;
    }
    normalized.active[MDKR_CONTROLLER_SOURCE_LEFT_TRIGGER] =
        source->axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] > 8000;
    normalized.active[MDKR_CONTROLLER_SOURCE_RIGHT_TRIGGER] =
        source->axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] > 8000;

    /* Right-stick directions are normalized digital sources too, so they can
     * retain the default C-button camera mapping or be reassigned like a face
     * button. The left stick remains the fixed analog steering source. */
    int rx = source->axes[SDL_CONTROLLER_AXIS_RIGHTX];
    int ry = source->axes[SDL_CONTROLLER_AXIS_RIGHTY];
    normalized.active[MDKR_CONTROLLER_SOURCE_RIGHT_STICK_LEFT] = rx < -12000;
    normalized.active[MDKR_CONTROLLER_SOURCE_RIGHT_STICK_RIGHT] = rx > 12000;
    normalized.active[MDKR_CONTROLLER_SOURCE_RIGHT_STICK_UP] = ry < -12000;
    normalized.active[MDKR_CONTROLLER_SOURCE_RIGHT_STICK_DOWN] = ry > 12000;
    p->buttons |= mdkr_controller_mapped_buttons(
        mdkr_video_config_current(), &normalized);

    /* Left stick -> analog (N64 range ±80; SDL axis is ±32767, +Y is down). */
    int lx = source->axes[SDL_CONTROLLER_AXIS_LEFTX];
    int ly = source->axes[SDL_CONTROLLER_AXIS_LEFTY];
    int sx = (lx * STICK_FULL) / 32767;
    int sy = (-ly * STICK_FULL) / 32767;
    if (sx < -STICK_FULL) sx = -STICK_FULL;
    if (sx > STICK_FULL) sx = STICK_FULL;
    if (sy < -STICK_FULL) sy = -STICK_FULL;
    if (sy > STICK_FULL) sy = STICK_FULL;
    if (sx < -8 || sx > 8) p->stick_x = sx;   /* small deadzone */
    if (sy < -8 || sy > 8) p->stick_y = sy;
}

/* Read the keyboard (P1 only) into N64 button bits + stick. */
static void kbd_read(struct pad_state *p) {
    const Uint8 *k = s_keyboardDown;
    /*
     * DEFAULT KEYBOARD LAYOUT (v0.1). Chosen for a kart racer, not inherited from
     * the generic N64-emulator default, and documented on the web page's Controls
     * card so the two cannot drift apart.
     *
     *   Arrows / WASD  analog stick   steering, and PLANE PITCH — both axes matter
     *   X              A              accelerate; the key you hold all race
     *   Z              B              brake / reverse; sits next to X
     *   Space          R              HOP / power-slide  <-- see note
     *   Shift          Z-trigger      fire item; a trigger belongs on a modifier
     *   Q              L              rarely used in this game
     *   I J K L        C-buttons      rear view / camera
     *   Enter          Start
     *
     * Two deliberate changes from the previous mapping, both fixing real problems:
     *
     *  1. R (hop) was UNBOUND. In DKR the hop button is how you power-slide, which
     *     is the core skill for going fast, so it gets Space — the largest, most
     *     comfortable key to tap repeatedly and hold.
     *  2. Z-trigger moved off `C`. Previously `Z` was the B button while `C` was
     *     the Z-trigger, so "Z" meant two different things depending on whether you
     *     were talking about the key or the N64 button. That is a genuine
     *     documentation trap; the trigger now lives on Shift.
     *
     * A/D are no longer L/R because WASD now mirrors the stick, which is worth more
     * to a left-hand-steering player than two rarely-used shoulder buttons.
     *
     * Keyboard steering is DIGITAL (full deflection) while the real controller is
     * analog, so a gamepad genuinely plays better here — especially for drifting.
     * The web page says so rather than pretending otherwise.
     */
    if (k[SDL_SCANCODE_X])      p->buttons |= N64_A;       /* accelerate */
    if (k[SDL_SCANCODE_Z])      p->buttons |= N64_B;       /* brake / reverse */
    if (k[SDL_SCANCODE_SPACE])  p->buttons |= N64_R;       /* hop / power-slide */
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT])
                                p->buttons |= N64_Z;       /* fire item */
    if (k[SDL_SCANCODE_Q])      p->buttons |= N64_L;
    if (k[SDL_SCANCODE_RETURN]) p->buttons |= N64_START;
    if (k[SDL_SCANCODE_I])      p->buttons |= N64_CU;      /* IJKL = C-buttons */
    if (k[SDL_SCANCODE_K])      p->buttons |= N64_CD;
    if (k[SDL_SCANCODE_J])      p->buttons |= N64_CL;
    if (k[SDL_SCANCODE_L])      p->buttons |= N64_CR;
    /* Arrows AND WASD = analog stick (full deflection; menus read it digitally). */
    int sx = 0, sy = 0;
    if (k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A]) sx -= STICK_FULL;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) sx += STICK_FULL;
    if (k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W]) sy += STICK_FULL;  /* N64 +up */
    if (k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S]) sy -= STICK_FULL;
    if (sx) p->stick_x = sx;
    if (sy) p->stick_y = sy;
    /* Mirror both stick sources onto the D-pad bits for gameplay/menus that read
     * jpad directly. WASD must be included or it would steer but fail to navigate
     * any menu that reads the D-pad. */
    if (k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A]) p->buttons |= N64_DL;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) p->buttons |= N64_DR;
    if (k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W]) p->buttons |= N64_DU;
    if (k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S]) p->buttons |= N64_DD;
}

#ifdef __EMSCRIPTEN__
static void browser_touch_clamp(struct pad_state *p) {
    p->stick_x = p->stick_x < -STICK_FULL ? -STICK_FULL :
                 p->stick_x >  STICK_FULL ?  STICK_FULL : p->stick_x;
    p->stick_y = p->stick_y < -STICK_FULL ? -STICK_FULL :
                 p->stick_y >  STICK_FULL ?  STICK_FULL : p->stick_y;
    p->buttons &=
        (N64_A | N64_B | N64_Z | N64_START |
         N64_DU | N64_DD | N64_DL | N64_DR |
         N64_L | N64_R | N64_CU | N64_CD | N64_CL | N64_CR);
}

static void browser_touch_read_current(struct pad_state *p) {
    unsigned int buttons = 0;
    int stickX = 0;
    int stickY = 0;
    int enabled = 0;

    browser_touch_pad_read(&buttons, &stickX, &stickY, &enabled);
    p->buttons = enabled ? buttons : 0;
    p->stick_x = enabled ? stickX : 0;
    p->stick_y = enabled ? stickY : 0;
    p->present = enabled;
    browser_touch_clamp(p);
}

static int browser_touch_pop(struct pad_state *p) {
    unsigned int buttons = 0;
    int stickX = 0;
    int stickY = 0;
    int enabled = 0;
    if (!browser_touch_pad_pop(&buttons, &stickX, &stickY, &enabled)) {
        return 0;
    }
    p->buttons = enabled ? buttons : 0;
    p->stick_x = enabled ? stickX : 0;
    p->stick_y = enabled ? stickY : 0;
    p->present = enabled;
    browser_touch_clamp(p);
    return 1;
}

static void browser_touch_merge(
    const struct pad_state *touch, struct pad_state *p) {
    unsigned int buttons = touch->buttons;
    int stickX = touch->stick_x;
    int stickY = touch->stick_y;

    /* The analog stick is the authoritative steering source. Mirror a decisive
     * deflection onto the D-pad as the keyboard path does because a few menus
     * and gameplay screens read jpad directly instead of the analog axes. */
    if (stickX < -24) buttons |= N64_DL;
    if (stickX >  24) buttons |= N64_DR;
    if (stickY < -24) buttons |= N64_DD;
    if (stickY >  24) buttons |= N64_DU;

    p->buttons |= buttons;

    /* A resting touch stick must not erase a physical pad. If both are active,
     * retain the larger deflection independently on each axis. */
    if (abs(stickX) > abs(p->stick_x)) p->stick_x = stickX;
    if (abs(stickY) > abs(p->stick_y)) p->stick_y = stickY;
}
#endif

static void input_clear_game_sources(void) {
    memset(s_keyboardDown, 0, sizeof(s_keyboardDown));
    memset(s_controllerSource, 0, sizeof(s_controllerSource));
}

#ifdef MDKR_APP
/* The single handoff between the shell overlay's input capture and the game.
 *
 * The overlay does not only open and close from inside process_event. Its
 * on-screen Resume button closes it from the RENDER callback -- whether it was
 * activated by mouse, by Enter, or by ImGui gamepad nav (A/Cross) -- and so
 * does the scripted open/close schedule. Deriving the latch from a transition
 * observed *during event dispatch* therefore saw no edge at all on those
 * paths: overlayActive and overlayNow were both already 0 by the next event,
 * s_gameInputSuppressed stayed 1, and the game never saw another button for
 * the rest of the session.
 *
 * Reconciling against the overlay's live answer makes the handoff independent
 * of which code path opened or closed the menu, including an engine session
 * that ends with it open and unregisters the hooks entirely. Retiring the
 * latched host sources on each edge is what makes it robust to a button that
 * is still held across the transition: the game resumes neutral and latches
 * the next real host edge instead of inheriting a press it never saw begin. */
static void overlay_capture_sync(uint64_t target_tick) {
    const int wants = platformOverlayWantsInput() ? 1 : 0;
    if (wants == s_gameInputSuppressed) {
        return;
    }
    s_gameInputSuppressed = wants;
    input_clear_game_sources();
    input_capture_live(target_tick);
    fprintf(stderr, "[overlay-input] game input %s at tick %d\n",
            wants ? "captured by the overlay" : "released to the game",
            g_simTickCounter);
}
#endif

static int test_script_only_input(void) {
    if (s_testScriptOnlyInput < 0) {
        const char *value = getenv("MDKR_TEST_SCRIPT_ONLY_INPUT");
        s_testScriptOnlyInput = value != NULL && value[0] == '1';
    }
    return s_testScriptOnlyInput;
}

static void input_capture_live(uint64_t target_tick) {
    int port;
    /* Exact scripted-input gates must not inherit a transient host key,
     * controller connection, or focus event. The explicit test seam leaves
     * SDL/window event processing intact but keeps the fixed-tick queue on its
     * deterministic neutral initial sample; script_apply() still merges the
     * authored P1-P4 route at each authoritative ticket. */
    if (!s_inputQueueReady || test_script_only_input()) {
        return;
    }
    for (port = 0; port < DKR_MAXPADS; port++) {
        struct pad_state live = { 0, 0, 0, 0 };
        MdkrInputSample sample;
        live.present = port == 0 || s_gc[port] != NULL;
        if (!s_gameInputSuppressed) {
            if (port == 0) {
                kbd_read(&live);
            }
            if (s_gc[port] != NULL) {
                gc_read(&s_controllerSource[port], &live);
            }
#ifdef __EMSCRIPTEN__
            if (port == 0) {
                browser_touch_merge(&s_browserTouchSource, &live);
            }
#endif
        }
        sample.buttons = (uint16_t)live.buttons;
        sample.stick_x = (int8_t)live.stick_x;
        sample.stick_y = (int8_t)live.stick_y;
        sample.present = live.present != 0;
        mdkr_input_tick_queue_capture(
            &s_inputQueue, (unsigned)port, target_tick, sample);
    }
    if (input_latency_census_enabled()) {
        input_latency_census_note_capture(pace_host_ns());
    }
}

/*
 * JUST-IN-TIME INPUT SAMPLING.
 *
 * Off unless MDKR_INPUT_JIT is set to something other than 0, latched once so
 * no tick can observe the setting changing under it. Deliberately NOT keyed on
 * the simulation cadence: sampling the pad later in wall-clock time changes
 * nothing about how the sample is processed, so it is correct under Original
 * and Enhanced alike and has no business asking which one is running.
 *
 * What it buys, and why the term exists at all: host capture runs from the
 * input pump, and the pump runs on presentation opportunities. The last
 * opportunity before an authored tick becomes due is one present interval
 * before the commit that publishes the ticket -- and when the presentation rate
 * equals the tick rate, that is the whole authored quantum. The sample is taken
 * and then simply waits. This runs the same capture again at the last moment
 * it can still reach the ticket about to be issued.
 */
static int s_inputJitSampling = -1;

static int input_jit_sampling_enabled(void) {
    if (s_inputJitSampling < 0) {
        const char *value = getenv("MDKR_INPUT_JIT");
        s_inputJitSampling =
            (value != NULL && value[0] != '\0' && value[0] != '0');
    }
    return s_inputJitSampling;
}

/* The pacer's live display-rate re-derivation, called from the SDL event pump
 * above and defined with the rest of the present-pacing state below. */
static void present_pace_note_display_changed(void);

/* PAC-007: native surface suspension state. Ordinary finite hidden tests keep
 * rendering offscreen; an interactive window (or the explicit visible-headless
 * lifecycle gate) elides GPU walks while SDL says it is hidden/minimized. */
static void platform_surface_visibility_update(void) {
#ifndef __EMSCRIPTEN__
    bool forced = false;
    bool automation_hidden;
    bool elide;
    if (s_testMinimizeStart == -2) {
        const char *value = getenv("MDKR_TEST_MINIMIZE_TICKS");
        int start = -1;
        int end = -1;
        char tail = '\0';
        if (value != NULL &&
            sscanf(value, "%d:%d%c", &start, &end, &tail) == 2 &&
            start >= 1 && end > start && end <= 1000000) {
            s_testMinimizeStart = start;
            s_testMinimizeEnd = end;
        } else {
            s_testMinimizeStart = -1;
            s_testMinimizeEnd = -1;
        }
    }
    if (s_testMinimizeStart >= 0) {
        forced = g_simTickCounter >= s_testMinimizeStart &&
                 g_simTickCounter < s_testMinimizeEnd;
        if ((int)forced != s_testForcedMinimized) {
            s_testForcedMinimized = forced ? 1 : 0;
            if (s_window != NULL) {
                if (forced) {
                    SDL_MinimizeWindow(s_window);
                } else {
                    SDL_RestoreWindow(s_window);
                }
            }
        }
    }
    automation_hidden = g_headlessFrames >= 0 &&
        getenv("MDKR_TEST_VISIBLE_HEADLESS") == NULL;
    elide = s_testMinimizeStart >= 0 ? forced :
        (!automation_hidden && getenv("MDKR64_HIDDEN") == NULL &&
         !platform_sdl_surface_presentable());
    if ((int)elide != s_surfaceRenderElided) {
        s_surfaceRenderElided = elide ? 1 : 0;
        present_sched_set_surface_elided(elide);
        if (!elide) {
            s_surfaceResumeRebasePending = 1;
        }
        fprintf(stderr,
                "[SURFACE-PACING] presentable=%d renderElided=%d "
                "resumeRebase=%d tick=%d frame=%d\n",
                elide ? 0 : 1, elide ? 1 : 0,
                elide ? 0 : 1, g_simTickCounter, g_frameCounter);
    }
#endif
}

/* ---- MDKR_TEST_SETTINGS_TOGGLE ------------------------------------------ *
 *
 * The headless seam for the live-settings apply path, in the same spirit as
 * MDKR_TEST_DISPLAY_RATE_SWITCH above: an automated run cannot open the overlay
 * and click a combo box, but everything downstream of the EDIT can be driven,
 * because from mdkr_video_config_runtime_set() onward the two are the same
 * call.
 *
 * WHY IT FIRES FROM platform_input_pump AND NOT FROM THE APPLY BOUNDARY. The
 * point of the arm is that a settings edit arrives at an arbitrary, hostile
 * moment — the overlay is drawn inside the engine's own frame, and the pump
 * runs on every present opportunity INCLUDING the presentation subloop's. So
 * firing here reproduces the real hazard: a smoothing change landing while a
 * retained display list is mid-replay. Firing at the boundary instead would
 * test only the half that was never in doubt.
 *
 * FORMAT: Key=value@tick[,Key=value@tick]... e.g.
 *   MDKR_TEST_SETTINGS_TOGGLE=Video.MotionSmoothing=off@400,Video.MotionSmoothing=interpolate@430
 * Each entry fires once, at the first present opportunity at or after its tick.
 * Entries are independent, so a soak is just a long list.
 *
 * A key pinned by the environment or the command line resolves as LOCKED here
 * exactly as it would for a player, and the [SETTINGS-TOGGLE] row says so
 * rather than failing quietly — a gate that drove MDKR_PRESENT_RATE and then
 * asserted a toggle took effect would otherwise pass by testing nothing.
 *
 * ONE PSEUDO-KEY. `Presentation.Pace=original|smooth` is the settings panel's
 * quick choice rather than a schema key, and it is here for the same reason
 * every real key is: an automated run cannot press a radio button, but from
 * mdkr_video_config_runtime_set_presentation_pace() onward the press and this
 * are the same call. Routing it through that function rather than expanding it
 * into two entries is what makes the arm test the quick choice -- including
 * that its two writes are ONE transaction and reach ONE apply boundary -- and
 * not an imitation of it that could keep passing after the real control broke.
 *
 * Test-only and inert unless set.
 */
#define MDKR_TEST_TOGGLE_MAX 64
#define MDKR_TEST_TOGGLE_PACE_NAME "Presentation.Pace"

typedef struct MdkrTestToggle {
    /* MDKR_VIDEO_KEY_COUNT means the pseudo-key above; `value` then holds the
     * pace name rather than a key value. */
    MdkrVideoKey key;
    char value[MDKR_VIDEO_NAME_MAX];
    uint64_t tick;
    int fired;
} MdkrTestToggle;

static int s_toggleState = -1;   /* -1 unparsed, 0 disarmed, 1 armed */
static MdkrTestToggle s_toggles[MDKR_TEST_TOGGLE_MAX];
static int s_toggleCount;

static void settings_toggle_lazy_init(void) {
    const char *value;
    const char *cursor;

    if (s_toggleState >= 0) {
        return;
    }
    s_toggleState = 0;
    value = getenv("MDKR_TEST_SETTINGS_TOGGLE");
    if (value == NULL || value[0] == '\0') {
        return;
    }
    cursor = value;
    while (*cursor != '\0' && s_toggleCount < MDKR_TEST_TOGGLE_MAX) {
        const char *end = strchr(cursor, ',');
        const char *entry_end = end != NULL ? end : cursor + strlen(cursor);
        const char *eq = memchr(cursor, '=', (size_t)(entry_end - cursor));
        const char *at = NULL;
        char name[MDKR_VIDEO_NAME_MAX];
        MdkrVideoKey key;
        size_t name_len;
        size_t value_len;
        char *tail = NULL;
        unsigned long long tick;

        /* The LAST '@' separates the value from the tick, so a value that ever
         * contains one still parses. */
        for (const char *scan = eq != NULL ? eq + 1 : cursor;
             scan < entry_end; scan++) {
            if (*scan == '@') at = scan;
        }
        if (eq == NULL || at == NULL || at < eq) {
            fprintf(stderr, "[SETTINGS-TOGGLE] entry \"%.*s\" unrecognized "
                            "(<Key>=<value>@<tick>); ignored\n",
                    (int)(entry_end - cursor), cursor);
            cursor = end != NULL ? end + 1 : entry_end;
            continue;
        }
        name_len = (size_t)(eq - cursor);
        value_len = (size_t)(at - eq - 1);
        if (name_len == 0 || name_len >= sizeof(name) ||
            value_len >= sizeof(s_toggles[0].value)) {
            fprintf(stderr, "[SETTINGS-TOGGLE] entry \"%.*s\" is out of range; "
                            "ignored\n", (int)(entry_end - cursor), cursor);
            cursor = end != NULL ? end + 1 : entry_end;
            continue;
        }
        memcpy(name, cursor, name_len);
        name[name_len] = '\0';
        tick = strtoull(at + 1, &tail, 10);
        if (strcmp(name, MDKR_TEST_TOGGLE_PACE_NAME) == 0) {
            char pace[MDKR_VIDEO_NAME_MAX];
            memcpy(pace, eq + 1, value_len);
            pace[value_len] = '\0';
            /* The sentinel the poll below reads as "this is the quick choice";
             * `value` then carries the pace name rather than a key value. */
            key = MDKR_VIDEO_KEY_COUNT;
            if (mdkr_video_presentation_pace_from_name(pace) < 0 ||
                tail != entry_end) {
                fprintf(stderr, "[SETTINGS-TOGGLE] entry \"%.*s\" names no "
                                "presentation pace or no tick; ignored\n",
                        (int)(entry_end - cursor), cursor);
                cursor = end != NULL ? end + 1 : entry_end;
                continue;
            }
        } else {
            key = mdkr_video_key_from_name(name);
            if (key == MDKR_VIDEO_KEY_COUNT || tail != entry_end) {
                fprintf(stderr, "[SETTINGS-TOGGLE] entry \"%.*s\" names no "
                                "setting or no tick; ignored\n",
                        (int)(entry_end - cursor), cursor);
                cursor = end != NULL ? end + 1 : entry_end;
                continue;
            }
        }
        s_toggles[s_toggleCount].key = key;
        memcpy(s_toggles[s_toggleCount].value, eq + 1, value_len);
        s_toggles[s_toggleCount].value[value_len] = '\0';
        s_toggles[s_toggleCount].tick = (uint64_t)tick;
        s_toggles[s_toggleCount].fired = 0;
        s_toggleCount++;
        s_toggleState = 1;
        cursor = end != NULL ? end + 1 : entry_end;
    }
}

static void settings_toggle_poll(void) {
    const uint64_t now = present_sched_ticks();

    settings_toggle_lazy_init();
    if (s_toggleState != 1) {
        return;
    }
    for (int i = 0; i < s_toggleCount; i++) {
        const MdkrVideoSchema *schema;
        MdkrVideoRuntimeResult result;
        if (s_toggles[i].fired || now < s_toggles[i].tick) {
            continue;
        }
        s_toggles[i].fired = 1;
        if (s_toggles[i].key == MDKR_VIDEO_KEY_COUNT) {
            schema = NULL;
            result = mdkr_video_config_runtime_set_presentation_pace(
                (MdkrPresentationPace)
                    mdkr_video_presentation_pace_from_name(
                        s_toggles[i].value));
        } else {
            schema = mdkr_video_schema(s_toggles[i].key);
            result = mdkr_video_config_runtime_set(s_toggles[i].key,
                                                   s_toggles[i].value);
        }
        fprintf(stderr,
                "[SETTINGS-TOGGLE] tick=%llu key=%s value=%s result=%d "
                "applied=%d\n",
                (unsigned long long)now,
                schema != NULL ? schema->name
                               : (s_toggles[i].key == MDKR_VIDEO_KEY_COUNT
                                      ? MDKR_TEST_TOGGLE_PACE_NAME : "?"),
                s_toggles[i].value, (int)result,
                mdkr_video_runtime_result_applied(result) ? 1 : 0);
        fflush(stderr);
    }
}

/* Drain and dispatch the SDL event queue into the latched host sources, then
 * capture each change into the fixed-tick input queue.
 *
 * Factored out of platform_input_pump so the just-in-time sampler can run the
 * SAME dispatch immediately before a ticket is committed. That sharing is the
 * point: the overlay's swallow contract, the focus-loss retirement and the
 * device add/remove handling all live in this loop, and a second drain that
 * did not honour them would steal events from the shell rather than shorten
 * anyone's input latency. */
static void input_dispatch_events(uint64_t target_tick) {
    if (s_sdlReady) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int input_changed = 0;
#ifdef MDKR_APP
            /*
             * Input-swallowing contract with the in-game overlay.
             *
             * Let the overlay see every event, then drop input events if it was
             * capturing input EITHER before or after dispatch. Both edges matter:
             * the toggle key flips the state inside process_event, and that same
             * keypress must be swallowed on the open AND the close transition, or
             * it leaks through to the game on one of them.
             *
             * No-op when no hooks are registered — the automation path never
             * registers any, so its event handling is unchanged.
             */
            const int overlayActive = platformOverlayWantsInput();
            const int overlayConsumed = platformOverlayProcessEvent(&e);
            /* Publishes one neutral game sample when this event opened capture,
             * and releases it when this event closed it. The shared routine is
             * also what the pre-dispatch reconcile above runs, so no close path
             * has its own idea of what "give the pad back" means. */
            overlay_capture_sync(target_tick);
            if (overlayConsumed) {
                continue;
            }
            if (overlayActive || platformOverlayWantsInput()) {
                switch (e.type) {
                    case SDL_KEYDOWN:
                    case SDL_KEYUP:
                    case SDL_MOUSEMOTION:
                    case SDL_MOUSEBUTTONDOWN:
                    case SDL_MOUSEBUTTONUP:
                    case SDL_MOUSEWHEEL:
                    case SDL_TEXTINPUT:
                    case SDL_CONTROLLERBUTTONDOWN:
                    case SDL_CONTROLLERBUTTONUP:
                    case SDL_CONTROLLERAXISMOTION:
                    case SDL_JOYBUTTONDOWN:
                    case SDL_JOYBUTTONUP:
                    case SDL_JOYAXISMOTION:
                    case SDL_JOYHATMOTION:
                        continue;   /* the overlay owns this event */
                    default:
                        break;      /* QUIT / device add/remove still get through */
                }
            }
#endif
            switch (e.type) {
                case SDL_QUIT:
                    s_quitRequested = 1;
                    break;
                case SDL_KEYDOWN:
                    if (e.key.keysym.scancode >= 0 &&
                        e.key.keysym.scancode < SDL_NUM_SCANCODES) {
                        s_keyboardDown[e.key.keysym.scancode] = 1;
                        input_changed = 1;
                    }
                    /*
                     * Tab: content-pack overrides off and back on, live, so a
                     * pack can be compared against the original without a
                     * restart. Deliberately handled HERE and not next to the
                     * overlay's own F1/F10 bindings: everything above this
                     * switch already dropped the event when the overlay was
                     * capturing input, which is the one notion of "the menu has
                     * the keyboard" this port has. Tab therefore still walks the
                     * settings UI's fields while the menu is up, without a
                     * second focus test that could disagree with the first.
                     * `repeat` is filtered so holding the key does not strobe.
                     */
                    if (!e.key.repeat && e.key.keysym.sym == SDLK_TAB) {
                        platform_content_packs_toggle();
                    }
#ifndef MDKR_APP
                    if (e.key.keysym.sym == SDLK_ESCAPE && g_headlessFrames < 0)
                        s_quitRequested = 1;   /* legacy non-shell host quit */
#endif
                    break;
                case SDL_KEYUP:
                    if (e.key.keysym.scancode >= 0 &&
                        e.key.keysym.scancode < SDL_NUM_SCANCODES) {
                        s_keyboardDown[e.key.keysym.scancode] = 0;
                        input_changed = 1;
                    }
                    break;
                case SDL_WINDOWEVENT:
                    /* The dimensions are sampled after present in
                     * platform_frame_sync; consuming the event here keeps SDL's
                     * state current without reconfiguring a surface mid-frame. */
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        /* Releases delivered while another app owns focus are
                         * not guaranteed to reach this window. Retire every
                         * latched host source now so returning cannot resume a
                         * stale throttle, button, or steering axis. */
                        input_clear_game_sources();
                        input_changed = 1;
                    }
#if SDL_VERSION_ATLEAST(2, 0, 18)
                    /*
                     * The window moved to a display with, potentially, a
                     * different refresh. Re-derive the rate and everything
                     * standing on it; the latched policy is untouched. This is
                     * the ONE event SDL2 offers here -- a mode change on the
                     * SAME display raises nothing. A player who changes their
                     * refresh in place can now re-pick their frame limit to
                     * force a re-derivation (the pacing keys are SCOPE_LIVE),
                     * which is a far better answer than the relaunch this
                     * comment used to have to offer them.
                     */
                    else if (e.window.event ==
                             SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                        present_pace_note_display_changed();
                    }
#endif
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    gc_try_open(e.cdevice.which);
                    input_changed = 1;
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    gc_close_instance(e.cdevice.which);
                    input_changed = 1;
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    const int port = gc_port_for_instance(e.cbutton.which);
                    if (port >= 0 &&
                        e.cbutton.button < SDL_CONTROLLER_BUTTON_MAX) {
                        s_controllerSource[port].buttons[e.cbutton.button] =
                            e.type == SDL_CONTROLLERBUTTONDOWN;
                        input_changed = 1;
                    }
                    break;
                }
                case SDL_CONTROLLERAXISMOTION: {
                    const int port = gc_port_for_instance(e.caxis.which);
                    if (port >= 0 && e.caxis.axis < SDL_CONTROLLER_AXIS_MAX) {
                        s_controllerSource[port].axes[e.caxis.axis] =
                            e.caxis.value;
                        input_changed = 1;
                    }
                    break;
                }
                default:
                    break;
            }
            if (input_changed) {
                if (input_latency_census_enabled()) {
                    switch (e.type) {
                        case SDL_KEYDOWN:
                        case SDL_KEYUP:
                        case SDL_CONTROLLERBUTTONDOWN:
                        case SDL_CONTROLLERBUTTONUP:
                        case SDL_CONTROLLERAXISMOTION: {
                            /* SDL2 stamps events from its own millisecond
                             * clock, so this term is reported at millisecond
                             * resolution and no better. */
                            const Uint32 now_ms = SDL_GetTicks();
                            const Uint32 stamp = e.common.timestamp;
                            input_latency_census_note_event(
                                now_ms >= stamp ? (unsigned)(now_ms - stamp)
                                                : 0u);
                            break;
                        }
                        default:
                            break;
                    }
                }
                input_capture_live(target_tick);
            }
        }
    }
}

/* Capture host input transitions. This may run many times between simulation
 * ticks; it never directly replaces the DKR-visible s_pads snapshot. */
void platform_input_pump(void) {
    const uint64_t target_tick = present_sched_input_target_tick();
    /* Test-only; one compare in every real run (see settings_toggle_poll). */
    settings_toggle_poll();
    pad_hotplug_poll();
#ifdef MDKR_APP
    /* Applies deferred shell/window work after the previous present and before
     * any new frame acquires a drawable. No-op without registered app hooks. */
    platformOverlayService();
    /* The overlay may have opened or closed from the render callback since the
     * last pump -- the Resume button and the scripted schedule both do -- so
     * reconcile before any event is dispatched rather than waiting for an edge
     * that dispatch will never observe. */
    overlay_capture_sync(target_tick);
#endif
    input_dispatch_events(target_tick);
    platform_surface_visibility_update();
#ifdef __EMSCRIPTEN__
    /* JS pointer callbacks append bounded snapshots without re-entering wasm.
     * Drain them in order so a complete tap between rAF callbacks survives;
     * then sample current state as the analog/latest-value backstop. */
    while (browser_touch_pop(&s_browserTouchSource)) {
        input_capture_live(target_tick);
    }
    browser_touch_read_current(&s_browserTouchSource);
#endif
    /* Sample touch/current source state even when this opportunity carried no
     * discrete SDL event. The queue coalesces unchanged/analog-only samples. */
    input_capture_live(target_tick);

    if (s_quitRequested) {
        platform_request_exit(0);
    }
}

/*
 * Sample the host one last time, as late as the ticket allows.
 *
 * Called from the tick boundary in stubs_dkr.c immediately before
 * platform_input_commit_tick. It resolves its target from
 * present_sched_input_target_tick() -- the same accessor the pump uses -- so it
 * cannot file a sample against a different ticket than the pump would have,
 * and mdkr_input_tick_queue_capture's own monotonic clamp keeps the target
 * non-decreasing regardless.
 *
 * What this deliberately does NOT do, and why:
 *
 *   settings_toggle_poll / platform_surface_visibility_update -- frame-boundary
 *   services, not input. They stay on the pump, once per opportunity, exactly
 *   as before.
 *
 *   platform_request_exit on a quit -- s_quitRequested is latched here and
 *   acted on by the next pump, at most one present interval later. Acting on it
 *   here would let a quit arriving in the last few milliseconds of a tick
 *   suppress a ticket that would otherwise have been issued, which is a
 *   scheduling change and not a latency improvement.
 *
 * The DKR-visible contract is unchanged: this adds host captures, which are
 * already unbounded per tick and coalesced by the queue, and adds no ticket, no
 * consume, and no controller read. osContGetReadData still sees exactly one
 * published sample per authored tick.
 */
void platform_input_sample_late(void) {
    uint64_t target_tick;
    if (!input_jit_sampling_enabled() || !s_inputQueueReady) {
        return;
    }
    target_tick = present_sched_input_target_tick();
#ifdef MDKR_APP
    overlay_capture_sync(target_tick);
#endif
    input_dispatch_events(target_tick);
#ifdef __EMSCRIPTEN__
    while (browser_touch_pop(&s_browserTouchSource)) {
        input_capture_live(target_tick);
    }
    browser_touch_read_current(&s_browserTouchSource);
#endif
    input_capture_live(target_tick);
}

void platform_input_commit_tick(uint64_t ticket) {
    MdkrInputSample published[MDKR_INPUT_PORTS];
    unsigned port;
    if (!s_inputQueueReady) {
        return;
    }
    if (input_latency_census_enabled()) {
        input_latency_census_note_commit(pace_host_ns());
    }
    mdkr_input_tick_queue_consume(&s_inputQueue, ticket, published);
    for (port = 0; port < DKR_MAXPADS; port++) {
        s_pads[port].buttons = published[port].buttons;
        s_pads[port].stick_x = published[port].stick_x;
        s_pads[port].stick_y = published[port].stick_y;
        s_pads[port].present = published[port].present ||
            (s_scriptPresentMask & (1u << port)) != 0u;
    }
    /* Scripts are authored against the pass that just completed. osRecvMesg
     * now accounts that pass before publishing the next input ticket, so the
     * global counter is one step ahead of the long-standing script phase here.
     * Preserve that phase explicitly: an entry at N is consumed by the next
     * simulation sample and is traced at N+1. Presentation count still cannot
     * move an edge. */
    script_apply(s_pads, g_simTickCounter > 0 ? g_simTickCounter - 1 : 0);
    /* After script_apply, so the trace is the published snapshot DKR reads and
     * not an intermediate one. Inert unless MDKR_TEST_PAD_HOTPLUG is set. */
    pad_hotplug_trace_channels();
}

/* Publishes the pacing configuration the budget was taken under, then the
 * budget. Gathered here rather than in the census because the swap interval and
 * the latched present policy are this file's state. */
void platform_input_latency_summary(void) {
    int swap_interval = -1;
    if (!input_latency_census_enabled()) {
        return;
    }
#ifndef __EMSCRIPTEN__
    if (s_glReady) {
        swap_interval = SDL_GL_GetSwapInterval();
    }
#endif
    input_latency_census_note_config(
        present_sched_present_policy_name(), present_sched_present_rate(),
        present_sched_tick_fields(), swap_interval,
        present_sched_smoothing_enabled(), input_jit_sampling_enabled() != 0);
    input_latency_census_summary();
}

void platform_input_queue_summary(void) {
    unsigned pending = 0;
    unsigned port;
    if (!s_inputQueueReady || !input_consumption_trace_enabled()) {
        return;
    }
    for (port = 0; port < MDKR_INPUT_PORTS; port++) {
        pending += mdkr_input_tick_queue_pending_edges(&s_inputQueue, port);
    }
    fprintf(stderr,
            "[INPUTQ-SUMMARY] captures=%llu ticks=%llu edges=%llu "
            "stretched=%llu analog=%llu coalesced=%llu presence=%llu "
            "overflow=%llu reordered=%llu maxedge=%llu maxanalog=%llu "
            "pending=%u edgecap=%u samplecap=%u\n",
            (unsigned long long)s_inputQueue.stats.captures,
            (unsigned long long)s_inputQueue.stats.ticks,
            (unsigned long long)s_inputQueue.stats.button_edges,
            (unsigned long long)s_inputQueue.stats.stretched_edges,
            (unsigned long long)s_inputQueue.stats.analog_samples,
            (unsigned long long)s_inputQueue.stats.analog_coalesced,
            (unsigned long long)s_inputQueue.stats.presence_edges,
            (unsigned long long)s_inputQueue.stats.overflow_neutralizations,
            (unsigned long long)s_inputQueue.stats.reordered_targets,
            (unsigned long long)s_inputQueue.stats.max_button_depth,
            (unsigned long long)s_inputQueue.stats.max_analog_depth,
            pending, MDKR_INPUT_EDGE_CAPACITY,
            MDKR_INPUT_SAMPLE_CAPACITY);
    fflush(stderr);
}

/* Low-rate progress evidence for long resource-lifecycle qualification.  This
 * is deliberately separate from MDKR_TRACE: the plateau gate needs to locate
 * a blocked swap or teardown without enabling and flushing several per-frame
 * trace streams for 25,000 frames. */
static void sdl_gl_resource_heartbeat(const char *phase, int force) {
    unsigned inflight = 0u;
    if (!mdkr_resource_trace_enabled() || phase == NULL ||
        (!force && (g_frameCounter % 1000) != 0)) {
        return;
    }
#ifndef __EMSCRIPTEN__
    inflight = s_glFencesInFlight;
#endif
    fprintf(stderr,
            "[RESOURCE-HEARTBEAT] backend=gl phase=%s frame=%d surface=%llu "
            "inflight=%u\n",
            phase, g_frameCounter,
            (unsigned long long)g_surfaceFrameCounter,
            inflight);
    fflush(stderr);
}

void platform_sdl_present(void) {
    if (s_glReady) {
        /* The F3DDKR backend clears + renders into the default framebuffer each
         * frame (gfx_opengl_start_frame), so present must NOT clear here — doing
         * so would wipe the rendered frame right before the swap. Just swap. */
#ifdef MDKR_APP
        /* Draw the app shell's in-game overlay over the finished GL frame, just
         * before the swap. The WebGPU backend has its own overlay pass inside
         * wgpu_end_frame (gfx_webgpu.c); GL has no such seam, so this is it.
         * No-op when no overlay hooks are registered. */
        (void)platformOverlayRender();
#endif
        sdl_gl_resource_heartbeat("before-swap", 0);
        SDL_GL_SwapWindow(s_window);
        g_surfaceFrameCounter++;
#ifndef __EMSCRIPTEN__
        sdl_gl_backpressure_after_swap();
#endif
        sdl_gl_resource_heartbeat("after-swap", 0);
    }
    /* WebGPU has no swap here: wgpu_end_frame already presented the surface
     * (WGPU_COMPAT_PRESENT) inside gfx_end_frame. Nothing to do. */
}

void platform_sdl_shutdown(void) {
#ifdef MDKR_APP
    /*
     * An adopted window belongs to the app shell, which is still running (it
     * called us and will tear down its own ImGui context afterwards). Destroying
     * it here, or calling SDL_Quit under it, would pull the surface out from
     * under the launcher. Release only what the engine itself created.
     */
    if (s_windowAdopted) {
#ifndef __EMSCRIPTEN__
        if (s_glReady && s_glctx != NULL) {
            sdl_gl_resource_heartbeat("before-teardown", 1);
            sdl_gl_backpressure_shutdown();
            sdl_gl_resource_heartbeat("after-teardown", 1);
        }
#endif
        for (int i = 0; i < DKR_MAXPADS; i++) {
            if (s_gc[i]) {
                (void)platform_pad_rumble(i, 0);
                SDL_GameControllerClose(s_gc[i]);
                s_gc[i] = NULL;
                s_rumbleSupported[i] = -1;
                s_rumbleRequested[i] = 0;
            }
        }
        s_window = NULL;
        s_glctx = NULL;
        g_sdlWindow = NULL;
        s_glReady = 0;
        s_windowAdopted = 0;
        fprintf(stderr,
                "[SDL-SHUTDOWN] window=host glContext=host controllers=0 sdl=host\n");
        return;
    }
#endif
    if (s_glctx)  {
#ifndef __EMSCRIPTEN__
        if (s_glReady) {
            sdl_gl_resource_heartbeat("before-teardown", 1);
            sdl_gl_backpressure_shutdown();
            sdl_gl_resource_heartbeat("after-teardown", 1);
        }
#endif
        SDL_GL_DeleteContext(s_glctx);
        s_glctx = NULL;
    }
#if defined(MDKR_WEBGPU_BACKEND) && defined(__APPLE__)
    if (s_metalView) { SDL_Metal_DestroyView(s_metalView); s_metalView = NULL; }
#endif
    for (int i = 0; i < DKR_MAXPADS; i++) {
        if (s_gc[i]) {
            (void)platform_pad_rumble(i, 0);
            SDL_GameControllerClose(s_gc[i]);
            s_gc[i] = NULL;
            s_rumbleSupported[i] = -1;
            s_rumbleRequested[i] = 0;
        }
    }
    if (s_window) { SDL_DestroyWindow(s_window);  s_window = NULL; }
    g_sdlWindow = NULL;
    s_glReady = 0;
    if (s_sdlReady) { SDL_Quit(); s_sdlReady = 0; }
    /* SDL handles still held after teardown. Every field is READ back from the
     * owning variable, not asserted: a clean shutdown prints zeros, an early
     * return or a missed close prints what survived. */
    int openControllers = 0;
    for (int i = 0; i < DKR_MAXPADS; i++) {
        if (s_gc[i] != NULL) openControllers++;
    }
    fprintf(stderr,
            "[SDL-SHUTDOWN] window=%d glContext=%d controllers=%d sdl=%d\n",
            s_window != NULL, s_glctx != NULL, openControllers, s_sdlReady);
}

/* ======================================================================== *
 *  VI retrace / updateRate pacing  (the frame-pacing fix)
 *
 *  See platform_os.h for the mechanism. platform_vi_pace_measure() is called
 *  once per present from the video-queue recv (stubs_dkr.c). It returns the
 *  number of source VI fields the just-finished frame occupied — the value the game
 *  needs as `updateRate` to keep wall-clock speed constant — and (in realtime
 *  mode) paces the present to the configured one- or two-field floor so a
 *  fast/high-refresh path cannot outrun its selected simulation cadence.
 *
 *  Two modes:
 *    realtime  — measure the true wall-clock interval between presents (default
 *                for a windowed run). Original cadence uses a two-field floor;
 *                the explicit enhanced mode uses a one-field floor. Drift-
 *                free field accumulator; refresh-
 *                independent (logic timestep is tied to wall time, not to the
 *                display vblank count).
 *    synthetic — every present represents a FIXED field count (default for
 *                --headless-frames, so headless runs are deterministic). The
 *                default follows the resolved gameplay cadence. Env
 *                MDKR_SYNTH_FIELDS=N requests an explicit controlled stand-in
 *                for N source fields without a display or wall-clock jitter;
 *                it cannot undercut the selected cadence minimum.
 *
 *  Env overrides:
 *    MDKR_PACE_REALTIME=1  force realtime pacing even under --headless-frames.
 *    MDKR_SYNTH_FIELDS=N    synthetic field count per frame (default: selected
 *                           cadence minimum; clamped to that minimum..6).
 *    MDKR_VI_PACE=off       diagnostic only: disable elapsed-field compensation
 *                           and inject one field while retaining the selected
 *                           wall-time floor. Under original cadence this is a
 *                           deliberate 30/25 Hz slowdown, not the historical
 *                           60/50 Hz one-field mode. Use enhanced for that.
 *    MDKR_FIELD_HZ=H        diagnostic field cadence override (20..240);
 *                           default is derived from the active NTSC/PAL ROM.
 *
 * Gameplay.SimulationCadence (MDKR_SIMULATION_CADENCE):
 *    original (default)     minimum 2 fields/update, matching authored physics.
 *    enhanced              minimum 1 field/update, historical 60 Hz port mode.
 *
 * Headless mode follows the same resolved cadence as an interactive run.
 * Historical one-field fixtures must select enhanced and request one field
 * explicitly.
 * ======================================================================== */
int g_viLastFields     = 1;   /* fields injected == updateRate the game will see */
int g_viLastWallFields = 1;   /* true wall/synthetic fields elapsed (speed ref)  */
static int s_viLastPaceRebased = 0;

enum { PACE_REALTIME, PACE_SYNTH };
static int      s_paceMode        = -1;      /* lazy-init */
static int      s_paceCompensate  = 1;       /* 0 == diagnostic compensation off */
static int      s_synthFields     = 2;
static int      s_minFields       = 2;
static int      s_fieldHz         = 60;
static MdkrPacingClock s_paceClock;
static uint64_t s_paceCumWall     = 0;       /* cumulative host fields (trace) */
static uint64_t s_paceUnitResidual = 0;      /* sub-field carry for telemetry */
static uint64_t s_simCumFields    = 0;       /* fixed-ticket COUNTER source */
static unsigned s_testRebaseEvery = 0;       /* deterministic suspension gate */
static uint64_t s_paceSamples     = 0;

/* Local-only independent-oracle replay. Shipping converts elapsed time into
 * exact fixed tickets. This opt-in schedule answers the separate diagnostic
 * question of whether native gameplay agrees when it receives the real ROM's
 * observed variable 1..6-field update widths. */
typedef struct MdkrOracleFieldEntry {
    uint64_t tick;
    uint8_t fields;
} MdkrOracleFieldEntry;

static MdkrOracleFieldEntry *s_oracleFieldEntries;
static size_t s_oracleFieldCount;
static size_t s_oracleFieldCursor;
static int s_oracleFieldConfigured = -1;
static uint64_t s_oracleFieldApplied;

static void oracle_field_schedule_fatal(const char *path, unsigned line,
                                        const char *reason) {
    fprintf(stderr, "[FATAL] oracle update-field schedule %s:%u: %s\n",
            path != NULL ? path : "(null)", line, reason);
    fflush(stderr);
    exit(EXIT_FAILURE);
}

static void oracle_field_schedule_init(void) {
    const char *path;
    FILE *file;
    char line[256];
    unsigned line_number = 0u;
    size_t capacity = 0u;

    if (s_oracleFieldConfigured >= 0) {
        return;
    }
    s_oracleFieldConfigured = 0;
    path = getenv("MDKR_ORACLE_UPDATE_FIELDS");
    if (path == NULL || path[0] == '\0') {
        return;
    }
    file = mdkr_fopen_utf8(path, "r");
    if (file == NULL) {
        oracle_field_schedule_fatal(path, 0u, "cannot open file");
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *cursor = line;
        char *end;
        unsigned long long parsed_tick;
        unsigned long parsed_fields;
        MdkrOracleFieldEntry *grown;

        line_number++;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            fclose(file);
            oracle_field_schedule_fatal(path, line_number,
                                        "line exceeds 255 bytes");
        }
        end = strchr(cursor, '#');
        if (end != NULL) {
            *end = '\0';
        }
        while (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n') {
            cursor++;
        }
        if (*cursor == '\0') {
            continue;
        }
        errno = 0;
        parsed_tick = strtoull(cursor, &end, 10);
        if (errno != 0 || end == cursor || *cursor == '-') {
            fclose(file);
            oracle_field_schedule_fatal(path, line_number,
                                        "expected an integer tick");
        }
        cursor = end;
        errno = 0;
        parsed_fields = strtoul(cursor, &end, 10);
        if (errno != 0 || end == cursor || parsed_fields < 1u ||
            parsed_fields > MDKR_PACING_MAX_FIELDS) {
            fclose(file);
            oracle_field_schedule_fatal(
                path, line_number, "fields must be an integer in 1..6");
        }
        while (*end == ' ' || *end == '\t' ||
               *end == '\r' || *end == '\n') {
            end++;
        }
        if (*end != '\0') {
            fclose(file);
            oracle_field_schedule_fatal(path, line_number,
                                        "unexpected trailing data");
        }
        if (s_oracleFieldCount != 0u &&
            (s_oracleFieldEntries[s_oracleFieldCount - 1u].tick == UINT64_MAX ||
             parsed_tick !=
                 s_oracleFieldEntries[s_oracleFieldCount - 1u].tick + 1u)) {
            fclose(file);
            oracle_field_schedule_fatal(
                path, line_number,
                "ticks must be strictly increasing and contiguous");
        }
        if (s_oracleFieldCount == capacity) {
            size_t next = capacity != 0u ? capacity * 2u : 1024u;
            if (s_oracleFieldCount >= 1000000u) {
                fclose(file);
                oracle_field_schedule_fatal(path, line_number,
                                            "entry limit exceeded");
            }
            if (next > 1000000u) {
                next = 1000000u;
            }
            grown = realloc(s_oracleFieldEntries,
                            next * sizeof(*s_oracleFieldEntries));
            if (grown == NULL) {
                fclose(file);
                oracle_field_schedule_fatal(path, line_number,
                                            "allocation failed");
            }
            s_oracleFieldEntries = grown;
            capacity = next;
        }
        s_oracleFieldEntries[s_oracleFieldCount].tick =
            (uint64_t)parsed_tick;
        s_oracleFieldEntries[s_oracleFieldCount].fields =
            (uint8_t)parsed_fields;
        s_oracleFieldCount++;
    }
    if (ferror(file)) {
        fclose(file);
        oracle_field_schedule_fatal(path, line_number, "read failed");
    }
    fclose(file);
    if (s_oracleFieldCount == 0u) {
        oracle_field_schedule_fatal(path, line_number,
                                    "schedule contains no entries");
    }
    s_oracleFieldConfigured = 1;
    fprintf(stderr,
            "[ORACLE-SCHEDULE] loaded=%zu first=%llu last=%llu path=%s\n",
            s_oracleFieldCount,
            (unsigned long long)s_oracleFieldEntries[0].tick,
            (unsigned long long)
                s_oracleFieldEntries[s_oracleFieldCount - 1u].tick,
            path);
}

int platform_oracle_update_fields(uint64_t tick, int *fields) {
    oracle_field_schedule_init();
    if (!s_oracleFieldConfigured || fields == NULL ||
        s_oracleFieldCursor >= s_oracleFieldCount) {
        return 0;
    }
    while (s_oracleFieldCursor < s_oracleFieldCount &&
           s_oracleFieldEntries[s_oracleFieldCursor].tick < tick) {
        s_oracleFieldCursor++;
    }
    if (s_oracleFieldCursor >= s_oracleFieldCount ||
        s_oracleFieldEntries[s_oracleFieldCursor].tick != tick) {
        return 0;
    }
    *fields = (int)s_oracleFieldEntries[s_oracleFieldCursor].fields;
    s_oracleFieldCursor++;
    s_oracleFieldApplied++;
    return 1;
}

int platform_oracle_update_fields_finish(void) {
    int complete = 1;

    oracle_field_schedule_init();
    if (s_oracleFieldConfigured) {
        complete = s_oracleFieldApplied == (uint64_t)s_oracleFieldCount;
        fprintf(stderr,
                "[ORACLE-SCHEDULE] applied=%llu total=%zu complete=%d\n",
                (unsigned long long)s_oracleFieldApplied,
                s_oracleFieldCount, complete);
        if (!complete) {
            fprintf(stderr,
                    "[FATAL] oracle update-field schedule was not consumed "
                    "completely\n");
        }
    }
    free(s_oracleFieldEntries);
    s_oracleFieldEntries = NULL;
    s_oracleFieldCount = 0u;
    s_oracleFieldCursor = 0u;
    /* Finalization is idempotent. A second cleanup must not reload the same
     * environment-named schedule and falsely report that it was never used. */
    s_oracleFieldConfigured = 0;
    s_oracleFieldApplied = 0u;
    return complete;
}

static uint64_t pace_host_ns(void) {
#ifdef __EMSCRIPTEN__
    /* Browser work within one opportunity observes one rAF timestamp. This is
     * both the browser's real display clock and the deterministic schedule
     * harness seam; do not mix a second performance.now() opinion into it. */
    if (s_browserFrameNowNs != 0u) {
        return s_browserFrameNowNs;
    }
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void pace_credit_units(uint64_t units) {
    uint64_t total;
    uint64_t fields;

    if (UINT64_MAX - s_paceUnitResidual < units) {
        total = UINT64_MAX;
    } else {
        total = s_paceUnitResidual + units;
    }
    fields = total / UINT64_C(1000000000);
    s_paceUnitResidual = total % UINT64_C(1000000000);
    if (UINT64_MAX - s_paceCumWall < fields) {
        s_paceCumWall = UINT64_MAX;
    } else {
        s_paceCumWall += fields;
    }
    g_viLastWallFields = fields > (uint64_t)INT_MAX
        ? INT_MAX : (int)fields;
}

static void pace_lazy_init(void) {
    if (s_paceMode >= 0) return;
    const char *e;
    const char *fieldHzOverride;
    const MdkrVideoConfig *videoConfig = mdkr_video_config_current();
    const char *cadence = "original";
    if (videoConfig != NULL &&
        videoConfig->values[MDKR_VIDEO_SIMULATION_CADENCE].text[0] != '\0') {
        cadence =
            videoConfig->values[MDKR_VIDEO_SIMULATION_CADENCE].text;
    }
    s_minFields = mdkr_pacing_min_fields(cadence);
    fieldHzOverride = getenv("MDKR_FIELD_HZ");
    s_fieldHz = mdkr_pacing_field_hz(
        platform_source_field_hz(), fieldHzOverride);
    if (!mdkr_pacing_clock_init(
            &s_paceClock, s_fieldHz, s_minFields,
            MDKR_PACING_MAX_FIELDS)) {
        /* All inputs above are validated; retain a fail-safe NTSC original
         * clock if future integration violates that invariant. */
        s_fieldHz = 60;
        s_minFields = 2;
        (void)mdkr_pacing_clock_init(
            &s_paceClock, s_fieldHz, s_minFields,
            MDKR_PACING_MAX_FIELDS);
    }
    /* realtime for a windowed run; synthetic (deterministic) for headless. */
    s_paceMode = (g_headlessFrames < 0) ? PACE_REALTIME : PACE_SYNTH;
#ifdef __EMSCRIPTEN__
    /* Web is always realtime: the frame boundary suspends to requestAnimationFrame
     * and the elapsed-field count is derived from real (rAF-timed) wall clock, so
     * DKR's updateRate frameskip compensation keeps speed correct in-browser. */
    s_paceMode = PACE_REALTIME;
    if (platformAnimationFrameClockSynthetic() &&
        s_browserFrameNowNs == 0u) {
        /* The injector owns an exact logical timeline from this epoch onward.
         * Each rAF callback advances it only by its supplied rational delta;
         * real startup work cannot leak into the first interval. */
        s_browserFrameNowNs = pace_host_ns();
    }
#endif
    if ((e = getenv("MDKR_PACE_REALTIME")) && atoi(e) != 0) s_paceMode = PACE_REALTIME;
    e = getenv("MDKR_SYNTH_FIELDS");
    s_synthFields = mdkr_pacing_synthetic_fields(
        e != NULL ? atoi(e) : 0, s_minFields, MDKR_PACING_MAX_FIELDS);
    if ((e = getenv("MDKR_VI_PACE")) && strcasecmp(e, "off") == 0) s_paceCompensate = 0;
    if ((e = getenv("MDKR_TEST_PACE_REBASE_EVERY")) != NULL) {
        int parsed = atoi(e);
        if (parsed > 0 && parsed <= 1000000) {
            s_testRebaseEvery = (unsigned)parsed;
        }
    }
    MDKR_TRACE("pace init: mode=%s cadence=%s minFields=%d compensate=%d "
               "synthFields=%d fieldHz=%d sourceFieldHz=%d",
               s_paceMode == PACE_REALTIME ? "realtime" : "synth",
               cadence, s_minFields, s_paceCompensate, s_synthFields,
               s_fieldHz, platform_source_field_hz());
}

/*
 * The cadence the player selected, resolved once at launch.
 *
 * Gameplay code that must behave differently under Enhanced has to ask THIS,
 * never the per-tick updateRate. Under Original a lag tick legitimately
 * arrives with updateRate 3 or more, and the authored code must handle it
 * byte-identically -- bosses slowing down on a lag frame is authored N64
 * behaviour. Keying off updateRate would therefore change Original's output on
 * exactly the frames the determinism contract cares about most.
 */
bool platform_sim_cadence_is_enhanced(void) {
    /* s_minFields is resolved by present_pace_init() during startup, long
     * before any racer update runs, and defaults to 2 (Original) -- so an
     * unexpectedly early call reports Original and leaves the authored path
     * untouched, which is the fail-safe direction. */
    return s_minFields == 1;
}

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
/* ---- Windows wait precision (M3 slice 4) --------------------------------- *
 *
 * THE PROBLEM. mingw-w64's nanosleep is a Sleep() shim, and Sleep() rounds up
 * to the system timer tick -- 15.6 ms by default. A pacer asking for 16.67 ms
 * gets 31.2 ms, so a 60 Hz cadence collapses to 32 and every present-interval
 * distribution this milestone measures is meaningless on Windows. The tick is
 * not per-process precision: it is what the whole scheduler quantizes to.
 *
 * WHY A WAITABLE TIMER AND NOT timeBeginPeriod. timeBeginPeriod(1) works, and
 * used to be the only option, but it raises the timer resolution GLOBALLY --
 * every process on the machine wakes more often and burns more power for the
 * duration, which is why Windows has progressively de-fanged it. A
 * high-resolution waitable timer (Windows 10 1803+) gives this thread ~0.5 ms
 * wait accuracy without touching anyone else's scheduling, and lives in
 * kernel32, which the executable already imports -- so it adds no DLL to the
 * import table tools/check_windows_imports.sh allowlists, where winmm would.
 *
 * WHY IT IS RESOLVED AT RUNTIME. CREATE_WAITABLE_TIMER_HIGH_RESOLUTION and
 * CreateWaitableTimerExW are newer than some MinGW header sets and newer than
 * Windows 8. GetProcAddress keeps a build against older headers, and a run on
 * an older OS, from failing to link or to start; both fall back to the
 * portable wait below, which is exactly what shipped before this.
 *
 * WHY A SPIN AT THE END. Even a high-resolution timer is a scheduler wake, so
 * the last fraction of a millisecond is not reliably landed. The wait stops
 * PACE_WIN_SPIN_NS short and spins the remainder with a pause hint. One
 * millisecond of spin per present is ~6% of one core at 60 Hz, paid only when
 * the pacer would otherwise overshoot its deadline.
 *
 * NOT TESTED ON HARDWARE. This lane has no Windows box; it is compile-verified
 * against the MinGW cross toolchain (cmake/mingw-w64-x86_64.cmake) only. The
 * non-Windows builds do not reach any of it.
 */
#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define PACE_WIN_SPIN_NS UINT64_C(1000000)   /* spin the last millisecond */

typedef HANDLE(WINAPI *PaceCreateWaitableTimerExW)(LPSECURITY_ATTRIBUTES,
                                                   LPCWSTR, DWORD, DWORD);

static HANDLE s_paceWinTimer;
static int s_paceWinTimerState;   /* 0 untried, 1 ready, -1 unavailable */

/*
 * Process-lifetime handle by design: it is created at most once and the wait
 * below is on the hot path. The OS reclaims it at exit like every other
 * process-lifetime handle the host holds.
 */
static HANDLE pace_win_timer(void) {
    if (s_paceWinTimerState == 0) {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        PaceCreateWaitableTimerExW create = NULL;

        s_paceWinTimerState = -1;
        if (kernel32 != NULL) {
            create = (PaceCreateWaitableTimerExW)(void (*)(void))
                GetProcAddress(kernel32, "CreateWaitableTimerExW");
        }
        if (create != NULL) {
            /* Without the high-resolution flag this timer is no better than
             * the portable wait, so a refusal is treated as unavailable
             * rather than retried without it. */
            s_paceWinTimer = create(
                NULL, NULL,
                CREATE_WAITABLE_TIMER_MANUAL_RESET |
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_ALL_ACCESS);
            if (s_paceWinTimer != NULL) {
                s_paceWinTimerState = 1;
            }
        }
    }
    return s_paceWinTimerState == 1 ? s_paceWinTimer : NULL;
}
#endif /* _WIN32 && !__EMSCRIPTEN__ */

static uint64_t pace_sleep_until(uint64_t target_ns) {
    uint64_t now = pace_host_ns();
#ifdef __EMSCRIPTEN__
    /* The browser cannot block a thread; suspend to requestAnimationFrame (via
     * Asyncify) until the 1/60 s grid point is reached. ALWAYS yield at least one
     * rAF — this is the only per-frame yield, so it must run even when the frame
     * already overran the budget (keeps the tab responsive + presents the canvas).
     * Stop within ~2 ms of the target: another whole vsync would overshoot, and
     * the grid is absolute so finishing slightly early is phase lead, not drift. */
    do {
        now = browser_wait_animation_frame();
    } while (now + 2000000ULL < target_ns);
#elif defined(_WIN32)
    {
        HANDLE timer = pace_win_timer();
        while (now < target_ns) {
            const uint64_t rem = target_ns - now;
            if (rem <= PACE_WIN_SPIN_NS) {
                YieldProcessor();
                now = pace_host_ns();
                continue;
            }
            if (timer != NULL) {
                LARGE_INTEGER due;
                /* Negative is the relative form, in 100 ns units. */
                due.QuadPart =
                    -(LONGLONG)((rem - PACE_WIN_SPIN_NS) / UINT64_C(100));
                if (due.QuadPart == 0) {
                    due.QuadPart = -1;
                }
                if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
                    WaitForSingleObject(timer, INFINITE);
                } else {
                    Sleep(0);
                }
            } else {
                /* No high-resolution timer: the coarse wait still gets most of
                 * the way there, and the spin above lands the rest. */
                Sleep((DWORD)((rem - PACE_WIN_SPIN_NS) / UINT64_C(1000000)));
            }
            now = pace_host_ns();
        }
    }
#else
    while (now < target_ns) {
        uint64_t rem = target_ns - now;
        struct timespec req;
        req.tv_sec  = (time_t)(rem / 1000000000ULL);
        req.tv_nsec = (long)(rem % 1000000000ULL);
        nanosleep(&req, NULL);
        now = pace_host_ns();
    }
#endif
    return now;
}

/* Return the field count the just-completed frame represents (>=1), pacing to
 * the configured cadence floor in realtime mode. g_viLastWallFields = the TRUE
 * elapsed field count (unaffected by MDKR_VI_PACE=off); g_viLastFields = what
 * gets injected as updateRate (forced to 1 when compensation is disabled). */
int platform_vi_pace_measure(void) {
    pace_lazy_init();
    int wall;   /* true elapsed fields */
    const bool forced_rebase = s_surfaceResumeRebasePending != 0;

    s_viLastPaceRebased = 0;
    s_paceSamples++;
    if (s_paceMode == PACE_SYNTH) {
        wall = s_synthFields;
        if (forced_rebase) {
            wall = s_minFields;
            s_viLastPaceRebased = 1;
            s_surfaceResumeRebasePending = 0;
        } else if (s_testRebaseEvery != 0u &&
            s_paceSamples % s_testRebaseEvery == 0u) {
            /* Deterministic suspension/resume injector. The gap itself is
             * intentionally absent; the fresh interval is what gameplay may
             * observe. Shipping behavior is untouched unless the test-only
             * environment variable is explicitly armed. */
            wall = s_minFields;
            s_viLastPaceRebased = 1;
        }
    } else {
        uint64_t now = pace_host_ns();
        if (forced_rebase) {
            (void)mdkr_pacing_clock_init(
                &s_paceClock, s_fieldHz, s_minFields,
                MDKR_PACING_MAX_FIELDS);
            s_surfaceResumeRebasePending = 0;
        }
        uint64_t target = mdkr_pacing_clock_target(&s_paceClock, now);
        int rebased = 0;
#ifdef __EMSCRIPTEN__
        /* Web: yield EVERY frame (pace_sleep_until guarantees >=1 rAF), even
         * when the frame overran the configured floor — otherwise a heavy
         * frame would never suspend and the tab would hang. */
        now = pace_sleep_until(target);
#else
        if (now < target) {
            now = pace_sleep_until(target);
        }
#endif
        wall = mdkr_pacing_clock_commit(&s_paceClock, now, &rebased);
        if (rebased || forced_rebase) {
            /* Suspension time is not gameplay time. Begin a fresh authored
             * interval instead of turning the clamped stall into catch-up. */
            wall = s_minFields;
            s_viLastPaceRebased = 1;
            MDKR_TRACE("pace rebase: now=%llu fieldHz=%d",
                       (unsigned long long)now, s_fieldHz);
        }
    }

    pace_credit_units((uint64_t)wall * UINT64_C(1000000000));
    g_viLastFields = s_paceCompensate ? wall : 1;
    return g_viLastFields;
}

/* ---- Simulated N64 COUNTER source ---------------------------------------- *
 * In SYNTH pacing (headless) every present represents a FIXED field count, so
 * the cumulative field total is a deterministic clock — unlike the host
 * monotonic clock, which advances at whatever rate the machine happens to run
 * the frame loop. osGetCount() is derived from this in synth mode so that game
 * code reading the COUNTER (audio.c music_animation_fraction(), which drives
 * menu/character animation phase) is reproducible run to run. In REALTIME mode
 * the host clock is correct and is used unchanged. */
int platform_pace_is_synthetic(void) {
    pace_lazy_init();
    return s_paceMode == PACE_SYNTH;
}

int platform_pace_field_hz(void) {
    pace_lazy_init();
    return s_fieldHz;
}

int platform_sim_tick_fields(void) {
    pace_lazy_init();
    return s_minFields;
}

int platform_vi_pace_rebased(void) {
    return s_viLastPaceRebased;
}

int platform_vi_pace_compensating(void) {
    pace_lazy_init();
    return s_paceCompensate;
}

/* ---- presentation subloop pacing ---------------------------------------- *
 *
 * platform_vi_pace_measure() does two jobs at once: it measures elapsed fields
 * AND it is the only call that paces. Under N presents per tick those split.
 * The TICK floor (s_minFields) is untouched — it is what keeps authored physics
 * at its authored rate, and this path changes presentation only. What is added
 * is an absolute rational nanosecond deadline for capped native presentation.
 * Elapsed time is converted to SimSched units (one field == 1e9), preserving
 * exact sub-field alpha and audio time at 120/144/165/240 Hz. `uncapped` has no
 * software deadline; `display` follows rAF in the browser and the detected
 * display cadence plus backend sync on native.
 */
static int s_presentActive = -1;
static MdkrPresentPolicyKind s_presentKind = MDKR_PRESENT_ORIGINAL;
static unsigned s_presentEffectiveRate;
static bool s_presentSoftwareDeadline;
static MdkrPresentDeadlineClock s_presentDeadline;
#ifndef __EMSCRIPTEN__
static MdkrPresentDeadlineClock s_occludedDeadline;
#endif
static bool s_occludedDeadlineReady;
static uint64_t s_presentLastNs;
static uint64_t s_presentSyntheticPhase;
/*
 * The refresh of the display the window is on, re-derived live (M3 slice 2).
 *
 * present_pace_lazy_init() latches the POLICY once and must keep doing so --
 * video_config.c's schema rows spell out why re-resolving it under a running
 * engine is unsafe in both directions. The RATE is a different kind of value:
 * it is not a player choice at all, it is a property of the monitor the window
 * currently happens to be on, and dragging the window to a 144 Hz panel does
 * not change what the player asked for. So the rate refreshes and everything
 * derived from it refreshes with it, while kind/requested-rate/smoothing/
 * tearing stay exactly as they were latched.
 */
static unsigned s_presentDisplayRate;
/*
 * Shed floor (M3 slice 3). A policy that has no software cadence relies on the
 * presentation queue to block the loop -- which it does, but only for an
 * opportunity that actually SWAPS. An opportunity whose replay refused, or
 * whose optional image GPU admission shed, calls
 * platform_frame_sync_no_swap(): nothing is queued, so nothing blocks, and a
 * consistently-behind GPU can spin between refusals at whatever speed the loop
 * runs. Serving the refusal on the display cadence is the same remedy the
 * 1.0.4 held-frame deadline applies to smoothing-off, narrowed to the case
 * that actually lacks a limiter.
 *
 * This is a FLOOR, not a cadence: it arms only after an opportunity that did
 * not swap, so a run whose presents are all being retired by the queue never
 * touches it and its grid stays exactly the display's own.
 */
#ifndef __EMSCRIPTEN__
static MdkrPresentDeadlineClock s_shedDeadline;
#endif
static bool s_shedDeadlineReady;
static bool s_presentLastHeld;

/*
 * MDKR_TEST_DISPLAY_RATE_SWITCH=<hz>@<tick> -- the headless seam for the
 * display-change path (M3 slice 2).
 *
 * Dragging a window between monitors of different refresh cannot be performed
 * by an automated run, but everything downstream of the EVENT can: from the
 * moment the handler fires, the whole chain is "the host now reports a
 * different number". So this makes the host report a different number at a
 * chosen tick and synthesizes the same handler SDL's display-changed event
 * calls. What it does not cover is SDL's event delivery itself, which needs a
 * second monitor; tests/check_pacing_quality.py's docstring carries that manual
 * step.
 *
 * Test-only and inert unless set, like MDKR_TEST_PACE_REBASE_EVERY beside it.
 */
static int s_displaySwitchState = -1;   /* -1 unparsed, 0 disarmed, 1 armed */
static unsigned s_displaySwitchRate;
static uint64_t s_displaySwitchTick;
static bool s_displaySwitchFired;

static void display_switch_lazy_init(void) {
    const char *value;
    char *end = NULL;
    unsigned long rate;
    unsigned long long tick;

    if (s_displaySwitchState >= 0) {
        return;
    }
    s_displaySwitchState = 0;
    value = getenv("MDKR_TEST_DISPLAY_RATE_SWITCH");
    if (value == NULL || value[0] == '\0') {
        return;
    }
    rate = strtoul(value, &end, 10);
    if (end == value || end == NULL || *end != '@' ||
        rate < MDKR_PRESENT_RATE_MIN || rate > MDKR_PRESENT_RATE_MAX) {
        fprintf(stderr, "[PRESENT-DISPLAY] MDKR_TEST_DISPLAY_RATE_SWITCH='%s' "
                        "unrecognized (<hz>@<tick>); ignored\n", value);
        return;
    }
    tick = strtoull(end + 1, &end, 10);
    if (end == NULL || *end != '\0') {
        fprintf(stderr, "[PRESENT-DISPLAY] MDKR_TEST_DISPLAY_RATE_SWITCH='%s' "
                        "unrecognized (<hz>@<tick>); ignored\n", value);
        return;
    }
    s_displaySwitchRate = (unsigned)rate;
    s_displaySwitchTick = (uint64_t)tick;
    s_displaySwitchState = 1;
}

unsigned platform_present_display_rate(void) {
    display_switch_lazy_init();
    if (s_displaySwitchState == 1 && s_displaySwitchFired) {
        return s_displaySwitchRate;
    }
#ifdef __EMSCRIPTEN__
    return 0u; /* measured directly from requestAnimationFrame timestamps */
#else
    SDL_DisplayMode mode;
    int display = s_window != NULL ? SDL_GetWindowDisplayIndex(s_window) : 0;
    if (display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0 &&
        mode.refresh_rate >= (int)MDKR_PRESENT_RATE_MIN &&
        mode.refresh_rate <= (int)MDKR_PRESENT_RATE_MAX) {
        return (unsigned)mode.refresh_rate;
    }
    /* No window yet, a failed query, or a refresh outside the range the pacer
     * can work with: 0 is the contract's "the host does not report one", and
     * the policy's unknown-refresh branch is what decides from there. Guessing
     * 60 would make that branch unreachable on native. */
    return 0u;
#endif
}

static void present_pace_lazy_init(void) {
    MdkrPresentPolicy effectivePolicy;
    bool heldFrameDeadline;
    if (s_presentActive >= 0) {
        return;
    }
    pace_lazy_init();
    s_presentDisplayRate = platform_present_display_rate();
    s_presentKind = present_sched_present_kind();
    /* Every non-original policy owns host opportunity pacing, even when a
     * numeric cap is at/below an enhanced simulation tick rate. Replay is a
     * separate decision and remains armed only above the tick rate. */
    s_presentActive = s_presentKind != MDKR_PRESENT_ORIGINAL ? 1 : 0;
    if (!s_presentActive) {
        /*
         * A 50 Hz source keeps a 40 ms authored image. A 60 Hz monitor can only
         * show it for two refreshes (33.3 ms) or three (50 ms), so Original
         * alternates between them: the average is exactly right and the game
         * runs at its authored speed, but no two consecutive images are on
         * screen for the same length of time, which is what a PAL player sees
         * as unevenness and what a frame counter reads as a rate below the
         * source's own. A 60 Hz source divides evenly and has no such beat.
         *
         * Say so once, at the one place that knows both the source cadence and
         * the chosen policy, and name the setting that fixes it. This is a
         * property of the combination, not a fault, so it is a note and not a
         * warning, and nothing about the run changes.
         */
        if (s_paceMode == PACE_REALTIME && s_fieldHz == 50) {
            fprintf(stderr,
                    "[PRESENT-POLICY] source=50Hz policy=original "
                    "note=a 50 Hz game cannot hold every image for the same "
                    "time on a 60 Hz display; Video.FrameLimit=display with "
                    "Video.MotionSmoothing=interpolate gives even motion at "
                    "your display's rate, and leaves game speed and music "
                    "exactly as authored\n");
        }
        return;
    }
    effectivePolicy.kind = s_presentKind;
    effectivePolicy.rate = present_sched_present_rate();
    heldFrameDeadline = mdkr_present_policy_needs_held_frame_deadline(
        &effectivePolicy, present_sched_smoothing_enabled() ? 1 : 0) != 0;
    if (s_presentKind == MDKR_PRESENT_CAPPED) {
        s_presentEffectiveRate = present_sched_present_rate();
        s_presentSoftwareDeadline = true;
    } else if (s_presentKind == MDKR_PRESENT_DISPLAY) {
        s_presentEffectiveRate = s_presentDisplayRate;
        if (s_paceMode == PACE_SYNTH && s_presentEffectiveRate == 0u) {
            /* Synthetic pacing divides per present by this rate; an
             * unreported refresh keeps a deterministic 60 Hz stand-in. */
            s_presentEffectiveRate = 60u;
        }
        s_presentSoftwareDeadline = heldFrameDeadline;
    } else if (s_presentKind == MDKR_PRESENT_DISPLAY_MARGIN) {
        /*
         * The one policy whose whole content is a SOFTWARE cadence strictly
         * under the display's. `display` deliberately installs no limiter and
         * lets the blocking queue set the pace; this one cannot do that,
         * because the queue's pace IS the refresh and the point here is to
         * finish before it. So the deadline grid is unconditional rather than
         * inherited from heldFrameDeadline: it is the mechanism, not a
         * fallback for held frames.
         *
         * The margin is applied to the SAME live refresh `display` follows,
         * including the synthetic stand-in, so a headless run is deterministic
         * for the same reason display's is. An unreported refresh on a real
         * session leaves the rate at 0 and the branch below declines to build
         * a grid, which is exactly plain `display` behaviour -- the honest
         * answer when there is no number to sit under.
         */
        unsigned refresh = s_presentDisplayRate;
        if (s_paceMode == PACE_SYNTH && refresh == 0u) {
            refresh = 60u;
        }
        s_presentEffectiveRate =
            mdkr_present_policy_display_margin_rate(refresh);
        s_presentSoftwareDeadline = s_presentEffectiveRate != 0u;
    } else if (s_presentKind == MDKR_PRESENT_UNCAPPED &&
               s_paceMode == PACE_SYNTH) {
        /* Deterministic headless stand-in: 1 ms opportunities. Live uncapped
         * has no limiter and measures the actual loop duration below. */
        s_presentEffectiveRate = 1000u;
    } else if (s_presentKind == MDKR_PRESENT_UNCAPPED &&
               heldFrameDeadline) {
        /* With no new image between authored ticks, an unlimited no-swap loop
         * can only burn a core and starve the audio sink; it cannot improve
         * visible motion. Service held frames at the display cadence while
         * leaving the requested policy and authored output unchanged. */
        s_presentEffectiveRate = s_presentDisplayRate;
        s_presentSoftwareDeadline = true;
    }
    if (s_presentSoftwareDeadline && s_presentEffectiveRate != 0u &&
        !mdkr_present_deadline_init(
            &s_presentDeadline, s_presentEffectiveRate)) {
        s_presentActive = 0;
        return;
    }
    s_presentLastNs = pace_host_ns();
    MDKR_TRACE("present pace: policy=%s rate=%u tickFields=%d fieldHz=%d "
               "heldDeadline=%d",
               present_sched_present_policy_name(), s_presentEffectiveRate,
               s_minFields, s_fieldHz, heldFrameDeadline ? 1 : 0);
}

int platform_present_subloop_fields(void) {
    present_pace_lazy_init();
    return s_presentActive;
}

/*
 * The window is now on a display with a different refresh (M3 slice 2).
 *
 * WHAT REFRESHES. Everything the pacer derived FROM the refresh: the effective
 * rate for a policy that follows the display, every deadline grid standing on
 * that rate, and the backend's present-mode ranking (a rate above the old
 * refresh may be at or below the new one, or the reverse, and the swapchain
 * baked that decision at configuration time).
 *
 * WHAT DOES NOT. The latched policy -- kind, requested rate, smoothing,
 * tearing. video_config.c's schema rows argue at length why re-resolving those
 * under a running engine is unsafe in both directions, and none of that
 * argument is weakened by the monitor changing: the player asked for `display`
 * or for `144`, and they still are. A numeric cap in particular keeps its
 * number; only the ranking it is compared against moves.
 *
 * The grids are re-INITIALISED rather than re-phased. A deadline grid is an
 * absolute rational schedule anchored at an origin, so a grid built for 60 Hz
 * cannot be reinterpreted at 144 Hz; the next target re-anchors at the next
 * call, which is one opportunity of phase and no drift.
 */
/*
 * What THIS policy's cadence resolves to on a display reporting `rate`.
 *
 * One function for the log row and the install below, so the row is a statement
 * about the code that runs rather than a second opinion beside it. A numeric cap
 * and Original keep whatever they already had: their cadence is not a property
 * of the monitor.
 */
static unsigned present_pace_rate_for_display(unsigned rate) {
    switch (s_presentKind) {
        case MDKR_PRESENT_DISPLAY:
        case MDKR_PRESENT_UNCAPPED:
            return rate;
        case MDKR_PRESENT_DISPLAY_MARGIN:
            return mdkr_present_policy_display_margin_rate(rate);
        default:
            return s_presentEffectiveRate;
    }
}

static void present_pace_note_display_changed(void) {
    unsigned rate;
    unsigned resolved;

    if (s_presentActive < 0) {
        /* The policy has not been latched yet, so the lazy init that latches
         * it will read the current rate anyway. */
        return;
    }
    rate = platform_present_display_rate();
    if (rate == s_presentDisplayRate) {
        return;
    }
    resolved = present_pace_rate_for_display(rate);
    /*
     * `effectiveRate` is what the pacer is running on at this instant;
     * `resolvedRate` is what the latched policy's cadence WORKS OUT TO on the
     * new display. A realtime run installs the second as the first immediately
     * below. A synthetic run deliberately does not -- its effective rate is a
     * deterministic divisor, not a measurement -- so publishing both is what
     * lets a headless gate check the derivation without making the run depend
     * on which monitor it happened to be on.
     */
    fprintf(stderr,
            "[PRESENT-DISPLAY] event=display-changed oldHz=%u newHz=%u "
            "policy=%s effectiveRate=%u resolvedRate=%u\n",
            s_presentDisplayRate, rate,
            present_sched_present_policy_name(), s_presentEffectiveRate,
            resolved);
    s_presentDisplayRate = rate;
    /*
     * Synthetic pacing does not sleep: s_presentEffectiveRate is the DIVISOR of
     * a deterministic per-present timeline there, not a measurement of any
     * display. Moving it would make a headless run depend on which monitor the
     * window happened to be on, which is the one property those runs exist to
     * not have. The present-mode re-rank below still happens -- it is what the
     * display-change arm asserts, and it touches no clock.
     */
    if (s_paceMode == PACE_REALTIME) {
        if (s_presentActive && rate != 0u &&
            s_presentKind != MDKR_PRESENT_CAPPED &&
            s_presentKind != MDKR_PRESENT_ORIGINAL) {
            /* Only the policies whose cadence is DERIVED FROM the display's --
             * equal to it for display/uncapped, a fixed step under it for
             * display-margin. A numeric cap's effective rate is the player's
             * number and must not move. */
            s_presentEffectiveRate = resolved;
            if (s_presentKind == MDKR_PRESENT_DISPLAY_MARGIN) {
                s_presentSoftwareDeadline = s_presentEffectiveRate != 0u;
            }
        }
        if (s_presentSoftwareDeadline && s_presentEffectiveRate != 0u) {
            (void)mdkr_present_deadline_init(
                &s_presentDeadline, s_presentEffectiveRate);
        }
        s_occludedDeadlineReady = false;
        s_shedDeadlineReady = false;
    }
    /*
     * Re-rank the present mode. Only the WebGPU backend ranks against the
     * refresh at surface configuration; the GL swap interval is a function of
     * the tearing opt-in and the automation flag, neither of which a display
     * change moves, so there is nothing for GL to re-rank HERE. (A policy or
     * tearing change does move it, and platform_present_config_apply() re-runs
     * sdl_apply_gl_present_policy() for exactly that reason.)
     */
#if defined(MDKR_WEBGPU_BACKEND) && !defined(__EMSCRIPTEN__)
    if (mdkr_render_backend() == MDKR_BACKEND_WEBGPU) {
        gfx_webgpu_request_surface_reconfigure();
    }
#endif
}

/*
 * The PRESENTATION domain's boundary applier (video_config.h's deferred apply).
 *
 * WHERE IT RUNS. stubs_dkr.c's osRecvMesg video-queue branch, at the top,
 * before platform_present_subloop_fields(). Everything about that point is
 * load-bearing and none of it is incidental:
 *
 *   - The previous authoritative pass has completed (osRecvMesg is only
 *     reached after fb_update finished it), so no game state is half-written.
 *   - The previous tick's presentation subloop has fully exited — it is a
 *     `for(;;)` that only breaks on a due ticket or an exit request, and the
 *     branch cannot be re-entered until it does. So NO REPLAY WALK IS IN
 *     FLIGHT, which is the precondition the whole hazard turns on.
 *   - The next tick's pacing decision has not been made yet, so re-latching the
 *     policy here is indistinguishable from having booted with it.
 *
 * WHAT IT DOES, IN ORDER, AND WHY THAT ORDER.
 *
 *  1. gfx_dkr_replay_invalidate() FIRST, unconditionally, in both directions.
 *     This is the fix for the stale-walk hazard video_config.c's Video.
 *     FrameLimit row describes: dkr_walk_entry_* (the saved RDP/RSP state and,
 *     fatally, the saved SEGMENT TABLE) is refreshed by gfx_start_frame only
 *     while replay is armed, and cleared by nothing else. Retiring it before
 *     the policy moves means there is no instant at which an armed subloop can
 *     reach a walk entry captured under the other policy — gfx_dkr_replay_walk
 *     refuses outright while dkr_walk_entry_valid is false, and the next real
 *     walk is what makes it true again.
 *  2. presentation_snapshot_stage_reset(), for the same reason one tick of
 *     interpolation between two policies would otherwise blend a pair captured
 *     under different arming decisions.
 *  3. present_sched_replay_rearm(), so the store's one-shot belongs to the
 *     policy being switched to.
 *  4. The policy push. Now, and not earlier: after this line
 *     present_sched_smoothing_enabled() and present_sched_present_kind()
 *     answer with the new values, and by construction nothing that could
 *     misuse them still holds state from the old ones.
 *  5. The pacer state machine, re-INITIALISED rather than adjusted. s_present-
 *     Active back to -1 is what makes present_pace_lazy_init() re-resolve the
 *     whole derivation — kind, effective rate, software-deadline decision, and
 *     the deadline grid standing on it. A grid is an absolute rational schedule
 *     anchored at an origin and cannot be reinterpreted at another rate, so it
 *     is rebuilt, exactly as the display-change path above rebuilds it.
 *  6. The backend's present mode, which is a function of policy AND tearing
 *     together and therefore cannot be re-ranked before both are settled.
 */
void platform_present_config_apply(void) {
    const int wasActive = s_presentActive;

    /* 1-2: retire everything the old policy's replay could still be reached
     * through. Both are cheap no-ops when the features were never armed. */
    gfx_dkr_replay_invalidate();
    presentation_snapshot_stage_reset();

    /* 3-4 */
    present_sched_replay_rearm();
    mdkr_video_config_push_presentation();

    /* 5. Clear the derived state explicitly rather than trusting lazy_init to
     * overwrite every field: its ORIGINAL branch returns early, so a policy
     * change INTO original would otherwise leave a software-deadline flag and a
     * grid describing the policy that just left. */
    s_presentActive = -1;
    s_presentSoftwareDeadline = false;
    s_presentEffectiveRate = 0u;
    s_presentSyntheticPhase = 0u;
    s_occludedDeadlineReady = false;
    s_shedDeadlineReady = false;
    s_presentLastHeld = false;
    present_pace_lazy_init();

    fprintf(stderr,
            "[PRESENT-POLICY] event=live-apply policy=%s rate=%u smoothing=%d "
            "tearing=%d subloopWas=%d subloopNow=%d\n",
            present_sched_present_policy_name(),
            s_presentEffectiveRate,
            present_sched_smoothing_enabled() ? 1 : 0,
            present_sched_allow_tearing() ? 1 : 0,
            wasActive > 0 ? 1 : 0, s_presentActive > 0 ? 1 : 0);
    fflush(stderr);

    /* 6. Re-rank the present mode. Each backend re-emits its own [PRESENT-MODE]
     * row when it does, which is what a gate reads to prove the change reached
     * the swapchain rather than merely the config. */
#ifndef __EMSCRIPTEN__
#ifdef MDKR_WEBGPU_BACKEND
    if (mdkr_render_backend() == MDKR_BACKEND_WEBGPU) {
        gfx_webgpu_request_surface_reconfigure();
    }
#endif
    if (s_glReady) {
        sdl_apply_gl_present_policy();
    }
#endif
}

/*
 * Poll the test seam that stands in for SDL's display-changed event. Real
 * runs never enter the body: s_displaySwitchState is 0 unless
 * MDKR_TEST_DISPLAY_RATE_SWITCH parsed, and the whole call is one compare.
 */
static void present_pace_poll_display_switch(void) {
    display_switch_lazy_init();
    if (s_displaySwitchState != 1 || s_displaySwitchFired) {
        return;
    }
    if (present_sched_ticks() < s_displaySwitchTick) {
        return;
    }
    s_displaySwitchFired = true;
    present_pace_note_display_changed();
}

uint64_t platform_present_display_quantum_units(void) {
    present_pace_lazy_init();
    if (s_paceMode != PACE_REALTIME) {
        /* Synthetic pacing does not sleep and has no vblank to project onto.
         * Declining here is what keeps every headless arm byte-identical. */
        return 0u;
    }
    if (!s_presentActive || s_presentSoftwareDeadline) {
        /* Original presents one image per tick, and a software cadence is
         * already the thing spacing presents -- its grid, not the display's,
         * is what an opportunity lands on. */
        return 0u;
    }
    if (s_presentDisplayRate == 0u || present_sched_allow_tearing()) {
        return 0u;
    }
    if (present_sched_present_sync(s_presentDisplayRate) !=
        MDKR_PRESENT_SYNC_BLOCKING) {
        /* A latest-image queue drops undisplayed frames rather than pacing the
         * caller, so a present is not one refresh. */
        return 0u;
    }
    return (uint64_t)s_fieldHz * UINT64_C(1000000000) /
           (uint64_t)s_presentDisplayRate;
}

/*
 * Pace one present and return exact source-field units. Unlike
 * platform_vi_pace_measure() this never applies s_minFields: the tick floor is
 * enforced by the accumulator that consumes these counts, not by sleeping.
 */
uint64_t platform_vi_present_pace_units(void) {
    uint64_t units;

    present_pace_lazy_init();
    present_pace_poll_display_switch();
    s_viLastPaceRebased = 0;
    if (!s_presentActive) {
        /* Not engaged: one present is one tick, exactly as before. */
        (void)platform_vi_pace_measure();
        return (uint64_t)g_viLastWallFields * UINT64_C(1000000000);
    }
    if (s_surfaceResumeRebasePending) {
        s_surfaceResumeRebasePending = 0;
        s_presentLastNs = pace_host_ns();
        s_occludedDeadlineReady = false;
        /* Suspension time is retired, not paced across: the floor's grid
         * phase belongs to the session that was interrupted. */
        s_shedDeadlineReady = false;
        if (s_presentSoftwareDeadline) {
            (void)mdkr_present_deadline_init(
                &s_presentDeadline, s_presentEffectiveRate);
        }
        units = (uint64_t)s_minFields * UINT64_C(1000000000);
        s_viLastPaceRebased = 1;
        pace_credit_units(units);
        return units;
    }
    if (s_paceMode == PACE_SYNTH) {
        /*
         * Per-present wall fields advance host-time telemetry below, but they
         * do NOT advance s_simCumFields. That is the deterministic COUNTER
         * source in synthetic mode (osGetCount, stubs_dkr.c), and osGetCount
         * carries a monotonic clamp
         * that fabricates +1 whenever it is called twice without the clock
         * having moved. So the VALUE game code reads depends on where the clock
         * advances relative to the tick, not merely on how much it advances.
         * Feeding it per present shifted that phase by one present and a level
         * load completed one tick early — the [SIMHASH] streams for
         * MDKR_PRESENT_RATE unset and =60 diverged from tick 1345 onward, at
         * exactly the 270-object to 96-object transition.
         *
         * So in synthetic mode one fixed ticket is committed by
         * platform_vi_tick_clock_commit(), while the per-present count returned
         * here feeds host time and the authoritative accumulator. In realtime
         * mode osGetCount reads the host clock instead.
         */
        const uint64_t units_per_second =
            (uint64_t)s_fieldHz * UINT64_C(1000000000);
        s_presentSyntheticPhase += units_per_second;
        units = s_presentSyntheticPhase / s_presentEffectiveRate;
        s_presentSyntheticPhase %= s_presentEffectiveRate;
    } else {
        uint64_t now = pace_host_ns();
        uint64_t target = now;
        bool deadline = s_presentSoftwareDeadline;
        /* Which grid this opportunity is being spaced on, so the commit and
         * rebase paths below cannot disagree with the wait above about it. */
        MdkrPresentDeadlineClock *clock =
            s_presentSoftwareDeadline ? &s_presentDeadline : NULL;
        unsigned clock_rate = s_presentEffectiveRate;
#ifndef __EMSCRIPTEN__
        const unsigned tick_rate =
            (unsigned)s_fieldHz / (unsigned)s_minFields;
        if (present_sched_render_elided()) {
            if (!s_occludedDeadlineReady) {
                s_occludedDeadlineReady = mdkr_present_deadline_init(
                    &s_occludedDeadline, tick_rate) != 0;
            }
            if (s_occludedDeadlineReady) {
                deadline = true;
                clock = &s_occludedDeadline;
                clock_rate = tick_rate;
            }
        } else {
            s_occludedDeadlineReady = false;
            if (!s_presentSoftwareDeadline && s_presentLastHeld) {
                /*
                 * SHED FLOOR. A policy with no software cadence relies on the
                 * presentation queue to space its opportunities, and the queue
                 * only does that for one that actually SWAPS. An opportunity
                 * whose replay refused, or whose optional image GPU admission
                 * shed, hands the queue nothing -- so nothing blocks, and the
                 * loop's next speed is the machine's. That cannot put one more
                 * image on screen; it can burn a core and starve the audio
                 * sink trying.
                 *
                 * One display interval is the floor, because that is the
                 * soonest a NEW image could have mattered. It is a FLOOR and
                 * not a cadence: an opportunity that swapped does not consult
                 * it at all, so a run whose GPU keeps up is paced exactly as
                 * before by the queue itself, and the grid's fixed phase means
                 * a run that does consult it cannot accumulate drift the way
                 * sleeping a relative interval from each refusal would.
                 *
                 * When the host reports no refresh, the authoritative tick is
                 * the honest fallback: there is by definition nothing new to
                 * show between ticks.
                 */
                const unsigned shed_rate = s_presentEffectiveRate != 0u
                    ? s_presentEffectiveRate
                    : (s_presentDisplayRate != 0u ? s_presentDisplayRate
                                                  : tick_rate);
                if (!s_shedDeadlineReady) {
                    s_shedDeadlineReady = mdkr_present_deadline_init(
                        &s_shedDeadline, shed_rate) != 0;
                }
                if (s_shedDeadlineReady) {
                    /* Deliberately NOT `clock`: the floor carries no index for
                     * the commit/rebase paths below to advance, and `clock` is
                     * NULL on every path that reaches here. */
                    target = mdkr_present_grid_next(&s_shedDeadline, now);
                    deadline = true;
                }
            }
        }
#endif
        if (deadline && clock != NULL) {
            target = mdkr_present_deadline_target(clock, now);
        }
#ifdef __EMSCRIPTEN__
        /* Browser presentation is at most one opportunity per rAF. Numeric
         * caps skip rAF opportunities until their absolute deadline; display
         * consumes exactly one. Never spin inside a JS task. */
        do {
            now = browser_wait_animation_frame();
        } while (deadline && now < target);
#else
        if (deadline && now < target) {
            now = pace_sleep_until(target);
        }
#endif
        if (now > s_presentLastNs &&
            mdkr_pacing_interval_requires_rebase(
                now - s_presentLastNs)) {
            /* Suspension/debugger/occlusion time is retired, not simulated. */
            units = (uint64_t)s_minFields * UINT64_C(1000000000);
            s_viLastPaceRebased = 1;
            if (deadline && clock != NULL) {
                (void)mdkr_present_deadline_init(clock, clock_rate);
                (void)mdkr_present_deadline_target(clock, now);
            }
        } else {
            const uint64_t elapsed = now > s_presentLastNs
                ? now - s_presentLastNs : 0u;
            units = elapsed > UINT64_MAX / (uint64_t)s_fieldHz
                ? UINT64_MAX : elapsed * (uint64_t)s_fieldHz;
            if (deadline && clock != NULL) {
                mdkr_present_deadline_commit(clock, now);
            }
        }
        s_presentLastNs = now;
    }
    pace_credit_units(units);
    return units;
}

/*
 * Commit one authoritative tick's worth of synthetic field budget, at the point
 * in the branch where the non-subloop path commits it. See the synthetic arm of
 * platform_vi_present_pace_units for why the PHASE of this matters and not
 * just the total. No-op in realtime mode, where the COUNTER comes from the
 * host clock.
 */
void platform_vi_tick_clock_commit(unsigned tick_fields) {
    pace_lazy_init();
    if (s_paceMode != PACE_SYNTH) {
        return;
    }
    s_simCumFields += (uint64_t)tick_fields;
}

uint64_t platform_sim_field_count(void) {
    pace_lazy_init();
    return s_simCumFields;
}

/* ---- Objective motion/clock probe (published from racer.c) ----------------- *
 * One slot per HUMAN player. Player 1 keeps the original unsuffixed [PACE]
 * fields so every existing check parses unchanged; players 2-4 use [PACE2]-
 * [PACE4] lines which appear only after that human racer has published. Their
 * presence is therefore direct evidence that each local player exists, rather
 * than an inference from the requested viewport layout. */
#define MDKR_PROBE_PLAYERS DKR_MAXPADS
static float s_probeX, s_probeY, s_probeZ;
static int   s_probeClock;
static int   s_probeCheckpoint = -1, s_probeLap = -1;
static int   s_probeValid = 0;
static float s_probe2X[MDKR_PROBE_PLAYERS], s_probe2Y[MDKR_PROBE_PLAYERS], s_probe2Z[MDKR_PROBE_PLAYERS];
static int   s_probe2Clock[MDKR_PROBE_PLAYERS];
static int   s_probe2Cp[MDKR_PROBE_PLAYERS]  = { -1, -1, -1, -1 };
static int   s_probe2Lap[MDKR_PROBE_PLAYERS] = { -1, -1, -1, -1 };
static int   s_probe2Valid[MDKR_PROBE_PLAYERS];
void mdkr_pace_probe_racer(int playerIndex, float x, float y, float z, int clockFrames) {
    if (playerIndex < 0 || playerIndex >= MDKR_PROBE_PLAYERS) return;   /* humans only */
    s_probe2X[playerIndex] = x; s_probe2Y[playerIndex] = y; s_probe2Z[playerIndex] = z;
    s_probe2Clock[playerIndex] = clockFrames;
    s_probe2Valid[playerIndex] = 1;
    if (playerIndex != 0) return;   /* PLAYER_ONE also feeds the legacy fields */
    s_probeX = x; s_probeY = y; s_probeZ = z; s_probeClock = clockFrames;
    s_probeValid = 1;
}
/* Which vehicle module a human racer is actually being updated by, on its own
 * greppable "[PVEH]" line so the [PACE] format above stays byte-compatible with
 * every existing check.
 *
 * Why this is worth two stores: MDKR_LOAD_TRACK=<level>:<vehicle> only rewrites the
 * ARGUMENT to level_load(). Whether the racer object ends up with that vehicleID --
 * and therefore whether update_player_racer() dispatches into the hovercraft/plane
 * module at all -- is a separate question, and one a "the process survived and the
 * checkpoint counter climbed" assertion cannot answer: a silently-ignored override
 * would drive a CAR on every row of tests/check_vehicle_sweep.py and pass all 47.
 * Printed once per change, so a whole race costs one line. */
static int s_probeVehicle[MDKR_PROBE_PLAYERS] = { -1, -1, -1, -1 };
void mdkr_pace_probe_vehicle(int playerIndex, int vehicleID) {
    if (playerIndex < 0 || playerIndex >= MDKR_PROBE_PLAYERS) return;
    if (s_probeVehicle[playerIndex] == vehicleID) return;
    s_probeVehicle[playerIndex] = vehicleID;
    MDKR_TRACE("[PVEH] frame=%d player=%d vehicleID=%d", g_frameCounter, playerIndex, vehicleID);
}
/* Race progress along the spline (checkpoint index + lap). Position alone cannot
 * distinguish "driving the track" from "sliding along a wall" or "spinning in
 * place", so the regression check (tests/check_race_drive.sh) asserts on this. */
void mdkr_pace_probe_progress(int playerIndex, int checkpoint, int lap) {
    if (playerIndex < 0 || playerIndex >= MDKR_PROBE_PLAYERS) return;
    s_probe2Cp[playerIndex] = checkpoint; s_probe2Lap[playerIndex] = lap;
    if (playerIndex != 0) return;
    s_probeCheckpoint = checkpoint; s_probeLap = lap;
}
/* Race-finish state. Published from race_check_finish(), i.e. from OUTSIDE
 * update_player_racer -- which is the whole point. update_player_racer stops
 * running for a racer the moment it finishes (a finished racer is handed to the
 * AI for the finish cutscene), so mdkr_pace_probe_progress() above freezes one
 * lap short and can never report raceFinished. Silence from that probe is
 * therefore NOT evidence that the finish did not fire. Finish checks must read
 * these fields plus the finishing place and stable identity below. */
static int s_probeRaceLap = -1, s_probeRaceFinished = -1;
static int s_probeRaceFinishPosition = -1;
static int s_probeRaceRacerIndex = -1, s_probeRacePlayerIndex = -1;
void mdkr_pace_probe_finish(
    int lap, int raceFinished, int finishPosition,
    int racerIndex, int playerIndex,
    int racePosition, int racerCount, int lapCount, int itemQuantity,
    int itemType) {
    static int eventValid;
    static int eventLap, eventFinished, eventPosition;
    static int eventRacerIndex, eventPlayerIndex;
    if (!eventValid || lap != eventLap || raceFinished != eventFinished ||
        finishPosition != eventPosition || racerIndex != eventRacerIndex ||
        playerIndex != eventPlayerIndex) {
        GAMEPLAY_EVENT_TRACE(
            GAMEPLAY_EVENT_RACE_RESULT, playerIndex, racerIndex,
            (int32_t)(((uint32_t)(uint16_t)lap << 16) |
                      ((uint32_t)raceFinished & UINT32_C(0xFFFF))),
            finishPosition);
        eventLap = lap;
        eventFinished = raceFinished;
        eventPosition = finishPosition;
        eventRacerIndex = racerIndex;
        eventPlayerIndex = playerIndex;
        eventValid = 1;
    }
    s_probeRaceLap = lap;
    s_probeRaceFinished = raceFinished;
    s_probeRaceFinishPosition = finishPosition;
    s_probeRaceRacerIndex = racerIndex;
    s_probeRacePlayerIndex = playerIndex;
    /* The race announcer reads the same publication, one tick behind, and
     * writes nothing but text. Inert unless the player has speech on. */
    mdkr_a11y_race_publish(racePosition, racerCount, lap, lapCount,
                           raceFinished, finishPosition, itemQuantity,
                           itemType);
}
/* Time-trial ghost playback: a count of interpolated ghost frames, plus which
 * ghost bank the last one came from (0/1 = the player's own recorded ghost,
 * 2 = a staff ghost). Published from timetrial_ghost_read().
 *
 * This exists so a check can assert that its route ACTUALLY REACHES the ghost
 * path, instead of assuming it. The 3-vs-4 control-point overflow in
 * timetrial_ghost_read only ever showed up because a fixture happened to re-enter
 * the level after a finish; a route that quietly stopped doing that would still
 * pass every other assertion while covering nothing. Cheap: two stores per ghost
 * frame, and the counter is read only when the trace is on. */
static int s_probeGhostReads = 0, s_probeGhostBank = -1;
void mdkr_pace_probe_ghost(int ghostBank) {
    s_probeGhostReads++; s_probeGhostBank = ghostBank;
}

/* ---- Ground-contact probe (published from racer.c) ------------------------ *
 * `[GRND] frame=N gw=K surf=a,b,c,d` -- how many of the player's four wheels
 * found a collision surface this frame, and which surface each one landed on
 * (-1 == SURFACE_NONE, i.e. that wheel is over nothing).
 *
 * Why this is a separate line and not part of [PACE]: `gw == 0` for a sustained
 * stretch is the ONLY direct evidence that a racer is falling because the level
 * stopped existing under it, as opposed to jumping, being launched, or driving
 * off a real edge. Position alone cannot tell those apart -- a smooth
 * gravity-shaped descent looks identical either way -- and that ambiguity is
 * exactly what made the volcano fall look like a "tilt never recovers" bug.
 * Emitted only when MDKR_TRACE >= 1, for player 1, one line per frame. */
void mdkr_ground_probe(int playerIndex, int racerIndex, int groundedWheels, int s0, int s1, int s2, int s3,
                       int xrot, int zrot) {
    if (!mdkr_trace_enabled()) return;
    /* xrot/zrot are the racer object's pitch and roll -- the "tilt" a player
     * sees. They are derived from where the four wheels touched down, so they
     * are the observable for "it tilts and never tilts back": with the ground
     * missing there is nothing to restore them from.
     *
     * EVERY racer is traced, not just the human, and that is load-bearing: the
     * reporter's decisive observation was that the AI boss falls through at the
     * same place even when the player does not. A racer-indexed probe is what
     * makes "no racer loses the ground" assertable, rather than "player 1 did
     * not happen to drive over the bad spot". `ri` is racer->racerIndex, `pi`
     * the playerIndex (PLAYER_COMPUTER == 4). */
    mdkr_trace("[GRND] frame=%d pi=%d ri=%d gw=%d surf=%d,%d,%d,%d xrot=%d zrot=%d",
               g_frameCounter, playerIndex, racerIndex, groundedWheels, s0, s1, s2, s3, xrot, zrot);
}

/* One cooperative frame: pump events, present, advance/headless-exit. Called
 * from osRecvMesg when the game blocks on the (empty) video client queue.
 *
 * `swap` is false for a present that produced NO NEW IMAGE — the presentation
 * subloop's no-draw paths (stubs_dkr.c): motion smoothing off, or a pass that
 * submitted no graphics task so the held display list is stale. See
 * platform_frame_sync_no_swap() below for why that distinction has to exist.
 */
static void platform_frame_sync_impl(int swap, int count_present) {
    int renderer_recovered = 0;
    platform_input_pump();   /* pump events + build pad state before retrace */
    if (platform_exit_requested()) {
        return;
    }
    /*
     * Resource creation can fail while init_game() or another CPU-side load
     * path is preparing materials, before the first graphics task is submitted.
     * The scheduler boundary catches failures raised during a graphics task;
     * this cooperative retrace boundary catches the complementary pre-task
     * window so simulation can never keep advancing with a latched-dead
     * renderer merely because no display list has reached the scheduler yet.
     */
    if (gfx_renderer_failed()) {
#if defined(MDKR_WEBGPU_BACKEND) && !defined(__EMSCRIPTEN__)
        if (mdkr_render_backend() == MDKR_BACKEND_WEBGPU &&
            gfx_webgpu_runtime_recovery_pending()) {
            if (gfx_webgpu_recover_device()) {
                gfx_reset_renderer_caches();
                gfx_set_dimensions(gfx_output_dimensions.width,
                                   gfx_output_dimensions.height);
                renderer_recovered = 1;
                fprintf(stderr,
                        "[webgpu] native device recovery succeeded; gameplay "
                        "continues\n");
            } else {
                fprintf(stderr,
                        "[webgpu] recovery failed; terminating cleanly without "
                        "an automatic OpenGL fallback\n");
                platform_request_exit(EXIT_FAILURE);
                return;
            }
        } else
#endif
        {
            fprintf(stderr,
                    "[mdkr64] renderer entered an unrecoverable state during "
                    "resource preparation; stopping at the retrace boundary.\n");
            fflush(stderr);
            platform_request_exit(EXIT_FAILURE);
            return;
        }
    }
    if (!renderer_recovered && count_present) {
        platform_dump_frame();   /* read the rendered backbuffer before the swap */
        if (swap) {
            platform_sdl_present();
            /* After the swap returns, so the block the presentation queue
             * imposes is inside the measured term rather than after it. */
            if (input_latency_census_enabled()) {
                input_latency_census_note_present(pace_host_ns());
            }
        }
    }
    /* Publish a resize before the game builds its next display list. Camera FOV,
     * renderer viewports and WebGPU targets therefore change together (no
     * one-frame projection/viewport mismatch). */
    platform_sdl_sync_drawable_size();
    if (!count_present) {
        return;
    }
    g_frameCounter++;
    MDKR_TRACE("frame %d presented", g_frameCounter);
    {
        extern void mdkr_oracle_trace_racers(int frame);
        /* Tick-indexed for the same reason as the input script above: an oracle
         * row must stay comparable against a run at a different present rate. */
        mdkr_oracle_trace_racers(g_simTickCounter);
    }
#ifdef __EMSCRIPTEN__
    /* Publish actual canvas updates, not scheduler opportunities. Numeric caps
     * above the authored cadence can yield through rAF without a new image. */
    EM_ASM({ if (typeof Module !== 'undefined') { Module.__mdkrFrames = $0; } },
           (double)g_surfaceFrameCounter);
#endif

    /* Boss-progress observation (MDKR_WATCH_COURSEFLAGS / MDKR_BOSS_PRECLEARED).
     * Polled here rather than hooked into each writer so that a write through a
     * WRONG index is caught too -- see platform/mdkr_adventure.c. Two integer
     * compares and a return when neither variable is set. */
    {
        extern void mdkr_boss_state_probe(void);
        mdkr_boss_state_probe();
    }

    /* Pacing/motion trace (MDKR_TRACE>=1). Greppable "[PACE]" line: the
     * updateRate the game will see (R), the true wall/synthetic field count
     * (wf) and its cumulative (cumwf, a 60 Hz wall-clock proxy), plus the
     * player-1 racer world position + race clock so an automated run can
     * compare position-advance to clock-advance and to wall time. */
    if (mdkr_trace_enabled()) {
        static uint64_t s_prevPresentNs = 0;
        uint64_t nowp = pace_host_ns();
        double dtms = s_prevPresentNs ? (double)(nowp - s_prevPresentNs) / 1e6 : 0.0;
        s_prevPresentNs = nowp;
        if (s_probeValid) {
            mdkr_trace("[PACE] frame=%d R=%d wf=%d cumwf=%llu dtms=%.2f | racer x=%.9g y=%.9g z=%.9g clock=%d cp=%d lap=%d rlap=%d fin=%d ghost=%d gbank=%d fpos=%d ridx=%d pidx=%d",
                       g_frameCounter, g_viLastFields, g_viLastWallFields,
                       (unsigned long long)s_paceCumWall, dtms,
                       s_probeX, s_probeY, s_probeZ, s_probeClock,
                       s_probeCheckpoint, s_probeLap,
                       s_probeRaceLap, s_probeRaceFinished,
                       s_probeGhostReads, s_probeGhostBank,
                       s_probeRaceFinishPosition, s_probeRaceRacerIndex,
                       s_probeRacePlayerIndex);
            /* Additional humans stay on their own greppable lines so existing
             * P1 parsers cannot accidentally consume them. The player-two line
             * remains byte-for-byte the historical format. */
            for (int player = 1; player < MDKR_PROBE_PLAYERS; player++) {
                if (!s_probe2Valid[player]) {
                    continue;
                }
                mdkr_trace("[PACE%d] frame=%d | racer%d x=%.9g y=%.9g z=%.9g clock=%d cp=%d lap=%d",
                           player + 1, g_frameCounter, player + 1,
                           s_probe2X[player], s_probe2Y[player], s_probe2Z[player],
                           s_probe2Clock[player], s_probe2Cp[player], s_probe2Lap[player]);
            }
        } else {
            mdkr_trace("[PACE] frame=%d R=%d wf=%d cumwf=%llu dtms=%.2f",
                       g_frameCounter, g_viLastFields, g_viLastWallFields,
                       (unsigned long long)s_paceCumWall, dtms);
        }
    }

    if (g_headlessFrames >= 0 && g_frameCounter >= g_headlessFrames) {
        /* Texture-decode path counters, so a headless check can confirm the route
         * it drove actually exercised them (see gfx_pc_dkr.h). */
        printf("[TEX] lineSwappedUploads=%u\n", gfx_dkr_texload_line_swapped);
        printf(
            "[TEXCACHE] staleHits=%u created=%llu deleted=%llu live=%u "
            "high=%u shaders=%llu\n",
            gfx_dkr_texcache_stale_hits,
            (unsigned long long)gfx_dkr_texture_ids_created,
            (unsigned long long)gfx_dkr_texture_ids_deleted,
            gfx_dkr_texture_ids_live,
            gfx_dkr_texture_ids_high_water,
            (unsigned long long)gfx_dkr_shader_programs_created);
        printf(
            "[PTRREG] live=%u high=%u ambiguous=%u fullFails=%u "
            "maxProbe=%u\n",
            gfx_ptr_live, gfx_ptr_high_water, gfx_ptr_ambiguous,
            gfx_ptr_full_fails, gfx_ptr_max_probe);
        printf("[FONT] sdfUploads=%u outlineUploads=%u registryFailures=%u "
               "clippedTexels=%u\n",
               gfx_dkr_font_sdf_uploads,
               gfx_dkr_font_outline_uploads,
               gfx_dkr_font_registry_failures,
               gfx_font_outline_clipped_texels);
        printf("[MIP] uploads=%u levels=%llu\n",
               gfx_dkr_mipmapped_uploads,
               (unsigned long long) gfx_dkr_mip_levels_uploaded);
        printf("[RL1] arm=%s triangles=%llu\n",
               gfx_dkr_rl1_active_arm_name(),
               (unsigned long long) gfx_dkr_rl1_triangles);
        {
            const GfxLevelLightingRig *rig =
                gfx_level_lighting_current();
            printf(
                "[LIGHT] arm=%s valid=%d world=%d geometry=%d "
                "dir=%.4f,%.4f,%.4f colour=%.4f,%.4f,%.4f "
                "strength=%.4f sources=0x%x racerTris=%llu "
                "characterTris=%llu missingNormals=%llu "
                "space=srgb-authored/linear-light/srgb-output\n",
                gfx_dkr_rl5_active_arm_name(),
                rig != NULL && rig->valid,
                rig != NULL ? rig->world_id : -1,
                rig != NULL ? rig->geometry_id : -1,
                rig != NULL ? (double)rig->direction_world[0] : 0.0,
                rig != NULL ? (double)rig->direction_world[1] : 1.0,
                rig != NULL ? (double)rig->direction_world[2] : 0.0,
                rig != NULL ? (double)rig->colour_linear[0] : 1.0,
                rig != NULL ? (double)rig->colour_linear[1] : 1.0,
                rig != NULL ? (double)rig->colour_linear[2] : 1.0,
                rig != NULL ? (double)rig->strength : 0.0,
                rig != NULL ? (unsigned)rig->source_mask : 0u,
                (unsigned long long)
                    gfx_dkr_remaster_racer_triangles,
                (unsigned long long)
                    gfx_dkr_remaster_character_triangles,
                (unsigned long long)
                    gfx_dkr_remaster_missing_normal_batches);
            printf(
                "[GRADE] valid=%d sat=%.5f contrast=%.5f "
                "tint=%.5f,%.5f,%.5f "
                "space=srgb-input/linear-grade/srgb-output\n",
                rig != NULL && rig->grade_valid,
                rig != NULL ? (double)rig->grade_saturation : 1.0,
                rig != NULL ? (double)rig->grade_contrast : 1.0,
                rig != NULL ? (double)rig->grade_tint[0] : 1.0,
                rig != NULL ? (double)rig->grade_tint[1] : 1.0,
                rig != NULL ? (double)rig->grade_tint[2] : 1.0);
        }
        if (gfx_world_fx_trace_enabled()) {
            GfxWorldFxStats stats;
            GfxShadowPlan plan;
            bool plan_valid;
            gfx_world_fx_get_stats(&stats);
            printf(
                "[WORLD-FX] mode=neutral frames=%llu committed=%llu "
                "failed=%llu views=%zu triangles=%zu ranges=%zu "
                "static=%zu dynamic=%zu cache=%llu/%llu "
                "opaque=%llu masked=%llu matrices=%zu matrixPeak=%zu "
                "lookup=%llu/%llu allocFails=%llu "
                "implausible=%llu excluded=%llu "
                "staleCasters=%llu staleWorst=%.1f\n",
                (unsigned long long)stats.frames_begun,
                (unsigned long long)stats.frames_committed,
                (unsigned long long)stats.frames_failed,
                stats.current_views,
                stats.current_triangles,
                stats.current_ranges,
                stats.current_static_triangles,
                stats.current_dynamic_triangles,
                (unsigned long long)stats.static_cache_hits,
                (unsigned long long)stats.static_cache_misses,
                (unsigned long long)stats.opaque_triangles,
                (unsigned long long)stats.masked_triangles,
                stats.matrix_entries,
                stats.matrix_peak,
                (unsigned long long)stats.matrix_lookup_hits,
                (unsigned long long)stats.matrix_lookup_misses,
                (unsigned long long)stats.allocation_failures,
                (unsigned long long)stats.implausible_triangles,
                (unsigned long long)stats.excluded_triangles,
                (unsigned long long)stats.stale_casters,
                (double)stats.stale_worst_delta);
            plan_valid = gfx_shadow_build_plan(
                gfx_shadow_frame_previous(),
                g_pc_sun_dir_world,
                &plan);
            printf(
                "[SHADOW-PLAN] valid=%d views=%zu maps=%zu res=%u "
                "bytes=%zu nearTexel=%.6f farTexel=%.6f zSpan=%.1f "
                "invalidWorldRecv=%llu\n",
                plan_valid ? 1 : 0,
                plan.view_count,
                plan.cascade_count,
                plan.budget.resolution,
                plan.budget.depth_bytes,
                plan_valid
                    ? (double)plan.cascades[0].world_units_per_texel
                    : 0.0,
                plan_valid
                    ? (double)plan.cascades[plan.cascade_count - 1]
                        .world_units_per_texel
                    : 0.0,
                plan_valid
                    ? (double)(plan.cascades[0].light_bounds_max[2] -
                               plan.cascades[0].light_bounds_min[2])
                    : 0.0,
                (unsigned long long)
                    gfx_dkr_shadow_receiver_invalid_world);
        }
        /* Collision-candidate saturation. A nonzero `truncated` means
         * generate_collision_candidates() threw terrain away, which is how a
         * racer ends up standing on nothing -- see the comment on the Z-row test
         * in game/src/hasm/collision.c. */
        {
            extern int  mdkr_coll_max_candidates(void);
            extern long mdkr_coll_truncations(void);
            extern int  mdkr_coll_cap(int romCap);
            /* `cap` is the EFFECTIVE cap (MDKR_COLLCAP can lower it, or set
             * `legacy` to remove the guards); maxCandidates is the write index
             * high-water mark, so maxCandidates > cap means the cap was stepped
             * over -- which is the whole assertion. */
            extern long mdkr_coll_canary_trips(void);
            extern int  mdkr_coll_canary_armed(void);
            /* `canary` is the MDKR_COLLALLOC control: -1 when unarmed (the
             * default build allocates exactly 500 and has no canary slot), and
             * otherwise the number of generate_collision_candidates() calls that
             * wrote the slot at index == cap. That slot is the FIRST one a
             * missing or mis-ordered bounds guard touches and one no correct path
             * can reach, so a nonzero count is a guard regression -- the evidence
             * MDKR_COLLCAP alone cannot produce, because it leaves the boundary
             * inside a full-size allocation. */
            printf("[COLL] maxCandidates=%d truncated=%ld cap=%d canary=%ld\n",
                   mdkr_coll_max_candidates(), mdkr_coll_truncations(),
                   mdkr_coll_cap(500),
                   mdkr_coll_canary_armed() ? mdkr_coll_canary_trips() : -1L);
        }
        /* Segment-overlap list high-water marks. These two lists are written
         * through a bare pointer by get_inside_segment_count_x[y]z(), so the
         * bound parameter is the only instrument that can see them overflow --
         * no sanitizer can (see stubs_dkr.c). `clamped` must stay 0. */
        {
            /* Each denominator is the SMALLEST capacity that site was handed at
             * run time, not a literal repeated here -- so it is evidence the
             * caller's ARRAY_COUNT reached the callee. Expected on a full race
             * route: 8 / 28 / 8 / 64 / 300. */
            printf("[SEGS] xzMax=%d/%d slack=%d clamped=%ld  xyzMax=%d/%d slack=%d "
                   "clamped=%ld  colYMax=%d/%d slack=%d clamped=%ld  shTriMax=%d/%d "
                   "slack=%d clamped=%ld  shHgtMax=%d/%d slack=%d clamped=%ld\n",
                   mdkr_bound_max(0), mdkr_bound_min(0), mdkr_bound_slack(0),
                   mdkr_bound_clamped(0),
                   mdkr_bound_max(1), mdkr_bound_min(1), mdkr_bound_slack(1),
                   mdkr_bound_clamped(1),
                   mdkr_bound_max(2), mdkr_bound_min(2), mdkr_bound_slack(2),
                   mdkr_bound_clamped(2),
                   mdkr_bound_max(3), mdkr_bound_min(3), mdkr_bound_slack(3),
                   mdkr_bound_clamped(3),
                   mdkr_bound_max(4), mdkr_bound_min(4), mdkr_bound_slack(4),
                   mdkr_bound_clamped(4));
        }
        {
            int dataPeak, triPeak, vtxPeak;
            int overflowDrops, emptyMeshes, drawGroups, nonDecalDrawGroups;
            int dataCap, triCap, vtxCap;
            mdkr_shadow_stats(
                &dataPeak, &triPeak, &vtxPeak,
                &overflowDrops, &emptyMeshes,
                &drawGroups, &nonDecalDrawGroups,
                &dataCap, &triCap, &vtxCap);
            printf("[SHADOW] decal=%s dataPeak=%d/%d triPeak=%d/%d vtxPeak=%d/%d "
                   "overflowDrops=%d emptyMeshes=%d drawGroups=%d nonDecal=%d\n",
                   mdkr_shadow_decal_enabled() ? "on" : "off",
                   dataPeak, dataCap, triPeak, triCap, vtxPeak, vtxCap,
                   overflowDrops, emptyMeshes, drawGroups, nonDecalDrawGroups);
        }
        /*
         * Unlike [SHADOW], which records the game-side request, [DEPTH] samples
         * the final decoded RDP state at triangle emission. A production versus
         * MDKR_SHADOW_DECAL=0 A/B must therefore separate here too.
         */
        printf("[DEPTH] decalTriangles=%llu comparedTriangles=%llu\n",
               (unsigned long long)gfx_dkr_decal_triangles,
               (unsigned long long)gfx_dkr_depth_compared_triangles);
        printf("[SDL] headless: reached %d frames, exiting cleanly.\n", g_frameCounter);
        /* Requesting exit is a liveness signal, not teardown. Proof of final
         * teardown is main()'s __mdkrShutdownComplete, published only after
         * every owner has released its resources. */
        platform_request_exit(0);
        return;
    }
}

void platform_frame_sync(void) {
    /* This opportunity hands an image to the presentation queue, so the queue
     * is what paces the next one; the shed floor stands down. */
    s_presentLastHeld = false;
    platform_frame_sync_impl(1, 1);
}

/*
 * A frame boundary that does everything platform_frame_sync does EXCEPT the
 * buffer swap, for the presentation subloop.
 *
 * An extra host opportunity can still be a no-draw path: motion smoothing may
 * be off, an immutable replay may fail closed, or GPU admission may shed the
 * optional image. The prior complete image remains on the front/surface; there
 * is no duplicate swap. These paths used to call platform_frame_sync(), which
 * swaps.
 *
 * On a double-buffered GL context (platform_sdl_present -> SDL_GL_SwapWindow)
 * the contents of the back buffer AFTER a swap are undefined. So a present with
 * nothing newly drawn into it does not repeat the tick's image at all -- it puts
 * whatever the driver left in that buffer on screen, which at
 * FrameLimit=60/MotionSmoothing=off is every other frame: visible flicker, and
 * the same hazard on any tick whose graphics task was skipped even with
 * smoothing on. (WebGPU is unaffected -- wgpu_end_frame already presented the
 * surface inside gfx_end_frame and platform_sdl_present is a no-op there --
 * which is part of why this went unnoticed.)
 *
 * NOT swapping is the correct hold: the front buffer already contains the
 * authored image and keeps being scanned out. FrameLimit=60 therefore updates the
 * screen at TICK rate when smoothing is off, not at 60. That is the honest
 * consequence of asking for a rate with nothing new to show at it.
 *
 * Everything else platform_frame_sync does still happens, deliberately: the
 * input pump, the renderer-failure check, the frame counter, the frame dump and
 * the traces. In particular the dump reads the BACKEND's captured frame (see
 * platform_dump_frame / gfx_read_framebuffer_rgb), not the swapchain, so it
 * still records the image actually on screen -- which is exactly what
 * check_presentation_matrix.py's arm C positive control asserts.
 *
 * Pacing is unaffected: the subloop already paced this present at the top of
 * its loop (platform_vi_present_pace), so skipping the swap cannot busy-spin,
 * and in the browser that pacing call is the Asyncify rAF yield.
 */
void platform_frame_sync_no_swap(void) {
    /* Nothing was queued, so nothing will block the next opportunity. The
     * pacer's shed floor reads this (platform_vi_present_pace_units). */
    s_presentLastHeld = true;
    platform_frame_sync_impl(0, 1);
}

void platform_frame_service(void) {
    platform_frame_sync_impl(0, 0);
}

void platform_headless_tick_complete(int tick_count) {
    if (g_headlessTicks >= 0 && tick_count >= g_headlessTicks &&
        !platform_exit_requested()) {
        printf("[SDL] headless: reached %d simulation ticks, exiting cleanly.\n",
               tick_count);
        platform_request_exit(0);
    }
}

/* ---- Pad accessors (read by osContGetReadData in stubs_dkr.c) ------------ */
int platform_pad_present(int port) {
    if (port < 0 || port >= DKR_MAXPADS) return 0;
    /*
     * The keyboard is a complete P1 controller, so port zero remains present
     * even without a physical gamepad. Script presence is based on the whole
     * route rather than only the active frame: a P2 join script must look like
     * a plugged-in controller from boot until shutdown, exactly like hardware.
     */
    if (port == 0) return 1;
    return s_pads[port].present ||
           (s_scriptPresentMask & (1u << port)) != 0;
}
int platform_pad_rumble_supported(int port) {
    if (port < 0 || port >= DKR_MAXPADS || s_gc[port] == NULL) return 0;
    if (s_rumbleSupported[port] >= 0) return s_rumbleSupported[port];
#ifdef __EMSCRIPTEN__
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(s_gc[port]);
    if (!joystick) return 0;
    s_rumbleSupported[port] = (int8_t)browser_gamepad_rumble_supported(
        SDL_JoystickInstanceID(joystick));
#elif SDL_VERSION_ATLEAST(2, 0, 18)
    s_rumbleSupported[port] =
        SDL_GameControllerHasRumble(s_gc[port]) == SDL_TRUE;
#elif SDL_VERSION_ATLEAST(2, 0, 9)
    /* Older SDL has no read-only capability query. Perform its zero-duration,
     * zero-strength compatibility probe exactly once per opened controller. */
    s_rumbleSupported[port] =
        SDL_GameControllerRumble(s_gc[port], 0, 0, 0) == 0;
#else
    s_rumbleSupported[port] = 0;
#endif
    return s_rumbleSupported[port];
}
static int platform_pad_rumble_send(int port, int enabled) {
    uint16_t strength;
    if (port < 0 || port >= DKR_MAXPADS || s_gc[port] == NULL) return 0;
    strength = mdkr_controller_rumble_output_strength(
        mdkr_video_config_current(), enabled);
#ifdef __EMSCRIPTEN__
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(s_gc[port]);
    if (!joystick) return 0;
    return browser_gamepad_rumble(
        SDL_JoystickInstanceID(joystick), (int)strength);
#elif SDL_VERSION_ATLEAST(2, 0, 9)
    return SDL_GameControllerRumble(
               s_gc[port], strength, strength,
               enabled ? 60000u : 0u) == 0;
#else
    (void)enabled;
    return 0;
#endif
}

int platform_pad_rumble(int port, int enabled) {
    int audible;
    if (port < 0 || port >= DKR_MAXPADS || s_gc[port] == NULL) return 0;
    s_rumbleRequested[port] = enabled != 0;
    audible = enabled &&
        mdkr_controller_rumble_enabled(mdkr_video_config_current());
    /* A muted start is still a successful Rumble Pak motor operation: the
     * physical capability remains truthful while the host-output layer emits
     * silence. Stops always reach SDL so a previously active motor cannot run
     * out its 60-second safety duration after the preference changes. */
    return platform_pad_rumble_send(port, audible);
}

void platform_pad_rumble_preferences_changed(void) {
    const int allowed =
        mdkr_controller_rumble_enabled(mdkr_video_config_current());
    for (int port = 0; port < DKR_MAXPADS; port++) {
        if (s_gc[port] != NULL) {
            (void)platform_pad_rumble_send(
                port, allowed && s_rumbleRequested[port]);
        }
    }
}
unsigned int platform_pad_buttons(int port) {
    if (port < 0 || port >= DKR_MAXPADS) return 0;
    return s_pads[port].buttons;
}
void platform_pad_stick(int port, int *sx, int *sy) {
    if (port < 0 || port >= DKR_MAXPADS) { if (sx) *sx = 0; if (sy) *sy = 0; return; }
    if (sx) *sx = s_pads[port].stick_x;
    if (sy) *sy = s_pads[port].stick_y;
}
