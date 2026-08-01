# The WebGPU backend

> **This work has landed.** Written ahead of the M4.5 wave and kept because the
> vendoring list, the wgpu-native dependency and the CMake wiring below are
> still an accurate description of how the backend is built.

WebGPU is selectable natively; GL is the native throughput default. WebGPU is the only
in-browser backend, so this was the bridge to the browser build.

## De-risking (verified)
`gfx_webgpu.c` implements `struct GfxRenderingAPI gfx_webgpu_api` — the SAME vtable
our F3DDKR renderer (gfx_pc_dkr.c) already drives (gfx_rendering_api.h: init/
start_frame/end_frame/draw_triangles/upload_texture/select_texture/set_depth_mode/
set_blend_mode/create_and_load_new_shader/read_framebuffer_rgb/…). So the backend is
a true drop-in at the existing seam: `gfx_init(&gfx_webgpu_api)` instead of
`&gfx_opengl_api`. Our renderer is backend-agnostic below the seam — nothing in the
F3DDKR front-end needs to change. Backend selection in mgb64 is env-var driven
(GE007_RENDERER); mirror as MDKR_RENDERER=webgpu|gl|metal.

## Vendor from mgb64 (src/platform/fast3d/)
- gfx_webgpu.c (~205KB), gfx_webgpu.h
- gfx_webgpu_shader.c, gfx_webgpu_shader.h
- gfx_webgpu_compat.h (WGPU_COMPAT_WAIT + native/emscripten WebGPU shims)
- (ImGui bits gfx_webgpu_imgui.* are optional — skip unless we add the ImGui overlay)
Check each for GE-specific assumptions (framebuffer dims, GE combiner quirks); the
shader generator consumes the same gfx_cc.c combiner output we already use, so it
should port clean. The pipeline-prewarm-cache (ge007_pipecache.txt equivalent) is an
optional perf add — defer; on first bring-up let pipelines compile on demand.

## Dependency (native)
- wgpu-native (Rust-built WebGPU impl; dispatches to Metal on macOS / D3D12 / Vulkan).
  mgb64 links a pinned `webgpu` target. Check how mgb64's CMake finds/builds it
  (CMakeLists ~104-136, the MGB64_WEBGPU_BACKEND block) and replicate: either vendor
  the prebuilt lib or fetch the pinned release. macOS arm64 prebuilt exists.
- IMPORTANT (web): under emscripten NO wgpu-native is needed — Emscripten supplies
  WebGPU (-sUSE_WEBGPU=1). So the same gfx_webgpu.c compiles for both; only the
  native build links wgpu-native. (This is why WebGPU is the browser path.)

## CMake changes
- `option(MDKR_WEBGPU_BACKEND ... ON)`; when ON, add gfx_webgpu*.c to the build,
  define MDKR_WEBGPU_BACKEND, link the webgpu target. Keep GL + Metal compiled too so
  MDKR_RENDERER can switch at runtime.
- Backend selector (platform/main_pc.c or a gfx_backend chooser): read MDKR_RENDERER,
  default gl, and keep webgpu selectable. Pass the chosen `&gfx_*_api` to gfx_init.

## Validation — achieved

- `MDKR_RENDERER=webgpu ./build/mdkr64` renders the title + OPTIONS menu + a race
  identically to GL (dump frames both backends, compare — they should match modulo
  minor filtering). 20× crash loop on char-select + race under webgpu = 0 crashes.
- `MDKR_RENDERER=gl` still works (fallback intact).
- Native queue production is explicitly bounded. WebGPU registers one
  completion after each frame submission and allows at most two outstanding;
  native waits for the older completion, while the browser never blocks inside
  renderer code and can omit a saturated rAF render attempt. GL interval 0
  inserts completion fences with the same ceiling (FIFO swap remains its own
  bound). Both report submission/completion/high-water/wait/rate telemetry.
- Hidden or minimized native windows stop real display-list walks and retained
  replay, service input/audio/fixed ticks at authored cadence, then reset
  presentation history on resume. `check_gpu_backpressure.py` and
  `check_surface_suspension.py` exercise both production backends.

## Then → M8 browser
Once WebGPU had been proven natively, M8 (docs/architecture/web.md) reused the
exact same gfx_webgpu.c under emscripten. Native now defaults to the faster GL
path; the renderer, OS shim (cooperative single thread), arena, and dkrptr32
tokens remain web-safe.
