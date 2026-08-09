// app_host.cpp — see app_host.h.
#include "app_host.h"
#include "a11y_speech.h"
#include "fs_utf8.h"
#include "app_activation.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "app_window.h"
#include "crash_screen.h"
#include "engine_entry.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>

#if defined(__APPLE__)
#  include <OpenGL/gl3.h>
#else
// Non-Apple: the engine uses glad; the app shell resolves GL via SDL. We only
// call a couple of raw GL entry points (glClear/glViewport), declared here to
// avoid a hard glad dependency in the shell TU. They resolve at link time
// against the same GL the engine links.
#  include <glad/glad.h>
#endif

#ifdef MDKR_WEBGPU_BACKEND
#  include <webgpu/webgpu.h>
#  include <webgpu/wgpu.h>   /* wgpuDevicePoll */
#  include "gfx_webgpu.h"
#  include "gfx_webgpu_fault.h"
#  include "gfx_webgpu_imgui.h"
#endif

// Backend selector (platform/platform_sdl_min.c). The shell asks the SAME
// resolver the engine does — MDKR_RENDERER, validated and cached once — so the
// launcher can never bring up a device the engine would then refuse to adopt.
#include "platform_os.h"

// The GLSL version string handed to the ImGui GL3 backend. Matches the engine's
// "#version 330 core" shaders; valid on the macOS 4.1-core context too.
static const char *kGlslVersion = "#version 330 core";

static void bindSmokeGamepadToImGui(SDL_GameController *gamepad) {
    if (gamepad == nullptr) {
        return;
    }
    /* Keep the deterministic virtual device under this test's ownership.
     * ImGui's normal AutoFirst scan may select a user's physical controller.
     * Interactive launches have no smoke device and retain AutoFirst. */
    SDL_GameController *gamepads[] = {gamepad};
    ImGui_ImplSDL2_SetGamepadMode(
        ImGui_ImplSDL2_GamepadMode_Manual, gamepads, 1);
}

bool AppHost::init(const char *title, int width, int height) {
    active_ = true;
    /* Resolve before the availability check so diagnostics still name an
     * explicitly requested WebGPU backend in a GL-only build. */
    useWebGpu_ = (mdkr_render_backend() == MDKR_BACKEND_WEBGPU);
    if (!mdkr_render_backend_available()) {
        std::fprintf(stderr,
                     "[app] requested renderer is unavailable; stopping without "
                     "an automatic fallback.\n");
        return false;
    }
    const AppUiSmokeInputMode smokeInputMode = AppUi_smokeInputMode();
    smokeScriptOwnsInput_ =
        smokeInputMode == AppUiSmokeInputMode::Keyboard ||
        smokeInputMode == AppUiSmokeInputMode::Gamepad ||
        std::getenv("MDKR_APP_SMOKE_UI_SCALE_DRAG") != nullptr ||
        std::getenv("MDKR_APP_SMOKE_TOUCH_SCROLL") != nullptr ||
        std::getenv("MDKR_APP_SMOKE_NAV_TARGET") != nullptr;
    if (smokeInputMode == AppUiSmokeInputMode::Gamepad) {
        /* Hidden launcher automation has no keyboard focus. SDL otherwise
         * discards joystick press transitions while any window exists but none
         * is focused, leaving the virtual controller permanently neutral. This
         * override is reachable only through the validated smoke contract and
         * does not change interactive background-input policy. */
        SDL_SetHintWithPriority(
            SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_OVERRIDE);
    }
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) < 0) {
            std::fprintf(stderr, "[app] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        sdlOwned_ = true;
    }

    if (smokeInputMode == AppUiSmokeInputMode::Gamepad) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        smokeGamepadDeviceIndex_ = SDL_JoystickAttachVirtual(
            SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
            SDL_CONTROLLER_BUTTON_MAX, 0);
        if (smokeGamepadDeviceIndex_ < 0 ||
            !SDL_IsGameController(smokeGamepadDeviceIndex_)) {
            std::fprintf(stderr, "[app-ui-test] virtual gamepad attach failed: %s\n",
                         SDL_GetError());
            return false;
        }
        smokeGamepad_ = SDL_GameControllerOpen(smokeGamepadDeviceIndex_);
        if (!smokeGamepad_) {
            std::fprintf(stderr, "[app-ui-test] virtual gamepad open failed: %s\n",
                         SDL_GetError());
            return false;
        }
        std::fprintf(stderr, "[app-ui-test] virtual SDL gamepad connected\n");
#else
        std::fprintf(stderr,
                     "[app-ui-test] virtual gamepad requires SDL 2.0.14 or newer\n");
        return false;
#endif
    }

    bool ready;
#ifdef MDKR_WEBGPU_BACKEND
    // WebGPU is the qualified native default; MDKR_RENDERER=gl selects the GL
    // diagnostic path end to end. Either way both halves stay on one device.
    if (useWebGpu_) {
        ready = initWebGpu(title, width, height);
    } else
#endif
    {
        ready = initGL(title, width, height);
    }
    /* The crash screen can only show a player anything if a window exists, and
     * this is the only place in the process that knows one does. Registering
     * on success (and clearing in shutdown()) is what makes "headless does not
     * present" structural rather than a flag: automation never gets here. */
    if (ready) CrashScreen_setWindow(window_);
    return ready;
}

