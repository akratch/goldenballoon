/**
 * platform_os.h — mdkr64 platform layer (native macOS/PC port of DKR).
 *
 * NOTE: Unlike mgb64, the DKR decomp KEEPS its full PR SDK headers, which
 * already declare every os/al/gu function and type. This header therefore
 * does NOT redeclare the libultra API - it only declares the mdkr64-internal
 * platform glue that game code and the platform TUs share:
 *   - the RDRAM stand-in arena (16 MB) + 32-bit pointer reconstruction,
 *   - ROM-buffer access for the dmacopy/DMA path,
 *   - the cooperative frame-sync / headless-run hooks,
 *   - the renderer (fast3d) entry points.
 */
#ifndef MDKR64_PLATFORM_OS_H
#define MDKR64_PLATFORM_OS_H

#include <stdint.h>
#include <stddef.h>
#include "mdkr_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== RDRAM stand-in arena (PLAN decision 2) ============================= *
 * One contiguous, 16 MB-aligned block stands in for N64 RDRAM. Aligning the
 * block to its own size guarantees every pointer into it shares the same high
 * 32 bits, so a value truncated to u32 by game code (`(u32)ptr`, pervasive in
 * the asset/DMA path) can be losslessly widened back to a host pointer by
 * OR-ing the arena's high bits. */
#define DKR_ARENA_SIZE   0x1000000u   /* 16 MB */

/* Allocate + register the arena. Returns the usable base (host pointer). */
void *dkr_arena_init(uint32_t size);
/* Release the arena after audio/game/renderer owners have stopped using it. */
void dkr_arena_shutdown(void);
/* Reconstruct a full host pointer from a 32-bit-truncated arena address. */
void *dkr_lo32_to_ptr(uint32_t lo32);
extern uintptr_t g_dkrArenaHi;   /* arena base & ~0xFFFFFFFF */
extern void     *g_dkrArenaBase;
extern uint32_t  g_dkrArenaSize;

/* ===== ROM buffer (rom_io.c) ============================================= */
extern uint8_t *g_romData;   /* whole .z64 image, big-endian bytes */
extern uint32_t g_romSize;
int platformInitRom(const char *path);
/* Raw ROM read used by the dmacopy path: memcpy rom[off..off+len) -> dst. */
int platform_rom_read(uint32_t romOffset, void *dst, int32_t len);
void platform_rom_shutdown(void);

/* ===== Frame pacing / headless (platform_sdl_min.c) ====================== *
 * The collapsed single-threaded loop blocks in osRecvMesg on an empty
 * scheduler-client (video) queue; that block is the frame boundary and calls
 * platform_frame_sync(): poll input, present, advance the headless counter. */
void platform_frame_sync(void);
/*
 * The same frame boundary MINUS the buffer swap, for a present that produced no
 * new image (Phase 3 Wave B's presentation subloop: motion smoothing off, or a
 * stale display list). Swapping there would put an undefined back buffer on
 * screen instead of repeating the tick's image; see the definition.
 */
void platform_frame_sync_no_swap(void);
/* Catch-up boundary: service host input/lifecycle without an image present. */
void platform_frame_service(void);
extern int g_headlessFrames;    /* -1 = windowed/forever; >=0 = exit after N */
extern int g_headlessTicks;     /* -1 = frame budget; >=0 = exit after N ticks */
extern int g_frameCounter;
/*
 * Images actually committed to the window/canvas. Unlike g_frameCounter,
 * which counts host pacing opportunities for scheduler and headless control,
 * this advances only after a successful GL swap or WebGPU surface present.
 */
extern uint64_t g_surfaceFrameCounter;
/*
 * Cooperative process exit. Frame/input code requests termination and the
 * game loop unwinds to main(), which tears down renderer, audio, and SDL in
 * dependency order instead of calling exit() from inside a frame.
 */
void platform_request_exit(int code);
int platform_exit_requested(void);
int platform_exit_code(void);
void platform_headless_tick_complete(int tick_count);

/* Source-release timing selected by ROM identification before game boot. */
int platform_source_tv_type(void);   /* 0 PAL, 1 NTSC, 2 MPAL */
int platform_source_field_hz(void);  /* 50 PAL, 60 NTSC/MPAL */
/* True only for an accepted European ROM revision.  This is deliberately
 * narrower than PAL timing: menu language capability belongs to the validated
 * cartridge identity, not to a mutable header field or display standard. */
