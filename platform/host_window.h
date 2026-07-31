// host_window.h — app-shell window/context handoff seam.
//
// When MGB64's in-process app shell owns the SDL window + GL context, it
// registers them here BEFORE booting the engine. platformInitSDL() then adopts
// them instead of creating its own, so the launcher and the game render into
// one window. When nothing is registered (automation/CLI path, or the bare
// -DMGB64_APP=OFF engine), the engine creates its own window exactly as before
// — the automation path stays byte-identical.
#ifndef MGB64_HOST_WINDOW_H
#define MGB64_HOST_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

// Register (or clear, with NULL) the host window + GL context. Call before the
// engine boot. Passing a NULL window clears the host state.
void platformSetHostWindow(void *sdl_window, void *gl_context);

// 1 if a host window is registered, else 0.
int platformHasHostWindow(void);

void *platformHostWindow(void);      // SDL_Window*
void *platformHostGLContext(void);   // SDL_GLContext

// Register (or clear) the app shell's WebGPU objects so the engine adopts them
// instead of creating its own device/surface. Call before the engine boot,
// after platformSetHostWindow. All handles are opaque (WGPU* as void*);
// surface_format is the WGPUTextureFormat the surface is configured with.
// A host WebGPU is considered present only when device+queue+surface are set.
void platformSetHostWebGpu(void *instance, void *adapter, void *device,
                           void *queue, void *surface, int surface_format);

int   platformHasHostWebGpu(void);          // 1 if a host WebGPU is registered
void *platformHostWgpuInstance(void);       // WGPUInstance
void *platformHostWgpuAdapter(void);        // WGPUAdapter
void *platformHostWgpuDevice(void);         // WGPUDevice
void *platformHostWgpuQueue(void);          // WGPUQueue
void *platformHostWgpuSurface(void);        // WGPUSurface
int   platformHostWgpuSurfaceFormat(void);  // WGPUTextureFormat

#ifdef __cplusplus
}
#endif

#endif  // MGB64_HOST_WINDOW_H
