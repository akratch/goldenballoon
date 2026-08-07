// ui_common.cpp — see ui_common.h.
#include "ui_common.h"
#include "app_theme.h"

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace ui {

// Scaled sizing: base desktop dimensions times the player's UI.Scale.
ImVec2 kBtnPrimary()   { float s = AppTheme::uiScale(); return ImVec2(190 * s, 48 * s); }
ImVec2 kBtnSecondary() { float s = AppTheme::uiScale(); return ImVec2(190 * s, 44 * s); }
ImVec2 kBtnWide()      { float s = AppTheme::uiScale(); return ImVec2(210 * s, 44 * s); }
float kTouchRowHeight() { return 44.0f * AppTheme::uiScale(); }
float  kControlWidth(float multiplier) {
    const float requested = 340.0f * AppTheme::uiScale() * multiplier;
    const float available = ImGui::GetContentRegionAvail().x;
    return std::max(1.0f, std::min(requested, available));
}
float  kNavWidth()     { return 244.0f * AppTheme::uiScale(); }
// Two checker rows of one tile each; BrandRule() below reserves exactly this.
float  kBrandRuleHeight() { return 8.0f * AppTheme::uiScale(); }

void Gap(float y) { ImGui::Dummy(ImVec2(0.0f, y)); }

void TextSubtle(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::subtle());
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
}

void TextSubtleWrapped(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::subtle());
    ImGui::TextWrapped("%s", buf);
    ImGui::PopStyleColor();
}

void TextSubtleUnformattedWrapped(const char *text) {
    if (text == nullptr) text = "";
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::subtle());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void BrandWordmark() {
    // Echo the public wordmark's gold/blue split with the embedded font. This
    // keeps native builds self-contained instead of adding a raster logo and
    // another DPI/resource failure path to the launcher.
    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
    ImGui::TextUnformatted("Golden");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 5.0f * AppTheme::uiScale());
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::brandSky());
    ImGui::TextUnformatted("Balloon");
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void BrandRule() {
    // The website uses a checkered start/finish gantry as structure. A quiet,
    // two-row rule carries that identity into the native shell without motion
    // or decorative imagery competing with the launcher task.
    const float scale = AppTheme::uiScale();
    const float tile = 4.0f * scale;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, kBrandRuleHeight()));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImVec4 gold = AppTheme::accent();
    ImVec4 blue = AppTheme::primary();
    gold.w = 0.72f;
    blue.w = 0.72f;
    const ImU32 colors[] = {
        ImGui::GetColorU32(gold), ImGui::GetColorU32(blue),
    };
    for (int row = 0; row < 2; ++row) {
        for (float x = 0.0f; x < width; x += tile) {
            draw->AddRectFilled(
                ImVec2(min.x + x, min.y + row * tile),
                ImVec2(min.x + (std::min)(x + tile, width),
                       min.y + (row + 1) * tile),
                colors[(static_cast<int>(x / tile) + row) & 1]);
        }
    }
}

void TouchScrollCurrentWindow() {
    ImGuiIO &io = ImGui::GetIO();
    static ImGuiID gestureOwner = 0;
    if (io.MouseSource != ImGuiMouseSource_TouchScreen) {
        if (!io.MouseDown[ImGuiMouseButton_Left]) gestureOwner = 0;
        return;
    }
    const ImGuiID windowOwner = ImGui::GetID("##touch-scroll-owner");
    if (io.MouseClicked[ImGuiMouseButton_Left] &&
        ImGui::GetScrollMaxY() > 0.0f && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered()) {
        // Leave the scrollbar's own enlarged interaction strip untouched.
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const float scrollbarGuard = ImGui::GetStyle().ScrollbarSize +
                                     ImGui::GetStyle().WindowPadding.x;
        if (io.MousePos.x < windowPos.x + windowSize.x - scrollbarGuard) {
            gestureOwner = windowOwner;
        }
    }
    if (gestureOwner != windowOwner) return;
    if (io.MouseReleased[ImGuiMouseButton_Left]) {
        gestureOwner = 0;
        return;
    }
    const float threshold = 8.0f * AppTheme::uiScale();
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, threshold)) return;
    // The gesture is claimed only when it begins away from a widget. Wait until
    // intent is predominantly vertical, then keep content glued to the finger.
    // No post-release animation means the gesture is immediately interruptible
    // and respects the launcher's reduced-motion contract.
    if (std::fabs(io.MouseDelta.y) < std::fabs(io.MouseDelta.x) * 0.75f) return;
    ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
}

void SectionHeader(const char *title, const char *subtitle) {
    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle && subtitle[0]) {
        ImGui::PushFont(AppTheme::fonts().small);
        TextSubtleWrapped("%s", subtitle);
        ImGui::PopFont();
    }
    ImGui::Separator();
    Gap(kGapS);
}

void RestartBadge() {
    ImGui::SameLine();
    ImGui::PushFont(AppTheme::fonts().small);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
    ImGui::TextUnformatted("• restart required");
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/*
 * The counterpart chip, for a setting that changes under the running game.
 *
 * It is deliberately SUBTLE where the restart badge is accent-coloured. The
 * restart badge warns: it tells a player their choice is not what they are
 * looking at yet. This one only informs, and a screen where every row shouts is
 * a screen where the one row that matters does not stand out.
 */
void LiveBadge(const char *text) {
    ImGui::SameLine();
    ImGui::PushFont(AppTheme::fonts().small);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::subtle());
    ImGui::Text("• %s", text != nullptr ? text : "");
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static void pushFilledButtonColors(const ImVec4 &p, const ImVec4 &text) {
    auto clamp1 = [](float v) { return v > 1.0f ? 1.0f : v; };
    ImGui::PushStyleColor(ImGuiCol_Button, p);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(clamp1(p.x * 1.15f), clamp1(p.y * 1.15f), clamp1(p.z * 1.15f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(p.x * 0.88f, p.y * 0.88f, p.z * 0.88f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, text);
}

bool PrimaryButton(const char *label, const ImVec2 &size) {
    pushFilledButtonColors(AppTheme::primary(), ImVec4(1, 1, 1, 1));
    // A zero size means "use the scaled default primary button".
    ImVec2 sz = (size.x == 0.0f && size.y == 0.0f) ? kBtnPrimary() : size;
    bool clicked = ImGui::Button(label, sz);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool BrandPrimaryButton(const char *label, const ImVec2 &size) {
    // Solid gold belongs to the launcher’s persistent next action. Keep this
    // scoped instead of recoloring in-game overlay actions as a side effect of
    // launcher branding work.
    pushFilledButtonColors(AppTheme::accent(), AppTheme::hex(0x33220A));
    const ImVec2 resolved =
        (size.x == 0.0f && size.y == 0.0f) ? kBtnPrimary() : size;
    const bool clicked = ImGui::Button(label, resolved);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool CardBegin(const char *id, const ImVec4 &borderColor, float height) {
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(borderColor.x, borderColor.y, borderColor.z, 0.55f));
    const ImGuiChildFlags flags = height > 0.0f
        ? ImGuiChildFlags_Borders
        : ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;
    bool open = ImGui::BeginChild(id, ImVec2(0, height), flags);
    return open;
}

void CardEnd() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace ui