int platform_source_is_european(void);

/* ===== VI retrace / logic-update-rate pacing (the frame-pacing fix) ======= *
 * DKR normalises game speed against framerate via fb_update() (game/src/video.c),
 * which drains the video message queue to count how many 60 Hz VI fields elapsed
 * per rendered frame and returns that as `updateRate` (LOGIC_60FPS=1 @ 60fps,
 * LOGIC_30FPS=2 @ 30fps, ...). Movement, physics AND the race clock all scale by
 * updateRate, so a correct updateRate is what keeps wall-clock speed constant as
 * the render rate varies (DKR's native frameskip compensation).
 *
 * On N64 the VI interrupt posts retrace messages to that queue ASYNCHRONOUSLY at
 * 60 Hz. Our cooperative shim has no async producer, so without this pacer the
 * drain always finds 0 and updateRate pins at 1 regardless of the real present
 * cadence — motion desyncs from real time. The native driver converts host
 * elapsed time into fixed one- or two-field tickets. Ordinary lateness becomes
 * repeated tickets; it never becomes one oversized gameplay update.
 *
 * platform_vi_pace_measure() paces the present to the configured simulation
 * floor: two 60 Hz fields for original/authored gameplay (default), or one for
 * the explicit enhanced compatibility mode. It returns the host fields in that
 * pace sample. The driver consumes them and the video-queue adapter publishes
 * exactly one fixed-width ticket to fb_update(). */
int  platform_vi_pace_measure(void);   /* pace + return this frame's field count (>=1) */
/* Local oracle diagnostic only. When MDKR_ORACLE_UPDATE_FIELDS names a strict
 * `tick fields` schedule, return the real-ROM update width for `tick`. This
 * never changes the shipping fixed-ticket path. */
int  platform_oracle_update_fields(uint64_t tick, int *fields);
/* Report and release the optional oracle schedule. Returns zero when a named
 * schedule was not consumed completely, so diagnostic runs fail closed. */
int  platform_oracle_update_fields_finish(void);
extern int g_viLastFields;             /* fixed ticket width published as updateRate */
extern int g_viLastWallFields;         /* true wall-clock/synthetic fields elapsed (trace: speed vs realtime) */
int      platform_pace_is_synthetic(void); /* 1 == deterministic fixed fields/frame (headless) */
uint64_t platform_sim_field_count(void);   /* cumulative 60 Hz fields; the deterministic clock */
/* The pacer's RESOLVED field clock (source rate, or the MDKR_FIELD_HZ
 * diagnostic override). present_sched.c models the same second the pacer
 * enforces, so it must read this rather than platform_source_field_hz(). */
int      platform_pace_field_hz(void);
/* Fixed gameplay-ticket width selected by SimulationCadence. */
int      platform_sim_tick_fields(void);
/* The most recent pace sample crossed a suspension/debugger rebase. */
int      platform_vi_pace_rebased(void);
/* 0 == MDKR_VI_PACE=off: the diagnostic that injects updateRate 1 regardless of
 * elapsed time. The present subloop applies it once per tick, not per present. */
int      platform_vi_pace_compensating(void);
/* Presentation subloop. Returns nonzero when the resolved policy needs host
 * opportunities between authoritative ticks. */
int      platform_present_subloop_fields(void);
/*
 * Apply a staged Video.FrameLimit / Video.MotionSmoothing / Video.AllowTearing
 * change as one ordered step. Registered as the MDKR_VIDEO_APPLY_PRESENTATION
 * domain's applier and called ONLY from video_config's boundary runner, which
 * the engine invokes at the host-frame boundary — never from a settings stack.
 * See the definition in platform_sdl_min.c for what that boundary guarantees.
 */
void     platform_present_config_apply(void);
/* Refresh of the display the window is on, or 0 where the host does not report
 * one. The deadline pacer and the backend present-mode choice must agree about
 * what "above the display" means, so both read this one number. */