bool AppHost::initGL(const char *title, int width, int height) {
    // Request the SAME GL context attributes the engine uses (platform_sdl.c).
#if defined(MDKR_PORTMASTER_GLES)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#elif defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                   SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN |
                   AppWindow_creationFlags();
    window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               width, height, flags);
    if (!window_) {
        std::fprintf(stderr, "[app] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(window_, 640, 480);
    AppActivation_requestForeground(window_);

    gl_ = SDL_GL_CreateContext(window_);
    if (!gl_) {
        std::fprintf(stderr, "[app] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    if (SDL_GL_MakeCurrent(window_, gl_) != 0) {
        std::fprintf(stderr, "[app] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        return false;
    }
    /* Interactive launcher UI stays synchronized. Autoplay is a bounded host
     * handoff test and must not block forever in the first swap when the
     * command-launched NSWindow has not been serviced by the compositor; the
     * engine applies and validates the requested numeric/uncapped policy as
     * soon as it adopts this same context. */
    const int hostSwapInterval = std::getenv("MDKR_APP_AUTOPLAY") ? 0 : 1;
    const bool injectSwapFailure =
        std::getenv("MDKR_TEST_GL_SWAP_INTERVAL_FAILURE") != nullptr;
    if (injectSwapFailure || SDL_GL_SetSwapInterval(hostSwapInterval) != 0) {
        std::fprintf(stderr, "[app] SDL_GL_SetSwapInterval(%d) failed: %s\n",
                     hostSwapInterval,
                     injectSwapFailure ? "injected test failure" : SDL_GetError());
    }
    const int effectiveSwapInterval = SDL_GL_GetSwapInterval();
    glSoftwarePacing_ = hostSwapInterval > 0 && effectiveSwapInterval <= 0;
    if (glSoftwarePacing_) {
        std::fprintf(stderr,
                     "[app] OpenGL swap interval unavailable; using bounded 60 Hz "
                     "launcher pacing\n");
    }

#if !defined(__APPLE__)
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::fprintf(stderr, "[app] gladLoadGLLoader failed\n");
        return false;
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiContextReady_ = true;
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't litter imgui.ini in the cwd (persist later)
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    AppTheme::setup(framebufferScale());
    if (!ImGui_ImplSDL2_InitForOpenGL(window_, gl_)) {
        std::fprintf(stderr, "[app] ImGui_ImplSDL2_InitForOpenGL failed\n");
        return false;
    }
    imguiSdlReady_ = true;
    bindSmokeGamepadToImGui(smokeGamepad_);
    if (!ImGui_ImplOpenGL3_Init(kGlslVersion)) {
        std::fprintf(stderr, "[app] ImGui_ImplOpenGL3_Init failed\n");
        return false;
    }
    imguiRendererReady_ = true;
    std::fprintf(stderr, "[app] host: OpenGL\n");
    return true;
}

#ifdef MDKR_WEBGPU_BACKEND
bool AppHost::initWebGpu(const char *title, int width, int height) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                   SDL_WINDOW_SHOWN | AppWindow_creationFlags();
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;   // wgpu-native wraps the CAMetalLayer as its surface
#endif
    window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               width, height, flags);
    if (!window_) {
        std::fprintf(stderr, "[app] SDL_CreateWindow (WebGPU) failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(window_, 640, 480);

    void *layer = nullptr;
#if defined(__APPLE__)
    metalView_ = SDL_Metal_CreateView(window_);
    if (!metalView_) {
        std::fprintf(stderr, "[app] SDL_Metal_CreateView failed: %s\n", SDL_GetError());
        return false;
    }
    layer = SDL_Metal_GetLayer((SDL_MetalView)metalView_);
#endif
    /* On macOS order the native NSWindow only after SDL has attached the Metal
     * view/CAMetalLayer. Otherwise a command-launched app can be foregrounded
     * while the surface still remains permanently Occluded. */
    AppActivation_requestForeground(window_, metalView_);

    WGPUInstance inst = nullptr; WGPUAdapter adapter = nullptr; WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr; WGPUSurface surface = nullptr; int fmt = 0;
    if (!gfx_webgpu_bringup(layer, window_, &inst, &adapter, &device, &queue, &surface, &fmt)) {
        std::fprintf(stderr, "[app] WebGPU bring-up failed\n");
        return false;
    }
    wgpuInstance_ = inst; wgpuAdapter_ = adapter; wgpuDevice_ = device;
    wgpuQueue_ = queue; wgpuSurface_ = surface; wgpuFormat_ = fmt;

    int dw = 0, dh = 0;
    drawableSize(&dw, &dh);
    if (!configureWgpuSurface(dw, dh)) {
        std::fprintf(stderr, "[app] initial WebGPU surface configuration failed\n");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiContextReady_ = true;
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    AppTheme::setup(framebufferScale());   // build fonts BEFORE gfx_webgpu_imgui_init
    if (!ImGui_ImplSDL2_InitForOther(window_)) {
        std::fprintf(stderr, "[app] ImGui_ImplSDL2_InitForOther failed\n");
        return false;
    }
    imguiSdlReady_ = true;
    bindSmokeGamepadToImGui(smokeGamepad_);
    /* The in-house backend's shutdown is null-safe and must run even when a
     * late pipeline allocation makes init return false. */
    imguiRendererReady_ = true;
    if (!gfx_webgpu_imgui_init(device, queue, fmt)) {
        std::fprintf(stderr, "[app] gfx_webgpu_imgui_init failed\n");
        return false;
    }
    std::fprintf(stderr, "[app] host: WebGPU (%dx%d drawable, format=%d)\n", dw, dh, fmt);
    return true;
}

int AppHost::recoverWebGpuRoots(int phase) {
    auto releaseCandidate = [&]() {
        if (recoverySurface_ != nullptr) {
            wgpuSurfaceRelease((WGPUSurface)recoverySurface_);
        }
        if (recoveryQueue_ != nullptr) {
            wgpuQueueRelease((WGPUQueue)recoveryQueue_);
        }
        if (recoveryDevice_ != nullptr) {
            gfx_webgpu_host_device_will_release(
                (WGPUDevice)recoveryDevice_);
            wgpuDeviceDestroy((WGPUDevice)recoveryDevice_);
            wgpuDeviceRelease((WGPUDevice)recoveryDevice_);
        }
        if (recoveryAdapter_ != nullptr) {
            wgpuAdapterRelease((WGPUAdapter)recoveryAdapter_);
        }
        if (recoveryInstance_ != nullptr) {
            wgpuInstanceRelease((WGPUInstance)recoveryInstance_);
        }
        recoveryInstance_ = recoveryAdapter_ = recoveryDevice_ = nullptr;
        recoveryQueue_ = recoverySurface_ = nullptr;
        recoveryFormat_ = 0;
    };

    if (!useWebGpu_) return 0;
    if (phase == PLATFORM_HOST_WEBGPU_RECOVERY_ABORT) {
        releaseCandidate();
        if (wgpuRecoveryError_.empty()) {
            wgpuRecoveryError_ =
                "WebGPU recovery stopped before the replacement device was ready.";
        }
        return 1;
    }

    if (phase == PLATFORM_HOST_WEBGPU_RECOVERY_PREPARE) {
        releaseCandidate();
        wgpuRecoveryError_.clear();
        void *layer = nullptr;
#if defined(__APPLE__)
        if (metalView_ != nullptr) {
            layer = SDL_Metal_GetLayer((SDL_MetalView)metalView_);
        }
#endif
        WGPUInstance instance = nullptr;
        WGPUAdapter adapter = nullptr;
        WGPUDevice device = nullptr;
        WGPUQueue queue = nullptr;
        WGPUSurface surface = nullptr;
        int format = 0;
        if (!gfx_webgpu_bringup(
                layer, window_, &instance, &adapter, &device, &queue,
                &surface, &format)) {
            wgpuRecoveryError_ =
                "WebGPU could not create a replacement graphics device. "
                "Your game stopped safely; restart the app to continue from "
                "the latest save.";
            std::fprintf(stderr,
                         "[app] replacement WebGPU root preparation failed\n");
            return 0;
        }
        recoveryInstance_ = instance;
        recoveryAdapter_ = adapter;
        recoveryDevice_ = device;
        recoveryQueue_ = queue;
        recoverySurface_ = surface;
        recoveryFormat_ = format;
        std::fprintf(stderr,
                     "[app] replacement WebGPU roots prepared (format=%d)\n",
                     format);
        return 1;
    }

    if (phase != PLATFORM_HOST_WEBGPU_RECOVERY_COMMIT ||
        recoveryInstance_ == nullptr || recoveryAdapter_ == nullptr ||
        recoveryDevice_ == nullptr || recoveryQueue_ == nullptr ||
        recoverySurface_ == nullptr || recoveryFormat_ == 0) {
        wgpuRecoveryError_ =
            "WebGPU recovery reached an invalid root-replacement state. "
            "Your game stopped safely; restart the app to continue.";
        return 0;
    }

    /* The engine calls COMMIT only after releasing every child it created from
     * the failed borrowed device. Replace the host-owned ImGui children next;
     * root publication is the final step, so the engine can observe either the
     * complete old set or the complete new set, never a mixture. */
    if (imguiRendererReady_) {
        gfx_webgpu_imgui_shutdown();
        imguiRendererReady_ = false;
    }
    if (!gfx_webgpu_imgui_init(
            (WGPUDevice)recoveryDevice_, (WGPUQueue)recoveryQueue_,
            recoveryFormat_)) {
        wgpuRecoveryError_ =
            "WebGPU created a replacement device, but the interface renderer "
            "could not be restored. Your game stopped safely; restart the app.";
        std::fprintf(stderr,
                     "[app] replacement WebGPU ImGui initialization failed\n");
        /* No renderer child may retain a candidate device that is about to be
         * destroyed. init is transactional and shutdown is null-safe; keeping
         * the ownership boundary explicit also protects future init changes. */
        gfx_webgpu_imgui_shutdown();
        releaseCandidate();
        return 0;
    }
    imguiRendererReady_ = true;

    if (captureView_ != nullptr) {
        wgpuTextureViewRelease((WGPUTextureView)captureView_);
        captureView_ = nullptr;
    }
    if (captureTex_ != nullptr) {
        wgpuTextureRelease((WGPUTexture)captureTex_);
        captureTex_ = nullptr;
    }
    captureW_ = captureH_ = 0;

    WGPUInstance oldInstance = (WGPUInstance)wgpuInstance_;
    WGPUAdapter oldAdapter = (WGPUAdapter)wgpuAdapter_;
    WGPUDevice oldDevice = (WGPUDevice)wgpuDevice_;
    WGPUQueue oldQueue = (WGPUQueue)wgpuQueue_;
    WGPUSurface oldSurface = (WGPUSurface)wgpuSurface_;

    wgpuInstance_ = recoveryInstance_;
    wgpuAdapter_ = recoveryAdapter_;
    wgpuDevice_ = recoveryDevice_;
    wgpuQueue_ = recoveryQueue_;
    wgpuSurface_ = recoverySurface_;
    wgpuFormat_ = recoveryFormat_;
    recoveryInstance_ = recoveryAdapter_ = recoveryDevice_ = nullptr;
    recoveryQueue_ = recoverySurface_ = nullptr;
    recoveryFormat_ = 0;
    wgpuSurfaceConfigured_ = false;
    cfgW_ = cfgH_ = 0;
    wgpuFatal_ = false;

    platformSetHostWebGpu(
        wgpuInstance_, wgpuAdapter_, wgpuDevice_, wgpuQueue_, wgpuSurface_,
        wgpuFormat_);

    if (oldSurface != nullptr) {
        wgpuSurfaceUnconfigure(oldSurface);
        wgpuSurfaceRelease(oldSurface);
    }
    if (oldQueue != nullptr) wgpuQueueRelease(oldQueue);
    if (oldDevice != nullptr) {
        gfx_webgpu_host_device_will_release(oldDevice);
        wgpuDeviceDestroy(oldDevice);
        wgpuDeviceRelease(oldDevice);
    }
    if (oldAdapter != nullptr) wgpuAdapterRelease(oldAdapter);
    if (oldInstance != nullptr) wgpuInstanceRelease(oldInstance);

    std::fprintf(stderr,
                 "[app] replacement WebGPU roots committed transactionally\n");
    return 1;
}

bool AppHost::configureWgpuSurface(int w, int h) {
    if (!useWebGpu_ || wgpuSurface_ == nullptr || wgpuDevice_ == nullptr || w <= 0 || h <= 0) {
        return false;
    }
    WGPUSurfaceCapabilities caps = {};
    const WGPUStatus status = wgpuSurfaceGetCapabilities(
        (WGPUSurface)wgpuSurface_, (WGPUAdapter)wgpuAdapter_, &caps);
    bool formatSupported = false;
    for (size_t i = 0; status == WGPUStatus_Success && i < caps.formatCount; ++i) {
        if (caps.formats[i] == (WGPUTextureFormat)wgpuFormat_) {
            formatSupported = true;
            break;
        }
    }
    const bool usageSupported = status == WGPUStatus_Success &&
        (caps.usages & WGPUTextureUsage_RenderAttachment) != 0;
    if (!formatSupported || !usageSupported) {
        std::fprintf(stderr,
                     "[app] WebGPU surface cannot be configured "
                     "(status=%d format=%d renderAttachment=%d)\n",
                     (int)status, wgpuFormat_, usageSupported ? 1 : 0);
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        return false;
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);

    WGPUSurfaceConfiguration cfg = {};
    cfg.device = (WGPUDevice)wgpuDevice_;
    cfg.format = (WGPUTextureFormat)wgpuFormat_;
    // Normal UI frames render directly to the acquired drawable. Requiring
    // CopyDst here rejected otherwise valid surfaces and paid for a full-frame
    // offscreen copy on every launcher present.
    cfg.usage = WGPUTextureUsage_RenderAttachment;
    cfg.width = (uint32_t)w;
    cfg.height = (uint32_t)h;
    cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
    cfg.presentMode = WGPUPresentMode_Fifo;   // vsync; matches the GL swap interval
    if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_HOST_SURFACE_CONFIGURE)) {
        std::fprintf(stderr,
                     "[app] injected initial WebGPU surface configuration failure\n");
        wgpuFatal_ = true;
        return false;
    }
    wgpuSurfaceConfigure((WGPUSurface)wgpuSurface_, &cfg);
    (void)wgpuDevicePoll((WGPUDevice)wgpuDevice_, false, nullptr);
    if (gfx_webgpu_device_failed()) {
        std::fprintf(stderr,
                     "[app] WebGPU device rejected surface configuration\n");
        wgpuFatal_ = true;
        return false;
    }
    cfgW_ = (unsigned)w;
    cfgH_ = (unsigned)h;
    wgpuSurfaceConfigured_ = true;
    return true;
}

void AppHost::ensureWgpuCaptureTarget(int w, int h) {
    if (!useWebGpu_ || wgpuDevice_ == nullptr || w <= 0 || h <= 0) return;
    if (captureTex_ != nullptr && captureView_ != nullptr &&
        captureW_ == (unsigned)w && captureH_ == (unsigned)h) return;
    if (captureView_ != nullptr) {
        wgpuTextureViewRelease((WGPUTextureView)captureView_);
        captureView_ = nullptr;
    }
    if (captureTex_ != nullptr) {
        wgpuTextureRelease((WGPUTexture)captureTex_);
        captureTex_ = nullptr;
    }
    captureW_ = captureH_ = 0;
    WGPUTextureDescriptor td = {};
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)w; td.size.height = (uint32_t)h; td.size.depthOrArrayLayers = 1;
    td.format = (WGPUTextureFormat)wgpuFormat_;   // match the surface for capture parity
    td.mipLevelCount = 1; td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture((WGPUDevice)wgpuDevice_, &td);
    if (tex == nullptr) return;
    WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
    if (view == nullptr) {
        wgpuTextureRelease(tex);
        return;
    }
    captureTex_ = tex;
    captureView_ = view;
    captureW_ = (unsigned)w;
    captureH_ = (unsigned)h;
}
#endif  // MDKR_WEBGPU_BACKEND

void AppHost::beginFrame() {
    AppTheme::applyPendingUiScale();
    float frameScale = framebufferScale();
    if (const char *sequence = std::getenv("MDKR_APP_SMOKE_DPI_SEQUENCE")) {
        if (std::strcmp(sequence, "1,2,1") == 0 && dpiSmokeFrame_ < 3) {
            static const float kScales[] = {1.0f, 2.0f, 1.0f};
            frameScale = kScales[dpiSmokeFrame_++];
            AppTheme::refreshFramebufferScale(frameScale);
            std::fprintf(stderr,
                         "[app-ui-test] dpi transition scale=%.0f atlasGeneration=%u\n",
                         static_cast<double>(frameScale),
                         AppTheme::atlasGeneration());
        } else {
            AppTheme::refreshFramebufferScale(frameScale);
        }
    } else {
        AppTheme::refreshFramebufferScale(frameScale);
    }
#ifdef MDKR_WEBGPU_BACKEND
    if (useWebGpu_) {
        // The frame is cleared by the render pass in endFrameWebGpu.
        gfx_webgpu_imgui_new_frame();
        ImGui_ImplSDL2_NewFrame();
        // imgui_impl_sdl2 uses SDL_GL_GetDrawableSize for the framebuffer scale,
        // which returns the LOGICAL size for our Metal/native window (no GL
        // drawable) → scale (1,1). Set the true high-DPI scale from the actual
        // drawable so geometry fills the target and scissors land in pixels.
        ImGuiIO &io = ImGui::GetIO();
        int lw = 0, lh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(window_, &lw, &lh);
        drawableSize(&dw, &dh);
        if (lw > 0 && lh > 0) {
            io.DisplaySize = ImVec2((float)lw, (float)lh);
            io.DisplayFramebufferScale = ImVec2((float)dw / (float)lw, (float)dh / (float)lh);
        }
        reassertSmokePointer();
        releaseSmokeMouseClick();
        ImGui::NewFrame();
        return;
    }
#endif
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    reassertSmokePointer();
    releaseSmokeMouseClick();
    ImGui::NewFrame();

    int w = drawableWidth(), h = drawableHeight();
    glViewport(0, 0, w, h);
    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Save the current GL back buffer as a bottom-up 24-bit BMP. GL reads
// bottom-to-top, which matches BMP row order, so no vertical flip is needed.
// Returns true only if the BMP was fully written (AUDIT-0046: a smoke capture
// must not report success without its image).
static bool writeBackbufferBmp(const char *path, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());

    const int rowBytes = w * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const size_t imgSize = (size_t)(rowBytes + pad) * (size_t)h;  // size_t: no int overflow on 8K+
    const size_t fileSize = 54 + imgSize;

    FILE *f = mdkr_fopen_utf8(path, "wb");
    if (!f) { std::fprintf(stderr, "[app] could not open %s\n", path); return false; }
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = fileSize & 0xFF; hdr[3] = (fileSize >> 8) & 0xFF;
    hdr[4] = (fileSize >> 16) & 0xFF; hdr[5] = (fileSize >> 24) & 0xFF;
    hdr[10] = 54;                       // pixel data offset
    hdr[14] = 40;                       // DIB header size
    hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
    hdr[20] = (w >> 16) & 0xFF; hdr[21] = (w >> 24) & 0xFF;
    hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
    hdr[24] = (h >> 16) & 0xFF; hdr[25] = (h >> 24) & 0xFF;
    hdr[26] = 1;                        // planes
    hdr[28] = 24;                       // bpp
    hdr[34] = imgSize & 0xFF; hdr[35] = (imgSize >> 8) & 0xFF;
    hdr[36] = (imgSize >> 16) & 0xFF; hdr[37] = (imgSize >> 24) & 0xFF;
    bool ok = (std::fwrite(hdr, 1, 54, f) == 54);

    std::vector<uint8_t> row(rowBytes + pad, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t *src = &rgb[(size_t)y * rowBytes];
        for (int x = 0; x < w; ++x) {  // RGB -> BGR
            row[x * 3 + 0] = src[x * 3 + 2];
            row[x * 3 + 1] = src[x * 3 + 1];
            row[x * 3 + 2] = src[x * 3 + 0];
        }
        if (std::fwrite(row.data(), 1, rowBytes + pad, f) != (size_t)(rowBytes + pad)) ok = false;
    }
    if (std::fclose(f) != 0) ok = false;
    if (ok) {
        std::fprintf(stderr, "[app] wrote %s (%dx%d)\n", path, w, h);
    } else {
        std::fprintf(stderr, "[app] FAILED to write %s (short write/IO error); removing\n", path);
        mdkr_remove_utf8(path);
    }
    return ok;
}

#ifdef MDKR_WEBGPU_BACKEND
namespace {
struct WgpuMapReq {
    bool pending;
    bool done;
    bool cancelRequested;
    WGPUMapAsyncStatus status;
    WGPUBuffer buffer;
};

/* mapAsync may resolve after a bounded capture wait gives up. Its userdata must
 * therefore outlive writeWgpuBmp(). Only one shell capture can be outstanding;
 * a timed-out request remains pending here until the device callback resolves. */
WgpuMapReq s_captureMapReq = {};

void on_map(WGPUMapAsyncStatus s, WGPUStringView m, void *u1, void *u2) {
    (void)m; (void)u2;
    WgpuMapReq *r = (WgpuMapReq *)u1;
    r->status = s;
    r->done = true;
    r->pending = false;
}

void release_capture_map_request() {
    if (s_captureMapReq.buffer != nullptr) {
        wgpuBufferRelease(s_captureMapReq.buffer);
    }
    s_captureMapReq = {};
}

/* Reap a callback that completed after writeWgpuBmp() timed out. A successful
 * late map is still mapped until the application explicitly unmaps it. */
void reap_capture_map_request() {
    if (s_captureMapReq.buffer == nullptr || s_captureMapReq.pending) return;
    if (s_captureMapReq.done &&
        s_captureMapReq.status == WGPUMapAsyncStatus_Success &&
        !s_captureMapReq.cancelRequested) {
        wgpuBufferUnmap(s_captureMapReq.buffer);
    }
    release_capture_map_request();
}

void drain_capture_map_request(WGPUDevice device) {
    if (s_captureMapReq.buffer == nullptr) return;
    if (s_captureMapReq.pending) {
        std::fprintf(stderr,
                     "[app] capture map drain: cancelling pending request\n");
        s_captureMapReq.cancelRequested = true;
        /* WebGPU defines unmap on a pending map as cancellation. Keep both the
         * callback state and an explicit buffer reference alive until that
         * cancellation callback has been delivered. */
        wgpuBufferUnmap(s_captureMapReq.buffer);
        while (s_captureMapReq.pending) {
            (void)wgpuDevicePoll(device, true, nullptr);
        }
    }
    const WGPUMapAsyncStatus status = s_captureMapReq.status;
    reap_capture_map_request();
    std::fprintf(stderr,
                 "[app] capture map drain: callback resolved status=%d; "
                 "retained buffer released\n",
                 (int)status);
}

// Write a mapped 8-bit readback buffer (row stride `bpr`, top-down) as a 24-bit
// BMP. `bgra` selects the source channel order (BGRA8 — the near-universal
// surface format — vs RGBA8). BMP rows are bottom-up, so source rows are emitted
// in reverse; BMP pixels are B,G,R.
// Returns true only if the BMP was fully written (AUDIT-0046).
bool writeWgpuBmp(WGPUBuffer buf, uint32_t bpr, int w, int h, const char *path,
                  WGPUDevice device, bool bgra) {
    if (w <= 0 || h <= 0) return false;
    reap_capture_map_request();
    if (s_captureMapReq.pending) {
        std::fprintf(stderr,
                     "[app] capture map refused while a prior request is pending\n");
        return false;
    }
    size_t size = (size_t)bpr * (uint32_t)h;
    wgpuBufferAddRef(buf);
    s_captureMapReq = {
        true, false, false, (WGPUMapAsyncStatus)0, buf
    };
    WGPUBufferMapCallbackInfo ci = {};
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = on_map;
    ci.userdata1 = &s_captureMapReq;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, size, ci);

    const bool injectTimeout =
        gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_HOST_CAPTURE_MAP_TIMEOUT);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!injectTimeout && !s_captureMapReq.done &&
           std::chrono::steady_clock::now() < deadline) {
        (void)wgpuDevicePoll(device, false, nullptr);
        if (!s_captureMapReq.done) SDL_Delay(1);
    }
    if (!s_captureMapReq.done) {
        std::fprintf(stderr,
                     "[app] capture map timed out; callback remains safely owned\n");
        return false;
    }
    if (s_captureMapReq.status != WGPUMapAsyncStatus_Success) {
        std::fprintf(stderr, "[app] capture map failed (status=%d)\n",
                     (int)s_captureMapReq.status);
        release_capture_map_request();
        return false;
    }
    const uint8_t *px = (const uint8_t *)wgpuBufferGetConstMappedRange(buf, 0, size);
    FILE *f = px ? mdkr_fopen_utf8(path, "wb") : nullptr;
    bool ok = false;
    if (f != nullptr) {
        const int rowBytes = w * 3;
        const int pad = (4 - (rowBytes % 4)) % 4;
        const size_t imgSize = (size_t)(rowBytes + pad) * (size_t)h;
        const size_t fileSize = 54 + imgSize;
        uint8_t hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2] = fileSize & 0xFF; hdr[3] = (fileSize >> 8) & 0xFF;
        hdr[4] = (fileSize >> 16) & 0xFF; hdr[5] = (fileSize >> 24) & 0xFF;
        hdr[10] = 54; hdr[14] = 40;
        hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
        hdr[20] = (w >> 16) & 0xFF; hdr[21] = (w >> 24) & 0xFF;
        hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
        hdr[24] = (h >> 16) & 0xFF; hdr[25] = (h >> 24) & 0xFF;
        hdr[26] = 1; hdr[28] = 24;
        hdr[34] = imgSize & 0xFF; hdr[35] = (imgSize >> 8) & 0xFF;
        hdr[36] = (imgSize >> 16) & 0xFF; hdr[37] = (imgSize >> 24) & 0xFF;
        ok = (std::fwrite(hdr, 1, 54, f) == 54);
        std::vector<uint8_t> row(rowBytes + pad, 0);
        for (int y = h - 1; y >= 0; --y) {   // BMP bottom-up
            const uint8_t *srow = px + (size_t)y * bpr;
            for (int x = 0; x < w; ++x) {     // -> BMP B,G,R (drop alpha)
                row[x * 3 + 0] = bgra ? srow[x * 4 + 0] : srow[x * 4 + 2];  // B
                row[x * 3 + 1] = srow[x * 4 + 1];                            // G
                row[x * 3 + 2] = bgra ? srow[x * 4 + 2] : srow[x * 4 + 0];  // R
            }
            if (std::fwrite(row.data(), 1, rowBytes + pad, f) != (size_t)(rowBytes + pad)) ok = false;
        }
        if (std::fclose(f) != 0) ok = false;
        if (ok) {
            std::fprintf(stderr, "[app] wrote %s (%dx%d)\n", path, w, h);
        } else {
            std::fprintf(stderr, "[app] FAILED to write %s (short write/IO error); removing\n", path);
            mdkr_remove_utf8(path);
        }
    } else {
        std::fprintf(stderr, "[app] could not open %s\n", path);
    }
    wgpuBufferUnmap(buf);
    release_capture_map_request();
    return ok;
}

