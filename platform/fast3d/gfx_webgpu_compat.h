// gfx_webgpu_compat.h — single seam between wgpu-native (desktop) and
// emdawnwebgpu (browser). Everything dialect-specific in gfx_webgpu.c goes
// through these three primitives so the 4k-line backend stays shared.
//
// Browser rule: WebGPU is async-only; a spin that never yields to the JS
// event loop deadlocks. Under Asyncify, emscripten_sleep() unwinds the wasm
// stack and lets mapAsync/requestAdapter callbacks fire — so the SAME
// "loop until done" call-sites work on both dialects with a different pump.
//
// Seam discipline (web-port design note 2026-07-15, Task W7):
// ALL dialect divergence lives HERE. gfx_webgpu.c carries no inline
// `#ifdef __EMSCRIPTEN__` outside this include and the wgpuCompatCreateSurface
// bodies.
#ifndef GFX_WEBGPU_COMPAT_H
#define GFX_WEBGPU_COMPAT_H

#ifdef __EMSCRIPTEN__
  #include <webgpu/webgpu.h>          /* emdawnwebgpu: modern unified header */
  #include <emscripten/emscripten.h>
  #include <emscripten/html5.h>
  /* emdawnwebgpu's webgpu.h omits the WGPU_TRUE/WGPU_FALSE convenience macros
   * that wgpu-native's header provides. WGPUBool is a uint32_t on both dialects,
   * so define them here at the seam — keeps gfx_webgpu.c dialect-agnostic (the
   * depth-clip-control unclippedDepth assignments use them). */
  #ifndef WGPU_TRUE
  #define WGPU_TRUE ((WGPUBool)1)
  #endif
  #ifndef WGPU_FALSE
  #define WGPU_FALSE ((WGPUBool)0)
  #endif
  /* Browser: yield to the event loop so the JS-side promise resolves, THEN
   * drain the future queue so AllowProcessEvents callbacks actually dispatch.
   * emscripten_sleep() alone resolves the promise but never fires the C
   * callback (it only runs during ProcessEvents), so both steps are required. */
  /* NOTE: `device` is accepted for call-site symmetry with the native pump
   * below but is otherwise unused/discarded here without being evaluated —
   * call sites must not pass a side-effecting expression for it. */
  #define WGPU_COMPAT_PUMP(instance, device)                    \
      do {                                                      \
          emscripten_sleep(1);                                  \
          if (instance) wgpuInstanceProcessEvents((instance));  \
      } while (0)
  /* PERF-005: drain outstanding async render-pipeline creations so their
   * AllowProcessEvents callbacks dispatch at the renderer-owned boundary. Called once per frame in
   * wgpu_end_frame, and ONLY when a create is in flight (s_pending_pipelines>0)
   * — a no-op otherwise, so a steady state never touches unrelated futures.
   * Unlike WGPU_COMPAT_PUMP this does not yield to the event loop (no
   * emscripten_sleep): end_frame is not an Asyncify unwind point, and the
   * spontaneous callbacks only need a ProcessEvents dispatch here. */
  #define WGPU_COMPAT_DRAIN(instance)                           \
      do { if (instance) wgpuInstanceProcessEvents((instance)); } while (0)
  /* Queue-completion callback compatibility. emdawnwebgpu omits the message
   * parameter that current wgpu-native supplies; both use callback-info
   * futures. Keep frame-in-flight accounting shared in gfx_webgpu.c. */
  #define WGPU_COMPAT_QUEUE_DONE_CALLBACK(name)                \
      static void name(WGPUQueueWorkDoneStatus status, void *userdata, \
                       void *userdata2)
  #define WGPU_COMPAT_QUEUE_DONE_UNUSED() ((void)userdata2)
  #define WGPU_COMPAT_QUEUE_ON_DONE(queue, cb, userdata_value) \
      do {                                                      \
          WGPUQueueWorkDoneCallbackInfo _wc_info =              \
              WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;          \
          _wc_info.mode = WGPUCallbackMode_AllowProcessEvents;  \
          _wc_info.callback = (cb);                             \
          _wc_info.userdata1 = (userdata_value);                \
          (void)wgpuQueueOnSubmittedWorkDone((queue), _wc_info);\
      } while (0)
  #define WGPU_COMPAT_QUEUE_POLL(instance, device)             \
      do {                                                      \
          (void)(device);                                       \
          if (instance) wgpuInstanceProcessEvents((instance));  \
      } while (0)
  /* Waiting here would require a new Asyncify boundary inside renderer code.
   * The browser instead returns to rAF and skips a render attempt when the
   * bounded queue is saturated. */
  #define WGPU_COMPAT_QUEUE_CAN_BLOCK 0
  #define WGPU_COMPAT_QUEUE_BLOCK(instance, device)            \
      WGPU_COMPAT_QUEUE_POLL((instance), (device))
  /* Browser: the canvas is presented automatically when the frame's JS task
   * yields (via requestAnimationFrame); emscripten's WebGPU binding ABORTS on
   * an explicit wgpuSurfacePresent, so presenting is a no-op here. */
  #define WGPU_COMPAT_PRESENT(surface) ((void)(surface))
  /* WEB-026: one-time bring-up waits (adapter + device) get a bigger budget on
   * the browser, where cold-start adapter enumeration under Asyncify (each pump
   * is a ~4ms setTimeout-clamped emscripten_sleep) can exceed the native budget
   * on exactly the slow machines where bring-up is fragile. Native drives
   * wgpu-native's poll synchronously, so 1000 is already generous there. */
  #define WGPU_COMPAT_BRINGUP_WAIT_ITERS 10000
  /* WEB-049: on the browser take the surface's platform-preferred format
   * (caps.formats[0]) rather than forcing BGRA8 where the platform prefers
   * RGBA8 — that mismatch costs a per-present swizzle on some Android GPUs. */
  #define WGPU_COMPAT_PREFER_FIRST_SURFACE_FORMAT 1
  #define WGPU_COMPAT_SURFACE_OCCLUDED(status) false
  /* emdawnwebgpu currently leaves WGPUSurfaceCapabilities.usages at zero.
   * Browser surfaces are render attachments by contract; zero means the mask
   * is unavailable, not that RenderAttachment is unsupported. */
  #define WGPU_COMPAT_SURFACE_USAGES_REPORTED 0
