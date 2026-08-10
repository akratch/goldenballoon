/**
 * main_pc.c — native entry point for mdkr64.
 *
 * Replaces the N64 boot chain (entrypoint.s -> mainproc -> thread1_main ->
 * thread3_main). On native the "threads" are no-ops (see stubs_dkr.c), so we
 * run the real init/loop directly: load the ROM, bring up SDL with the resolved
 * WebGPU or explicit diagnostic GL backend, then call the game's thread3 entry,
 * which runs init_game() and the infinite main loop. The blocking osRecvMesg
 * inside that loop is the cooperative frame pacer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#ifndef __EMSCRIPTEN__
#include <signal.h>
#ifndef _WIN32
#include <execinfo.h>       /* backtrace() — glibc/macOS only; absent on wasm AND Windows */
#endif
#endif
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
/* After SDL.h on purpose: windows.h defines a large macro surface and SDL's
 * own headers expect to be parsed first. WIN32_LEAN_AND_MEAN trims it to what
 * CaptureStackBackTrace needs. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* windows.h defines the 16-bit-era `near`/`far` keywords away as empty
 * macros, and this TU now reaches level_object_entries.h (through the camera
 * runtime's structs.h chain), whose fog-changer entry carries fields with
 * those authored names. The struct is the decomp's wire layout and keeps its
 * names; the legacy macros serve nothing here. */
#undef near
#undef far
#endif
#include "platform_os.h"
#include "display_config.h"
#include "video_config.h"
#include "enhancement_registry.h"
#include "present_sched.h"
#include "gameplay_event_trace.h"
#include "input_consumption_trace.h"
#include "audi_port_dkr.h"
#include "camera_dynamic_occlusion.h"
#include "camera_obstruction_runtime.h"

/*
 * Fatal-crash write mirror. The app shell's DiagLog tee (platform/app/
 * diag_log.cpp) redirects stdout/stderr through a pipe drained by a reader
 * thread; that thread dies with the process, so a crash-time write to stderr
 * can be lost or block on a full pipe. The tee publishes the pre-tee console fd
 * and the raw log fd here so the crash handler below can bypass the pipe.
 * -1 = no tee installed (every CLI invocation), where stderr IS the console.
 *
 * Native only: the wasm build has no shell, no tee and no crash handler, so
 * defining them there would add symbols to the browser binary for nothing.
 */
#ifndef __EMSCRIPTEN__
int g_diagLogRealErrFd = -1;
int g_diagLogFileFd    = -1;

/*
 * Crash-screen presentation hook (platform/app/crash_screen.cpp), published the
 * same way and for the same reason as the two descriptors above: the C engine
 * must not know about the C++ shell, so the shell registers itself here.
 *
 * Called by mdkr_crash_handler below AFTER the `[CRASH]` marker and the
 * backtrace have been written and flushed, and before the disposition is
 * restored. That ordering is the contract: every harness in tests/ greps for
 * that marker, so the player-facing screen is strictly additive output that
 * follows it. NULL on every CLI invocation, where nothing registers.
 */
void (*g_mdkrCrashScreenHook)(int signo) = NULL;
#endif

#ifndef __EMSCRIPTEN__
#ifdef _WIN32
#include <io.h>   /* _write — async-signal-safe enough for a crash path */
/* Write straight to the mirrored fds, bypassing the tee pipe. Falls back to
 * stderr when no tee is installed. */
static void mdkr_crash_write(const char *text, unsigned len) {
    if (g_diagLogRealErrFd < 0 && g_diagLogFileFd < 0) {
        fwrite(text, 1, len, stderr);
        return;
    }
    if (g_diagLogRealErrFd >= 0) (void)_write(g_diagLogRealErrFd, text, len);
    if (g_diagLogFileFd >= 0)    (void)_write(g_diagLogFileFd, text, len);
}
#endif

/* Crash backtrace (renderer bring-up diagnostic): print a symbolized stack on
 * SIGSEGV/SIGBUS so layout-dependent wild-pointer faults are diagnosable even
 * when they vanish under lldb/ASan. Enabled unconditionally — cheap, and only
 * fires on an actual fault. Native only: wasm has no execinfo/POSIX signals and
 * a wasm trap surfaces in the browser's JS console instead (see the shell). */