unsigned platform_present_display_rate(void);
/*
 * One presentation-grid interval in clock units (one source field == 1e9), or
 * 0 when this run's presents are NOT quantized to the display's own refresh.
 *
 * Nonzero means the display's vblank is the only thing retiring presents --
 * realtime pacing, a policy with no software cadence of its own, a
 * vblank-synchronized queue that is not allowed to tear, and a refresh the host
 * actually reports. Under exactly those conditions one present is one refresh,
 * so the interpolation phase can be projected onto that grid instead of read
 * off a jittery wake (mdkr_present_quantize_phase). Any other combination
 * returns 0 and the measured phase stands, which is what keeps synthetic,
 * capped, uncapped, browser and original runs bit-for-bit as they were.
 *
 * The refresh underneath this is re-derived live (see the display-changed
 * handler); the latched POLICY is not.
 */
uint64_t platform_present_display_quantum_units(void);
/* Pace one present; returns exact clock units (one source field == 1e9). */
uint64_t platform_vi_present_pace_units(void);
/* Commit one fixed ticket to the synthetic COUNTER at the tick boundary. */
void     platform_vi_tick_clock_commit(unsigned tick_fields);

/* Diagnostic intra-thread wall clock. Unlike the VI pacer clock, this keeps
 * advancing inside a synthetic browser animation frame, so fixed-tick work
 * can be profiled truthfully. Values are relative-only monotonic nanoseconds. */
uint64_t platform_perf_monotonic_ns(void);

/* Objective motion-vs-clock probe (published from game/src/racer.c under
 * NATIVE_PORT for player 1 each frame; logged by the pacer trace). Lets an
 * automated run compare the racer's world-position advance against the race
 * clock advance without a human watching the screen. */
void mdkr_pace_probe_racer(int playerIndex, float x, float y, float z, int clockFrames);
void mdkr_pace_probe_vehicle(int playerIndex, int vehicleID);
/* Race progress (checkpoint index + lap) for the same probe — the invariant an
 * automated drive check asserts on ("the racer keeps advancing along the track"). */
void mdkr_pace_probe_progress(int playerIndex, int checkpoint, int lap);
/* Published from race_check_finish() -- outside update_player_racer, which stops
 * for a racer as soon as it finishes and so cannot observe the finish itself.
 * The identity fields prove that the probe follows controller port 1 rather
 * than whichever racer happened to occupy starting-grid slot zero. */
void mdkr_pace_probe_finish(
    int lap, int raceFinished, int finishPosition,
    int racerIndex, int playerIndex);
/* Time-trial ghost playback: counts interpolated ghost frames and records the
 * source bank (0/1 = the player's own ghost, 2 = staff). Lets a check assert that
 * its route really reaches timetrial_ghost_read() rather than assuming it. */
void mdkr_pace_probe_ghost(int ghostBank);

/* ===== Render backend selection (M4.5) =================================== *
 * The renderer backend is chosen once at startup from MDKR_RENDERER
 * (webgpu|gl|metal), defaulting to WebGPU on native builds. The window/context created by
 * platform_sdl_init() differs per backend (a Metal-layer window for WebGPU vs a
 * GL-context window for OpenGL), so the choice must be resolved BEFORE the
 * window is created and used again by main_pc to pick the &gfx_*_api passed to
 * gfx_init. mdkr_render_backend() resolves + caches it. An explicit selector
 * for a backend that was not compiled in is unavailable and startup fails;
 * renderer selection never silently changes backend. */
enum MdkrRenderBackend {
    MDKR_BACKEND_GL     = 0,
    MDKR_BACKEND_WEBGPU = 1,
    MDKR_BACKEND_METAL  = 2,
};
int mdkr_render_backend(void);          /* resolved once from MDKR_RENDERER */
const char *mdkr_render_backend_name(void);
int mdkr_render_backend_available(void); /* false for an unavailable explicit choice */

/* SDL window + GL/Metal context lifecycle. */
int  platform_sdl_init(void);
void platform_sdl_shutdown(void);
void platform_sdl_present(void);   /* swap GL buffers (WebGPU presents in end_frame) */
void platform_sdl_drawable_size(int *w, int *h);  /* HiDPI framebuffer pixels */
void platform_sdl_set_initial_size(int w, int h);
void platform_sdl_sync_drawable_size(void);
/* False while the native window is hidden/minimized and cannot supply a
 * swapchain drawable. Browser canvases remain presentable even though SDL's
 * synthetic window flags do not describe DOM visibility. */
int  platform_sdl_surface_presentable(void);

/* Native window handles the WebGPU backend (platform/fast3d/gfx_webgpu.c)
 * resolves its WGPUSurface from. macOS uses the CAMetalLayer; every backend
 * shares the SDL_Window. NULL before the window exists / off the Metal path. */
