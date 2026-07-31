/*
 * gfx_webgpu_imgui.cpp — minimal Dear ImGui renderer on WebGPU (see the header).
 * v29 API, ImGui 1.92 dynamic-texture model (ImGuiBackendFlags_RendererHasTextures).
 * Compiled C++, C ABI.
 *
 * ImGui 1.92 grows the font atlas on demand (glyphs are baked the first frame
 * they are used), so a backend MUST honor ImTextureData create/update/destroy
 * requests each frame — a one-time atlas upload drops every glyph added after
 * init (em dash, middle dot, …). This mirrors imgui_impl_opengl3's texture path.
 */
#include "gfx_webgpu_imgui.h"

#include "imgui.h"

/* WEB-055: route WebGPU through the single dialect seam instead of including
 * <webgpu/webgpu.h> + <webgpu/wgpu.h> directly — this was the only translation
 * unit bypassing gfx_webgpu_compat.h, and it fails to compile the moment the
 * overlay ships on web (emdawnwebgpu has no <webgpu/wgpu.h>). The compat header
 * pulls in webgpu.h + wgpu.h on native (this file is only built when
 * MGB64_APP=ON) and the emdawnwebgpu unified header on the browser. */
/* Seam rule: WebGPU types deliberately route through the compat header. */
#include "gfx_webgpu_compat.h" // IWYU pragma: keep

#include <cstring>
#include <cstdint>
#include <vector>

