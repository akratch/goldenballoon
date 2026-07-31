// host_window.c — see host_window.h.
#include "host_window.h"

#include <stddef.h>

static void *g_hostWindow = NULL;
static void *g_hostGL     = NULL;
static int   g_hasHost    = 0;

void platformSetHostWindow(void *sdl_window, void *gl_context) {
    g_hostWindow = sdl_window;
    g_hostGL     = gl_context;
    g_hasHost    = (sdl_window != NULL);
}

int platformHasHostWindow(void) { return g_hasHost; }

void *platformHostWindow(void) { return g_hostWindow; }

void *platformHostGLContext(void) { return g_hostGL; }

/* --- WebGPU host handoff (app shell owns the device/surface) ------------- */
static void *g_hostWgpuInstance = NULL;
static void *g_hostWgpuAdapter  = NULL;
static void *g_hostWgpuDevice   = NULL;
static void *g_hostWgpuQueue    = NULL;
static void *g_hostWgpuSurface  = NULL;
static int   g_hostWgpuFormat   = 0;
static int   g_hasHostWgpu      = 0;

void platformSetHostWebGpu(void *instance, void *adapter, void *device,
                           void *queue, void *surface, int surface_format) {
    g_hostWgpuInstance = instance;
    g_hostWgpuAdapter  = adapter;
    g_hostWgpuDevice   = device;
    g_hostWgpuQueue    = queue;
    g_hostWgpuSurface  = surface;
    g_hostWgpuFormat   = surface_format;
    /* A host WebGPU is present only when the essential objects are all set. */
    g_hasHostWgpu = (device != NULL && queue != NULL && surface != NULL);
}

int   platformHasHostWebGpu(void)        { return g_hasHostWgpu; }
void *platformHostWgpuInstance(void)     { return g_hostWgpuInstance; }
void *platformHostWgpuAdapter(void)      { return g_hostWgpuAdapter; }
void *platformHostWgpuDevice(void)       { return g_hostWgpuDevice; }
void *platformHostWgpuQueue(void)        { return g_hostWgpuQueue; }
void *platformHostWgpuSurface(void)      { return g_hostWgpuSurface; }
int   platformHostWgpuSurfaceFormat(void){ return g_hostWgpuFormat; }