bool encodeImGuiPass(WGPUCommandEncoder encoder, WGPUTextureView target,
                     ImDrawData *drawData, int w, int h) {
    if (encoder == nullptr || target == nullptr || drawData == nullptr) return false;

    WGPURenderPassColorAttachment color = {};
    color.view = target;
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue.r = 0.06;
    color.clearValue.g = 0.07;
    color.clearValue.b = 0.09;
    color.clearValue.a = 1.0;

    WGPURenderPassDescriptor descriptor = {};
    descriptor.colorAttachmentCount = 1;
    descriptor.colorAttachments = &color;
    WGPURenderPassEncoder pass =
        wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    if (pass == nullptr) return false;
    const bool rendered = gfx_webgpu_imgui_render(drawData, pass, w, h);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    return rendered;
}
}  // namespace

bool AppHost::endFrameWebGpu(const char *captureBmpPath) {
    if (wgpuFatal_ || gfx_webgpu_device_failed()) {
        wgpuFatal_ = true;
        return false;
    }
    ImGui::Render();

    const bool captureRequested = captureBmpPath != nullptr;
    if (captureRequested) ++wgpuCaptureRequests_;

    int w = 0, h = 0;
    drawableSize(&w, &h);
    if (w <= 0 || h <= 0) {
        if (captureRequested) ++wgpuCaptureFailures_;
        return !captureRequested;
    }

    WGPUSurface surface = (WGPUSurface)wgpuSurface_;
    WGPUDevice device = (WGPUDevice)wgpuDevice_;
    WGPUQueue queue = (WGPUQueue)wgpuQueue_;
    if ((unsigned)w != cfgW_ || (unsigned)h != cfgH_) {
        if (!configureWgpuSurface(w, h)) {
            if (captureRequested) ++wgpuCaptureFailures_;
            wgpuFatal_ = true;
            return false;
        }
    }

    // Acquire before encoding. The normal path renders ImGui directly into this
    // view, so the surface needs RenderAttachment only and there is no per-frame
    // scene texture or full-frame CopyTextureToTexture operation.
    WGPUSurfaceTexture st = {};
    WGPUTextureView surfaceView = nullptr;
    bool surfaceAcquired = false;
    ++wgpuPresentAttempts_;
    if (surface != nullptr && wgpuSurfaceConfigured_) {
        if (gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_HOST_SURFACE_ACQUIRE)) {
            st.status = WGPUSurfaceGetCurrentTextureStatus_Error;
            std::fprintf(stderr,
                         "[app] injected WebGPU host surface acquisition failure\n");
        } else {
            wgpuSurfaceGetCurrentTexture(surface, &st);
        }
        wgpuLastSurfaceStatus_ = (int)st.status;
        surfaceAcquired = st.texture != nullptr &&
        (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
         st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal);
    }
    if (!surfaceAcquired) {
        ++wgpuUnavailableFrames_;
        if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
            st.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
            // Force a fresh configuration on the next frame. Do not reconfigure
            // while the unsuccessful texture result is still live.
            cfgW_ = cfgH_ = 0;
        } else if (st.status == WGPUSurfaceGetCurrentTextureStatus_Error) {
            std::fprintf(stderr,
                         "[app] unrecoverable WebGPU host surface status=%d\n",
                         (int)st.status);
            wgpuFatal_ = true;
        }
        // Headless macOS CI commonly has no drawable. Consume this one
        // explicitly test-gated constructor fault at the same host boundary
        // so the fatal accounting remains executable there; an interactive
        // release qualification reaches the real create-view call below.
        if (std::getenv("MDKR_TEST_OCCLUDED_SURFACE_FAULTS") != nullptr &&
            gfx_webgpu_fault_selected(GFX_WEBGPU_FAULT_HOST_SURFACE_VIEW) &&
            gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_HOST_SURFACE_VIEW)) {
            ++wgpuEncodeFailures_;
            std::fprintf(stderr,
                         "[app-test] no surface drawable; injecting configured "
                         "host surface-view failure at the presentation boundary\n");
            wgpuFatal_ = true;
        }
    } else {
        surfaceView = gfx_webgpu_fault_hit(GFX_WEBGPU_FAULT_HOST_SURFACE_VIEW)
            ? nullptr : wgpuTextureCreateView(st.texture, nullptr);
        if (surfaceView == nullptr) ++wgpuEncodeFailures_;
    }
    const bool directRenderExpected = surfaceAcquired;

    // Screenshot capture deliberately gets a second, identical render pass.
    // This retains hidden-window capture semantics without taxing ordinary UI
    // presentation with an offscreen texture and copy.
    WGPUBuffer capBuf = nullptr;
    uint32_t capBpr = 0;
    bool captureResourcesReady = false;
    if (captureRequested) {
        ensureWgpuCaptureTarget(w, h);
        capBpr = ((uint32_t)w * 4u + 255u) / 256u * 256u;   // 256-align for CopyTextureToBuffer
        WGPUBufferDescriptor bd = {};
        bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        bd.size = (uint64_t)capBpr * (uint32_t)h;
        if (captureTex_ != nullptr && captureView_ != nullptr) {
            capBuf = wgpuDeviceCreateBuffer(device, &bd);
            captureResourcesReady = capBuf != nullptr;
        }
    }

    const bool needEncoder = surfaceView != nullptr || captureResourcesReady;
    WGPUCommandEncoder encoder = needEncoder
        ? wgpuDeviceCreateCommandEncoder(device, nullptr) : nullptr;
    bool directEncoded = false;
    bool captureEncoded = false;
    if (needEncoder && encoder == nullptr) {
        ++wgpuEncodeFailures_;
    } else if (encoder != nullptr) {
        ImDrawData *drawData = ImGui::GetDrawData();
        if (surfaceView != nullptr) {
            directEncoded = encodeImGuiPass(encoder, surfaceView, drawData, w, h);
            if (!directEncoded) ++wgpuEncodeFailures_;
        }
        if (captureResourcesReady) {
            captureEncoded = encodeImGuiPass(
                encoder, (WGPUTextureView)captureView_, drawData, w, h);
            if (captureEncoded) {
                WGPUTexelCopyTextureInfo src = {};
                src.texture = (WGPUTexture)captureTex_;
                src.aspect = WGPUTextureAspect_All;
                WGPUTexelCopyBufferInfo dst = {};
                dst.buffer = capBuf;
                dst.layout.bytesPerRow = capBpr;
                dst.layout.rowsPerImage = (uint32_t)h;
                WGPUExtent3D ext = { (uint32_t)w, (uint32_t)h, 1 };
                wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &ext);
            } else {
                ++wgpuEncodeFailures_;
            }
        }
    }

    bool submitted = false;
    if (encoder != nullptr && (directEncoded || captureEncoded)) {
        WGPUCommandBuffer command = wgpuCommandEncoderFinish(encoder, nullptr);
        if (command != nullptr) {
            wgpuQueueSubmit(queue, 1, &command);
            submitted = true;
            wgpuCommandBufferRelease(command);
        } else {
            ++wgpuEncodeFailures_;
        }
    }
    if (encoder != nullptr) wgpuCommandEncoderRelease(encoder);

    if (submitted && directEncoded) {
        wgpuSurfacePresent(surface);
        ++presentedFrames_;
    }
    if (directRenderExpected && (!directEncoded || !submitted)) {
        std::fprintf(stderr,
                     "[app] WebGPU host could not encode and submit an acquired "
                     "surface frame\n");
        wgpuFatal_ = true;
    }

    bool captureOk = !captureRequested;
    if (submitted && captureEncoded && capBuf != nullptr) {
        const bool bgra = ((WGPUTextureFormat)wgpuFormat_ == WGPUTextureFormat_BGRA8Unorm ||
                           (WGPUTextureFormat)wgpuFormat_ == WGPUTextureFormat_BGRA8UnormSrgb);
        captureOk = writeWgpuBmp(capBuf, capBpr, w, h, captureBmpPath, device, bgra);
    }
    if (captureRequested && !captureOk) ++wgpuCaptureFailures_;

    if (capBuf != nullptr) wgpuBufferRelease(capBuf);
    if (surfaceView != nullptr) wgpuTextureViewRelease(surfaceView);
    if (st.texture != nullptr) wgpuTextureRelease(st.texture);
    if (gfx_webgpu_device_failed()) wgpuFatal_ = true;
    return captureOk && !wgpuFatal_;
}
#endif  // MDKR_WEBGPU_BACKEND