namespace {

WGPUDevice        s_device = nullptr;
WGPUQueue         s_queue = nullptr;
WGPUTextureFormat s_format = WGPUTextureFormat_Undefined;

WGPUShaderModule    s_module = nullptr;
WGPURenderPipeline  s_pipe = nullptr;
WGPUBindGroupLayout s_bgl0 = nullptr;   /* uniform + sampler (fixed) */
WGPUBindGroupLayout s_bgl1 = nullptr;   /* texture (per-image) */
WGPUPipelineLayout  s_pl = nullptr;
WGPUBuffer          s_ubuf = nullptr;   /* 16-float ortho */
WGPUSampler         s_sampler = nullptr;
WGPUBindGroup       s_bg0 = nullptr;

/* Growable per-frame vertex/index buffers. */
WGPUBuffer s_vbuf = nullptr; size_t s_vbuf_cap = 0;
WGPUBuffer s_ibuf = nullptr; size_t s_ibuf_cap = 0;

/* Single-entry image bind-group cache keyed on the texture-view pointer. */
WGPUBindGroup   s_img_bg = nullptr;
WGPUTextureView s_img_bg_key = nullptr;

WGPUStringView sv(const char *s) { WGPUStringView v; v.data = s; v.length = s ? std::strlen(s) : 0; return v; }

const char *kWGSL =
    "struct U { mvp : mat4x4<f32> };\n"
    "@group(0) @binding(0) var<uniform> u : U;\n"
    "@group(0) @binding(1) var samp : sampler;\n"
    "@group(1) @binding(0) var tex : texture_2d<f32>;\n"
    "struct VOut { @builtin(position) pos : vec4<f32>, @location(0) uv : vec2<f32>, @location(1) col : vec4<f32> };\n"
    "@vertex fn vs_main(@location(0) pos : vec2<f32>, @location(1) uv : vec2<f32>, @location(2) col : vec4<f32>) -> VOut {\n"
    "  var o : VOut;\n"
    "  o.pos = u.mvp * vec4<f32>(pos, 0.0, 1.0);\n"
    "  o.uv = uv;\n"
    "  o.col = col;\n"
    "  return o;\n}\n"
    "@fragment fn fs_main(in : VOut) -> @location(0) vec4<f32> {\n"
    "  return in.col * textureSample(tex, samp, in.uv);\n}\n";

WGPUBindGroup image_bind_group(WGPUTextureView view) {
    if (s_img_bg != nullptr && s_img_bg_key == view) {
        return s_img_bg;
    }
    WGPUBindGroupEntry e = {};
    e.binding = 0;
    e.textureView = view;
    WGPUBindGroupDescriptor d = {};
    d.layout = s_bgl1;
    d.entryCount = 1;
    d.entries = &e;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(s_device, &d);
    if (bg != nullptr) {
        if (s_img_bg != nullptr) wgpuBindGroupRelease(s_img_bg);
        s_img_bg = bg;
        s_img_bg_key = view;
    }
    return bg;
}

/* Honor one ImTextureData create/update/destroy request. TexID stores the
 * WGPUTextureView (referenced by draw commands); BackendUserData stores the
 * owning WGPUTexture so destroy can release both. Mirrors the GL backend. */
void update_texture(ImTextureData *tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
        WGPUTextureDescriptor td = {};
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = (uint32_t)tex->Width; td.size.height = (uint32_t)tex->Height;
        td.size.depthOrArrayLayers = 1;
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.mipLevelCount = 1; td.sampleCount = 1;
        WGPUTexture wtex = wgpuDeviceCreateTexture(s_device, &td);
        if (wtex == nullptr) return;

        WGPUTexelCopyTextureInfo dst = {};
        dst.texture = wtex; dst.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout lay = {};
        lay.bytesPerRow = (uint32_t)tex->Width * 4u; lay.rowsPerImage = (uint32_t)tex->Height;
        WGPUExtent3D ext = { (uint32_t)tex->Width, (uint32_t)tex->Height, 1 };
        wgpuQueueWriteTexture(s_queue, &dst, tex->GetPixels(),
                              (size_t)tex->Width * tex->Height * 4u, &lay, &ext);

        WGPUTextureView view = wgpuTextureCreateView(wtex, nullptr);
        tex->BackendUserData = wtex;
        tex->SetTexID((ImTextureID)(intptr_t)view);
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        WGPUTexture wtex = (WGPUTexture)tex->BackendUserData;
        if (wtex == nullptr) return;
        static std::vector<unsigned char> tmp;
        for (const ImTextureRect &r : tex->Updates) {
            // Copy the sub-rect into a contiguous staging block (its source rows
            // are Width*4 apart in the atlas) so bytesPerRow is exactly r.w*4.
            tmp.resize((size_t)r.w * r.h * 4u);
            for (int y = 0; y < r.h; ++y) {
                std::memcpy(tmp.data() + (size_t)y * r.w * 4u,
                            tex->GetPixelsAt(r.x, r.y + y), (size_t)r.w * 4u);
            }
            WGPUTexelCopyTextureInfo dst = {};
            dst.texture = wtex; dst.aspect = WGPUTextureAspect_All;
            dst.origin.x = (uint32_t)r.x; dst.origin.y = (uint32_t)r.y;
            WGPUTexelCopyBufferLayout lay = {};
            lay.bytesPerRow = (uint32_t)r.w * 4u; lay.rowsPerImage = (uint32_t)r.h;
            WGPUExtent3D ext = { (uint32_t)r.w, (uint32_t)r.h, 1 };
            wgpuQueueWriteTexture(s_queue, &dst, tmp.data(), tmp.size(), &lay, &ext);
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        WGPUTextureView view = (WGPUTextureView)(intptr_t)tex->TexID;
        WGPUTexture wtex = (WGPUTexture)tex->BackendUserData;
        if (view != nullptr && view == s_img_bg_key) {
            wgpuBindGroupRelease(s_img_bg); s_img_bg = nullptr; s_img_bg_key = nullptr;
        }
        if (view != nullptr) wgpuTextureViewRelease(view);
        if (wtex != nullptr) wgpuTextureRelease(wtex);
        tex->SetTexID(ImTextureID_Invalid);
        tex->BackendUserData = nullptr;
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void clamp_rect(int *x, int *y, int *w, int *h, int maxw, int maxh) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x >= maxw || *y >= maxh) { *w = 0; *h = 0; return; }
    if (*x + *w > maxw) *w = maxw - *x;
    if (*y + *h > maxh) *h = maxh - *y;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

} // namespace

extern "C" bool gfx_webgpu_imgui_init(void *device, void *queue, int surface_format) {
    s_device = (WGPUDevice)device;
    s_queue = (WGPUQueue)queue;
    s_format = (WGPUTextureFormat)surface_format;
    if (s_device == nullptr || s_queue == nullptr) return false;

    ImGuiIO &io = ImGui::GetIO();
    io.BackendRendererName = "gfx_webgpu_imgui";
    // We honor ImGuiPlatformIO::Textures[] each frame (dynamic font atlas) and
    // use per-draw-command vertex offsets.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    WGPUShaderSourceWGSL src = {};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = sv(kWGSL);
    WGPUShaderModuleDescriptor smd = {};
    smd.nextInChain = (WGPUChainedStruct *)&src;
    s_module = wgpuDeviceCreateShaderModule(s_device, &smd);
    if (s_module == nullptr) return false;

    /* group 0: uniform (vertex) + sampler (fragment). */
    WGPUBindGroupLayoutEntry e0[2] = {};
    e0[0].binding = 0; e0[0].visibility = WGPUShaderStage_Vertex;
    e0[0].buffer.type = WGPUBufferBindingType_Uniform; e0[0].buffer.minBindingSize = 64;
    e0[1].binding = 1; e0[1].visibility = WGPUShaderStage_Fragment;
    e0[1].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor bgld0 = {};
    bgld0.entryCount = 2; bgld0.entries = e0;
    s_bgl0 = wgpuDeviceCreateBindGroupLayout(s_device, &bgld0);

    /* group 1: texture (fragment). */
    WGPUBindGroupLayoutEntry e1 = {};
    e1.binding = 0; e1.visibility = WGPUShaderStage_Fragment;
    e1.texture.sampleType = WGPUTextureSampleType_Float;
    e1.texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor bgld1 = {};
    bgld1.entryCount = 1; bgld1.entries = &e1;
    s_bgl1 = wgpuDeviceCreateBindGroupLayout(s_device, &bgld1);

    WGPUBindGroupLayout bgls[2] = { s_bgl0, s_bgl1 };
    WGPUPipelineLayoutDescriptor pld = {};
    pld.bindGroupLayoutCount = 2; pld.bindGroupLayouts = bgls;
    s_pl = wgpuDeviceCreatePipelineLayout(s_device, &pld);

    /* uniform buffer + sampler + group 0 bind group. */
    WGPUBufferDescriptor ubd = {};
    ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst; ubd.size = 64;
    s_ubuf = wgpuDeviceCreateBuffer(s_device, &ubd);
    WGPUSamplerDescriptor sd = {};
    sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
    sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sd.maxAnisotropy = 1;
    s_sampler = wgpuDeviceCreateSampler(s_device, &sd);
    WGPUBindGroupEntry bge0[2] = {};
    bge0[0].binding = 0; bge0[0].buffer = s_ubuf; bge0[0].size = 64;
    bge0[1].binding = 1; bge0[1].sampler = s_sampler;
    WGPUBindGroupDescriptor bgd0 = {};
    bgd0.layout = s_bgl0; bgd0.entryCount = 2; bgd0.entries = bge0;
    s_bg0 = wgpuDeviceCreateBindGroup(s_device, &bgd0);

    /* Vertex layout: ImDrawVert = pos(f32x2)@0, uv(f32x2)@8, col(unorm8x4)@16. */
    WGPUVertexAttribute attrs[3] = {};
    attrs[0].format = WGPUVertexFormat_Float32x2; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
    attrs[1].format = WGPUVertexFormat_Float32x2; attrs[1].offset = 8;  attrs[1].shaderLocation = 1;
    attrs[2].format = WGPUVertexFormat_Unorm8x4;  attrs[2].offset = 16; attrs[2].shaderLocation = 2;
    WGPUVertexBufferLayout vbl = {};
    vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.arrayStride = sizeof(ImDrawVert);
    vbl.attributeCount = 3; vbl.attributes = attrs;

    WGPUBlendState blend = {};
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    WGPUColorTargetState color = {};
    color.format = s_format; color.writeMask = WGPUColorWriteMask_All; color.blend = &blend;

    WGPUFragmentState fs = {};
    fs.module = s_module; fs.entryPoint = sv("fs_main");
    fs.targetCount = 1; fs.targets = &color;
    WGPURenderPipelineDescriptor pd = {};
    pd.layout = s_pl;
    pd.vertex.module = s_module; pd.vertex.entryPoint = sv("vs_main");
    pd.vertex.bufferCount = 1; pd.vertex.buffers = &vbl;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CW;   /* ImGui winds CW */
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    s_pipe = wgpuDeviceCreateRenderPipeline(s_device, &pd);
    if (s_pipe == nullptr) return false;

    return true;
}

extern "C" void gfx_webgpu_imgui_new_frame(void) { /* texture updates run in render */ }

extern "C" void gfx_webgpu_imgui_render(ImDrawData *draw_data, void *render_pass_encoder,
                                        int fb_width, int fb_height) {
    if (draw_data == nullptr || render_pass_encoder == nullptr || s_pipe == nullptr) return;

    /* Honor dynamic font-atlas texture requests before drawing (grow-on-demand). */
    if (draw_data->Textures != nullptr) {
        for (ImTextureData *tex : *draw_data->Textures) {
            if (tex->Status != ImTextureStatus_OK) update_texture(tex);
        }
    }

    if (draw_data->TotalVtxCount <= 0 || fb_width <= 0 || fb_height <= 0) return;
    WGPURenderPassEncoder pass = (WGPURenderPassEncoder)render_pass_encoder;

    /* Grow the vertex/index buffers. */
    size_t vbytes = (size_t)draw_data->TotalVtxCount * sizeof(ImDrawVert);
    size_t ibytes = (size_t)draw_data->TotalIdxCount * sizeof(ImDrawIdx);
    if (s_vbuf == nullptr || vbytes > s_vbuf_cap) {
        if (s_vbuf) wgpuBufferRelease(s_vbuf);
        s_vbuf_cap = vbytes + 8192;
        WGPUBufferDescriptor bd = {};
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst; bd.size = s_vbuf_cap;
        s_vbuf = wgpuDeviceCreateBuffer(s_device, &bd);
    }
    if (s_ibuf == nullptr || ibytes > s_ibuf_cap) {
        if (s_ibuf) wgpuBufferRelease(s_ibuf);
        s_ibuf_cap = ibytes + 8192;
        WGPUBufferDescriptor bd = {};
        bd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst; bd.size = s_ibuf_cap;
        s_ibuf = wgpuDeviceCreateBuffer(s_device, &bd);
    }
    if (s_vbuf == nullptr || s_ibuf == nullptr) return;

    /* Concatenate all lists into contiguous CPU staging, then one write each.
     * wgpuQueueWriteBuffer requires a 4-aligned size; 16-bit indices with an odd
     * total are not, so the index stage is padded to 4 (zero-filled). No gaps
     * between lists → draw offsets are plain cumulative counts. */
    static std::vector<unsigned char> vtx_stage, idx_stage;
    size_t ibytes_pad = (ibytes + 3) & ~size_t(3);
    vtx_stage.resize(vbytes);
    idx_stage.assign(ibytes_pad, 0);
    size_t vo = 0, io = 0;
    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList *cl = draw_data->CmdLists[n];
        size_t vb = (size_t)cl->VtxBuffer.Size * sizeof(ImDrawVert);
        size_t ib = (size_t)cl->IdxBuffer.Size * sizeof(ImDrawIdx);
        if (vb) std::memcpy(vtx_stage.data() + vo, cl->VtxBuffer.Data, vb);
        if (ib) std::memcpy(idx_stage.data() + io, cl->IdxBuffer.Data, ib);
        vo += vb; io += ib;
    }
    if (vbytes)   wgpuQueueWriteBuffer(s_queue, s_vbuf, 0, vtx_stage.data(), vbytes);
    if (ibytes_pad) wgpuQueueWriteBuffer(s_queue, s_ibuf, 0, idx_stage.data(), ibytes_pad);

