// app_host.h — the app shell's host: owns the SDL window, render device, ImGui.
//
// Picks its backend at init() from mdkr_render_backend() — the SAME selector the
// engine uses (MDKR_RENDERER), so the launcher and the game can never disagree
// about which device the window is backed by. WebGPU is the qualified default
// (Metal-layer window + wgpu device/surface, with UI drawn by
// gfx_webgpu_imgui); MDKR_RENDERER=gl selects the diagnostic GL window/context
// and imgui_impl_opengl3 path.
//
// Either way the engine adopts the same objects (platformSetHostWindow +
// platformSetHostWebGpu), so the launcher and the game render into ONE window
// rather than the shell closing and the engine opening a second one.
#ifndef MDKR64_APP_HOST_H
#define MDKR64_APP_HOST_H

#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

class AppHost {
public:
    AppHost() = default;
    AppHost(const AppHost &) = delete;
    AppHost &operator=(const AppHost &) = delete;
    AppHost(AppHost &&) = delete;
    AppHost &operator=(AppHost &&) = delete;

    // Create the window + render context + ImGui. Returns false on failure.
    bool init(const char *title, int width, int height);

    // Begin an ImGui frame (and clear, on GL).
    void beginFrame();

    // Render ImGui draw data and present. When captureBmpPath is non-null the
    // finished frame is written as a 24-bit BMP before the swap — this is what
    // makes the headless shell smoke gate honest: it proves pixels, not just a
    // return code. Returns false when a requested capture was not fully written
    // or the host renderer entered an unrecoverable state.
    bool endFrame(const char *captureBmpPath = nullptr);

    // Framebuffer/logical ratio (2.0 on Retina). Valid after init().
    float framebufferScale() const;

    // Pump SDL events into ImGui. Returns true on quit (close / Cmd-Q).
    bool pumpAndShouldQuit();

    // As above, but sleep until an event or a bounded timeout. Used while a
    // minimized/occluded surface cannot present so the launcher never spins.
    bool waitAndPump(int timeoutMs);

    // Queue one synthetic file drop for the next event pump. This is only for
    // the headless launcher smoke: the pump builds an SDL_DROPFILE and sends it
    // through the same handler as a platform-delivered drop. It deliberately
    // does not SDL_PushEvent() a reserved drop event; SDL2 compatibility layers
    // are not required to support application-generated platform events.
    void queueDropFileForSmoke(const char *path);

    // Queue a complete key press through the same SDL -> ImGui event adapter as
    // physical keyboard input. The key is held for one rendered frame so
    // ImGui's input-trickling behavior is exercised faithfully.
    void queueKeyPressForSmoke(SDL_Keycode key);
    bool queueGamepadPressForSmoke(SDL_GameControllerButton button);
    void queueMouseClickForSmoke(int x, int y);
    // Queue one step of a held left-button drag. `held=true` begins/continues
    // the drag; `held=false` releases it. Enabled only by the dedicated scale
    // smoke environment and routed through the production SDL event adapter.
    bool queueMouseDragStepForSmoke(int x, int y, bool held);
    // As above, but mark the SDL mouse event as touch-generated. This exercises
    // ImGui's production touchscreen source path and is gated by its own smoke
    // contract so interactive input can never synthesize a gesture.
    bool queueTouchDragStepForSmoke(int x, int y, bool held);

    // Path of a file dragged onto the window since the last call, or "" — the
    // ROM panel's drag-and-drop entry point. Returns and clears.
    std::string takeDroppedFile();

    void shutdown();

    SDL_Window   *window()    const { return window_; }
    SDL_GLContext glContext() const { return gl_; }
    int drawableWidth()  const;
    int drawableHeight() const;
    std::uint64_t presentedFrames() const { return presentedFrames_; }
    bool lastSurfaceWasOccluded() const;

    bool usingWebGpu() const { return useWebGpu_; }

    // Engine-side WebGPU recovery calls this through the host-window bridge in
    // PREPARE/COMMIT/ABORT phases. The split guarantees all engine children of
    // the failed borrowed device are released before AppHost replaces roots.
    int recoverWebGpuRoots(int phase);
    const std::string &webGpuRecoveryError() const {
        return wgpuRecoveryError_;
    }

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
    bool configureWgpuSurface(int w, int h);
    void ensureWgpuCaptureTarget(int w, int h);
    bool endFrameWebGpu(const char *captureBmpPath);
    bool endFrameGL(const char *captureBmpPath);
    bool processEvent(SDL_Event &event);
    void releaseSmokeMouseClick();

    SDL_Window   *window_ = nullptr;
    SDL_GLContext gl_     = nullptr;
    bool imguiContextReady_  = false;
    bool imguiSdlReady_      = false;
    bool imguiRendererReady_ = false;
    bool sdlOwned_        = false;
    std::string droppedFile_;
    std::string pendingSmokeDrop_;
    std::vector<SDL_Keycode> pendingSmokeKeys_;
    SDL_Keycode smokeHeldKey_ = SDLK_UNKNOWN;
    struct SmokeClick { int x; int y; };
    std::vector<SmokeClick> pendingSmokeClicks_;
    SmokeClick smokeHeldClick_ = {0, 0};
    bool smokeClickHeld_ = false;
    bool smokeClickReleasePending_ = false;
    struct SmokeDragStep { int x; int y; bool held; bool touch; };
    std::vector<SmokeDragStep> pendingSmokeDragSteps_;
    bool smokeDragHeld_ = false;
    std::vector<SDL_GameControllerButton> pendingSmokeGamepadButtons_;
    SDL_GameController *smokeGamepad_ = nullptr;
    int smokeGamepadDeviceIndex_ = -1;
    int smokeHeldGamepadButton_ = -1;

    bool  useWebGpu_    = false;
    bool  glSoftwarePacing_ = false;
    Uint64 glLastPresentCounter_ = 0;
    int dpiSmokeFrame_ = 0;
    void *metalView_    = nullptr;   // SDL_MetalView backing the surface layer (macOS)
    void *wgpuInstance_ = nullptr;
    void *wgpuAdapter_  = nullptr;
    void *wgpuDevice_   = nullptr;
    void *wgpuQueue_    = nullptr;
    void *wgpuSurface_  = nullptr;
    int   wgpuFormat_   = 0;
    bool  wgpuSurfaceConfigured_ = false;
    std::uint64_t presentedFrames_ = 0;
    std::uint64_t wgpuPresentAttempts_ = 0;
    std::uint64_t wgpuUnavailableFrames_ = 0;
    std::uint64_t wgpuEncodeFailures_ = 0;
    std::uint64_t wgpuCaptureRequests_ = 0;
    std::uint64_t wgpuCaptureFailures_ = 0;
    int wgpuLastSurfaceStatus_ = -1;
    bool wgpuFatal_ = false;
    unsigned cfgW_ = 0, cfgH_ = 0;   // last-configured surface pixel size
    // Allocated only for an explicit screenshot. Normal launcher frames render
    // directly into the acquired surface texture, avoiding an offscreen pass
    // and full-frame copy on every present. Capture stays window-independent by
    // rendering a second identical pass here when a BMP was requested.
    void *captureTex_  = nullptr;
    void *captureView_ = nullptr;
    unsigned captureW_ = 0, captureH_ = 0;
    void *recoveryInstance_ = nullptr;
    void *recoveryAdapter_ = nullptr;
    void *recoveryDevice_ = nullptr;
    void *recoveryQueue_ = nullptr;
    void *recoverySurface_ = nullptr;
    int recoveryFormat_ = 0;
    std::string wgpuRecoveryError_;
};

#endif  // MDKR64_APP_HOST_H
