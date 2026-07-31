// ui_common.cpp — see ui_common.h.
#include "ui_common.h"
#include "app_theme.h"

#include <cstdarg>
#include <cstdio>

namespace ui {

// Scaled sizing: base desktop dimensions times the player's UI.Scale.
ImVec2 kBtnPrimary()   { float s = AppTheme::uiScale(); return ImVec2(190 * s, 46 * s); }
ImVec2 kBtnSecondary() { float s = AppTheme::uiScale(); return ImVec2(190 * s, 40 * s); }
ImVec2 kBtnWide()      { float s = AppTheme::uiScale(); return ImVec2(210 * s, 40 * s); }
float  kControlWidth() { return 340.0f * AppTheme::uiScale(); }
float  kNavWidth()     { return 244.0f * AppTheme::uiScale(); }

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

void SectionHeader(const char *title, const char *subtitle) {
    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle && subtitle[0]) {
        ImGui::PushFont(AppTheme::fonts().small);
        TextSubtle("%s", subtitle);
        ImGui::PopFont();
    }
    Gap(kGapS);
    ImGui::Separator();
    Gap(kGapM);
}

void RestartBadge() {
    ImGui::SameLine();
    ImVec4 c = AppTheme::subtle();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x, c.y, c.z, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7, 2));
    ImGui::SmallButton("restart");   // non-interactive chip look
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void PushPrimaryButtonColors() {
    ImVec4 p = AppTheme::primary();
    auto clamp1 = [](float v) { return v > 1.0f ? 1.0f : v; };
    ImGui::PushStyleColor(ImGuiCol_Button, p);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(clamp1(p.x * 1.15f), clamp1(p.y * 1.15f), clamp1(p.z * 1.15f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(p.x * 0.88f, p.y * 0.88f, p.z * 0.88f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
}

void PopPrimaryButtonColors() { ImGui::PopStyleColor(4); }

bool PrimaryButton(const char *label, const ImVec2 &size) {
    PushPrimaryButtonColors();
    // A zero size means "use the scaled default primary button".
    ImVec2 sz = (size.x == 0.0f && size.y == 0.0f) ? kBtnPrimary() : size;
    bool clicked = ImGui::Button(label, sz);
    PopPrimaryButtonColors();
    return clicked;
}

bool CardBegin(const char *id, const ImVec4 &borderColor, float height) {
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(borderColor.x, borderColor.y, borderColor.z, 0.55f));
    bool open = ImGui::BeginChild(id, ImVec2(0, height), true);
    return open;
}

void CardEnd() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace ui
