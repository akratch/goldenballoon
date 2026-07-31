/**
 * platform_sdl_min.c — the SDL2 host layer: window/context lifecycle, controller
 * and keyboard input, the cooperative frame boundary, and VI pacing.
 *
 * Responsibilities:
 *   - Create the window for whichever backend is active (GL 3.3 core via glad,
 *     or a Metal view / browser canvas for WebGPU) and tear it down cleanly,
 *     including the GL fallback path when WebGPU device creation fails.
 *   - Open game controllers, load SDL mappings, and drive the deterministic
 *     input-script fixtures the regression checks replay.
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
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdint.h>
#include <limits.h>
#include <time.h>
#ifndef __EMSCRIPTEN__
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
    await new Promise((resolve) => requestAnimationFrame(resolve));
});
#endif
#include <SDL.h>
#include "platform_os.h"
#include "pacing_policy.h"
#include "present_sched.h"
#include "video_config.h"
#include "mdkr_bounds.h"
#include "gfx_ptr.h"
#ifdef MDKR_APP
#include "host_window.h"          /* app-shell window/device handoff */
#include "app_overlay_hooks.h"    /* in-game overlay event/render hooks */
#endif
#include "fast3d/gfx_pc_dkr.h"   /* gfx_dkr_texload_line_swapped (headless report) */
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
int g_frameCounter   = 0;
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
#ifdef MDKR_WEBGPU_BACKEND
static SDL_MetalView s_metalView = NULL;   /* CAMetalLayer host for the WGPUSurface */
#endif
#ifdef MDKR_APP
/* The app shell owns the window; the engine must not destroy it at shutdown or
 * the launcher's ImGui context is left pointing at freed memory. */
static int           s_windowAdopted = 0;
#endif

/* ---- Render backend selection (M4.5) ------------------------------------ *
 * Resolved once from MDKR_RENDERER (webgpu|gl|metal), default webgpu. Validated
 * against the backends actually compiled in: webgpu needs MDKR_WEBGPU_BACKEND;
 * metal is not built in mdkr64 (gfx_metal.mm is Obj-C++, excluded), so it falls
 * back to GL. GL is always the guaranteed fallback. The choice drives BOTH the
 * window/context kind (platform_sdl_init) and the &gfx_*_api gfx_init receives
 * (main_pc), so it must be a single cached source of truth. */
int mdkr_render_backend(void) {
    if (s_renderBackend >= 0) return s_renderBackend;
    const char *e = getenv("MDKR_RENDERER");
    int want = MDKR_BACKEND_WEBGPU;   /* default: webgpu */
    if (e && e[0]) {
        if      (!strcasecmp(e, "gl") || !strcasecmp(e, "opengl")) want = MDKR_BACKEND_GL;
        else if (!strcasecmp(e, "webgpu") || !strcasecmp(e, "wgpu")) want = MDKR_BACKEND_WEBGPU;
        else if (!strcasecmp(e, "metal")) want = MDKR_BACKEND_METAL;
        else {
            fprintf(stderr, "[mdkr64] MDKR_RENDERER='%s' unrecognized "
                            "(webgpu|gl|metal); using webgpu.\n", e);
            want = MDKR_BACKEND_WEBGPU;
        }
    }
#ifndef MDKR_WEBGPU_BACKEND
    if (want == MDKR_BACKEND_WEBGPU) {
        fprintf(stderr, "[mdkr64] WebGPU backend not compiled in "
                        "(MDKR_WEBGPU_BACKEND=OFF); falling back to GL.\n");
        want = MDKR_BACKEND_GL;
    }
#endif
    if (want == MDKR_BACKEND_METAL) {
        /* Native Metal backend (gfx_metal.mm) is not compiled in mdkr64. */
        fprintf(stderr, "[mdkr64] Metal backend not available in mdkr64; "
                        "use webgpu (Metal-backed via wgpu-native) or gl. "
                        "Falling back to GL.\n");
        want = MDKR_BACKEND_GL;
    }
    s_renderBackend = want;
    return s_renderBackend;
}

