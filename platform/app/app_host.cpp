// app_host.cpp — see app_host.h.
#include "app_host.h"
#include "app_theme.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include <cstdio>
#include <cstdint>
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
#  include "gfx_webgpu_imgui.h"
#endif

// Backend selector (platform/platform_sdl_min.c). The shell asks the SAME
// resolver the engine does — MDKR_RENDERER, validated and cached once — so the
// launcher can never bring up a device the engine would then refuse to adopt.
#include "platform_os.h"

// The GLSL version string handed to the ImGui GL3 backend. Matches the engine's
// "#version 330 core" shaders; valid on the macOS 4.1-core context too.
static const char *kGlslVersion = "#version 330 core";

bool AppHost::init(const char *title, int width, int height) {
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) < 0) {
            std::fprintf(stderr, "[app] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        sdlOwned_ = true;
    }

#ifdef MDKR_WEBGPU_BACKEND
    // MDKR_RENDERER=webgpu selects WebGPU end to end. The native default is a GL
    // window that the engine then adopts, so both halves stay on one device.
    useWebGpu_ = (mdkr_render_backend() == MDKR_BACKEND_WEBGPU);
    if (useWebGpu_) {
        return initWebGpu(title, width, height);
    }
#endif
    return initGL(title, width, height);
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
                   SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN;
    window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               width, height, flags);
    if (!window_) {
        std::fprintf(stderr, "[app] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    gl_ = SDL_GL_CreateContext(window_);
    if (!gl_) {
        std::fprintf(stderr, "[app] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_);
    SDL_GL_SetSwapInterval(1);  // vsync

#if !defined(__APPLE__)
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::fprintf(stderr, "[app] gladLoadGLLoader failed\n");
        return false;
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't litter imgui.ini in the cwd (persist later)
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    AppTheme::setup(framebufferScale());
    if (!ImGui_ImplSDL2_InitForOpenGL(window_, gl_)) {
        std::fprintf(stderr, "[app] ImGui_ImplSDL2_InitForOpenGL failed\n");
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(kGlslVersion)) {
        std::fprintf(stderr, "[app] ImGui_ImplOpenGL3_Init failed\n");
        return false;
    }
    imguiReady_ = true;
    std::fprintf(stderr, "[app] host: OpenGL\n");
    return true;
}

#ifdef MDKR_WEBGPU_BACKEND
bool AppHost::initWebGpu(const char *title, int width, int height) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;   // wgpu-native wraps the CAMetalLayer as its surface
#endif
    window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               width, height, flags);
    if (!window_) {
        std::fprintf(stderr, "[app] SDL_CreateWindow (WebGPU) failed: %s\n", SDL_GetError());
        return false;
    }

    void *layer = nullptr;
#if defined(__APPLE__)
    metalView_ = SDL_Metal_CreateView(window_);
    if (!metalView_) {
        std::fprintf(stderr, "[app] SDL_Metal_CreateView failed: %s\n", SDL_GetError());
        return false;
    }
    layer = SDL_Metal_GetLayer((SDL_MetalView)metalView_);
#endif

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
    configureWgpuSurface(dw, dh);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    AppTheme::setup(framebufferScale());   // build fonts BEFORE gfx_webgpu_imgui_init
    if (!ImGui_ImplSDL2_InitForOther(window_)) {
        std::fprintf(stderr, "[app] ImGui_ImplSDL2_InitForOther failed\n");
        return false;
    }
    if (!gfx_webgpu_imgui_init(device, queue, fmt)) {
        std::fprintf(stderr, "[app] gfx_webgpu_imgui_init failed\n");
        return false;
    }
    imguiReady_ = true;
    std::fprintf(stderr, "[app] host: WebGPU (%dx%d drawable, format=%d)\n", dw, dh, fmt);
    return true;
}

void AppHost::configureWgpuSurface(int w, int h) {
    if (!useWebGpu_ || wgpuSurface_ == nullptr || wgpuDevice_ == nullptr || w <= 0 || h <= 0) {
        return;
    }
    WGPUSurfaceConfiguration cfg = {};
    cfg.device = (WGPUDevice)wgpuDevice_;
    cfg.format = (WGPUTextureFormat)wgpuFormat_;
    // The UI renders to an offscreen scene target and is copied here at present,
    // so the surface only needs to be a copy destination (+ the mandatory
    // RenderAttachment). Matches gfx_webgpu.c's surface usage.
    cfg.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
    cfg.width = (uint32_t)w;
    cfg.height = (uint32_t)h;
    cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
    cfg.presentMode = WGPUPresentMode_Fifo;   // vsync; matches the GL swap interval
    wgpuSurfaceConfigure((WGPUSurface)wgpuSurface_, &cfg);
    cfgW_ = (unsigned)w;
    cfgH_ = (unsigned)h;
}

void AppHost::ensureWgpuSceneTarget(int w, int h) {
    if (!useWebGpu_ || wgpuDevice_ == nullptr || w <= 0 || h <= 0) return;
    if (sceneTex_ != nullptr && sceneW_ == (unsigned)w && sceneH_ == (unsigned)h) return;
    if (sceneView_ != nullptr) { wgpuTextureViewRelease((WGPUTextureView)sceneView_); sceneView_ = nullptr; }
    if (sceneTex_ != nullptr)  { wgpuTextureRelease((WGPUTexture)sceneTex_); sceneTex_ = nullptr; }
    WGPUTextureDescriptor td = {};
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)w; td.size.height = (uint32_t)h; td.size.depthOrArrayLayers = 1;
    td.format = (WGPUTextureFormat)wgpuFormat_;   // match the surface for the present copy
    td.mipLevelCount = 1; td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture((WGPUDevice)wgpuDevice_, &td);
    if (tex == nullptr) return;
    sceneTex_ = tex;
    sceneView_ = wgpuTextureCreateView(tex, nullptr);
    sceneW_ = (unsigned)w; sceneH_ = (unsigned)h;
}
#endif  // MDKR_WEBGPU_BACKEND

