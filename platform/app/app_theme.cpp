// app_theme.cpp — see app_theme.h.
#include "app_theme.h"
#include "app_font.h"
#include "app_ui_policy.h"

namespace AppTheme {

static AppFonts g_fonts;
static float      g_fbScale   = 1.0f;   // framebuffer/logical ratio from setup()
static float      g_uiScale   = 1.0f;   // player UI.Scale multiplier
static ImGuiStyle g_baseStyle;          // metrics at scale 1.0, captured in setup()
static bool       g_baseCaptured = false;
static bool       g_scalePending = false;
static float      g_pendingScale = 1.0f;
static unsigned   g_scaleApplicationCount = 0;
static AppUiDpiState g_dpiState;

static void buildFonts(float fbScale) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    // Roboto Medium is the sole redistributable family currently embedded in
    // the application. Size and color provide the hierarchy without reaching
    // into a platform font path that would make release output non-reproducible.
    g_fonts.body = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        RobotoMedium_compressed_data_base85, 17.0f * fbScale);
    g_fonts.title = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        RobotoMedium_compressed_data_base85, 24.0f * fbScale);
    // One step between title and body. A settings page has three levels of
    // heading — page, group, setting — and two font sizes could only spell two
    // of them, which is why every group used to look like another page title.
    g_fonts.section = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        RobotoMedium_compressed_data_base85, 19.0f * fbScale);
    g_fonts.small = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        RobotoMedium_compressed_data_base85, 13.0f * fbScale);
    io.FontDefault = g_fonts.body;
    io.FontGlobalScale = g_uiScale / fbScale;
}

ImVec4 hex(unsigned rgb, float a) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                  ((rgb >> 8) & 0xFF) / 255.0f,
                  (rgb & 0xFF) / 255.0f, a);
}

// Native companion to the public web shell's gold-and-blue identity.
ImVec4 primary() { return hex(0x315C98); }  // Brand cobalt
ImVec4 accent()  { return hex(0xD4A843); }  // Amber Gold
ImVec4 brandSky() { return hex(0x71BEF9); }  // Website sky / wordmark blue
ImVec4 surface() { return hex(0x2C2C2E); }  // Graphite
// Secondary copy carries instructions, paths, and setting descriptions. Keep
// it comfortably readable on ordinary SDR displays instead of treating it as
// disposable decoration.
ImVec4 subtle()  { return hex(0xB0B0B6); }
ImVec4 good()    { return hex(0x5CC08A); }  // green
ImVec4 bad()     { return hex(0xE5675E); }  // red
ImVec4 warn()    { return hex(0xEE9A3C); }  // orange — changes gameplay
ImVec4 line()    { return ImVec4(1, 1, 1, 0.08f); }
ImVec4 raised()  { return hex(0x1E1E22); }
ImVec4 field()   { return hex(0x27272C); }

// Navigation. See the contract in app_theme.h: the selected fill is solid brand
// cobalt on purpose, and the modern treatment lives in the geometry.
ImVec4 navSelected()       { return primary(); }
ImVec4 navSelectedActive() { return hex(0x284B7C); }
ImVec4 navHover()          { return hex(0x3A3A3D); }
ImVec4 navPressed()        { return hex(0x424247); }

ImVec4 groupHeader()       { return hex(0x212127); }
ImVec4 groupHeaderHover()  { return hex(0x2C2C33); }
ImVec4 groupHeaderActive() { return hex(0x37373F); }

ImVec4 focusRing()         { return accent(); }

ButtonColors destructiveButton() {
    const ImVec4 base = bad();
    const auto clamp1 = [](float v) { return v > 1.0f ? 1.0f : v; };
    return {
        base,
        ImVec4(clamp1(base.x * 1.12f), clamp1(base.y * 1.12f),
               clamp1(base.z * 1.12f), 1.0f),
        ImVec4(base.x * 0.82f, base.y * 0.82f, base.z * 0.82f, 1.0f),
        hex(0x1A0A08),
    };
}

ImVec4 qrLight() { return ImVec4(1, 1, 1, 1); }
ImVec4 qrDark()  { return ImVec4(0, 0, 0, 1); }

const AppFonts &fonts() { return g_fonts; }