#else
  #include <webgpu/webgpu.h>
  #include <webgpu/wgpu.h>            /* wgpu-native extensions             */
  /* Native: drive wgpu-native's event machinery.                           */
  #define WGPU_COMPAT_PUMP(instance, device)                    \
      do {                                                      \
          if (device)        wgpuDevicePoll((device), true, NULL); \
          else if (instance) wgpuInstanceProcessEvents((instance)); \
      } while (0)
  /* PERF-005: native never creates pipelines asynchronously (wgpu_pipeline_for
   * always takes the synchronous path off-web), so the completion drain is a
   * no-op — a pipeline is ready the moment wgpuDeviceCreateRenderPipeline
   * returns. Keeps wgpu_end_frame free of an inline __EMSCRIPTEN__ guard. */
  #define WGPU_COMPAT_DRAIN(instance) ((void)(instance))
  #define WGPU_COMPAT_QUEUE_DONE_CALLBACK(name)                         \
      static void name(WGPUQueueWorkDoneStatus status,                  \
                       WGPUStringView message, void *userdata,           \
                       void *userdata2)
  #define WGPU_COMPAT_QUEUE_DONE_UNUSED()                               \
      do { (void)message; (void)userdata2; } while (0)
  #define WGPU_COMPAT_QUEUE_ON_DONE(queue, cb, userdata_value)          \
      do {                                                              \
          WGPUQueueWorkDoneCallbackInfo _wc_info =                      \
              WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;                  \
          _wc_info.mode = WGPUCallbackMode_AllowProcessEvents;          \
          _wc_info.callback = (cb);                                     \
          _wc_info.userdata1 = (userdata_value);                        \
          (void)wgpuQueueOnSubmittedWorkDone((queue), _wc_info);        \
      } while (0)
  #define WGPU_COMPAT_QUEUE_POLL(instance, device)                      \
      do {                                                              \
          (void)(instance);                                             \
          if (device) wgpuDevicePoll((device), false, NULL);            \
      } while (0)
  #define WGPU_COMPAT_QUEUE_CAN_BLOCK 1
  #define WGPU_COMPAT_QUEUE_BLOCK(instance, device)                     \
      do {                                                              \
          (void)(instance);                                             \
          if (device) wgpuDevicePoll((device), true, NULL);             \
      } while (0)
  /* Native: wgpu-native presents the surface explicitly. */
  #define WGPU_COMPAT_PRESENT(surface) wgpuSurfacePresent((surface))
  /* WEB-026: native bring-up waits drive wgpuDevicePoll synchronously — 1000
   * pumps is already generous; keep it unchanged so native timing is untouched. */
  #define WGPU_COMPAT_BRINGUP_WAIT_ITERS 1000
  /* WEB-049: native keeps the long-standing BGRA8-preferring surface-format
   * scan so the offscreen scene target format and readback swizzle stay
   * byte-identical to every recorded baseline (Metal already prefers BGRA8). */
  #define WGPU_COMPAT_PREFER_FIRST_SURFACE_FORMAT 0
  #define WGPU_COMPAT_SURFACE_OCCLUDED(status)                              \
      ((status) == (WGPUSurfaceGetCurrentTextureStatus)                     \
                       WGPUSurfaceGetCurrentTextureStatus_Occluded)
  #define WGPU_COMPAT_SURFACE_USAGES_REPORTED 1