void AppHost::beginFrame() {
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
        ImGui::NewFrame();
        return;
    }
#endif
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
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

    FILE *f = std::fopen(path, "wb");
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
        std::remove(path);
    }
    return ok;
}

#ifdef MDKR_WEBGPU_BACKEND
namespace {
struct WgpuMapReq { int done; WGPUMapAsyncStatus status; };
void on_map(WGPUMapAsyncStatus s, WGPUStringView m, void *u1, void *u2) {
    (void)m; (void)u2;
    WgpuMapReq *r = (WgpuMapReq *)u1; r->status = s; r->done = 1;
}

// Write a mapped 8-bit readback buffer (row stride `bpr`, top-down) as a 24-bit
// BMP. `bgra` selects the source channel order (BGRA8 — the near-universal
// surface format — vs RGBA8). BMP rows are bottom-up, so source rows are emitted
// in reverse; BMP pixels are B,G,R.
// Returns true only if the BMP was fully written (AUDIT-0046).
bool writeWgpuBmp(WGPUBuffer buf, uint32_t bpr, int w, int h, const char *path,
                  WGPUDevice device, bool bgra) {
    if (w <= 0 || h <= 0) return false;
    size_t size = (size_t)bpr * (uint32_t)h;
    WgpuMapReq mr = {0, (WGPUMapAsyncStatus)0};
    WGPUBufferMapCallbackInfo ci = {};
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = on_map;
    ci.userdata1 = &mr;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, size, ci);
    for (int i = 0; !mr.done && i < 100000; ++i) wgpuDevicePoll(device, true, nullptr);
    if (!mr.done || mr.status != WGPUMapAsyncStatus_Success) {
        std::fprintf(stderr, "[app] capture map failed (status=%d)\n", (int)mr.status);
        return false;
    }
    const uint8_t *px = (const uint8_t *)wgpuBufferGetConstMappedRange(buf, 0, size);
    FILE *f = px ? std::fopen(path, "wb") : nullptr;
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
            std::remove(path);
        }
    } else {
        std::fprintf(stderr, "[app] could not open %s\n", path);
    }
    wgpuBufferUnmap(buf);
    return ok;
}
}  // namespace