#ifndef MDKR_WEBGPU_BACKEND
int AppHost::recoverWebGpuRoots(int phase) {
    (void)phase;
    wgpuRecoveryError_ =
        "This build does not include the WebGPU recovery backend.";
    return 0;
}
#endif

bool AppHost::endFrameGL(const char *captureBmpPath) {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    bool captureOk = true;
    if (captureBmpPath) {
        captureOk = writeBackbufferBmp(captureBmpPath, drawableWidth(), drawableHeight());
    }
    if (glSoftwarePacing_) {
        const Uint64 frequency = SDL_GetPerformanceFrequency();
        const Uint64 now = SDL_GetPerformanceCounter();
        const Uint64 target = frequency / 60u;
        if (glLastPresentCounter_ != 0 && now - glLastPresentCounter_ < target) {
            const Uint64 remaining = target - (now - glLastPresentCounter_);
            const Uint32 delayMs = static_cast<Uint32>(remaining * 1000u / frequency);
            if (delayMs > 0) SDL_Delay(delayMs);
        }
    }
    SDL_GL_SwapWindow(window_);
    glLastPresentCounter_ = SDL_GetPerformanceCounter();
    ++presentedFrames_;
    return captureOk;
}

// Returns true when the host renderer is healthy and no capture failed.
bool AppHost::endFrame(const char *captureBmpPath) {
#ifdef MDKR_WEBGPU_BACKEND
    if (useWebGpu_) { return endFrameWebGpu(captureBmpPath); }
#endif
    return endFrameGL(captureBmpPath);
}