#endif

/* WEB-003 / WEB-010 / WEB-025: hand a human-readable bring-up/device-lost
 * failure to the JS shell so the user sees a message instead of an inert black
 * canvas. On web this routes to window.mdkr64ShowError (mdkr64-shell.js), which
 * swaps the canvas for an error panel; on native it is a no-op (the failure is
 * already logged to stderr). Kept in the seam so gfx_webgpu.c stays free of
 * inline __EMSCRIPTEN__ guards. `msg` must be a C string. */
#ifdef __EMSCRIPTEN__
  #define WGPU_COMPAT_REPORT_FAILURE(msg)                                      \
      EM_ASM({                                                                 \
          if (typeof window !== 'undefined' && window.mdkr64ShowError)         \
              window.mdkr64ShowError(UTF8ToString($0));                        \
      }, (msg))
#else
  #define WGPU_COMPAT_REPORT_FAILURE(msg) ((void)(msg))
#endif

/* Loop until `cond` is nonzero or max_iters pumps elapse. Identical shape to
 * the existing spins; only the pump differs per dialect.                    */
#define WGPU_COMPAT_WAIT(cond, instance, device, max_iters)                  \
    do {                                                                     \
        for (int _wc_i = 0; !(cond) && _wc_i < (max_iters); ++_wc_i) {       \
            WGPU_COMPAT_PUMP((instance), (device));                          \
        }                                                                    \
    } while (0)

struct SDL_Window;
/* Create a surface for the dialect: native window descriptors on desktop,
 * the #canvas selector in the browser. Implemented in gfx_webgpu.c —
 * the desktop `#else` body holds the Metal-layer / Win32 / X11 / Wayland
 * descriptor code (wgpu-native types kept off the browser compile); the
 * browser body builds the canvas descriptor.
 *
 * NOTE: carries `metal_layer` in addition to the plan's `window` because the
 * native app shell (app_host.cpp → gfx_webgpu_bringup) owns a window/layer
 * distinct from platformGetMetalLayer(); dropping it would break the shell's
 * surface. Browser body ignores both handles (the page owns one canvas).     */
WGPUSurface wgpuCompatCreateSurface(WGPUInstance instance, void *metal_layer,
                                    struct SDL_Window *window);

#endif /* GFX_WEBGPU_COMPAT_H */
