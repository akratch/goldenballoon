// ui_common.cpp — see ui_common.h.
#include "ui_common.h"
#include "app_theme.h"

#include "a11y_model.h"
#include "video_config.h"

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>

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

void Chip(const char *text, const ImVec4 &color) {
    if (text == nullptr || text[0] == '\0') return;
    const float scale = AppTheme::uiScale();
    ImGui::PushFont(AppTheme::fonts().small);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float padX = 7.0f * scale;
    const float padY = 2.0f * scale;
    const ImVec2 size(textSize.x + padX * 2.0f, textSize.y + padY * 2.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    // Reserve the pill, then paint it. Dummy keeps it in the layout flow, so a
    // chip can sit on a SameLine row without any manual cursor arithmetic.
    ImGui::Dummy(size);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImVec4 fill = color;
    fill.w = 0.16f;
    draw->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y),
                        ImGui::GetColorU32(fill), size.y * 0.5f);
    ImVec4 stroke = color;
    stroke.w = 0.38f;
    draw->AddRect(min, ImVec2(min.x + size.x, min.y + size.y),
                  ImGui::GetColorU32(stroke), size.y * 0.5f);
    draw->AddText(ImVec2(min.x + padX, min.y + padY),
                  ImGui::GetColorU32(color), text);
    ImGui::PopFont();
}

void GameplayChip() { Chip("Changes gameplay", AppTheme::warn()); }

void ConfirmModalSize() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float margin = 2.0f * kGapL * AppTheme::uiScale();
    float width = 420.0f * AppTheme::uiScale();
    if (viewport != nullptr && viewport->WorkSize.x - margin < width) {
        width = viewport->WorkSize.x - margin;
    }
    /* Height 0 = auto-fit that axis; only the width is pinned. */
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
}

void HelpMarker(const char *text) {
    if (text == nullptr || text[0] == '\0') return;
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::subtle());
    ImGui::TextUnformatted("(?)");
    ImGui::PopStyleColor();
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void GroupHeader(const char *title, const char *subtitle) {
    ImGui::PushFont(AppTheme::fonts().section);
    ImGui::TextUnformatted(title != nullptr ? title : "");
    ImGui::PopFont();
    if (subtitle != nullptr && subtitle[0] != '\0') {
        ImGui::PushFont(AppTheme::fonts().small);
        TextSubtleUnformattedWrapped(subtitle);
        ImGui::PopFont();
    }
    Gap(kGapXS);
    ImGui::Separator();
    Gap(kGapS);
}

void SettingLabel(const char *label, const char *description,
                  const RowStyle &style) {
    const float scale = AppTheme::uiScale();
    const ImVec2 rowTop = ImGui::GetCursorScreenPos();

    ImGui::TextUnformatted(label != nullptr ? label : "");
    if (style.gameplay) {
        ImGui::SameLine(0.0f, 8.0f * scale);
        GameplayChip();
    }
    if (style.experimental) {
        ImGui::SameLine(0.0f, 6.0f * scale);
        Chip("Experimental", AppTheme::brandSky());
    }
    if (style.restart) {
        ImGui::SameLine(0.0f, 6.0f * scale);
        Chip("Next launch", AppTheme::accent());
    }
    if (style.badge != nullptr && style.badge[0] != '\0') {
        ImGui::SameLine(0.0f, 6.0f * scale);
        Chip(style.badge, AppTheme::subtle());
    }
    if (style.tooltip != nullptr && style.tooltip[0] != '\0') {
        ImGui::SameLine(0.0f, 8.0f * scale);
        HelpMarker(style.tooltip);
    }
    if (description != nullptr && description[0] != '\0') {
        ImGui::PushFont(AppTheme::fonts().small);
        TextSubtleUnformattedWrapped(description);
        ImGui::PopFont();
    }
    if (!style.gameplay) return;
    // The chip scrolls away with the label; the rule does not. Draw it down the
    // whole label block so a player scanning the left edge can see which rows
    // touch gameplay without reading a word.
    const float bottom = ImGui::GetItemRectMax().y;
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(rowTop.x - 10.0f * scale, rowTop.y),
        ImVec2(rowTop.x - 10.0f * scale + 3.0f * scale, bottom),
        ImGui::GetColorU32(AppTheme::warn()), 1.5f * scale);
}

