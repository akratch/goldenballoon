/**
 * gfx_webgpu_stubs.c — satisfiers for optional shell-side WebGPU symbols
 * backend (platform/fast3d/gfx_webgpu.c) references. Compiled ONLY when
 * MDKR_WEBGPU_BACKEND is on.
 *
 * The backend originated in mgb64 and retains optional subsystem hooks. What is
 * real and what is inert here
 * depends on MDKR_APP:
 *
 *   - App-shell WebGPU handoff (platformHasHostWebGpu + the host getters).
 *     REAL under MDKR_APP: platform/host_window.c holds the launcher's
 *     device/surface so the game renders into the same window. Without the
 *     shell there is no host, so the inert versions below return 0/NULL and
 *     wgpu_init does its own bring-up.
 *   - In-game ImGui overlay (platformOverlayRender / platformOverlayWantsInput).
 *     REAL under MDKR_APP: platform/app_overlay_hooks.c dispatches to the F1
 *     overlay in platform/app/ui_overlay.cpp. Inert without the shell.
 *   - Minimap/radar overlay (minimap_overlay_draw_queued_frames_webgpu): an
 *     mgb64 remaster HUD feature. DKR has its own in-game map drawn by the game
 *     itself, so this stays a genuine no-op in BOTH configurations — there is no
 *     second, shell-drawn minimap to turn on.
 *   - The pipeline-prewarm cache (savedirPath): only reached via
 *     gfx_webgpu_set_stage(), which DKR's game loop never calls, so prewarm stays
 *     dormant (s_prewarm_cur_stage == -1) and savedirPath is never invoked at
 *     runtime — it exists only to link. Prewarm is deferred per docs/M4.5 plan.
 *
 * g_deterministic mirrors mgb64's global (0 = normal). The prewarm gate reads it.
 */
#include <stddef.h>
#include <stdlib.h>

/* mgb64's determinism flag (src/platform/...); the webgpu prewarm gate reads it.
 * mdkr64 has no --deterministic mode, so it is a constant 0. */
int g_deterministic = 0;

/* Screenshot-session signals read by wgpu_readback_possible() to decide
 * whether to retain the offscreen present target for a post-present readback.
 * This port drives no such session — the frame-dump path (platform_dump_frame ->
 * gfx_read_framebuffer_rgb) latches s_readback_latched inside the backend on its
 * first read, so these stay inert (no active session): a value >= 0 / nonzero
 * would mean "a capture is armed". In headless (hidden-window) runs the backend
 * always keeps the offscreen path anyway (no drawable to present into directly),
 * so every dumped frame is captured from the offscreen scene target. */
int g_screenshotFrameSessionActive = 0;
int g_autoScreenshotFrame          = -1;
int g_autoScreenshotGameTimer      = -1;

/*
 * MDKR_APP: the first two groups below are no longer stubs. The native app shell
 * (platform/app/) really does stand up its own device and really does draw an
 * in-game overlay, so host_window.c and app_overlay_hooks.c provide the genuine
 * implementations and these must NOT be compiled — two definitions of
 * platformHasHostWebGpu is a duplicate-symbol link error, not a merge. Only the
 * minimap and savedir satisfiers below survive into the app build.
 */
#ifndef MDKR_APP

/* ---- App-shell WebGPU handoff (host device adoption) — none without the shell. */
int   platformHasHostWebGpu(void)        { return 0; }
void *platformHostWgpuInstance(void)     { return NULL; }
void *platformHostWgpuAdapter(void)      { return NULL; }
void *platformHostWgpuDevice(void)       { return NULL; }
void *platformHostWgpuQueue(void)        { return NULL; }
void *platformHostWgpuSurface(void)      { return NULL; }
int   platformHostWgpuSurfaceFormat(void){ return 0; }
int   platformHasHostWebGpuRecovery(void){ return 0; }
int   platformRecoverHostWebGpu(int phase) { (void)phase; return 0; }

#endif /* !MDKR_APP */

/* ---- Minimap/radar overlay (mgb64 remaster HUD) — absent in mdkr64. ---- */
void minimap_overlay_draw_queued_frames_webgpu(int fb_width, int fb_height) {
    (void)fb_width; (void)fb_height;   /* no minimap overlay */
}

/* ---- Pipeline-prewarm cache path — never called (prewarm deferred). ---- */
const char *savedirPath(const char *filename) {
    /* Unreachable at runtime (DKR never calls gfx_webgpu_set_stage). Return the
     * bare filename so a hypothetical caller writes to CWD rather than crashing. */
    return filename;
}