void *platformGetMetalLayer(void);
void *platformGetSdlWindow(void);

enum MdkrWebGpuWindowSystem {
    MDKR_WGPU_WINDOW_UNKNOWN = 0,
    MDKR_WGPU_WINDOW_WIN32,
    MDKR_WGPU_WINDOW_X11,
    MDKR_WGPU_WINDOW_WAYLAND,
};

/*
 * Resolve the live SDL window into the native pair required by webgpu.h.
 * Win32: out_display=HINSTANCE, out_window=HWND.
 * X11:   out_display=Display*, out_id=Window.
 * Wayland: out_display=wl_display*, out_window=wl_surface*.
 */
enum MdkrWebGpuWindowSystem platformWebGpuWindowInfo(
    void *sdl_window, void **out_display, void **out_window,
    unsigned long long *out_id);

/* ===== Frame dumping (--dump-frames DIR) ================================= *
 * When g_dumpFramesDir is non-NULL the last completed backend frame is written
 * as DIR/frame_%04d.ppm (P6 binary). */
extern const char *g_dumpFramesDir;
/* True only when the current host-frame ordinal matches the active dump filter.
 * WebGPU uses this to make that explicit diagnostic capture exact even when
 * ordinary runtime admission is intentionally nonblocking. */
int platform_frame_dump_due(void);
/* WebGPU evidence captures admit a short sequence immediately before a due
 * frame so renderer-only temporal state cannot depend on how many earlier
 * nonblocking opportunities each A/B arm happened to obtain. */
int platform_frame_dump_prepare_due(void);

/* ===== Content packs (platform_sdl_min.c) =============================== *
 * Host-side ownership of the pack registry (platform/mod_registry.h) and the
 * decoded override textures the renderer reads through
 * platform/mod_texture_store.h. It lives beside the event pump because the
 * player's live A/B toggle is a key in that pump, and the "is anything even
 * installed" fact that makes the toggle a no-op has to be the same fact the
 * scan produced.
 *
 * A missing mods/ directory is the ordinary case: init resolves the path,
 * finds nothing, logs nothing, and leaves the store inactive so the renderer
 * never pays for a digest. */
void platform_content_packs_init(void);
void platform_content_packs_shutdown(void);
/* Flip the texture override layer for a live before/after comparison. Deliberately
 * does NOT write Content.PacksEnabled back: a momentary A/B is not a settings
 * change, and persisting each keystroke would rewrite the config file mid-race.
 * No-op, and silent, when no pack is installed. */
void platform_content_packs_toggle(void);

/* The scan's result, for a read-only list in the settings panel. Never NULL.
 *
 * Scans on first use if platform_content_packs_init() has not run yet, which is
 * the launcher: the engine calls init before any frame exists, but a player can
 * open Settings -> Content before ever pressing Play, and a list that is empty
 * only because the engine has not started would be the exact silent-loss
 * failure the skip table exists to prevent.
 *
 * Include mod_registry.h to read the returned value; it is forward-declared
 * here so platform_os.h does not grow a dependency for one accessor. */
const struct MdkrModRegistry *platform_content_packs_registry(void);

/* True when `name` appears in a Content.PackDisabled list: comma-separated,
 * entries trimmed of surrounding blanks, ASCII case-insensitive. Public so the
 * settings list names the reason a pack was skipped using the same rule the
 * scan applied, rather than a second matcher that can disagree with it. */
int platform_content_pack_name_disabled(const char *list, const char *name);

/* ===== Input (platform_sdl_min.c) ======================================= *
 * Host events are captured on presentation opportunities, but DKR-visible pad
 * state is published only when the host driver issues a fixed-step ticket.
 * Script entries bypass host poll cadence and remain authoritative-tick
 * indexed. */
void platform_input_init(void);
void platform_input_pump(void);
void platform_input_commit_tick(uint64_t ticket);
void platform_input_queue_summary(void);
int  platform_input_load_script(const char *path);
int          platform_pad_present(int port);
int          platform_pad_rumble_supported(int port);
int          platform_pad_rumble(int port, int enabled);
void         platform_pad_rumble_preferences_changed(void);
unsigned int platform_pad_buttons(int port);
void         platform_pad_stick(int port, int *sx, int *sy);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_PLATFORM_OS_H */