bool AppHost::endFrameWebGpu(const char *captureBmpPath) {
    ImGui::Render();

    int w = 0, h = 0;
    drawableSize(&w, &h);
    if (w <= 0 || h <= 0) return captureBmpPath == nullptr;   // couldn't capture
    ensureWgpuSceneTarget(w, h);
    if (sceneTex_ == nullptr || sceneView_ == nullptr) return captureBmpPath == nullptr;

    WGPUSurface surface = (WGPUSurface)wgpuSurface_;
    WGPUDevice device = (WGPUDevice)wgpuDevice_;
    WGPUQueue queue = (WGPUQueue)wgpuQueue_;

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, nullptr);

    // 1) Render the UI into the offscreen scene target (window-independent).
    WGPURenderPassColorAttachment ca = {};
    ca.view = (WGPUTextureView)sceneView_;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue.r = 0.06; ca.clearValue.g = 0.07; ca.clearValue.b = 0.09; ca.clearValue.a = 1.0;
    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    gfx_webgpu_imgui_render(ImGui::GetDrawData(), pass, w, h);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    // 2) Present: copy the scene into the window's surface texture (if the window
    // has a drawable — a hidden/occluded one does not, which is fine, the frame
    // still rendered offscreen and can be captured).
    if ((unsigned)w != cfgW_ || (unsigned)h != cfgH_) {
        configureWgpuSurface(w, h);
    }
    WGPUSurfaceTexture st = {};
    wgpuSurfaceGetCurrentTexture(surface, &st);
    bool present_ok = st.texture != nullptr &&
        (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
         st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal);
    if (present_ok) {
        WGPUTexelCopyTextureInfo cs = {};
        cs.texture = (WGPUTexture)sceneTex_; cs.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyTextureInfo cd = {};
        cd.texture = st.texture; cd.aspect = WGPUTextureAspect_All;
        WGPUExtent3D ext = { (uint32_t)w, (uint32_t)h, 1 };
        wgpuCommandEncoderCopyTextureToTexture(enc, &cs, &cd, &ext);
    }

    // 3) Optional capture (design review / CI): read the OFFSCREEN scene back —
    // window-independent, so it works even when the window has no drawable.
    WGPUBuffer capBuf = nullptr;
    uint32_t capBpr = 0;
    if (captureBmpPath) {
        capBpr = ((uint32_t)w * 4u + 255u) / 256u * 256u;   // 256-align for CopyTextureToBuffer
        WGPUBufferDescriptor bd = {};
        bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        bd.size = (uint64_t)capBpr * (uint32_t)h;
        capBuf = wgpuDeviceCreateBuffer(device, &bd);
        if (capBuf != nullptr) {
            WGPUTexelCopyTextureInfo src = {};
            src.texture = (WGPUTexture)sceneTex_; src.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferInfo dst = {};
            dst.buffer = capBuf;
            dst.layout.bytesPerRow = capBpr;
            dst.layout.rowsPerImage = (uint32_t)h;
            WGPUExtent3D ext = { (uint32_t)w, (uint32_t)h, 1 };
            wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
        }
    }

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    bool captureOk = (captureBmpPath == nullptr);
    if (capBuf != nullptr) {
        const bool bgra = ((WGPUTextureFormat)wgpuFormat_ == WGPUTextureFormat_BGRA8Unorm ||
                           (WGPUTextureFormat)wgpuFormat_ == WGPUTextureFormat_BGRA8UnormSrgb);
        captureOk = writeWgpuBmp(capBuf, capBpr, w, h, captureBmpPath, device, bgra);
        wgpuBufferRelease(capBuf);
    } else if (captureBmpPath != nullptr) {
        captureOk = false;   // requested a capture but the readback buffer alloc failed
    }

    if (present_ok) {
        wgpuSurfacePresent(surface);
    }
    if (st.texture != nullptr) {
        wgpuTextureRelease(st.texture);
    }
    return captureOk;
}
#endif  // MDKR_WEBGPU_BACKEND

bool AppHost::endFrameGL(const char *captureBmpPath) {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    bool captureOk = true;
    if (captureBmpPath) {
        captureOk = writeBackbufferBmp(captureBmpPath, drawableWidth(), drawableHeight());
    }
    SDL_GL_SwapWindow(window_);
    return captureOk;
}

// Returns true when no capture was requested, or the requested BMP was fully
// written; false when a requested capture failed (AUDIT-0046).
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

bool AppHost::processEvent(SDL_Event &e) {
    ImGui_ImplSDL2_ProcessEvent(&e);
    if (e.type == SDL_QUIT) {
        return true;
    }
    if (e.type == SDL_DROPFILE && e.drop.file) {
        droppedFile_ = e.drop.file;  // consumed by takeDroppedFile()
        SDL_free(e.drop.file);
        return false;
    }
    return e.type == SDL_WINDOWEVENT &&
           e.window.event == SDL_WINDOWEVENT_CLOSE &&
           e.window.windowID == SDL_GetWindowID(window_);
}

void AppHost::queueDropFileForSmoke(const char *path) {
    pendingSmokeDrop_ = path ? path : "";
}

bool AppHost::pumpAndShouldQuit() {
    bool quit = false;

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
            quit = processEvent(e) || quit;
        } else {
            std::fprintf(stderr, "[app] smoke: could not allocate drop path\n");
        }
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        quit = processEvent(e) || quit;
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

std::string AppHost::takeDroppedFile() {
    std::string s;
    s.swap(droppedFile_);
    return s;
}

void AppHost::shutdown() {
    if (imguiReady_) {
#ifdef MDKR_WEBGPU_BACKEND
        if (useWebGpu_) {
            gfx_webgpu_imgui_shutdown();
        } else
#endif
        {
            ImGui_ImplOpenGL3_Shutdown();
        }
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }
#ifdef MDKR_WEBGPU_BACKEND
    // Our own offscreen scene target is safe to release (nothing else references it).
    if (sceneView_) { wgpuTextureViewRelease((WGPUTextureView)sceneView_); sceneView_ = nullptr; }
    if (sceneTex_)  { wgpuTextureRelease((WGPUTexture)sceneTex_); sceneTex_ = nullptr; }
    // The engine adopts our device/surface and does not own them; the launcher's
    // objects are process-lived. Release only the metal view here (destroying it
    // before SDL_DestroyWindow avoids a dangling layer). The wgpu objects are
    // left to process teardown (a single shared device; freeing under an active
    // game adoption would be a use-after-free).
    if (metalView_) { SDL_Metal_DestroyView((SDL_MetalView)metalView_); metalView_ = nullptr; }
#endif
    if (gl_) { SDL_GL_DeleteContext(gl_); gl_ = nullptr; }
    if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
    if (sdlOwned_) { SDL_Quit(); sdlOwned_ = false; }
}