static void mdkr_crash_handler(int sig) {
    void *bt[40];
#ifdef _WIN32
    /* Windows has neither <execinfo.h> nor backtrace_symbols_fd.
     * CaptureStackBackTrace lives in kernel32, so it needs no dbghelp.dll and
     * cannot itself fail to load inside a crash handler — but it only yields
     * return addresses, not names. Symbolize them offline against the build's
     * map file; that is strictly more than the nothing we had before. */
    USHORT frames = CaptureStackBackTrace(0, 40, bt, NULL);
    char line[128];
    int n = snprintf(line, sizeof(line),
                     "\n[CRASH] signal %d - backtrace (%u frames, unsymbolized):\n",
                     sig, (unsigned) frames);
    if (n > 0) mdkr_crash_write(line, (unsigned) n);
    for (USHORT i = 0; i < frames; i++) {
        n = snprintf(line, sizeof(line), "  #%u %p\n", (unsigned) i, bt[i]);
        if (n > 0) mdkr_crash_write(line, (unsigned) n);
    }
#else
    int n = backtrace(bt, 40);
    fprintf(stderr, "\n[CRASH] signal %d - backtrace (%d frames):\n", sig, n);
    backtrace_symbols_fd(bt, n, 2);
#endif
    /* The marker is out. Only now may anything else speak: the crash screen is
     * additive, and a screen that printed first would reorder the one line the
     * whole test suite keys on. */
    fflush(stderr);
    if (g_mdkrCrashScreenHook != NULL) {
        g_mdkrCrashScreenHook(sig);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

/* Game boot chain (declared here to avoid pulling the full game headers). */
extern void osInitialize(void);
extern void thread0_create(void);
extern void thread3_main(void *arg);
/* game/src/camera_obstruction_runtime.c — claims the CAMERA apply domain.
 * Declared here for the same reason as the three above: the entry point does
 * not pull the game's camera headers. */
extern void camera_obstruction_runtime_install_config_apply(void);

/* Renderer front-end (platform/fast3d/gfx_pc_dkr.c) + the OpenGL backend vtable
 * (platform/fast3d/gfx_opengl.c). Forward-declared opaquely to avoid pulling the
 * full fast3d/PR headers into the entry point. */
struct GfxRenderingAPI;
#ifndef __EMSCRIPTEN__
extern struct GfxRenderingAPI gfx_opengl_api;   /* GL backend — desktop only */
#endif
#ifdef MDKR_WEBGPU_BACKEND
extern struct GfxRenderingAPI gfx_webgpu_api;
#endif
extern bool gfx_init(struct GfxRenderingAPI *rapi);
extern void gfx_shutdown(void);
extern void gfx_set_dimensions(unsigned int width, unsigned int height);

extern void mdkr_memory_shutdown(void);
/* game/src/memory.c — pending entries in the deferred-free queue. Read (not
 * assumed) by the shutdown report so a non-empty queue is visible. */
extern int32_t gFreeQueueCount;

#define DEFAULT_ROM "baserom.us.v80.z64"

/* Set by CMake (see CMakeLists.txt's "Version stamping" block) to the
 * MDKR_VERSION cache variable; the packaged app bundle stamps the same value
 * into Info.plist's CFBundleShortVersionString. Fall back covers non-CMake
 * builds (e.g. tooling that compiles this TU standalone). */
#ifndef MDKR_VERSION_STRING
#define MDKR_VERSION_STRING "dev"
#endif

static void print_usage(const char *program) {
    printf("Usage: %s [ROM] [options]\n"
           "  --rom PATH                 ROM image\n"
           "  --version                  print the version and exit\n"
           "  --window-size WIDTHxHEIGHT initial window size\n"
           "  --aspect auto|4:3|16:9|16:10|21:9\n"
           "  --fov authored|DEGREES     gameplay FOV referenced to authored 60\n"
           "  --max-hfov off|DEGREES     horizontal safety cap (safe default 104)\n"
           "  --widescreen               enable aspect-correct rendering (default)\n"
           "  --legacy-stretch           reproduce the old 4:3 stretch\n"
           "  --pure                     original presentation: 4:3, authored FOV,\n"
           "                             no enhancements (the reference mode)\n"
           "  --restored                 original art direction at modern fidelity\n"
           "                             (default)\n"
           "  --remastered               opt-in work-in-progress art-directed effects\n"
           "  --video-set Key=Value      override one video setting\n"
           "  --video-launch-mode MODE   browser launcher seed (internal)\n"
           "  --video-launch-set K=V     browser launcher seed (internal)\n"
           "  --video-list               print resolved video settings and exit\n"
           "  --headless-frames N        stop after N presented frames\n"
           "  --headless-ticks N         stop after N simulation ticks\n"
           "  --dump-frames DIR          write PPM frame captures\n"
           "  --input-script FILE        deterministic controller fixture\n",
           program);
}

/*
 * Under MDKR_APP the native app shell (platform/app/main_app.cpp) owns main().
 * This body is renamed rather than changed: the shell calls it verbatim for
 * every non-interactive invocation, so that path stays byte-identical to the
 * pre-shell binary. Without MDKR_APP (the wasm build, and -DMDKR_APP=OFF) this
 * is still main() and nothing about the engine's startup differs.
 */
#ifdef MDKR_APP
int mdkr64_headless_main(int argc, char **argv);
int mdkr64_headless_main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    const char *romPath = NULL;
    const char *inputScript = NULL;
    int initialWindowWidth = 640;
    int initialWindowHeight = 480;
    int videoListRequested = 0;
    int exitCode = 0;
    bool audioInitialized = false;

    SDL_SetMainReady();
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
#ifndef __EMSCRIPTEN__
    if (!getenv("MDKR_NO_CRASH_HANDLER")) {
        signal(SIGSEGV, mdkr_crash_handler);
#ifdef SIGBUS
        /* Not defined by the Windows CRT: Win32 reports misaligned/bad-object
         * access as an access violation, i.e. SIGSEGV, which is hooked above. */
        signal(SIGBUS, mdkr_crash_handler);
#endif
    }
#endif

    mdkr_display_config_init();

    /*
     * Resolve and publish the presentation mode BEFORE the argument loop.
     * --aspect / --widescreen / --legacy-stretch / --fov are handled inside the
     * loop and call display_config's setters directly; publishing first is what
     * lets those explicit flags land on top of the mode preset, which is the
     * override-beats-preset rule. Publishing afterwards would silently discard
     * them.
     */
    mdkr_video_config_init(argc, argv);
    mdkr_video_config_publish();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            romPath = argv[++i];
        } else if (strcmp(argv[i], "--headless-frames") == 0 && i + 1 < argc) {
            g_headlessFrames = atoi(argv[++i]);
            g_headlessTicks = -1;
        } else if (strcmp(argv[i], "--headless-ticks") == 0 && i + 1 < argc) {
            g_headlessTicks = atoi(argv[++i]);
            if (g_headlessTicks <= 0) {
                fprintf(stderr,
                        "[mdkr64] invalid --headless-ticks (expected > 0)\n");
                return 2;
            }
            /* Reuse every existing hidden/synthetic/no-vsync headless policy;
             * tick completion, not this unreachable image budget, exits. */
            g_headlessFrames = INT_MAX;
        } else if (strcmp(argv[i], "--dump-frames") == 0 && i + 1 < argc) {
            g_dumpFramesDir = argv[++i];   /* write DIR/frame_%04d.ppm per frame */
        } else if (strcmp(argv[i], "--input-script") == 0 && i + 1 < argc) {
            inputScript = argv[++i];       /* scripted `frame TOKEN[+TOKEN]` file */
        } else if (strcmp(argv[i], "--window-size") == 0 && i + 1 < argc) {
            char trailing;
            if (sscanf(argv[++i], "%dx%d%c", &initialWindowWidth, &initialWindowHeight,
                       &trailing) != 2 ||
                initialWindowWidth < 160 || initialWindowHeight < 120 ||
                initialWindowWidth > 16384 || initialWindowHeight > 16384) {
                fprintf(stderr, "[mdkr64] invalid --window-size (expected WIDTHxHEIGHT, "
                                "160..16384 per axis)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--aspect") == 0 && i + 1 < argc) {
            if (!mdkr_display_set_aspect(argv[++i])) {
                fprintf(stderr, "[mdkr64] invalid --aspect (use auto, W:H, or 0.5..4.0)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--fov") == 0 && i + 1 < argc) {
            if (!mdkr_display_set_gameplay_fov(argv[++i])) {
                fprintf(stderr, "[mdkr64] invalid --fov (use authored or 20..140)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--max-hfov") == 0 && i + 1 < argc) {
            if (!mdkr_display_set_max_horizontal_fov(argv[++i])) {
                fprintf(stderr, "[mdkr64] invalid --max-hfov (use off or 60..175)\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--widescreen") == 0) {
            mdkr_display_set_widescreen("1");
        } else if (strcmp(argv[i], "--legacy-stretch") == 0) {
            mdkr_display_set_widescreen("0");
        } else if (strcmp(argv[i], "--pure") == 0 ||
                   strcmp(argv[i], "--restored") == 0 ||
                   strcmp(argv[i], "--remastered") == 0) {
            /* consumed by mdkr_video_config_init(argc, argv) above */
        } else if (strcmp(argv[i], "--video-set") == 0 && i + 1 < argc) {
            /* consumed by mdkr_video_config_init(argc, argv) above; reject a
             * key it did not recognise rather than ignoring the user silently */
            const char *pair = argv[++i];
            const char *eq = strchr(pair, '=');
            char name[64];
            size_t nl = (eq != NULL) ? (size_t) (eq - pair) : 0;
            if (eq == NULL || nl == 0 || nl >= sizeof(name)) {
                fprintf(stderr, "[mdkr64] invalid --video-set (expected Key=Value)\n");
                return 2;
            }
            memcpy(name, pair, nl);
            name[nl] = '\0';
            {
                MdkrVideoKey key = mdkr_video_key_from_name(name);
                MdkrVideoConfig validation;
                if (key == MDKR_VIDEO_KEY_COUNT) {
                    fprintf(stderr, "[mdkr64] unknown --video-set key '%s' "
                                    "(see --video-list)\n", name);
                    return 2;
                }
                mdkr_video_config_defaults(&validation);
                if (!mdkr_video_config_set(&validation, key, eq + 1,
                                           MDKR_VIDEO_SOURCE_CLI)) {
                    fprintf(stderr,
                            "[mdkr64] invalid value for --video-set key '%s'\n",
                            name);
                    return 2;
                }
            }
        } else if (strcmp(argv[i], "--video-launch-mode") == 0 &&
                   i + 1 < argc) {
            int mode = mdkr_video_mode_from_name(argv[++i]);
            if (mode < MDKR_VIDEO_MODE_PURE ||
                mode > MDKR_VIDEO_MODE_REMASTERED) {
                fprintf(stderr,
                        "[mdkr64] invalid --video-launch-mode\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--video-launch-set") == 0 &&
                   i + 1 < argc) {
            const char *pair = argv[++i];
            const char *eq = strchr(pair, '=');
            char name[64];
            size_t length = eq != NULL ? (size_t) (eq - pair) : 0;
            if (length == 0 || length >= sizeof(name)) {
                fprintf(stderr,
                        "[mdkr64] invalid --video-launch-set\n");
                return 2;
            }
            memcpy(name, pair, length);
            name[length] = '\0';
            {
                MdkrVideoKey key = mdkr_video_key_from_name(name);
                MdkrVideoConfig validation;
                if (key == MDKR_VIDEO_KEY_COUNT) {
                    fprintf(stderr,
                            "[mdkr64] unknown --video-launch-set key '%s'\n",
                            name);
                    return 2;
                }
                mdkr_video_config_defaults(&validation);
                if (!mdkr_video_config_set(&validation, key, eq + 1,
                                           MDKR_VIDEO_SOURCE_LAUNCHER)) {
                    fprintf(stderr,
                            "[mdkr64] invalid value for --video-launch-set key '%s'\n",
                            name);
                    return 2;
                }
            }
        } else if (strcmp(argv[i], "--video-launch-persist") == 0) {
            /* consumed by video_config_runtime before ROM/window startup */
        } else if (strcmp(argv[i], "--video-list") == 0) {
            videoListRequested = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("mdkr64 %s\n", MDKR_VERSION_STRING);
            return 0;
        } else if (argv[i][0] != '-') {
            romPath = argv[i];
        }
    }
    if (videoListRequested) {
        /* Same early-exit path as --help: returns before ROM, window and
         * audio initialization, per the audio-safety rule. */
        mdkr_video_config_report();
        return 0;
    }
    if (!romPath) romPath = DEFAULT_ROM;
    mdkr_display_report_config();
    platform_sdl_set_initial_size(initialWindowWidth, initialWindowHeight);

    /* Internal boot banner. Carries the build's real version stamp
     * (MDKR_VERSION_STRING, set from the MDKR_VERSION cache variable) so the log
     * identifies the binary; product naming lives in the window title, not here.
     * ASCII only — Windows consoles mojibake non-ASCII punctuation. */
    printf("[mdkr64] native port %s\n", MDKR_VERSION_STRING);

    /* Phase 1: ROM (fail-fast — assets are read from it at runtime). */
    if (platformInitRom(romPath) != 0) {
        fprintf(stderr, "[mdkr64] Could not load ROM: %s\n", romPath);
        fprintf(stderr, "[mdkr64] Pass --rom <path> or place baserom.us.v80.z64 here.\n");
        return 1;
    }

    /* Phase 2: SDL2 window + selected backend context/device. */
    if (platform_sdl_init() != 0) {
        fprintf(stderr, "[mdkr64] window/context initialization failed.\n");
        exitCode = 1;
        goto shutdown;
    }
    platform_input_init();               /* open gamepads + load mappings */
    /* A fixture that fails to parse must ABORT, not run a truncated route: a
     * partially loaded script still exits 0 and still "passes" any survival-only
     * check while silently covering nothing (the failure mode docs/ORACLE.md
     * records for the multi-tap routes). Fail fast instead. */
    if (inputScript && platform_input_load_script(inputScript) != 0) {
        fprintf(stderr, "[mdkr64] input script %s failed to load - aborting.\n", inputScript);
        exitCode = 1;
        goto shutdown;
    }

    /* Audio host output (SDL device; skipped headless). Synthesis is driven per
     * frame from stubs_dkr.c once audio_init has run. */
    dkr_audio_out_init();
    audioInitialized = true;

    /*
     * Content packs: scan mods/ and bind the texture override layer.
     *
     * Here, and not earlier, for two reasons. The resolved video config is
     * already published (Content.PacksEnabled and Content.PackDisabled are read
     * during the scan), and mdkr_user_paths_init() has already run in the app
     * shell, so the packaged writable root is known. Here, and not later,
     * because Phase 4 is where the first texture is bound and the store has to
     * be answering by then.
     *
     * No pack installed is the ordinary case: this resolves a path, finds no
     * directory, prints nothing, and leaves the store inactive.
     */
    platform_content_packs_init();

    /* The enhancement table, on request, for check_enhancement_authority.py.
     * Emitted from the running binary rather than kept as a copy in the test:
     * a test carrying its own list of enhancements keeps passing after someone
     * adds a row and forgets the gate, which is precisely the drift the
     * authority class exists to make impossible. */
    {
        const char *dump = getenv("MDKR_ENH_DUMP_TABLE");
        if (dump != NULL && dump[0] == '1') {
            mdkr_enhancement_dump_table();
        }
    }

    /* Phase 3: renderer front-end (F3DDKR HLE) on the selected backend
     * (MDKR_RENDERER; default WebGPU, GL selectable). gfx_init creates the
     * backend's GPU resources, so it needs the backend window/context from Phase 2
     * (created for the SAME backend by platform_sdl_init). Size it to the
     * drawable (HiDPI) framebuffer. */
#ifdef __EMSCRIPTEN__
    /* Web is WebGPU-only (no GL backend compiled in the wasm binary). */
    struct GfxRenderingAPI *rapi = &gfx_webgpu_api;
#else
    struct GfxRenderingAPI *rapi = &gfx_opengl_api;
#ifdef MDKR_WEBGPU_BACKEND
    if (mdkr_render_backend() == MDKR_BACKEND_WEBGPU) {
        rapi = &gfx_webgpu_api;
    }
#endif
#endif
    int renderer_width = 640;
    int renderer_height = 480;
    /*
     * Publish the physical drawable before backend initialization.
     *
     * This ordering is load-bearing on HiDPI Metal-backed WebGPU. wgpu_init()
     * configures the CAMetalLayer immediately; if it runs while the frontend
     * still carries its 320x240 bootstrap dimensions, that configuration
     * changes the layer to 320x240. A later SDL_Metal_GetDrawableSize() then
     * observes the value WebGPU just wrote instead of the real 2x drawable,
     * permanently feeding 320x240 back into the frontend. OpenGL does not
     * configure the drawable during init and therefore escaped the loop.
     *
     * The frontend owns the output/render split, so establish it first and let
     * every backend consume the same output dimensions during bring-up.
     */
    platform_sdl_drawable_size(&renderer_width, &renderer_height);
    gfx_set_dimensions((unsigned int)renderer_width,
                       (unsigned int)renderer_height);
    printf("[mdkr64] renderer backend: %s\n", mdkr_render_backend_name());
    if (!gfx_init(rapi)) {
        fprintf(stderr,
                "[mdkr64] renderer initialization failed for backend %s; "
                "stopping without an automatic fallback. Set MDKR_RENDERER=gl "
                "explicitly for diagnostics.\n",
                mdkr_render_backend_name());
        exitCode = 1;
        goto shutdown;
    }
    MDKR_TRACE("gfx_init(%s) done; dimensions %dx%d",
               mdkr_render_backend_name(), renderer_width, renderer_height);

    if (g_headlessTicks >= 0) {
        printf("[mdkr64] headless: will run %d simulation tick(s) then exit.\n",
               g_headlessTicks);
    } else if (g_headlessFrames >= 0) {
        printf("[mdkr64] headless: will run %d frame(s) then exit.\n", g_headlessFrames);
    }
    printf("[mdkr64] entering boot path...\n");

    /*
     * Claim the deferred-apply domains (video_config.h). AFTER gfx_init and
     * before the game boot chain, which is the only window that satisfies both
     * ends of the contract: every receiver these appliers touch now exists, and
     * no settings UI can have run yet, so no edit can have taken the inline
     * publish path that registration turns off.
     *
     * Before this point the same keys resolve and publish exactly as they
     * always did — which is what keeps --video-set, the launcher's pre-Play
     * panel, and headless runs on one unchanged path.
     */
    mdkr_video_config_register_apply(MDKR_VIDEO_APPLY_PRESENTATION,
                                     platform_present_config_apply);
    camera_obstruction_runtime_install_config_apply();

    /* Phase 4: the game boot chain, collapsed onto this thread. */
    osInitialize();
    thread0_create();
    thread3_main(NULL);   /* returns after a cooperative host-exit request */
    exitCode = platform_exit_code();

shutdown:
    /*
     * Release the apply domains before anything they touch is torn down. A
     * settings edit arriving after this point takes the inline publish path,
     * which writes the config and nothing else -- the correct outcome once
     * there is no longer a pacer or a camera sidecar to apply it to.
     */
    mdkr_video_config_register_apply(MDKR_VIDEO_APPLY_PRESENTATION, NULL);
    mdkr_video_config_register_apply(MDKR_VIDEO_APPLY_CAMERA, NULL);
    if (!platform_oracle_update_fields_finish()) {
        exitCode = EXIT_FAILURE;
    }
    gameplay_event_trace_summary();
    input_consumption_trace_summary();
    platform_input_queue_summary();
    platform_input_latency_summary();
    /* Fixed-ticket/presentation census, emitted before subsystem teardown so
     * a failing fixture still reports clock debt and issued work. */
    present_sched_trace_summary();
    /* Same reason, same place: the present-path cost census (MDKR_PRESENT_PERF)
     * must report whatever it measured even when the run ends badly. */
    present_perf_summary();
    camera_obstruction_perf_summary();
    /* MOTION-01 motion-quality census, same placement and reason. */
    camera_obstruction_motion_summary();
    if (audioInitialized) {
        dkr_audio_out_shutdown();
    }
    /* The dynamic occluder census owns host allocations sized to the object
     * pool. Released after its telemetry has been reported and before the
     * host-memory census below, so a retained side table shows up as a leak
     * rather than as noise. */
    mdkr_camera_dynamic_occlusion_shutdown();
    gfx_shutdown();
    /* After the renderer, because the renderer is the only thing that ever asks
     * the store for pixels. Safe on the early-failure paths above, where the
     * scan never ran. */
    platform_content_packs_shutdown();
    mdkr_memory_shutdown();
    dkr_arena_shutdown();
    platform_rom_shutdown();
    /* Host-owned memory still held after teardown. Every field is READ back from
     * the owning variable, not asserted: a clean shutdown prints zeros, a leak
     * prints what leaked. */
    fprintf(stderr,
            "[HOST-SHUTDOWN] rom=%u arena=%u delayedFree=%d\n",
            (unsigned)g_romSize, (unsigned)g_dkrArenaSize,
            (int)gFreeQueueCount);
    platform_sdl_shutdown();
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (typeof Module !== 'undefined') {
            Module.__mdkrExitCode = $0;
            Module.__mdkrShutdownComplete = true;
        }
    }, exitCode);
#endif
    return exitCode;
}