static void mdkr_render_backend_force_gl(void) {
    s_renderBackend = MDKR_BACKEND_GL;
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
 *  The event pump (platform_input_pump) runs at the top of platform_frame_sync,
 *  i.e. BEFORE the synthesized retrace message, so the game reads fresh pad
 *  state on the frame it wakes for. osContGetReadData (stubs_dkr.c) copies from
 *  s_pads[] via platform_pad_buttons/platform_pad_stick.
 *
 *  N64 controller button bits — MUST match game/include/PR/os_cont.h CONT_*.
 * ======================================================================== */
#define N64_A       0x8000u
#define N64_B       0x4000u
#define N64_Z       0x2000u   /* Z-trigger (CONT_G) */
#define N64_START   0x1000u
#define N64_DU      0x0800u   /* D-pad up   (CONT_UP)    */
#define N64_DD      0x0400u   /* D-pad down (CONT_DOWN)  */
#define N64_DL      0x0200u   /* D-pad left (CONT_LEFT)  */
#define N64_DR      0x0100u   /* D-pad right(CONT_RIGHT) */
#define N64_L       0x0020u   /* L shoulder (CONT_L) */
#define N64_R       0x0010u   /* R shoulder (CONT_R) */
#define N64_CU      0x0008u   /* C-up    (CONT_E) */
#define N64_CD      0x0004u   /* C-down  (CONT_D) */
#define N64_CL      0x0002u   /* C-left  (CONT_C) */
#define N64_CR      0x0001u   /* C-right (CONT_F) */

#define DKR_MAXPADS 4
#define STICK_FULL  80        /* menu deflection (spec: full ±80) */

struct pad_state { unsigned int buttons; int stick_x, stick_y; };
static struct pad_state s_pads[DKR_MAXPADS];

static SDL_GameController *s_gc[DKR_MAXPADS] = { NULL, NULL, NULL, NULL };
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
EM_JS(int, browser_gamepad_rumble, (int instanceId, int enabled), {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    const pad = pads && pads[instanceId];
    if (!pad) return 0;
    const actuator = pad.vibrationActuator ||
        (pad.hapticActuators && pad.hapticActuators[0]);
    if (!actuator) return 0;
    try {
        if (!enabled) {
            if (typeof actuator.reset === "function") actuator.reset();
            else if (typeof actuator.playEffect === "function") {
                actuator.playEffect(actuator.type || "dual-rumble", {
                    duration: 0, startDelay: 0,
                    strongMagnitude: 0, weakMagnitude: 0
                });
            }
            return 1;
        }
        if (typeof actuator.playEffect === "function") {
            actuator.playEffect(actuator.type || "dual-rumble", {
                duration: 60000, startDelay: 0,
                strongMagnitude: 1, weakMagnitude: 1
            }).catch(() => {});
            return 1;
        }
        if (typeof actuator.pulse === "function") {
            actuator.pulse(1, 60000).catch(() => {});
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
      (unsigned int *buttons, int *stickX, int *stickY), {
    const pad = (typeof Module !== "undefined") && Module.__mdkrTouchPad;
    const active = pad && pad.enabled !== false;
    HEAPU32[buttons >> 2] = active ? (Number(pad.buttons) >>> 0) : 0;
    HEAP32[stickX >> 2] = active ? (Number(pad.stickX) | 0) : 0;
    HEAP32[stickY >> 2] = active ? (Number(pad.stickY) | 0) : 0;
});
#endif

/* ---- Scripted input (--input-script FILE) ------------------------------- *
 * Lines: `frame TOKEN[+TOKEN...] [holdFrames] [P1..P4]`. Injected into the named
 * controller port for [frame, frame+hold) present-frames; the port field is
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
    FILE *f = fopen(path, "r");
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
static int sdl_init_gl(Uint32 base_flags) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    s_window = SDL_CreateWindow("mdkr64 — Diddy Kong Racing (native)",
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
    SDL_GL_SetSwapInterval(g_headlessFrames >= 0 ? 0 : 1);
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

int platform_sdl_fallback_to_gl(void) {
#ifdef __EMSCRIPTEN__
    return -1;
#else
    Uint32 base_flags;
    if (!s_sdlReady) {
        return -1;
    }
#ifdef MDKR_WEBGPU_BACKEND
    /*
     * The WebGPU surface refers to this native window/CAMetalLayer. Unconfigure
     * and detach it before either host object is destroyed.
     */
    gfx_webgpu_prepare_gl_fallback();
#endif
#if defined(MDKR_WEBGPU_BACKEND) && defined(__APPLE__)
    if (s_metalView) {
        SDL_Metal_DestroyView(s_metalView);
        s_metalView = NULL;
    }
#endif
    if (s_glctx) {
        SDL_GL_DeleteContext(s_glctx);
        s_glctx = NULL;
    }
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
    g_sdlWindow = NULL;
    s_glReady = 0;
    mdkr_render_backend_force_gl();
    base_flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    if (sdl_should_hide_window()) {
        base_flags |= SDL_WINDOW_HIDDEN;
    }
    return sdl_init_gl(base_flags);
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
            /* GL host: the shell's context is already current and glad is
             * already loaded in this process, so the GL path is live. */
            SDL_GL_MakeCurrent(s_window, s_glctx);
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
    s_window = SDL_CreateWindow("mdkr64", SDL_WINDOWPOS_CENTERED,
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
            /* Test-only fault injection for the window-kind fallback. A real
             * SDL failure reaches the same branch; no-op unless explicitly set. */
            s_window = NULL;
            fprintf(stderr, "[SDL] WebGPU window failure forced for fallback test.\n");
        } else {
            s_window = SDL_CreateWindow("mdkr64 — Diddy Kong Racing (native, WebGPU)",
                                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        s_initialWindowWidth, s_initialWindowHeight,
                                        flags);
        }
        if (!s_window) {
            const char *reason =
                (forceWindowFailure != NULL && forceWindowFailure[0] == '1')
                    ? "forced by MDKR_TEST_WEBGPU_WINDOW_FAIL"
                    : SDL_GetError();
            fprintf(stderr, "[SDL] WebGPU window creation failed: %s - "
                            "falling back to GL.\n", reason);
            mdkr_render_backend_force_gl();
            return sdl_init_gl(base_flags);
        }
        g_sdlWindow = s_window;
#ifdef __APPLE__
        s_metalView = SDL_Metal_CreateView(s_window);
        if (!s_metalView) {
            fprintf(stderr, "[SDL] Metal view creation failed: %s - "
                            "falling back to GL.\n", SDL_GetError());
            SDL_DestroyWindow(s_window);
            s_window = NULL; g_sdlWindow = NULL;
            mdkr_render_backend_force_gl();
            return sdl_init_gl(base_flags);
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
static void platform_dump_frame(void) {
    if (!g_dumpFramesDir) {
        return;
    }
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
    /* Optional: only dump frames >= MDKR_DUMP_FROM (keeps late-scene captures
     * tractable without writing thousands of early PPMs), and only every
     * MDKR_DUMP_EVERY-th frame after that. The stride is what makes a "does the
     * scene still render at the END of a long drive?" check affordable in a
     * single pass — sampling ~10 frames over 6000 instead of writing 6000 PPMs
     * (see tests/check_race_drive.sh). */
    {
        static int dumpFrom = -2;
        static int dumpEvery = 1;
        if (dumpFrom == -2) {
            const char *e = getenv("MDKR_DUMP_FROM");
            const char *n = getenv("MDKR_DUMP_EVERY");
            dumpFrom = (e && e[0]) ? atoi(e) : -1;
            if (n && n[0]) {
                dumpEvery = atoi(n);
            }
            if (dumpEvery < 1) {
                dumpEvery = 1;
            }
        }
        if (dumpFrom >= 0 && g_frameCounter < dumpFrom) {
            return;
        }
        if (dumpEvery > 1) {
            int base = dumpFrom > 0 ? dumpFrom : 0;
            if (((g_frameCounter - base) % dumpEvery) != 0) {
                return;
            }
        }
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
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) {
            fwrite(pix + (size_t) y * rowBytes, 1, rowBytes, f);
        }
        fclose(f);
    }
    free(pix);
}

/* ---- Game controller open/close ----------------------------------------- */
static void gc_try_open(int deviceIndex) {
    if (!SDL_IsGameController(deviceIndex)) return;
    for (int i = 0; i < DKR_MAXPADS; i++) {
        if (!s_gc[i]) {
            s_gc[i] = SDL_GameControllerOpen(deviceIndex);
            if (s_gc[i]) {
                printf("[SDL] gamepad P%d: %s\n", i + 1,
                       SDL_GameControllerName(s_gc[i]));
            }
            return;
        }
    }
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
                printf("[SDL] gamepad P%d disconnected\n", i + 1);
                return;
            }
        }
    }
}

void platform_input_init(void) {
    /* Layered on top of SDL's built-in DB (last write wins), so a curated entry
     * can override a stale built-in mapping. Non-fatal if the file is absent. */
    const char *paths[] = {
        "lib/sdl_gamecontrollerdb/gamecontrollerdb.txt",
        "gamecontrollerdb.txt",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int n = SDL_GameControllerAddMappingsFromFile(paths[i]);
        if (n > 0) {
            printf("[SDL] loaded %d controller mappings from %s\n", n, paths[i]);
            break;
        }
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) gc_try_open(i);
}

/* Read one SDL game controller into N64 button bits + stick. */
static void gc_read(SDL_GameController *gc, struct pad_state *p) {
    if (!gc) return;
    struct { SDL_GameControllerButton b; unsigned int bit; } map[] = {
        { SDL_CONTROLLER_BUTTON_A,             N64_A },
        { SDL_CONTROLLER_BUTTON_B,             N64_B },
        { SDL_CONTROLLER_BUTTON_X,             N64_B },   /* X = B alt */
        { SDL_CONTROLLER_BUTTON_Y,             N64_CU },
        { SDL_CONTROLLER_BUTTON_START,         N64_START },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  N64_L },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, N64_R },
        { SDL_CONTROLLER_BUTTON_DPAD_UP,       N64_DU },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     N64_DD },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     N64_DL },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    N64_DR },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (SDL_GameControllerGetButton(gc, map[i].b)) p->buttons |= map[i].bit;
    }
    /* Triggers: LT = Z (brake/reverse convention shared with menus), RT = Z too. */
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 8000) p->buttons |= N64_Z;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) p->buttons |= N64_Z;
    /* Right stick -> C-buttons (menus/camera). */
    int rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
    int ry = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
    if (rx < -12000) p->buttons |= N64_CL;
    if (rx >  12000) p->buttons |= N64_CR;
    if (ry < -12000) p->buttons |= N64_CU;
    if (ry >  12000) p->buttons |= N64_CD;
    /* Left stick -> analog (N64 range ±80; SDL axis is ±32767, +Y is down). */
    int lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    int sx = (lx * STICK_FULL) / 32767;
    int sy = (-ly * STICK_FULL) / 32767;
    if (sx < -STICK_FULL) sx = -STICK_FULL; if (sx > STICK_FULL) sx = STICK_FULL;
    if (sy < -STICK_FULL) sy = -STICK_FULL; if (sy > STICK_FULL) sy = STICK_FULL;
    if (sx < -8 || sx > 8) p->stick_x = sx;   /* small deadzone */
    if (sy < -8 || sy > 8) p->stick_y = sy;
}