    /* Ortho MVP (row-major; WGSL mat4x4 is column-major so u.mvp*v is the
     * row-vector transform, like the modern-mesh path). z -> 0.5 (WebGPU 0..1). */
    float L = draw_data->DisplayPos.x;
    float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
    float T = draw_data->DisplayPos.y;
    float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
    float mvp[16] = {
        2.0f / (R - L), 0.0f,           0.0f, 0.0f,
        0.0f,           2.0f / (T - B), 0.0f, 0.0f,
        0.0f,           0.0f,           0.5f, 0.0f,
        (R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f,
    };
    wgpuQueueWriteBuffer(s_queue, s_ubuf, 0, mvp, sizeof(mvp));

    wgpuRenderPassEncoderSetPipeline(pass, s_pipe);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, s_bg0, 0, nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, s_vbuf, 0, WGPU_WHOLE_SIZE);
    WGPUIndexFormat ifmt = sizeof(ImDrawIdx) == 2 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
    wgpuRenderPassEncoderSetIndexBuffer(pass, s_ibuf, ifmt, 0, WGPU_WHOLE_SIZE);

    ImVec2 clip_off = draw_data->DisplayPos;
    /* ImGui clip rects are in DisplayPos/DisplaySize (logical) space; scale to
     * the pixel framebuffer for the scissor (high-DPI: FramebufferScale = 2 on
     * a Retina/2x drawable). Mirrors every upstream imgui_impl backend. */
    ImVec2 clip_scale = draw_data->FramebufferScale;
    if (clip_scale.x <= 0.0f) clip_scale.x = 1.0f;
    if (clip_scale.y <= 0.0f) clip_scale.y = 1.0f;
    uint32_t global_vtx = 0, global_idx = 0;
    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList *cl = draw_data->CmdLists[n];
        for (int c = 0; c < cl->CmdBuffer.Size; c++) {
            const ImDrawCmd *cmd = &cl->CmdBuffer[c];
            if (cmd->UserCallback != nullptr) continue;   /* launcher uses none */
            int cx = (int)((cmd->ClipRect.x - clip_off.x) * clip_scale.x);
            int cy = (int)((cmd->ClipRect.y - clip_off.y) * clip_scale.y);
            int cw = (int)((cmd->ClipRect.z - cmd->ClipRect.x) * clip_scale.x);
            int ch = (int)((cmd->ClipRect.w - cmd->ClipRect.y) * clip_scale.y);
            clamp_rect(&cx, &cy, &cw, &ch, fb_width, fb_height);
            if (cw <= 0 || ch <= 0) continue;
            WGPUTextureView view = (WGPUTextureView)(intptr_t)cmd->GetTexID();
            if (view == nullptr) continue;   /* no texture bound: skip */
            WGPUBindGroup bg = image_bind_group(view);
            if (bg == nullptr) continue;
            wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch);
            wgpuRenderPassEncoderSetBindGroup(pass, 1, bg, 0, nullptr);
            wgpuRenderPassEncoderDrawIndexed(pass, cmd->ElemCount, 1,
                                             cmd->IdxOffset + global_idx,
                                             (int32_t)(cmd->VtxOffset + global_vtx), 0);
        }
        global_vtx += (uint32_t)cl->VtxBuffer.Size;
        global_idx += (uint32_t)cl->IdxBuffer.Size;
    }
}

