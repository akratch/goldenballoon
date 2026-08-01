# The WebGPU backend

> **This work has landed.** This page describes the backend as it is built and
> shipped today.

WebGPU is the qualified native default and the only in-browser backend. GL is
still selectable explicitly for diagnostics while its visual-parity work
continues, so WebGPU remains the bridge between the native and browser builds.

## Rendering seam

`gfx_webgpu.c` implements `struct GfxRenderingAPI gfx_webgpu_api`, the same
vtable that the F3DDKR renderer in `gfx_pc_dkr.c` drives for OpenGL. Backend
selection resolves once at startup: the default passes `&gfx_webgpu_api` to
`gfx_init`, while `MDKR_RENDERER=gl` explicitly selects `&gfx_opengl_api`.
Startup and runtime failures are fail-closed and never switch renderers inside a
live process.

## Backend modules

- `gfx_webgpu.c` and `gfx_webgpu.h` own device/surface setup, rendering,
  presentation, readback, bounded queueing, and same-backend recovery.
- `gfx_webgpu_shader.*` translates the shared combiner output into WGSL.
- `gfx_webgpu_compat.h` is the native/emdawnwebgpu dialect boundary.
- `gfx_webgpu_lifecycle.*` and `gfx_webgpu_fault.*` centralize resource accounting
  and the public fault-injection vocabulary.
- `gfx_webgpu_imgui.*` renders the native launcher and in-game overlay through
  the same device used by the game.

The implementation originated in mgb64; the current ownership and adaptation
boundary is recorded in `platform/fast3d/PROVENANCE.md` and
`docs/MGB64_BACKFLOW.md`.

## Dependency (native)

- Native builds use an exact, SHA-256-verified wgpu-native release selected by
  `cmake/webgpu_artifact.cmake` and fetched through `cmake/webgpu.cmake`. It
  uses Metal on macOS and automatically selects a compatible native API on
  Windows (normally Vulkan or Direct3D 12) and Linux. `MDKR_RENDERER` selects
  WebGPU versus the diagnostic OpenGL renderer; it does not force a particular
  WebGPU-native API.
- Emscripten builds use the `emdawnwebgpu` port and do not link wgpu-native. The
  compatibility header lets the same `gfx_webgpu.c` compile for both dialects.

## CMake changes

- `MDKR_WEBGPU_BACKEND` defaults to `ON`. It adds the WebGPU modules, defines
  `MDKR_WEBGPU_BACKEND`, and links the `webgpu` target.
- Native builds also compile OpenGL for the explicit `MDKR_RENDERER=gl`
  diagnostic route. The standalone Metal backend is not built.
- Browser builds force WebGPU on and omit the OpenGL backend.

## Validation — achieved

- `MDKR_RENDERER=webgpu ./build/mdkr64` renders the title, OPTIONS menu, and race.
  Sparse comparisons originally measured close GL/WebGPU agreement, but dense
  opening-sequence captures later exposed localized GL corruption. WebGPU is the
  qualified release path until that diagnostic backend regains full parity.
- `MDKR_RENDERER=gl` still works as an explicit diagnostic selection. WebGPU
  startup or device-recovery failures are terminal and never switch renderers
  inside the live process.
- Production native WebGPU registers one completion after each newly authored
  image, polls callbacks without blocking at the next frame, and records zero
  gameplay-time completion waits. It may drain outstanding work during orderly
  shutdown, after gameplay/audio service has stopped. The browser and explicit
  internal replay stress retain the nonblocking two-frame admission ceiling;
  GL interval 0 retains its two-fence ceiling (FIFO swap remains its own bound).
  Telemetry separates runtime waits from aggregate/shutdown waits and reports
  submission, completion, high-water, wait, and achieved-rate accounting.
- Hidden or minimized native windows stop real display-list walks and retained
  replay, service input/audio/fixed ticks at authored cadence, then reset
  presentation history on resume. `check_gpu_backpressure.py` and
  `check_surface_suspension.py` exercise both production backends.

## Browser sharing

The M8 browser build compiles this repository's `gfx_webgpu.c` under Emscripten.
Native and browser therefore share WebGPU as their qualified path; the renderer,
cooperative OS shim, arena, and `dkrptr32` tokens remain web-safe.