/* Read the keyboard (P1 only) into N64 button bits + stick. */
static void kbd_read(struct pad_state *p) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    if (!k) return;
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
static void browser_touch_read(struct pad_state *p) {
    unsigned int buttons = 0;
    int stickX = 0;
    int stickY = 0;

    browser_touch_pad_read(&buttons, &stickX, &stickY);
    stickX = stickX < -STICK_FULL ? -STICK_FULL :
             stickX >  STICK_FULL ?  STICK_FULL : stickX;
    stickY = stickY < -STICK_FULL ? -STICK_FULL :
             stickY >  STICK_FULL ?  STICK_FULL : stickY;

    /* The analog stick is the authoritative steering source. Mirror a decisive
     * deflection onto the D-pad as the keyboard path does because a few menus
     * and gameplay screens read jpad directly instead of the analog axes. */
    if (stickX < -24) buttons |= N64_DL;
    if (stickX >  24) buttons |= N64_DR;
    if (stickY < -24) buttons |= N64_DD;
    if (stickY >  24) buttons |= N64_DU;

    p->buttons |= buttons &
        (N64_A | N64_B | N64_Z | N64_START |
         N64_DU | N64_DD | N64_DL | N64_DR |
         N64_L | N64_R | N64_CU | N64_CD | N64_CL | N64_CR);

    /* A resting touch stick must not erase a physical pad. If both are active,
     * retain the larger deflection independently on each axis. */
    if (abs(stickX) > abs(p->stick_x)) p->stick_x = stickX;
    if (abs(stickY) > abs(p->stick_y)) p->stick_y = stickY;
}
#endif

