// app_theme.cpp — see app_theme.h.
#include "app_theme.h"
#include "app_font.h"

namespace AppTheme {

static AppFonts g_fonts;
static float      g_fbScale   = 1.0f;   // framebuffer/logical ratio from setup()
static float      g_uiScale   = 1.0f;   // player UI.Scale multiplier
static ImGuiStyle g_baseStyle;          // metrics at scale 1.0, captured in setup()
static bool       g_baseCaptured = false;

ImVec4 hex(unsigned rgb, float a) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                  ((rgb >> 8) & 0xFF) / 255.0f,
                  (rgb & 0xFF) / 255.0f, a);
}

// Brand palette (macos/BRANDING.md).
ImVec4 primary() { return hex(0x4A6FA5); }  // Steel Blue
ImVec4 accent()  { return hex(0xD4A843); }  // Amber Gold
ImVec4 surface() { return hex(0x2C2C2E); }  // Graphite
ImVec4 subtle()  { return hex(0x8E8E93); }  // System Gray
ImVec4 good()    { return hex(0x5CC08A); }  // green
ImVec4 bad()     { return hex(0xE5675E); }  // red

const AppFonts &fonts() { return g_fonts; }

static void applyStyle() {
    ImGuiStyle &s = ImGui::GetStyle();

    // Metrics — generous, rounded, calm.
    s.WindowPadding     = ImVec2(18, 16);
    s.FramePadding      = ImVec2(12, 7);
    s.ItemSpacing       = ImVec2(10, 9);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.CellPadding       = ImVec2(8, 6);
    s.IndentSpacing     = 18;
    s.ScrollbarSize     = 12;
    s.GrabMinSize       = 12;
    s.WindowBorderSize  = 1;
    s.ChildBorderSize   = 1;
    s.FrameBorderSize   = 0;
    s.WindowRounding    = 9;
    s.ChildRounding     = 9;
    s.FrameRounding     = 6;
    s.PopupRounding     = 6;
    s.GrabRounding      = 6;
    s.ScrollbarRounding = 8;
    s.TabRounding       = 6;
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);

    ImVec4 *c = s.Colors;
    const ImVec4 bg      = hex(0x161618);  // window background (below charcoal)
    const ImVec4 panel   = hex(0x1C1C1E);  // charcoal
    const ImVec4 card    = hex(0x2C2C2E);  // graphite
    const ImVec4 cardHi  = hex(0x3A3A3D);
    const ImVec4 text    = hex(0xECECEE);
    const ImVec4 border  = ImVec4(1, 1, 1, 0.06f);
    const ImVec4 pri     = primary();
    const ImVec4 acc     = accent();

    c[ImGuiCol_Text]                 = text;
    c[ImGuiCol_TextDisabled]         = subtle();
    c[ImGuiCol_WindowBg]             = bg;
    c[ImGuiCol_ChildBg]              = panel;
    c[ImGuiCol_PopupBg]              = hex(0x202022);
    c[ImGuiCol_Border]               = border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = card;
    c[ImGuiCol_FrameBgHovered]       = cardHi;
    c[ImGuiCol_FrameBgActive]        = ImVec4(pri.x, pri.y, pri.z, 0.55f);
    c[ImGuiCol_TitleBg]              = panel;
    c[ImGuiCol_TitleBgActive]        = panel;
    c[ImGuiCol_TitleBgCollapsed]     = panel;
    c[ImGuiCol_MenuBarBg]            = panel;
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = cardHi;
    c[ImGuiCol_ScrollbarGrabHovered] = subtle();
    c[ImGuiCol_ScrollbarGrabActive]  = pri;
    c[ImGuiCol_CheckMark]            = acc;
    c[ImGuiCol_SliderGrab]           = pri;
    c[ImGuiCol_SliderGrabActive]     = acc;
    c[ImGuiCol_Button]               = card;
    c[ImGuiCol_ButtonHovered]        = cardHi;
    c[ImGuiCol_ButtonActive]         = ImVec4(pri.x, pri.y, pri.z, 0.85f);
    c[ImGuiCol_Header]               = ImVec4(pri.x, pri.y, pri.z, 0.30f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(pri.x, pri.y, pri.z, 0.45f);
    c[ImGuiCol_HeaderActive]         = ImVec4(pri.x, pri.y, pri.z, 0.65f);
    c[ImGuiCol_Separator]            = border;
    c[ImGuiCol_SeparatorHovered]     = ImVec4(pri.x, pri.y, pri.z, 0.55f);
    c[ImGuiCol_SeparatorActive]      = pri;
    c[ImGuiCol_ResizeGrip]           = ImVec4(1, 1, 1, 0.04f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(pri.x, pri.y, pri.z, 0.5f);
    c[ImGuiCol_ResizeGripActive]     = pri;
    c[ImGuiCol_Tab]                  = card;
    c[ImGuiCol_TabHovered]           = ImVec4(pri.x, pri.y, pri.z, 0.55f);
    c[ImGuiCol_TabActive]            = ImVec4(pri.x, pri.y, pri.z, 0.85f);
    c[ImGuiCol_TabUnfocused]         = card;
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(pri.x, pri.y, pri.z, 0.5f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(pri.x, pri.y, pri.z, 0.45f);
    c[ImGuiCol_NavHighlight]         = acc;
    c[ImGuiCol_DragDropTarget]       = acc;
}

float uiScale() { return g_uiScale; }

void setUiScale(float uiScale) {
    if (uiScale < 0.5f) uiScale = 0.5f;
    if (uiScale > 3.0f) uiScale = 3.0f;
    if (!g_baseCaptured) return;             // setup() not run yet
    if (uiScale == g_uiScale) return;        // no-op: cheap to call every frame
    g_uiScale = uiScale;

    // Rescale metrics from the pristine base so repeated calls never compound.
    ImGuiStyle &s = ImGui::GetStyle();
    s = g_baseStyle;
    s.ScaleAllSizes(uiScale);
    // Fonts are rasterized at g_fbScale*pt; FontGlobalScale = uiScale/g_fbScale
    // makes the displayed size pt*uiScale (crisp on Retina, larger on handhelds).
    ImGui::GetIO().FontGlobalScale = uiScale / g_fbScale;
}

void setup(float fbScale) {
    if (fbScale < 1.0f) fbScale = 1.0f;
    g_fbScale = fbScale;

    applyStyle();
    // Capture the scale-1.0 metrics so setUiScale can rescale from a clean base.
    g_baseStyle = ImGui::GetStyle();
    g_baseCaptured = true;
    g_uiScale = 1.0f;

    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    // Rasterize at physical pixels for crispness; FontGlobalScale brings the
    // displayed size back to logical points.
    // A clear size hierarchy (body 17 / title 24 / small 13) so titles read as
    // titles even at one weight. Rasterized at physical px for Retina crispness.
    g_fonts.body  = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        RobotoMedium_compressed_data_base85, 17.0f * fbScale);
    g_fonts.title = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        RobotoMedium_compressed_data_base85, 24.0f * fbScale);
    g_fonts.small = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        RobotoMedium_compressed_data_base85, 13.0f * fbScale);
    // NOTE: this ImGui builds the atlas lazily (dynamic textures); no Build().
    io.FontGlobalScale = 1.0f / fbScale;
    io.FontDefault = g_fonts.body;
}

}  // namespace AppTheme