void AppHost::drawableSize(int *w, int *h) const {
    *w = 0; *h = 0;
    if (!window_) return;
#ifdef MDKR_WEBGPU_BACKEND
    if (useWebGpu_) {
#  if defined(__APPLE__)
        SDL_Metal_GetDrawableSize(window_, w, h);
#  elif SDL_VERSION_ATLEAST(2, 26, 0)
        SDL_GetWindowSizeInPixels(window_, w, h);
#  else
        SDL_GetWindowSize(window_, w, h);
#  endif
        return;
    }
#endif
    SDL_GL_GetDrawableSize(window_, w, h);
}

float AppHost::framebufferScale() const {
    int lw = 0, lh = 0, dw = 0, dh = 0;
    if (window_) {
        SDL_GetWindowSize(window_, &lw, &lh);
        drawableSize(&dw, &dh);
    }
    return (lw > 0) ? (float)dw / (float)lw : 1.0f;
}

// Pointer, keyboard, text, and window focus/hover traffic. A scripted run owns
// exactly this set; game-controller events are excluded because the gamepad
// script drives a virtual joystick whose presses arrive through the real queue.
static bool isWindowServerInput(const SDL_Event &e) {
    switch (e.type) {
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL:
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_TEXTINPUT:
        case SDL_TEXTEDITING:
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            return true;
        case SDL_WINDOWEVENT:
            return e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                   e.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                   e.window.event == SDL_WINDOWEVENT_ENTER ||
                   e.window.event == SDL_WINDOWEVENT_LEAVE;
        default:
            return false;
    }
}