/* Build the per-frame pad state. Called at the top of platform_frame_sync,
 * before the retrace message is synthesized (spec ordering). */
void platform_input_pump(void) {
    if (s_sdlReady) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
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
            int overlayActive = platformOverlayWantsInput();
            platformOverlayProcessEvent(&e);
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
                    if (e.key.keysym.sym == SDLK_ESCAPE && g_headlessFrames < 0)
                        s_quitRequested = 1;   /* Esc = host quit (interactive) */
                    break;
                case SDL_WINDOWEVENT:
                    /* The dimensions are sampled after present in
                     * platform_frame_sync; consuming the event here keeps SDL's
                     * state current without reconfiguring a surface mid-frame. */
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    gc_try_open(e.cdevice.which);
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    gc_close_instance(e.cdevice.which);
                    break;
                default:
                    break;
            }
        }
    }

    for (int i = 0; i < DKR_MAXPADS; i++) {
        s_pads[i].buttons = 0;
        s_pads[i].stick_x = 0;
        s_pads[i].stick_y = 0;
    }
    /* P1: keyboard + first gamepad; P2..P4: their gamepads only (structural). */
    if (s_sdlReady) {
        kbd_read(&s_pads[0]);
        for (int i = 0; i < DKR_MAXPADS; i++) gc_read(s_gc[i], &s_pads[i]);
    }