void CautionBox(const char *title, const char *body) {
    if (!CardBegin("##caution", AppTheme::warn(), 0.0f)) {
        CardEnd();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::warn());
    ImGui::TextUnformatted(title != nullptr ? title : "");
    ImGui::PopStyleColor();
    if (body != nullptr && body[0] != '\0') {
        ImGui::PushFont(AppTheme::fonts().small);
        TextSubtleUnformattedWrapped(body);
        ImGui::PopFont();
    }
    CardEnd();
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

// --- Self-voicing ----------------------------------------------------------
// See the contract in ui_common.h. Everything below this line is the whole of
// the shell's announcement surface.

namespace {

// What was said last, per stream. A settings panel redraws its focused row
// sixty times a second; without this the model would be handed sixty identical
// utterances, coalesce fifty-nine of them, and put sixty lines in the trace
// that no gate could read. Comparing the composed TEXT rather than the item id
// is deliberate: it also re-announces a value the player just changed, which is
// the confirmation they need and cannot get from the screen.
char g_lastFocusSpoken[MDKR_A11Y_TEXT_MAX];
char g_lastSectionSpoken[MDKR_A11Y_TEXT_MAX];

// Returns true and latches when `text` differs from what this stream last said.
bool claimIfNew(char *last, size_t capacity, const char *text) {
    if (text == nullptr || text[0] == '\0') return false;
    if (std::strncmp(last, text, capacity - 1) == 0) return false;
    std::snprintf(last, capacity, "%s", text);
    return true;
}

/*
 * As much of the help paragraph as the utterance buffer can hold, cut only at a
 * sentence boundary.
 *
 * The model refuses text that will not fit rather than chopping it mid-word,
 * which is the right rule and the reason this exists: three settings ship help
 * paragraphs longer than the whole buffer -- Frame limit, Allow tearing and
 * Camera motion -- so handing them straight over made the three controls with
 * the most to explain the only three that said nothing whatsoever. The name and
 * the value are what a player needs to act; the paragraph is what they need to
 * decide, and the first sentences of it are the part that says what the setting
 * does. Cutting at a full stop keeps every word that is spoken a word the
 * author wrote.
 *
 * Returns a pointer into `buffer`, or "" when not even one sentence fits.
 */
const char *helpThatFits(const char *name, const char *value, const char *help,
                         char *buffer, size_t capacity) {
    buffer[0] = '\0';
    if (help == nullptr || help[0] == '\0') return buffer;
    // Exactly what mdkr_a11y_focus() will spend before the help: the name, then
    // ", " + value when there is one, then the ". " that introduces the help.
    size_t spent = std::strlen(name) + 2u;
    if (value != nullptr && value[0] != '\0') spent += 2u + std::strlen(value);
    if (spent + 1u >= capacity) return buffer;
    size_t budget = capacity - 1u - spent;
    const size_t length = std::strlen(help);
    if (length <= budget) {
        std::snprintf(buffer, capacity, "%s", help);
        return buffer;
    }
    size_t cut = 0u;
    for (size_t i = 0u; i < budget && i < length; ++i) {
        // A boundary is a terminator followed by a space or the end of the
        // paragraph -- "4:3." inside a sentence is not one.
        if ((help[i] == '.' || help[i] == '!' || help[i] == '?') &&
            (help[i + 1u] == '\0' || help[i + 1u] == ' ')) {
            cut = i + 1u;
        }
    }
    if (cut == 0u) return buffer;  // no whole sentence fits: say none of it
    std::memcpy(buffer, help, cut);
    buffer[cut] = '\0';
    return buffer;
}

}  // namespace

bool SpeechEnabled() {
    // The live config, not a cached copy: Accessibility.Speech is LIVE, so a
    // player who switches it on hears the very next row they move to.
    const MdkrVideoConfig *config = mdkr_video_config_current();
    return config != nullptr &&
           config->values[MDKR_A11Y_SPEECH].number != 0.0f;
}

void SpeakFocusedItem(const char *name, const char *value, const char *help) {
    // Focus first: it is the cheapest test and it is false for all but one row.
    if (!ImGui::IsItemFocused()) return;
    if (!SpeechEnabled()) return;
    if (name == nullptr || name[0] == '\0') return;
    // Compose the identity exactly as mdkr_a11y_focus() will compose the
    // utterance's opening, so "the same row with the same value" is recognised
    // whatever the help paragraph does.
    char identity[MDKR_A11Y_TEXT_MAX];
    std::snprintf(identity, sizeof(identity), "%s|%s", name,
                  value != nullptr ? value : "");
    if (!claimIfNew(g_lastFocusSpoken, sizeof(g_lastFocusSpoken), identity)) {
        return;
    }
    char trimmedHelp[MDKR_A11Y_TEXT_MAX];
    mdkr_a11y_focus(name, value,
                    helpThatFits(name, value, help, trimmedHelp,
                                 sizeof(trimmedHelp)));
}

void SpeakSection(const char *name) {
    if (!SpeechEnabled()) return;
    if (name == nullptr || name[0] == '\0') return;
    if (!claimIfNew(g_lastSectionSpoken, sizeof(g_lastSectionSpoken), name)) {
        return;
    }
    char text[MDKR_A11Y_TEXT_MAX];
    std::snprintf(text, sizeof(text), "%s section", name);
    // NORMAL, not LOW: knowing which panel you are in is what makes the row
    // announcements that follow it mean anything.
    mdkr_a11y_announce(MDKR_A11Y_CAT_SECTION, MDKR_A11Y_PRI_NORMAL, text);
    // A new section makes the previous row's announcement stale: the next row
    // must speak even if the panel happens to open on one with the same name
    // and value.
    g_lastFocusSpoken[0] = '\0';
}

}  // namespace ui