bool AppHost::processEvent(SDL_Event &e) {
    if (smokeScriptOwnsInput_ && !injectingSmokeEvent_ && isWindowServerInput(e)) {
        /* Dropped before the shortcut handler and the ImGui adapter so a
         * foreground transition cannot clear a held scripted key or move the
         * pointer off the widget the script is operating. Quit, close, drop,
         * and controller events are unaffected. */
        return false;
    }
    if (AppWindow_handleEvent(window_, e)) {
        return false;
    }
    ImGui_ImplSDL2_ProcessEvent(&e);
    if (e.type == SDL_QUIT) {
        return true;
    }
    if (e.type == SDL_DROPFILE && e.drop.file) {
        droppedFile_ = e.drop.file;  // consumed by takeDroppedFile()
        SDL_free(e.drop.file);
        return false;
    }
    if (e.type == SDL_DROPTEXT && e.drop.file) {
        /* SDL allocates the payload for every drop event it delivers. The
         * launcher has no text-drop action, so release it here rather than
         * leaking one buffer per dropped selection. */
        SDL_free(e.drop.file);
        return false;
    }
    return e.type == SDL_WINDOWEVENT &&
           e.window.event == SDL_WINDOWEVENT_CLOSE &&
           e.window.windowID == SDL_GetWindowID(window_);
}