extern "C" void gfx_webgpu_imgui_shutdown(void) {
    // Release every backend-owned atlas texture (ImGui still owns the ImTextureData).
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (ctx != nullptr) {
        for (ImTextureData *tex : ImGui::GetPlatformIO().Textures) {
            if (tex->BackendUserData != nullptr) {
                WGPUTextureView view = (WGPUTextureView)(intptr_t)tex->TexID;
                WGPUTexture wtex = (WGPUTexture)tex->BackendUserData;
                if (view != nullptr) wgpuTextureViewRelease(view);
                if (wtex != nullptr) wgpuTextureRelease(wtex);
                tex->SetTexID(ImTextureID_Invalid);
                tex->BackendUserData = nullptr;
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    }
    if (s_img_bg) { wgpuBindGroupRelease(s_img_bg); s_img_bg = nullptr; s_img_bg_key = nullptr; }
    if (s_vbuf) { wgpuBufferRelease(s_vbuf); s_vbuf = nullptr; s_vbuf_cap = 0; }
    if (s_ibuf) { wgpuBufferRelease(s_ibuf); s_ibuf = nullptr; s_ibuf_cap = 0; }
    if (s_bg0) { wgpuBindGroupRelease(s_bg0); s_bg0 = nullptr; }
    if (s_sampler) { wgpuSamplerRelease(s_sampler); s_sampler = nullptr; }
    if (s_ubuf) { wgpuBufferRelease(s_ubuf); s_ubuf = nullptr; }
    if (s_pipe) { wgpuRenderPipelineRelease(s_pipe); s_pipe = nullptr; }
    if (s_pl) { wgpuPipelineLayoutRelease(s_pl); s_pl = nullptr; }
    if (s_bgl1) { wgpuBindGroupLayoutRelease(s_bgl1); s_bgl1 = nullptr; }
    if (s_bgl0) { wgpuBindGroupLayoutRelease(s_bgl0); s_bgl0 = nullptr; }
    if (s_module) { wgpuShaderModuleRelease(s_module); s_module = nullptr; }
}