#ifdef __EMSCRIPTEN__
    browser_touch_read(&s_pads[0]);
#endif
    /*
     * Scripted input is authored against the AUTHORITATIVE TICK index, not the
     * present counter. While one tick is one present the two are identical, and
     * every existing fixture is unaffected; once the presentation subloop runs
     * N presents per tick, g_frameCounter counts images while the script must
     * keep describing what the player did on tick k. Entries carry their own
     * target port (default P1), so pass the whole pad array.
     */
    script_apply(s_pads, g_simTickCounter);

    if (s_quitRequested) {
        platform_request_exit(0);
    }
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
        platformOverlayRender();
#endif
        SDL_GL_SwapWindow(s_window);
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
        for (int i = 0; i < DKR_MAXPADS; i++) {
            if (s_gc[i]) {
                (void)platform_pad_rumble(i, 0);
                SDL_GameControllerClose(s_gc[i]);
                s_gc[i] = NULL;
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
    if (s_glctx)  { SDL_GL_DeleteContext(s_glctx); s_glctx = NULL; }
#if defined(MDKR_WEBGPU_BACKEND) && defined(__APPLE__)
    if (s_metalView) { SDL_Metal_DestroyView(s_metalView); s_metalView = NULL; }
#endif
    for (int i = 0; i < DKR_MAXPADS; i++) {
        if (s_gc[i]) {
            (void)platform_pad_rumble(i, 0);
            SDL_GameControllerClose(s_gc[i]);
            s_gc[i] = NULL;
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

enum { PACE_REALTIME, PACE_SYNTH };
static int      s_paceMode        = -1;      /* lazy-init */
static int      s_paceCompensate  = 1;       /* 0 == diagnostic compensation off */
static int      s_synthFields     = 2;
static int      s_minFields       = 2;
static int      s_fieldHz         = 60;
static MdkrPacingClock s_paceClock;
static uint64_t s_paceCumWall     = 0;       /* cumulative wall fields (trace) */

static uint64_t pace_host_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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
#endif
    if ((e = getenv("MDKR_PACE_REALTIME")) && atoi(e) != 0) s_paceMode = PACE_REALTIME;
    e = getenv("MDKR_SYNTH_FIELDS");
    s_synthFields = mdkr_pacing_synthetic_fields(
        e != NULL ? atoi(e) : 0, s_minFields, MDKR_PACING_MAX_FIELDS);
    if ((e = getenv("MDKR_VI_PACE")) && strcasecmp(e, "off") == 0) s_paceCompensate = 0;
    MDKR_TRACE("pace init: mode=%s cadence=%s minFields=%d compensate=%d "
               "synthFields=%d fieldHz=%d sourceFieldHz=%d",
               s_paceMode == PACE_REALTIME ? "realtime" : "synth",
               cadence, s_minFields, s_paceCompensate, s_synthFields,
               s_fieldHz, platform_source_field_hz());
}

static void pace_sleep_until(uint64_t target_ns) {
    uint64_t now = pace_host_ns();
#ifdef __EMSCRIPTEN__
    /* The browser cannot block a thread; suspend to requestAnimationFrame (via
     * Asyncify) until the 1/60 s grid point is reached. ALWAYS yield at least one
     * rAF — this is the only per-frame yield, so it must run even when the frame
     * already overran the budget (keeps the tab responsive + presents the canvas).
     * Stop within ~2 ms of the target: another whole vsync would overshoot, and
     * the grid is absolute so finishing slightly early is phase lead, not drift. */
    do {
        platformWaitAnimationFrame();
        now = pace_host_ns();
    } while (now + 2000000ULL < target_ns);
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
}

/* Return the field count the just-completed frame represents (>=1), pacing to
 * the configured cadence floor in realtime mode. g_viLastWallFields = the TRUE
 * elapsed field count (unaffected by MDKR_VI_PACE=off); g_viLastFields = what
 * gets injected as updateRate (forced to 1 when compensation is disabled). */
int platform_vi_pace_measure(void) {
    pace_lazy_init();
    int wall;   /* true elapsed fields */

    if (s_paceMode == PACE_SYNTH) {
        wall = s_synthFields;
    } else {
        uint64_t now = pace_host_ns();
        uint64_t target = mdkr_pacing_clock_target(&s_paceClock, now);
        int rebased = 0;
#ifdef __EMSCRIPTEN__
        /* Web: yield EVERY frame (pace_sleep_until guarantees >=1 rAF), even
         * when the frame overran the configured floor — otherwise a heavy
         * frame would never suspend and the tab would hang. */
        pace_sleep_until(target);
        now = pace_host_ns();
#else
        if (now < target) {
            pace_sleep_until(target);
            now = pace_host_ns();
        }
#endif
        wall = mdkr_pacing_clock_commit(&s_paceClock, now, &rebased);
        if (rebased) {
            MDKR_TRACE("pace rebase: now=%llu fieldHz=%d",
                       (unsigned long long)now, s_fieldHz);
        }
    }

    s_paceCumWall += (uint64_t)wall;
    g_viLastWallFields = wall;
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
 * is a second, finer floor for the individual presents inside one tick.
 *
 * Engagement is deliberately narrow. The present period must be a whole number
 * of source fields, at least one, and strictly finer than the tick floor:
 *   MDKR_PRESENT_RATE=60 at 60 Hz NTSC -> 1 field/present, 2 presents/tick.
 *   MDKR_PRESENT_RATE=30 -> 2 fields/present == the tick floor -> not engaged,
 *                           behaviour identical to an unset rate.
 *   MDKR_PRESENT_RATE=120 -> half a field per present, which the field-grid
 *                           pacing clock cannot express -> not engaged.
 * Rates finer than the field grid (120/144/VRR/uncapped) would need the pacer
 * to leave the integer-field grid entirely, which it cannot do; refusing them
 * here is honest rather than silently rounding them to 60.
 */
static int s_presentFields = -1;      /* -1 unresolved, 0 == not engaged */
static MdkrPacingClock s_presentClock;

static void present_pace_lazy_init(void) {
    unsigned rate;
    int fields;

    if (s_presentFields >= 0) {
        return;
    }
    pace_lazy_init();
    s_presentFields = 0;
    rate = present_sched_present_rate();
    if (rate == 0) {
        return;
    }
    if ((unsigned)s_fieldHz % rate != 0) {
        MDKR_TRACE("present pace: rate=%u does not divide fieldHz=%d; "
                   "subloop not engaged", rate, s_fieldHz);
        return;
    }
    fields = s_fieldHz / (int)rate;
    if (fields < 1 || fields >= s_minFields) {
        MDKR_TRACE("present pace: rate=%u gives %d field(s)/present against a "
                   "%d-field tick floor; subloop not engaged",
                   rate, fields, s_minFields);
        return;
    }
    if (!mdkr_pacing_clock_init(&s_presentClock, s_fieldHz, fields,
                                MDKR_PACING_MAX_FIELDS)) {
        return;
    }
    s_presentFields = fields;
    MDKR_TRACE("present pace: rate=%u -> %d field(s)/present against a "
               "%d-field tick", rate, fields, s_minFields);
}

int platform_present_subloop_fields(void) {
    present_pace_lazy_init();
    return s_presentFields;
}

/*
 * Pace ONE present and return the true source fields it occupied. Unlike
 * platform_vi_pace_measure() this never applies s_minFields: the tick floor is
 * enforced by the accumulator that consumes these counts, not by sleeping.
 */
int platform_vi_present_pace(void) {
    int wall;

    present_pace_lazy_init();
    if (s_presentFields <= 0) {
        /* Not engaged: one present is one tick, exactly as before. */
        (void)platform_vi_pace_measure();
        return g_viLastWallFields;
    }
    if (s_paceMode == PACE_SYNTH) {
        /*
         * NOTE the asymmetry with the realtime arm below: this does NOT add to
         * s_paceCumWall.
         *
         * s_paceCumWall is the deterministic COUNTER source in synthetic mode
         * (osGetCount, stubs_dkr.c), and osGetCount carries a monotonic clamp
         * that fabricates +1 whenever it is called twice without the clock
         * having moved. So the VALUE game code reads depends on where the clock
         * advances relative to the tick, not merely on how much it advances.
         * Feeding it per present shifted that phase by one present and a level
         * load completed one tick early — the [SIMHASH] streams for
         * MDKR_PRESENT_RATE unset and =60 diverged from tick 1345 onward, at
         * exactly the 270-object to 96-object transition.
         *
         * So in synthetic mode the tick's whole field budget is committed once
         * per tick by platform_vi_tick_clock_commit(), in the same place the
         * non-subloop path commits it, and the per-present count returned here
         * feeds only the presentation accumulator. In realtime mode osGetCount
         * reads the host clock instead and this cannot arise.
         */
        wall = s_presentFields;
    } else {
        uint64_t now = pace_host_ns();
        uint64_t target = mdkr_pacing_clock_target(&s_presentClock, now);
        int rebased = 0;
#ifdef __EMSCRIPTEN__
        /* Web yields every present; the subloop body is present-and-yield only,
         * so each interpolated present gets its own rAF. */
        pace_sleep_until(target);
        now = pace_host_ns();
#else
        if (now < target) {
            pace_sleep_until(target);
            now = pace_host_ns();
        }
#endif
        wall = mdkr_pacing_clock_commit(&s_presentClock, now, &rebased);
        s_paceCumWall += (uint64_t)wall;
    }
    g_viLastWallFields = wall;
    return wall;
}

/*
 * Commit one authoritative tick's worth of synthetic field budget, at the point
 * in the branch where the non-subloop path commits it. See the synthetic arm of
 * platform_vi_present_pace for why the PHASE of this matters and not just the
 * total. No-op in realtime mode, where the COUNTER comes from the host clock.
 */
void platform_vi_tick_clock_commit(void) {
    present_pace_lazy_init();
    if (s_presentFields <= 0 || s_paceMode != PACE_SYNTH) {
        return;
    }
    s_paceCumWall += (uint64_t)s_synthFields;
}

uint64_t platform_sim_field_count(void) {
    pace_lazy_init();
    return s_paceCumWall;
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
    int racerIndex, int playerIndex) {
    s_probeRaceLap = lap;
    s_probeRaceFinished = raceFinished;
    s_probeRaceFinishPosition = finishPosition;
    s_probeRaceRacerIndex = racerIndex;
    s_probeRacePlayerIndex = playerIndex;
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
static void platform_frame_sync_impl(int swap) {
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
            extern struct GfxRenderingAPI gfx_opengl_api;
            if (gfx_webgpu_recover_device()) {
                gfx_reset_renderer_caches();
                gfx_set_dimensions(gfx_output_dimensions.width,
                                   gfx_output_dimensions.height);
                renderer_recovered = 1;
                fprintf(stderr,
                        "[webgpu] native device recovery succeeded; gameplay "
                        "continues\n");
            } else if (platform_sdl_fallback_to_gl() == 0 &&
                       gfx_rebind_renderer(&gfx_opengl_api)) {
                platform_sdl_sync_drawable_size();
                renderer_recovered = 1;
                fprintf(stderr,
                        "[webgpu] recovery failed; switched to OpenGL without "
                        "restarting\n");
            } else {
                fprintf(stderr,
                        "[webgpu] recovery and OpenGL fallback both failed; "
                        "terminating cleanly\n");
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
    if (!renderer_recovered) {
        platform_dump_frame();   /* read the rendered backbuffer before the swap */
        if (swap) {
            platform_sdl_present();
        }
    }
    /* Publish a resize before the game builds its next display list. Camera FOV,
     * renderer viewports and WebGPU targets therefore change together (no
     * one-frame projection/viewport mismatch). */
    platform_sdl_sync_drawable_size();
    g_frameCounter++;
    MDKR_TRACE("frame %d presented", g_frameCounter);
    {
        extern void mdkr_oracle_trace_racers(int frame);
        /* Tick-indexed for the same reason as the input script above: an oracle
         * row must stay comparable against a run at a different present rate. */
        mdkr_oracle_trace_racers(g_simTickCounter);
    }
#ifdef __EMSCRIPTEN__
    /* Publish the presented-frame count to JS so the shell can show liveness/FPS
     * and headless harnesses can confirm the rAF-driven loop is advancing. */
    EM_ASM({ if (typeof Module !== 'undefined') { Module.__mdkrFrames = $0; } }, g_frameCounter);
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
        printf("[FONT] sdfUploads=%u registryFailures=%u\n",
               gfx_dkr_font_sdf_uploads,
               gfx_dkr_font_registry_failures);
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
                "lookup=%llu/%llu allocFails=%llu\n",
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
                (unsigned long long)stats.allocation_failures);
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
            printf("[COLL] maxCandidates=%d truncated=%ld cap=%d\n",
                   mdkr_coll_max_candidates(), mdkr_coll_truncations(),
                   mdkr_coll_cap(500));
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
    platform_frame_sync_impl(1);
}

/*
 * A frame boundary that does everything platform_frame_sync does EXCEPT the
 * buffer swap, for the presentation subloop.
 *
 * The subloop has two no-draw paths: Video.MotionSmoothing=off, whose
 * documented behaviour is "present at the requested rate but REPEAT the tick's
 * own image", and a pass that submitted no graphics task, where the held
 * display list is being overwritten by the game and must not be re-walked.
 * Both used to call platform_frame_sync(), which swaps.
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
 * NOT swapping is the correct repeat: the front buffer already holds the tick's
 * image and keeps being scanned out. What the viewer sees is the tick's image
 * held for the whole tick, i.e. FrameLimit=60 with smoothing off updates the
 * screen at TICK rate, not at 60. That is the honest consequence of asking for
 * a rate with nothing new to show at it, and it is recorded in
 * Video.FrameLimit's schema description (platform/video_config.c).
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
    platform_frame_sync_impl(0);
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
    return s_gc[port] != NULL || (s_scriptPresentMask & (1u << port)) != 0;
}
int platform_pad_rumble_supported(int port) {
    if (port < 0 || port >= DKR_MAXPADS || s_gc[port] == NULL) return 0;
#ifdef __EMSCRIPTEN__
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(s_gc[port]);
    if (!joystick) return 0;
    return browser_gamepad_rumble_supported(SDL_JoystickInstanceID(joystick));
#elif SDL_VERSION_ATLEAST(2, 0, 9)
    /* A zero-duration, zero-strength request is SDL's capability probe. */
    return SDL_GameControllerRumble(s_gc[port], 0, 0, 0) == 0;
#else
    return 0;
#endif
}
int platform_pad_rumble(int port, int enabled) {
    if (port < 0 || port >= DKR_MAXPADS || s_gc[port] == NULL) return 0;
#ifdef __EMSCRIPTEN__
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(s_gc[port]);
    if (!joystick) return 0;
    return browser_gamepad_rumble(
        SDL_JoystickInstanceID(joystick), enabled != 0);
#elif SDL_VERSION_ATLEAST(2, 0, 9)
    return SDL_GameControllerRumble(
               s_gc[port],
               enabled ? UINT16_MAX : 0,
               enabled ? UINT16_MAX : 0,
               enabled ? 60000u : 0u) == 0;
#else
    (void)enabled;
    return 0;
#endif
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
