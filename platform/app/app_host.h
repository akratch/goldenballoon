// app_host.h — the app shell's host: owns the SDL window, render device, ImGui.
//
// Picks its backend at init() from mdkr_render_backend() — the SAME selector the
// engine uses (MDKR_RENDERER), so the launcher and the game can never disagree
// about which device the window is backed by. Default is GL (a GL window +
// context + imgui_impl_opengl3); MDKR_RENDERER=webgpu selects the Metal-layer
// window + wgpu device/surface, with UI drawn by gfx_webgpu_imgui.
//
// Either way the engine adopts the same objects (platformSetHostWindow +
// platformSetHostWebGpu), so the launcher and the game render into ONE window
// rather than the shell closing and the engine opening a second one.
#ifndef MDKR64_APP_HOST_H
#define MDKR64_APP_HOST_H

#include <SDL.h>

#include <string>

class AppHost {
public:
    // Create the window + render context + ImGui. Returns false on failure.
    bool init(const char *title, int width, int height);

    // Begin an ImGui frame (and clear, on GL).
    void beginFrame();

    // Render ImGui draw data and present. When captureBmpPath is non-null the
    // finished frame is written as a 24-bit BMP before the swap — this is what
    // makes the headless shell smoke gate honest: it proves pixels, not just a
    // return code. Returns false ONLY when a capture was requested and its BMP
    // was not fully written.
    bool endFrame(const char *captureBmpPath = nullptr);

    // Framebuffer/logical ratio (2.0 on Retina). Valid after init().
    float framebufferScale() const;

    // Pump SDL events into ImGui. Returns true on quit (close / Cmd-Q).
    bool pumpAndShouldQuit();

    // Queue one synthetic file drop for the next event pump. This is only for
    // the headless launcher smoke: the pump builds an SDL_DROPFILE and sends it
    // through the same handler as a platform-delivered drop. It deliberately
    // does not SDL_PushEvent() a reserved drop event; SDL2 compatibility layers
    // are not required to support application-generated platform events.
    void queueDropFileForSmoke(const char *path);

    // Path of a file dragged onto the window since the last call, or "" — the
    // ROM panel's drag-and-drop entry point. Returns and clears.
    std::string takeDroppedFile();

    void shutdown();

    SDL_Window   *window()    const { return window_; }
    SDL_GLContext glContext() const { return gl_; }
    int drawableWidth()  const;
    int drawableHeight() const;

    bool usingWebGpu() const { return useWebGpu_; }

    // WebGPU host objects (opaque WGPU* as void*, null on GL) for the engine to
    // adopt via platformSetHostWebGpu().
    void *wgpuInstance() const { return wgpuInstance_; }
    void *wgpuAdapter()  const { return wgpuAdapter_; }
    void *wgpuDevice()   const { return wgpuDevice_; }
    void *wgpuQueue()    const { return wgpuQueue_; }
    void *wgpuSurface()  const { return wgpuSurface_; }
    int   wgpuFormat()   const { return wgpuFormat_; }

private:
    bool initWebGpu(const char *title, int width, int height);
    bool initGL(const char *title, int width, int height);
    void drawableSize(int *w, int *h) const;
    void configureWgpuSurface(int w, int h);
    void ensureWgpuSceneTarget(int w, int h);
    bool endFrameWebGpu(const char *captureBmpPath);
    bool endFrameGL(const char *captureBmpPath);
    bool processEvent(SDL_Event &event);

    SDL_Window   *window_ = nullptr;
    SDL_GLContext gl_     = nullptr;
    bool imguiReady_      = false;
    bool sdlOwned_        = false;
    std::string droppedFile_;
    std::string pendingSmokeDrop_;

    bool  useWebGpu_    = false;
    void *metalView_    = nullptr;   // SDL_MetalView backing the surface layer (macOS)
    void *wgpuInstance_ = nullptr;
    void *wgpuAdapter_  = nullptr;
    void *wgpuDevice_   = nullptr;
    void *wgpuQueue_    = nullptr;
    void *wgpuSurface_  = nullptr;
    int   wgpuFormat_   = 0;
    unsigned cfgW_ = 0, cfgH_ = 0;   // last-configured surface pixel size
    // Offscreen scene target: the UI renders here (window-independent), then is
    // blitted to the surface for present and read back for capture — the same
    // decoupling gfx_webgpu.c uses, so a hidden window still produces frames.
    void *sceneTex_  = nullptr;
    void *sceneView_ = nullptr;
    unsigned sceneW_ = 0, sceneH_ = 0;
};

#endif  // MDKR64_APP_HOST_H