bool AppHost::injectSmokeEvent(SDL_Event &event) {
    injectingSmokeEvent_ = true;
    const bool quit = processEvent(event);
    injectingSmokeEvent_ = false;
    switch (event.type) {
        case SDL_MOUSEMOTION:
            smokePointer_ = {event.motion.x, event.motion.y};
            smokePointerValid_ = true;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            smokePointer_ = {event.button.x, event.button.y};
            smokePointerValid_ = true;
            break;
        default:
            break;
    }
    return quit;
}

void AppHost::reassertSmokePointer() {
    if (!smokeScriptOwnsInput_ || !smokePointerValid_) return;
    /* imgui_impl_sdl2's NewFrame samples the operating system cursor whenever
     * this window holds keyboard focus and no button is down. Queue the
     * scripted coordinate after that sample and before ImGui consumes the
     * frame's input, so the position the script chose is the one it gets. */
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(smokePointer_.x),
                                    static_cast<float>(smokePointer_.y));
}

void AppHost::queueDropFileForSmoke(const char *path) {
    pendingSmokeDrop_ = path ? path : "";
}

void AppHost::queueKeyPressForSmoke(SDL_Keycode key) {
    if (AppUi_smokeInputMode() != AppUiSmokeInputMode::Keyboard) return;
    pendingSmokeKeys_.push_back(key);
}

bool AppHost::queueGamepadPressForSmoke(SDL_GameControllerButton button) {
    if (AppUi_smokeInputMode() != AppUiSmokeInputMode::Gamepad ||
        !smokeGamepad_) {
        return false;
    }
    pendingSmokeGamepadButtons_.push_back(button);
    return true;
}

void AppHost::queueMouseClickForSmoke(int x, int y) {
    const char *navigationTarget = std::getenv("MDKR_APP_SMOKE_NAV_TARGET");
    const char *navigationToken = std::getenv("MDKR_APP_SMOKE_NAV_TOKEN");
    const bool navigationSmoke = navigationTarget && navigationTarget[0] &&
        navigationToken &&
        std::strcmp(navigationToken, "mdkr64-app-nav-v1") == 0;
    if (AppUi_smokeInputMode() != AppUiSmokeInputMode::Keyboard &&
        !navigationSmoke) {
        return;
    }
    pendingSmokeClicks_.push_back({x, y});
}

bool AppHost::queueMouseDragStepForSmoke(int x, int y, bool held) {
    if (std::getenv("MDKR_APP_SMOKE_UI_SCALE_DRAG") == nullptr) return false;
    pendingSmokeDragSteps_.push_back({x, y, held, false});
    return true;
}

bool AppHost::queueTouchDragStepForSmoke(int x, int y, bool held) {
    if (std::getenv("MDKR_APP_SMOKE_TOUCH_SCROLL") == nullptr) return false;
    pendingSmokeDragSteps_.push_back({x, y, held, true});
    return true;
}

bool AppHost::pumpAndShouldQuit() {
    bool quit = false;

    /* Settings drawn by the previous frame may have queued a fullscreen
     * transition. Apply it before ImGui/WebGPU begins another frame. */
    AppWindow_servicePending();

    /* Hand the previous frame's announcements to the speech worker. Here, at
     * the top of the launcher's frame, because this is the thread that pushed
     * them: a11y_model.c is single-owner and the pop has to happen on the same
     * side as the push (see platform/a11y_speech.h). Costs a config read and a
     * return unless the player has switched speech on. */
    mdkr_a11y_speech_service_pump();

    // SDL_DROPFILE is a platform-reserved event. In particular, sdl2-compat
    // cannot round-trip an application-built SDL2 DropEvent through SDL3: the
    // SDL2 `file` pointer and SDL3 `data` pointer occupy different layouts.
    // Materialize the smoke event on this side of that platform boundary, then
    // feed the exact same event type and ownership contract to processEvent().
    if (!pendingSmokeDrop_.empty()) {
        std::string path;
        path.swap(pendingSmokeDrop_);  // consume exactly once, even on OOM
        SDL_Event e = {};
        e.type = SDL_DROPFILE;
        e.drop.timestamp = SDL_GetTicks();
        e.drop.file = SDL_strdup(path.c_str());
        e.drop.windowID = SDL_GetWindowID(window_);
        if (e.drop.file) {
            quit = injectSmokeEvent(e) || quit;
        } else {
            std::fprintf(stderr, "[app] smoke: could not allocate drop path\n");
        }
    }


    if (smokeHeldKey_ != SDLK_UNKNOWN || !pendingSmokeKeys_.empty()) {
        SDL_Event event = {};
        event.key.timestamp = SDL_GetTicks();
        event.key.windowID = SDL_GetWindowID(window_);
        event.key.repeat = 0;
        if (smokeHeldKey_ != SDLK_UNKNOWN) {
            event.type = SDL_KEYUP;
            event.key.state = SDL_RELEASED;
            event.key.keysym.sym = smokeHeldKey_;
            event.key.keysym.scancode = SDL_GetScancodeFromKey(smokeHeldKey_);
            quit = injectSmokeEvent(event) || quit;
            smokeHeldKey_ = SDLK_UNKNOWN;
        }
        if (!pendingSmokeKeys_.empty()) {
            smokeHeldKey_ = pendingSmokeKeys_.front();
            pendingSmokeKeys_.erase(pendingSmokeKeys_.begin());
            event.type = SDL_KEYDOWN;
            event.key.state = SDL_PRESSED;
            event.key.keysym.sym = smokeHeldKey_;
            event.key.keysym.scancode = SDL_GetScancodeFromKey(smokeHeldKey_);
            quit = injectSmokeEvent(event) || quit;
        }
    }


    if (smokeClickHeld_) {
        // Keep the backend's held-button bit set through its next NewFrame so
        // the OS global-mouse fallback cannot overwrite the synthetic widget
        // coordinate. releaseSmokeMouseClick() queues the release immediately
        // afterward and before ImGui::NewFrame consumes the input stream.
        smokeClickHeld_ = false;
        smokeClickReleasePending_ = true;
    } else if (!smokeClickReleasePending_ && !pendingSmokeClicks_.empty()) {
        SDL_Event event = {};
        smokeHeldClick_ = pendingSmokeClicks_.front();
        pendingSmokeClicks_.erase(pendingSmokeClicks_.begin());

        event.type = SDL_MOUSEMOTION;
        event.motion.timestamp = SDL_GetTicks();
        event.motion.windowID = SDL_GetWindowID(window_);
        event.motion.x = smokeHeldClick_.x;
        event.motion.y = smokeHeldClick_.y;
        quit = injectSmokeEvent(event) || quit;

        event = {};
        event.type = SDL_MOUSEBUTTONDOWN;
        event.button.timestamp = SDL_GetTicks();
        event.button.windowID = SDL_GetWindowID(window_);
        event.button.button = SDL_BUTTON_LEFT;
        event.button.state = SDL_PRESSED;
        event.button.clicks = 1;
        event.button.x = smokeHeldClick_.x;
        event.button.y = smokeHeldClick_.y;
        quit = injectSmokeEvent(event) || quit;
        smokeClickHeld_ = true;
    }

    if (!pendingSmokeDragSteps_.empty()) {
        const SmokeDragStep step = pendingSmokeDragSteps_.front();
        pendingSmokeDragSteps_.erase(pendingSmokeDragSteps_.begin());
        SDL_Event event = {};
        event.type = SDL_MOUSEMOTION;
        event.motion.timestamp = SDL_GetTicks();
        event.motion.windowID = SDL_GetWindowID(window_);
        event.motion.which = step.touch ? SDL_TOUCH_MOUSEID : 0;
        event.motion.x = step.x;
        event.motion.y = step.y;
        quit = injectSmokeEvent(event) || quit;

        if (step.held != smokeDragHeld_) {
            event = {};
            event.type = step.held ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            event.button.timestamp = SDL_GetTicks();
            event.button.windowID = SDL_GetWindowID(window_);
            event.button.which = step.touch ? SDL_TOUCH_MOUSEID : 0;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.state = step.held ? SDL_PRESSED : SDL_RELEASED;
            event.button.clicks = 1;
            event.button.x = step.x;
            event.button.y = step.y;
            quit = injectSmokeEvent(event) || quit;
            smokeDragHeld_ = step.held;
        }
    }


#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (smokeGamepad_) {
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(smokeGamepad_);
        if (smokeHeldGamepadButton_ >= 0) {
            (void)SDL_JoystickSetVirtualButton(
                joystick, smokeHeldGamepadButton_, SDL_RELEASED);
            smokeHeldGamepadButton_ = -1;
        }
        if (!pendingSmokeGamepadButtons_.empty()) {
            smokeHeldGamepadButton_ = pendingSmokeGamepadButtons_.front();
            pendingSmokeGamepadButtons_.erase(pendingSmokeGamepadButtons_.begin());
            if (SDL_JoystickSetVirtualButton(
                    joystick, smokeHeldGamepadButton_, SDL_PRESSED) != 0) {
                std::fprintf(stderr,
                             "[app-ui-test] virtual gamepad button update failed: %s\n",
                             SDL_GetError());
            }
        }
    }