static void applyStyle() {
    ImGuiStyle &s = ImGui::GetStyle();

    // Metrics — generous, rounded, calm.
    s.WindowPadding     = ImVec2(18, 16);
    // A 17 px body font plus 10 px vertical frame padding produces a 37 px
    // visible control. TouchExtraPadding expands that to a 45 px effective
    // target without making the desktop launcher needlessly sparse.
    s.FramePadding      = ImVec2(12, 10);
    s.ItemSpacing       = ImVec2(10, 10);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.CellPadding       = ImVec2(8, 6);
    s.TouchExtraPadding = ImVec2(4, 4);
    s.IndentSpacing     = 18;
    // Both stay at 20 * uiScale. They look heavier than a desktop app needs,
    // and the first pass of this theme slimmed them to 16/18 -- which the
    // handheld touch gate correctly rejected: main_app.cpp's touch contract
    // requires a 20 px scrollbar and grab so a finger can hit them, and
    // TouchScrollCurrentWindow sizes its gesture-exclusion strip from
    // ScrollbarSize. Making the scrollbar look lighter is a job for the grab
    // COLOUR, not for its width.
    s.ScrollbarSize     = 20;
    s.GrabMinSize       = 20;
    s.WindowBorderSize  = 1;
    s.ChildBorderSize   = 1;
    // A hairline on every frame is what separates "a control" from "a coloured
    // rectangle" on a dark ground. It costs no layout height: ImGui draws the
    // border inside the frame rect.
    s.FrameBorderSize   = 1;
    s.PopupBorderSize   = 1;
    s.WindowRounding    = 10;
    s.ChildRounding     = 12;
    s.FrameRounding     = 8;
    s.PopupRounding     = 10;
    s.GrabRounding      = 8;
    s.ScrollbarRounding = 10;
    s.TabRounding       = 8;
    s.DisabledAlpha     = 0.45f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextPadding = ImVec2(0.0f, 10.0f);
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);

    ImVec4 *c = s.Colors;
    // Four levels of elevation instead of two. The page sits on `bg`, a panel
    // on `panel`, a settings group on `raised`, and a control on `card`. Every
    // step is small on purpose — the hairline does the separating, not contrast.
    const ImVec4 bg      = hex(0x121215);
    const ImVec4 panel   = hex(0x18181B);
    const ImVec4 card    = field();       // 0x27272C — control surface
    const ImVec4 cardHi  = hex(0x323238);
    const ImVec4 text    = hex(0xEDEDF0);
    const ImVec4 border  = line();
    const ImVec4 pri     = primary();
    const ImVec4 acc     = accent();

    c[ImGuiCol_Text]                 = text;
    c[ImGuiCol_TextDisabled]         = subtle();
    c[ImGuiCol_WindowBg]             = bg;
    c[ImGuiCol_ChildBg]              = panel;
    c[ImGuiCol_PopupBg]              = hex(0x1E1E22);
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
    // A shade above the control surface: a button is pressable, a combo's
    // background is not, and at identical values the eye cannot tell them apart.
    c[ImGuiCol_Button]               = hex(0x2E2E35);
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
    // Hover is a neutral lift, not a second selection. Any stock tab strip
    // therefore keeps exactly one cobalt active destination.
    c[ImGuiCol_TabHovered]           = cardHi;
    c[ImGuiCol_TabActive]            = pri;
    c[ImGuiCol_TabUnfocused]         = card;
    c[ImGuiCol_TabUnfocusedActive]   = hex(0x284B7C);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(pri.x, pri.y, pri.z, 0.45f);
    c[ImGuiCol_NavHighlight]         = focusRing();
    c[ImGuiCol_DragDropTarget]       = acc;
}

float uiScale() { return g_uiScale; }
unsigned atlasGeneration() { return g_dpiState.atlasGeneration; }
unsigned uiScaleApplicationCount() { return g_scaleApplicationCount; }

void setUiScale(float uiScale) {
    if (uiScale < 0.75f) uiScale = 0.75f;
    if (uiScale > 2.0f) uiScale = 2.0f;
    if (!g_baseCaptured) return;             // setup() not run yet
    if (uiScale == g_uiScale) return;        // no-op: cheap to call every frame
    g_uiScale = uiScale;
    ++g_scaleApplicationCount;

    // Rescale metrics from the pristine base so repeated calls never compound.
    ImGuiStyle &s = ImGui::GetStyle();
    s = g_baseStyle;
    s.ScaleAllSizes(uiScale);
    // Fonts are rasterized at g_fbScale*pt; FontGlobalScale = uiScale/g_fbScale
    // makes the displayed size pt*uiScale (crisp on Retina, larger on handhelds).
    ImGui::GetIO().FontGlobalScale = uiScale / g_fbScale;
}

void requestUiScale(float uiScale) {
    if (uiScale < 0.75f) uiScale = 0.75f;
    if (uiScale > 2.0f) uiScale = 2.0f;
    g_pendingScale = uiScale;
    g_scalePending = true;
}

void applyPendingUiScale() {
    if (!g_scalePending) return;
    g_scalePending = false;
    setUiScale(g_pendingScale);
}

void setup(float fbScale) {
    if (fbScale < 1.0f) fbScale = 1.0f;
    g_fbScale = fbScale;
    g_dpiState.framebufferScale = fbScale;
    g_dpiState.atlasGeneration = 1;

    applyStyle();
    // Capture the scale-1.0 metrics so setUiScale can rescale from a clean base.
    g_baseStyle = ImGui::GetStyle();
    g_baseCaptured = true;
    g_uiScale = 1.0f;
    g_scalePending = false;
    g_pendingScale = 1.0f;
    g_scaleApplicationCount = 0;

    buildFonts(fbScale);
}

void refreshFramebufferScale(float fbScale) {
    if (!g_baseCaptured) return;
    if (!AppUi_applyDpiTransition(&g_dpiState, fbScale)) return;
    g_fbScale = g_dpiState.framebufferScale;
    buildFonts(g_fbScale);
}

}  // namespace AppTheme
