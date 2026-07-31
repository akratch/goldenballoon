// engine_entry.h — the C entry points and seams the app shell delegates to.
//
// This is the ONLY header shared between the C++ app shell and the C engine, so
// the two never need hand-synced duplicate declarations. mdkr64_headless_main()
// is the original main() body from platform/main_pc.c (renamed under -DMDKR_APP);
// the shell calls it verbatim for every non-interactive invocation so that path
// stays byte-identical to the pre-shell binary.
#ifndef MDKR64_ENGINE_ENTRY_H
#define MDKR64_ENGINE_ENTRY_H

#ifdef __cplusplus
extern "C" {
#endif

// The engine's original entry point (platform/main_pc.c under MDKR_APP).
int mdkr64_headless_main(int argc, char **argv);

// --- Window / device handoff (platform/host_window.c) ----------------------
// Register the shell's SDL window + GL context so the engine adopts them
// instead of creating its own. Call before booting the engine.
void platformSetHostWindow(void *sdl_window, void *gl_context);

// Register the shell's WebGPU objects (all opaque as void*; surface_format is a
// WGPUTextureFormat) so the engine adopts the shell's device/surface and the
// game renders into the SAME window the launcher used. Call after
// platformSetHostWindow, before booting.
void platformSetHostWebGpu(void *instance, void *adapter, void *device,
                           void *queue, void *surface, int surface_format);
int  platformHasHostWebGpu(void);

// --- Launcher -> game boot -------------------------------------------------
// DKR ADAPTATION: mgb64's MgbBootConfig carries level/difficulty/multiplayer/
// players/mp_stage — GoldenEye's mission-select surface. DKR's engine has no
// equivalent CLI (platform/main_pc.c models no level or difficulty flags; the
// player picks a world in the game's own adventure hub), so those fields are
// deliberately absent rather than present-and-ignored. What the launcher CAN
// meaningfully choose is the ROM and the presentation mode, which is what this
// carries.
#define MDKR_BOOT_MAX_OVERRIDES 16

typedef struct {
    const char *rom_path;      // NULL/empty => engine default (baserom.us.v80.z64)
    int   video_mode;          // MdkrVideoMode, or -1 for "don't pass a preset"
    int   window_width;        // <= 0 => engine default
    int   window_height;
    // Staged RESTART-scope settings, as "Video.Key=Value" strings. The settings
    // panel writes these when the player changes a restart-scope key before
    // pressing Play, so the choice takes effect on THIS boot rather than
    // silently waiting for the next one.
    const char *overrides[MDKR_BOOT_MAX_OVERRIDES];
    int   override_count;
} MdkrBootConfig;

// Boot the engine with the given config. Blocks until the game exits; returns
// the engine's exit code. Implemented by synthesizing a CLI invocation and
// delegating to mdkr64_headless_main(), so the launcher shares the exact engine
// boot path rather than a parallel one that could drift.
int mdkr64_engine_boot(const MdkrBootConfig *cfg);

// --- Fatal-crash write mirror (Windows) ------------------------------------
// DiagLog_install() tees stdout/stderr through a pipe drained by a reader
// thread. That thread dies with the process, so a crash-time write to stderr
// can be lost or block forever on a full pipe — exactly when the diagnostic
// matters most. The tee therefore publishes the PRE-tee console fd and the raw
// mdkr64.log fd here, and the engine's crash handler writes to them directly,
// bypassing the pipe. -1 means the tee is not installed (every CLI invocation),
// in which case stderr is already the real console and needs no bypass.
// Defined in platform/main_pc.c.
extern int g_diagLogRealErrFd;
extern int g_diagLogFileFd;

// --- In-game overlay hooks (platform/app_overlay_hooks.c) ------------------
// The app registers these before boot; the engine's event pump and frame-end
// call them. Keeps the C engine free of any ImGui/C++ dependency.
typedef struct {
    void (*process_event)(const void *sdl_event);
    int  (*wants_input)(void);
    void (*render)(void);
} AppOverlayHooks;
void platformSetOverlayHooks(const AppOverlayHooks *hooks);

#ifdef __cplusplus
}
#endif

#endif  // MDKR64_ENGINE_ENTRY_H