#endif

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        quit = processEvent(e) || quit;
    }
    return quit;
}

void AppHost::releaseSmokeMouseClick() {
    if (!smokeClickReleasePending_) return;
    SDL_Event event = {};
    event.type = SDL_MOUSEBUTTONUP;
    event.button.timestamp = SDL_GetTicks();
    event.button.windowID = SDL_GetWindowID(window_);
    event.button.button = SDL_BUTTON_LEFT;
    event.button.state = SDL_RELEASED;
    event.button.clicks = 1;
    event.button.x = smokeHeldClick_.x;
    event.button.y = smokeHeldClick_.y;
    (void)injectSmokeEvent(event);
    smokeClickReleasePending_ = false;
}

bool AppHost::waitAndPump(int timeoutMs) {
    if (pumpAndShouldQuit()) return true;
    if (timeoutMs < 0) timeoutMs = 0;

    bool quit = false;
    SDL_Event event = {};
    if (SDL_WaitEventTimeout(&event, timeoutMs) == 1) {
        quit = processEvent(event);
        while (SDL_PollEvent(&event)) {
            quit = processEvent(event) || quit;
        }
    }
    return quit;
}

int AppHost::drawableWidth() const {
    int w = 0, h = 0;
    drawableSize(&w, &h);
    return w;
}

int AppHost::drawableHeight() const {
    int w = 0, h = 0;
    drawableSize(&w, &h);
    return h;
}

bool AppHost::lastSurfaceWasOccluded() const {
#ifdef MDKR_WEBGPU_BACKEND
    return useWebGpu_ &&
           wgpuLastSurfaceStatus_ == (int)WGPUSurfaceGetCurrentTextureStatus_Occluded;
#else
    return false;
#endif
}

std::string AppHost::takeDroppedFile() {
    std::string s;
    s.swap(droppedFile_);
    return s;
}

void AppHost::shutdown() {
    if (!active_) return;
    active_ = false;
    /* Before anything is released: a fault during teardown must print its
     * report rather than try to present into a window that is going away. */
    CrashScreen_setWindow(nullptr);
    /* Both the launcher and an adopted engine submit through these host roots.
     * The engine has returned before normal shutdown; wait for the remaining UI
     * work before releasing any resource it may reference. */
#ifdef MDKR_WEBGPU_BACKEND
    if (recoveryInstance_ != nullptr || recoveryDevice_ != nullptr ||
        recoverySurface_ != nullptr) {
        (void)recoverWebGpuRoots(PLATFORM_HOST_WEBGPU_RECOVERY_ABORT);
    }
    if (useWebGpu_ && wgpuDevice_ != nullptr) {
        drain_capture_map_request((WGPUDevice)wgpuDevice_);
        (void)wgpuDevicePoll((WGPUDevice)wgpuDevice_, true, nullptr);
    }
#endif
    if (imguiRendererReady_) {
#ifdef MDKR_WEBGPU_BACKEND
        if (useWebGpu_) {
            gfx_webgpu_imgui_shutdown();
        } else
#endif
        {
            ImGui_ImplOpenGL3_Shutdown();
        }
        imguiRendererReady_ = false;
    }
    if (imguiSdlReady_) {
        ImGui_ImplSDL2_Shutdown();
        imguiSdlReady_ = false;
    }
    if (imguiContextReady_) {
        ImGui::DestroyContext();
        imguiContextReady_ = false;
    }
    if (smokeGamepad_) {
        SDL_GameControllerClose(smokeGamepad_);
        smokeGamepad_ = nullptr;
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (smokeGamepadDeviceIndex_ >= 0) {
        (void)SDL_JoystickDetachVirtual(smokeGamepadDeviceIndex_);
        smokeGamepadDeviceIndex_ = -1;
    }
#endif
#ifdef MDKR_WEBGPU_BACKEND
    // Release children before the roots they reference.
    if (useWebGpu_) {
        std::fprintf(stderr,
                     "[APP-WGPU-PRESENT] attempts=%llu presented=%llu unavailable=%llu "
                     "lastStatus=%d encodeFailures=%llu captureRequests=%llu captureFailures=%llu\n",
                     (unsigned long long)wgpuPresentAttempts_,
                     (unsigned long long)presentedFrames_,
                     (unsigned long long)wgpuUnavailableFrames_,
                     wgpuLastSurfaceStatus_,
                     (unsigned long long)wgpuEncodeFailures_,
                     (unsigned long long)wgpuCaptureRequests_,
                     (unsigned long long)wgpuCaptureFailures_);
    }
    if (captureView_) {
        wgpuTextureViewRelease((WGPUTextureView)captureView_);
        captureView_ = nullptr;
    }
    if (captureTex_) {
        wgpuTextureRelease((WGPUTexture)captureTex_);
        captureTex_ = nullptr;
    }
    captureW_ = captureH_ = 0;
    if (wgpuSurface_) {
        if (wgpuSurfaceConfigured_) {
            wgpuSurfaceUnconfigure((WGPUSurface)wgpuSurface_);
            wgpuSurfaceConfigured_ = false;
        }
        wgpuSurfaceRelease((WGPUSurface)wgpuSurface_);
        wgpuSurface_ = nullptr;
    }
    if (wgpuQueue_) {
        wgpuQueueRelease((WGPUQueue)wgpuQueue_);
        wgpuQueue_ = nullptr;
    }
    if (wgpuDevice_) {
        gfx_webgpu_host_device_will_release((WGPUDevice)wgpuDevice_);
        wgpuDeviceDestroy((WGPUDevice)wgpuDevice_);
        wgpuDeviceRelease((WGPUDevice)wgpuDevice_);
        wgpuDevice_ = nullptr;
    }
    if (wgpuAdapter_) {
        wgpuAdapterRelease((WGPUAdapter)wgpuAdapter_);
        wgpuAdapter_ = nullptr;
    }
    if (wgpuInstance_) {
        wgpuInstanceRelease((WGPUInstance)wgpuInstance_);
        wgpuInstance_ = nullptr;
    }
    wgpuFormat_ = 0;
    cfgW_ = cfgH_ = 0;
    /* The surface no longer references this CAMetalLayer. */
    if (metalView_) { SDL_Metal_DestroyView((SDL_MetalView)metalView_); metalView_ = nullptr; }
#endif
    if (gl_) { SDL_GL_DeleteContext(gl_); gl_ = nullptr; }
    if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
    if (sdlOwned_) { SDL_Quit(); sdlOwned_ = false; }
}
