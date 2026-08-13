// ui_launcher.cpp — nav rail, panel router, Play button, About.
#include "ui_launcher.h"
#include "app_host.h"
#include "app_ui_policy.h"
#include "app_theme.h"
#include "app_brand.h"
#include "ui_common.h"
#include "ui_online_room.h"
#include "ui_settings.h"
#include "party/libdatachannel_party_transport.h"
#include "party/native_party_host.h"

#include "imgui.h"
#include "SDL.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Panel {
    const char *label;
    void (*draw)(LauncherState &, LauncherAction &);
};

void drawSettingsPanel(LauncherState &s, LauncherAction &out);
void drawAboutPanel(LauncherState &s, LauncherAction &out);

const Panel kPanels[] = {
    // Panel 0 is the home. It was "Game ROM": a first-run chore, named after a
    // file format, permanently occupying the destination the launcher opens on
    // and that a returning player never needs again. It is now "Play", and the
    // ROM flow is the onboarding STATE of that home rather than a panel to
    // hunt for. Indices are unchanged -- kLauncherPanelCount and the nav smoke
    // contract pin them.
    {"Play",        RomPanel_draw},
    {"Online Room", OnlineRoomPanel_draw},
    {"Settings",    drawSettingsPanel},
    {"Diagnostics", DiagPanel_draw},
    {"About",       drawAboutPanel},
};
constexpr int kPanelCount = (int)(sizeof(kPanels) / sizeof(kPanels[0]));
static_assert(kPanelCount == kLauncherPanelCount,
              "launcher panel count must match the public smoke contract");

/*
 * Online races are not part of this release, and the room is already
 * fail-closed (no I/O, Start unreachable). This hides the SURFACE as well, so a
 * shipped launcher offers exactly what the release offers rather than a room
 * for a mode the build does not have. MDKR_ONLINE_ROOM_PREVIEW=1 shows the
 * panel for the online-room gates and for development.
 *
 * The panel keeps its INDEX either way, so the smoke-contract arrays and panel
 * routing stay stable and check_launcher_tabs' MDKR_APP_SMOKE_NAV_TARGET=1
 * still names the same destination.
 */
static bool panelVisible(int index) {
    if (index < 0 || index >= kPanelCount) return false;
    if (std::strcmp(kPanels[index].label, "Online Room") != 0) return true;
    static const bool preview = [] {
        const char *value = std::getenv("MDKR_ONLINE_ROOM_PREVIEW");
        return value != nullptr && value[0] == '1';
    }();
    return preview;
}

ImVec2 g_smokeTopTabMin[kPanelCount];
ImVec2 g_smokeTopTabMax[kPanelCount];
bool g_smokeTopTabValid[kPanelCount] = {};
ImVec2 g_smokeSettingsScrollMin;
ImVec2 g_smokeSettingsScrollMax;
float g_smokeSettingsScrollY = 0.0f;
bool g_smokeSettingsScrollValid = false;
ImVec2 g_smokePanelScrollMin;
ImVec2 g_smokePanelScrollMax;
float g_smokePanelScrollY = 0.0f;
bool g_smokePanelScrollValid = false;

void fillBootConfig(const LauncherState &state, MdkrBootConfig &boot) {
    boot = MdkrBootConfig{};
    boot.rom_path = state.romPath.empty() ? nullptr : state.romPath.c_str();
    // -1: let the engine resolve the mode from the ini the settings panel wrote,
    // rather than the launcher second-guessing it with a preset flag that would
    // re-expand over the individual keys the player just staged.
    boot.video_mode = -1;
    boot.override_count =
        Settings_collectStagedOverrides(boot.overrides, MDKR_BOOT_MAX_OVERRIDES);
}

void selectPanelFromEnvironment(int &activePanel) {
    const char *requested = std::getenv("MDKR_APP_PANEL");
    if (requested == nullptr) return;

    if (requested[0] >= '0' && requested[0] <= '9') {
        const int index = std::atoi(requested);
        if (index >= 0 && index < kPanelCount && panelVisible(index)) {
            activePanel = index;
        }
        return;
    }

    for (int i = 0; i < kPanelCount; ++i) {
        if (!panelVisible(i)) continue;
        if (std::strcmp(kPanels[i].label, requested) == 0) {
            activePanel = i;
            return;
        }
    }
}

void acceptDroppedRom(AppHost &host, LauncherState &state, int &activePanel) {
    const std::string dropped = host.takeDroppedFile();
    if (dropped.empty()) return;

    RomPanel_ensureInit(state);
    RomPanel_setRom(state, dropped.c_str());
    activePanel = kLauncherPanelPlay;   // show the validation verdict
}

void preparePlay(LauncherState &state) {
    RomPanel_requestPlayValidation(state);
}

void drawPrimaryLauncherAction(LauncherState &state, const ImVec2 &size) {
    const bool ready = !state.romPath.empty() && state.romInfo.valid;
    const bool busy = state.romPlayValidationPending ||
                      (!ready && state.romValidationPending);
    const char *label = "Play";
    if (busy) {
        label = "Checking ROM…";
    } else if (!ready) {
        label = "Choose ROM";
    } else if (Settings_restartPending()) {
        label = "Play with Changes";
    }

    if (busy) ImGui::BeginDisabled();
    const bool pressed = ui::BrandPrimaryButton(label, size);
    if (busy) ImGui::EndDisabled();
    // The launcher's single most important control was the one control it never
    // said out loud: the settings rows, the ROM controls and the phone-party
    // buttons all voice on focus, but the persistent Play action did not, so a
    // blind player tabbing onto it heard nothing. Route it through the same
    // choke point. A disabled (busy) button is not focusable, so this no-ops
    // during a check without a special case.
    ui::SpeakFocusedItem(
        label, nullptr,
        ready ? "Starts the game with your current ROM and settings."
              : "Opens a file picker to choose the game ROM before you can play.");
    if (!pressed) return;

    if (ready) {
        preparePlay(state);
    } else {
        // A disabled-looking dead Play button gave first-run players no useful
        // action. Open the native picker where one exists, then show the ROM
        // panel for its validation result or its typed-path/drop alternatives.
        RomPanel_chooseRom(state);
        Launcher_requestTab(state, kLauncherPanelPlay, kLauncherTabPlayer);
    }
}

/*
 * One rail destination.
 *
 * The fill is painted here rather than left to Selectable, because Selectable's
 * own fill is a square, full-bleed rectangle and that slab -- fully saturated
 * cobalt, hard corners, edge to edge -- was the loudest element in the window
 * and the one that most dated the interface. It out-shouted the gold launch
 * action, which is the thing a launcher should be pointing at.
 *
 * What replaces it is the same solid cobalt in a rounded pill with a gold
 * leading rule and the label indented clear of that rule. Keeping the FILL
 * solid is deliberate: tests/check_launcher_tabs.py finds the selected
 * destination by locating exactly one connected #315C98 component above a
 * minimum area, so a low-alpha tint would have looked calmer and left that
 * assertion unable to fail. The geometry carries the redesign; the colour
 * carries the gate.
 *
 * Hover is resolved from the row rectangle before the item is submitted, which
 * is how the rounded fill can be painted UNDER the label. Painting it after
 * would cover the text, and the alternative -- an ImDrawList channel split for
 * one rectangle -- costs more than it explains.
 */
bool drawRailPanelItem(const char *label, bool selected) {
    const float height = ui::kTouchRowHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    const bool hovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseHoveringRect(min, max);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    if (selected || hovered) {
        draw->AddRectFilled(
            min, max,
            ImGui::GetColorU32(selected ? AppTheme::navSelected()
                                        : AppTheme::navHover()),
            ui::kNavPillRounding());
    }
    if (selected) {
        draw->AddRectFilled(
            min, ImVec2(min.x + ui::kNavRuleWidth(), max.y),
            ImGui::GetColorU32(AppTheme::accent()),
            ui::kNavRuleWidth() * 0.5f);
    }

    // The fill above is the whole visual state, so the widget contributes only
    // its label and its hit box.
    const ImVec4 clear(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_Header, clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, clear);
    ImGui::PushStyleColor(
        ImGuiCol_HeaderActive,
        selected ? clear : AppTheme::navPressed());
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign,
                        ImVec2(0.0f, 0.5f));
    ImGui::Indent(ui::kGapM);
    const bool pressed = ImGui::Selectable(
        label, selected, 0,
        ImVec2((std::max)(1.0f, width - ui::kGapM), height));
    ImGui::Unindent(ui::kGapM);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    // Now that the rail shares the shell's focus scope (see panelChildFlags),
    // Tab lands on these destinations, so they announce like every other
    // control rather than becoming reachable-but-silent.
    ui::SpeakFocusedItem(label, selected ? "current section" : nullptr,
                         "Switches the launcher to this section.");
    return pressed;
}

bool drawTopPanelTab(const char *label, bool selected) {
    // Buttons already carry the style's FrameRounding, so the compact strip
    // needs only the shared colours -- the same three the rail reads, so the
    // two navigation surfaces cannot drift apart.
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        selected ? AppTheme::navSelected() : AppTheme::surface());
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        selected ? AppTheme::navSelected() : AppTheme::navHover());
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        selected ? AppTheme::navSelectedActive() : AppTheme::navPressed());
    const bool pressed = ImGui::Button(
        label, ImVec2(0.0f, ui::kTouchRowHeight()));
    if (selected) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float inset = 6.0f * AppTheme::uiScale();
        const float thickness = 3.0f * AppTheme::uiScale();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(min.x + inset, max.y - thickness),
            ImVec2(max.x - inset, max.y),
            ImGui::GetColorU32(AppTheme::accent()), thickness * 0.5f);
    }
    ImGui::PopStyleColor(3);
    ui::SpeakFocusedItem(label, selected ? "current section" : nullptr,
                         "Switches the launcher to this section.");
    return pressed;
}

void drawTopPanelTabs(int activePanel, LauncherState &state) {
    // Render from one immutable selection snapshot. A click updates the model
    // for the next frame, so one frame can never paint both the previous and
    // newly clicked destinations as active.
    const int selectedPanel = activePanel;
    int requestedPanel = selectedPanel;
    ImGui::PushID("top-panel-tabs");
    ImGui::PushStyleVar(
        ImGuiStyleVar_ItemSpacing,
        ImVec2(8.0f * AppTheme::uiScale(), ImGui::GetStyle().ItemSpacing.y));
    bool firstTab = true;
    for (int i = 0; i < kPanelCount; ++i) {
        if (!panelVisible(i)) {
            g_smokeTopTabValid[i] = false;
            continue;
        }
        if (!firstTab) ImGui::SameLine();
        firstTab = false;
        ImGui::PushID(i);
        if (drawTopPanelTab(kPanels[i].label, selectedPanel == i)) {
            requestedPanel = i;
        }
        g_smokeTopTabMin[i] = ImGui::GetItemRectMin();
        g_smokeTopTabMax[i] = ImGui::GetItemRectMax();
        g_smokeTopTabValid[i] = true;
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    if (requestedPanel != selectedPanel) {
        Launcher_requestTab(state, requestedPanel, kLauncherTabPlayer);
    }
    ImGui::Separator();
}

/*
 * Height a just-drawn child region actually needed, in its own coordinates.
 *
 * ImGui advances the layout cursor at full size even where a child clips, so
 * the last item's rectangle measures the requirement whether or not the region
 * was given enough room. `regionTop` is the child's window origin and
 * `contentTop` its first cursor position, which together give the top padding
 * (children without a border have none); the bottom padding mirrors it.
 *
 * Call this immediately after the region's last item and before EndChild().
 */
float measuredRegionHeight(float regionTop, float contentTop) {
    const float padding = contentTop - regionTop;
    return ImGui::GetItemRectMax().y - regionTop + padding;
}

void drawNavigation(int &activePanel, LauncherState &state,
                    LauncherAction &action) {
    /*
     * The footer reservation splits the rail, so overstating it steals rows
     * from the destination list rather than from anything the footer owns. It
     * reserved five BODY-font lines for a footer whose text is at most a
     * status line plus a three-line wrapped hint in the SMALL font, which at
     * 1.00x cut the fourth destination ("About") in half for every window
     * height in [620, 672). The seed below is the real composition; from the
     * second frame the value the footer measured for itself replaces it, so an
     * unusually long ROM build string widens the footer instead of being
     * clipped by a constant that could not know about it.
     */
    static float measuredFooterHeight = 0.0f;
    ImGui::BeginChild("##nav", ImVec2(ui::kNavWidth(), 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(AppTheme::fonts().small);
    const float footerTextHeight = ImGui::GetTextLineHeightWithSpacing() * 4.0f;
    ImGui::PopFont();
    const float footerSeed =
        footerTextHeight + ui::kGapS + ImGui::GetStyle().ItemSpacing.y * 3.0f +
        ui::kBtnPrimary().y + ui::kBtnSecondary().y;
    const float footerHeight =
        measuredFooterHeight > 0.0f ? measuredFooterHeight : footerSeed;
    const float bodyHeight =
        (std::max)(1.0f, ImGui::GetContentRegionAvail().y - footerHeight);
    ImGui::BeginChild("##nav-body", ImVec2(0, bodyHeight),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ui::BrandWordmark();
    ImGui::PushFont(AppTheme::fonts().small);
    ui::TextSubtle("v%s", AppVersion());
    ui::TextSubtle("Native source port");
    ImGui::PopFont();

    ui::Gap(ui::kGapM);
    ui::BrandRule();
    ui::Gap(ui::kGapM);

    const int selectedPanel = activePanel;
    for (int i = 0; i < kPanelCount; ++i) {
        if (!panelVisible(i)) continue;
        if (drawRailPanelItem(kPanels[i].label, selectedPanel == i)) {
            Launcher_requestTab(state, i, kLauncherTabPlayer);
        }
    }

    ImGui::EndChild();

    // This measured footer is a separate non-scrolling region. Readiness and
    // Play therefore stay put while another panel scrolls independently.
    ImGui::BeginChild("##nav-footer", ImVec2(0, 0),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    const float footerTop = ImGui::GetWindowPos().y;
    const float footerContentTop = ImGui::GetCursorScreenPos().y;

    const bool ready = !state.romPath.empty() && state.romInfo.valid;
    const bool checking = !ready && state.romValidationPending;
    const char *status = "ROM required";
    if (checking) {
        status = "Checking ROM…";
    } else if (ready && state.romInfo.integrity_verified) {
        status = "ROM verified";
    } else if (ready) {
        status = "ROM ready";
    }
    ImGui::PushFont(AppTheme::fonts().small);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ready ? AppTheme::good()
                                : checking ? AppTheme::accent()
                                           : AppTheme::subtle());
    ImGui::TextUnformatted(status);
    ImGui::PopStyleColor();
    if (ready && state.romInfo.build[0]) {
        ui::TextSubtleWrapped(
            "%s • %s", state.romInfo.build,
            state.romPersistenceWarning[0]
                ? "path not remembered"
                : state.romInfo.integrity_verified
                ? "full-image integrity checked"
                : "modified-ROM developer override active");
    } else if (checking) {
        ui::TextSubtleWrapped("Verifying the complete 12 MB image.");
    } else {
        ui::TextSubtleWrapped("Choose your own US 1.1 or EU 1.1 ROM.");
    }
    ImGui::PopFont();
    ui::Gap(ui::kGapS);

    drawPrimaryLauncherAction(
        state, ImVec2(-1, ui::kBtnPrimary().y));

    if (ImGui::Button("Quit", ui::kBtnFullWidth())) {
        action.type = LauncherActionType::Quit;
    }
    ui::SpeakFocusedItem("Quit", nullptr,
                         "Closes the launcher without starting the game.");
    measuredFooterHeight = measuredRegionHeight(footerTop, footerContentTop);
    ImGui::EndChild();
    ImGui::EndChild();
}

void drawTopNavigation(int &activePanel, LauncherState &state,
                       LauncherAction &action) {
    const float scale = AppTheme::uiScale();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const bool dense = availableWidth < 720.0f * scale;
    const float denseFirstRowHeight =
        (std::max)(ImGui::GetFrameHeight(), ui::kBtnSecondary().y);
    const float denseHeight = ImGui::GetStyle().WindowPadding.y * 2.0f +
                              denseFirstRowHeight +
                              ImGui::GetStyle().ItemSpacing.y +
                              ui::kBtnPrimary().y;
    /*
     * The non-dense header owns the wordmark, brand rule, 44 px tabs, a
     * separator, status, and the 48 px primary action. Deriving that from live
     * metrics is the same discipline the dense branch above already uses. The
     * constant it replaces (196 * scale) sat below both the content, which ends
     * at 201 at 1.00x, and the 217 that content plus the bordered child's own
     * padding needs: the gold primary action lost its lower edge and its bottom
     * corner radius at exactly the 800x600 layout check_launcher_tabs asserts
     * against. As in the nav rail, the header's own measurement takes over from
     * the second frame, so a font or style change moves the height with it
     * instead of re-opening the same clipping bug.
     */
    static float measuredWideHeight = 0.0f;
    ImGui::PushFont(AppTheme::fonts().title);
    const float wordmarkHeight = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    // Five rows and the four gaps between them. The wordmark shares its row
    // with Quit, so that row is the taller of the two exactly as the dense
    // branch's denseFirstRowHeight is; a horizontal Separator adds
    // style.SeparatorSize plus the ordinary spacing on each side.
    const float wideFirstRowHeight =
        (std::max)(wordmarkHeight, ui::kBtnSecondary().y);
    const float wideSeed =
        ImGui::GetStyle().WindowPadding.y * 2.0f + wideFirstRowHeight +
        ui::kBrandRuleHeight() + ui::kTouchRowHeight() +
        (std::max)(ImGui::GetStyle().SeparatorSize, 1.0f) +
        ui::kBtnPrimary().y + ImGui::GetStyle().ItemSpacing.y * 4.0f;
    const float wideHeight =
        measuredWideHeight > 0.0f ? measuredWideHeight : wideSeed;
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##topnav",
                      ImVec2(0, dense ? denseHeight : wideHeight),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    const float navTop = ImGui::GetWindowPos().y;
    const float navContentTop = ImGui::GetCursorScreenPos().y;
    ImVec2 sectionMin, sectionMax, quitMin, quitMax;
    ImVec2 statusMin, statusMax, playMin, playMax;
    const float quitWidth = dense ? 96.0f * scale : 92.0f * scale;
    if (dense) {
        ImGui::SetNextItemWidth(
            (std::max)(1.0f, ImGui::GetContentRegionAvail().x - quitWidth -
                                 ImGui::GetStyle().ItemSpacing.x));
        // drawActivePanel range-checks the same index before dispatching; this
        // preview label is the only other place it is dereferenced, so it
        // carries the identical guard rather than trusting the caller.
        const char *activeLabel =
            (activePanel >= 0 && activePanel < kPanelCount)
                ? kPanels[activePanel].label : "";
        if (ImGui::BeginCombo("##compact-section", activeLabel)) {
            for (int i = 0; i < kPanelCount; ++i) {
                if (!panelVisible(i)) continue;
                const bool selected = activePanel == i;
                if (ImGui::Selectable(
                        kPanels[i].label, selected, 0,
                        ImVec2(0.0f, ui::kTouchRowHeight()))) {
                    Launcher_requestTab(state, i, kLauncherTabPlayer);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        sectionMin = ImGui::GetItemRectMin();
        sectionMax = ImGui::GetItemRectMax();
        ui::SpeakFocusedItem("Section", activeLabel,
                             "Choose which launcher section to view.");
        ImGui::SameLine();
        if (ImGui::Button(
                "Quit", ImVec2(quitWidth, ui::kBtnSecondary().y))) {
            action.type = LauncherActionType::Quit;
        }
        quitMin = ImGui::GetItemRectMin();
        quitMax = ImGui::GetItemRectMax();
        ui::SpeakFocusedItem("Quit", nullptr,
                             "Closes the launcher without starting the game.");
    } else {
        ui::BrandWordmark();
        ImGui::SameLine();
        ImGui::PushFont(AppTheme::fonts().small);
        ui::TextSubtle("v%s", AppVersion());
        ImGui::PopFont();

        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - quitWidth);
        if (ImGui::Button(
                "Quit", ImVec2(quitWidth, ui::kBtnSecondary().y))) {
            action.type = LauncherActionType::Quit;
        }
        ui::SpeakFocusedItem("Quit", nullptr,
                             "Closes the launcher without starting the game.");

        ui::BrandRule();
        drawTopPanelTabs(activePanel, state);
    }

    // At compact widths the side rail is gone, but readiness and the primary
    // action remain available on every section.
    const bool ready = !state.romPath.empty() && state.romInfo.valid;
    const bool checking = !ready && state.romValidationPending;
    const char *status = "ROM required";
    if (checking) {
        status = "Checking ROM…";
    } else if (ready && dense) {
        status = "ROM ready";
    } else if (ready && state.romPersistenceWarning[0]) {
        status = "ROM ready • path not remembered";
    } else if (ready && state.romInfo.integrity_verified) {
        status = "ROM verified";
    } else if (ready) {
        status = "ROM ready";
    }
    ImGui::PushFont(AppTheme::fonts().small);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ready ? AppTheme::good()
                                : checking ? AppTheme::accent()
                                           : AppTheme::subtle());
    ImGui::TextUnformatted(status);
    statusMin = ImGui::GetItemRectMin();
    statusMax = ImGui::GetItemRectMax();
    ImGui::PopStyleColor();
    ImGui::PopFont();

    const float playWidth = dense
        ? (std::min)(190.0f * scale, ImGui::GetContentRegionAvail().x * 0.62f)
        : 190.0f * scale;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - playWidth);
    drawPrimaryLauncherAction(
        state, ImVec2(playWidth, ui::kBtnPrimary().y));
    playMin = ImGui::GetItemRectMin();
    playMax = ImGui::GetItemRectMax();
    // The primary action is the header's last and lowest item in both branches,
    // so this is the whole region's requirement.
    if (!dense) {
        measuredWideHeight = measuredRegionHeight(navTop, navContentTop);
    }
    const bool traceLayout = std::getenv("MDKR_APP_UI_TRACE") != nullptr;
    if (traceLayout) {
        static bool tracedHeaderHeight = false;
        if (!tracedHeaderHeight) {
            std::fprintf(stderr,
                         "[app-ui] header dense=%d height=%.1f seed=%.1f "
                         "measured=%.1f playBottom=%.1f navBottom=%.1f\n",
                         dense ? 1 : 0, (double)(dense ? denseHeight : wideHeight),
                         (double)wideSeed, (double)measuredWideHeight,
                         (double)(playMax.y - navTop),
                         (double)ImGui::GetWindowSize().y);
            tracedHeaderHeight = true;
        }
    }
    const bool traceDenseLayout = dense && traceLayout;
    bool denseOverlap = false;
    bool denseControlsContained = false;
    if (traceDenseLayout) {
        const auto overlaps = [](const ImVec2 &aMin, const ImVec2 &aMax,
                                 const ImVec2 &bMin, const ImVec2 &bMax) {
            return aMin.x < bMax.x && aMax.x > bMin.x &&
                   aMin.y < bMax.y && aMax.y > bMin.y;
        };
        const ImVec2 windowMin = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 windowMax(windowMin.x + windowSize.x,
                               windowMin.y + windowSize.y);
        const ImVec2 mins[] = {sectionMin, quitMin, statusMin, playMin};
        const ImVec2 maxs[] = {sectionMax, quitMax, statusMax, playMax};
        denseControlsContained = true;
        for (int first = 0; first < 4; ++first) {
            denseControlsContained = denseControlsContained &&
                mins[first].x >= windowMin.x &&
                mins[first].y >= windowMin.y &&
                maxs[first].x <= windowMax.x &&
                maxs[first].y <= windowMax.y;
            for (int second = first + 1; second < 4; ++second) {
                denseOverlap = denseOverlap ||
                    overlaps(mins[first], maxs[first],
                             mins[second], maxs[second]);
            }
        }
    }
    ImGui::EndChild();
    if (traceDenseLayout) {
        static bool tracedDenseLayout = false;
        if (!tracedDenseLayout) {
            const float contentStartY = ImGui::GetCursorScreenPos().y;
            const float navBottomY = ImGui::GetItemRectMax().y;
            const bool contentSeparated = contentStartY >= navBottomY;
            std::fprintf(stderr,
                         "[app-ui] compact-layout dense=1 contained=%d "
                         "overlap=%d contentSeparated=%d\n",
                         denseControlsContained ? 1 : 0,
                         denseOverlap ? 1 : 0,
                         contentSeparated ? 1 : 0);
            tracedDenseLayout = true;
        }
    }
}

// Nav can cross into a child window only when the child shares the parent's
// focus scope. The launcher is meant to be ONE keyboard/gamepad surface -- Tab
// off the nav rail, through the panel, onto the persistent Play and Quit
// actions, with no scope you cannot leave -- so every launcher child is
// flattened into the root window's scope. This used to be armed only for the
// scripted accessibility walk, which meant the walk could Tab everywhere but an
// ordinary keyboard player could not reach Play at all: the shell and its panel
// were separate scopes and Tab stopped at the boundary. Flattening always is
// what makes "you can reach Play with the keyboard" true off the test bench.
ImGuiChildFlags panelChildFlags(ImGuiChildFlags flags) {
    return flags | ImGuiChildFlags_NavFlattened;
}

// Last panel announced. Guarding on the CHANGE, not on ui::SpeakSection's own
// repeat filter, is what keeps this quiet: the panel draws every frame while
// the settings headers announce themselves as the keyboard passes over them,
// so an unguarded call here and a header call there would take turns being
// "new" and the shell would talk without pause.
int g_spokenPanel = -1;

void drawActivePanel(int activePanel, LauncherState &state, LauncherAction &action) {
    // Which panel you are in, said once per arrival. The launcher's counterpart
    // to a settings section header; both go through the one announcement point
    // in ui_common.cpp.
    if (activePanel != g_spokenPanel && activePanel >= 0 &&
        activePanel < kPanelCount) {
        g_spokenPanel = activePanel;
        ui::SpeakSection(kPanels[activePanel].label);
    }
    ImGui::BeginChild("##content", ImVec2(0, 0), panelChildFlags(0));
    if (state.bootErrorVisible) {
        if (ui::CardBegin("##boot-recovery", AppTheme::bad(), 0.0f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ImGui::PushFont(AppTheme::fonts().title);
            ImGui::TextUnformatted("The Game Did Not Start");
            ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", state.bootError);
            ui::Gap(ui::kGapS);
            const AppUiButtonPairLayout actions = AppUi_fitButtonPair(
                ImGui::GetContentRegionAvail().x,
                ImGui::GetStyle().ItemSpacing.x,
                ui::kBtnSecondary().x, ui::kBtnSecondary().x,
                ui::kPairMinWidth());
            const ImVec2 first(actions.firstWidth, ui::kBtnSecondary().y);
            const ImVec2 second(actions.secondWidth, ui::kBtnSecondary().y);
            if (ImGui::Button("View Diagnostics", first)) {
                Launcher_requestTab(state, kLauncherPanelDiagnostics,
                                    kLauncherTabPlayer);
            }
            if (actions.sameLine) ImGui::SameLine();
            if (ImGui::Button("Dismiss", second)) {
                state.bootErrorVisible = false;
            }
        }
        ui::CardEnd();
        ui::Gap(ui::kGapM);
    }
    if (activePanel >= 0 && activePanel < kPanelCount) {
        kPanels[activePanel].draw(state, action);
    }
    ui::TouchScrollCurrentWindow();
    g_smokePanelScrollMin = ImGui::GetWindowPos();
    const ImVec2 panelScrollSize = ImGui::GetWindowSize();
    g_smokePanelScrollMax = ImVec2(
        g_smokePanelScrollMin.x + panelScrollSize.x,
        g_smokePanelScrollMin.y + panelScrollSize.y);
    g_smokePanelScrollY = ImGui::GetScrollY();
    g_smokePanelScrollValid = ImGui::GetScrollMaxY() > 0.0f;
    ImGui::EndChild();
}

}  // namespace

Launcher::Launcher()
    : partyTransport_(mdkr_create_native_party_transport()),
      phoneParty_(std::make_unique<MdkrNativePartyHost>(*partyTransport_)) {
    state_.phoneParty = phoneParty_.get();
}

Launcher::~Launcher() = default;

void Launcher_requestTab(LauncherState &s, int panel, int priority) {
    if (panel < 0 || panel >= kPanelCount) return;
    // Equal priority keeps last-writer-wins, which is what makes a second click
    // in the same frame supersede the first. A lower priority may not displace
    // a request already staged: that is the whole point of the ordering.
    if (s.requestTab >= 0 && priority < s.requestTabPriority) return;
    s.requestTab = panel;
    s.requestTabPriority = priority;
}

bool Launcher_smokeTopTabCenter(int panel, int *x, int *y) {
    if (!x || !y || panel < 0 || panel >= kPanelCount ||
        !g_smokeTopTabValid[panel]) {
        return false;
    }
    *x = static_cast<int>(
        (g_smokeTopTabMin[panel].x + g_smokeTopTabMax[panel].x) * 0.5f);
    *y = static_cast<int>(
        (g_smokeTopTabMin[panel].y + g_smokeTopTabMax[panel].y) * 0.5f);
    return true;
}

bool Launcher_smokeSettingsScrollRect(int *minX, int *minY,
                                      int *maxX, int *maxY) {
    if (!minX || !minY || !maxX || !maxY ||
        !g_smokeSettingsScrollValid) {
        return false;
    }
    *minX = static_cast<int>(g_smokeSettingsScrollMin.x);
    *minY = static_cast<int>(g_smokeSettingsScrollMin.y);
    *maxX = static_cast<int>(g_smokeSettingsScrollMax.x);
    *maxY = static_cast<int>(g_smokeSettingsScrollMax.y);
    return true;
}

float Launcher_smokeSettingsScrollY() {
    return g_smokeSettingsScrollY;
}

bool Launcher_smokePanelScrollRect(int *minX, int *minY,
                                   int *maxX, int *maxY) {
    if (!minX || !minY || !maxX || !maxY) return false;
    const bool settings = g_spokenPanel == kLauncherPanelSettings;
    const bool valid = settings ? g_smokeSettingsScrollValid
                                : g_smokePanelScrollValid;
    if (!valid) return false;
    const ImVec2 &minimum = settings ? g_smokeSettingsScrollMin
                                    : g_smokePanelScrollMin;
    const ImVec2 &maximum = settings ? g_smokeSettingsScrollMax
                                    : g_smokePanelScrollMax;
    *minX = static_cast<int>(minimum.x);
    *minY = static_cast<int>(minimum.y);
    *maxX = static_cast<int>(maximum.x);
    *maxY = static_cast<int>(maximum.y);
    return true;
}

float Launcher_smokePanelScrollY() {
    return g_spokenPanel == kLauncherPanelSettings
        ? g_smokeSettingsScrollY : g_smokePanelScrollY;
}

namespace {

void drawSettingsPanel(LauncherState &s, LauncherAction &out) {
    (void)out;
    // One page, one scroll owner. Keeping the introduction outside this child
    // left no reachable settings viewport at the supported 640x480 / 2.00x
    // extreme and wasted scarce height on 7-inch handhelds.
    // The scrollbar is RESERVED, not on-demand. The settings page's height
    // crosses the viewport boundary as rows appear (an error line and its
    // Retry button, say), and an on-demand scrollbar then oscillates: it
    // appears, the narrower content re-wraps one line shorter, it vanishes,
    // the line un-wraps, and the page breathes by a text line every few
    // frames -- moving every widget under a pointer or a scripted click.
    ImGui::BeginChild("##settingsscroll", ImVec2(0, 0), panelChildFlags(0),
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ui::SectionHeader("Settings",
                      "Everything saves as you change it. Anything marked "
                      "“Next launch” waits for Play.");

    const bool restartPending = Settings_restartPending();
    if (restartPending) {
        ui::CardBegin("##settingsready", AppTheme::accent(), 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextUnformatted("Ready for next launch");
        ImGui::PopStyleColor();
        ui::TextSubtle(
            "Press Play with Changes to start with these settings.");
        ui::CardEnd();
        ui::Gap(ui::kGapM);
    }
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        static bool tracedSettingsAction = false;
        if (!tracedSettingsAction) {
            std::fprintf(stderr, "[app-ui] settings action=%s restartPending=%d\n",
                         restartPending ? "play-with-changes" : "play",
                         restartPending ? 1 : 0);
            tracedSettingsAction = true;
        }
    }
    Settings_draw(s.hostWindow, /*compact=*/false);
    ui::TouchScrollCurrentWindow();
    g_smokeSettingsScrollMin = ImGui::GetWindowPos();
    const ImVec2 scrollSize = ImGui::GetWindowSize();
    g_smokeSettingsScrollMax = ImVec2(
        g_smokeSettingsScrollMin.x + scrollSize.x,
        g_smokeSettingsScrollMin.y + scrollSize.y);
    g_smokeSettingsScrollY = ImGui::GetScrollY();
    g_smokeSettingsScrollValid = ImGui::GetScrollMaxY() > 0.0f;
    ImGui::EndChild();
}

void drawAboutPanel(LauncherState &s, LauncherAction &out) {
    (void)s;
    (void)out;

    ui::SectionHeader("About",
                      "An unofficial fan project: a decompilation-based native "
                      "source port, for research, preservation and education.");

    ImGui::TextUnformatted(AppBrandVersionLine());

    ui::Gap(ui::kGapM);
    ImGui::PushTextWrapPos(0.0f);
    ui::TextSubtle(
        MDKR_BRAND_NAME " contains no game data. It reads assets from a copy of "
        "the original game that you supply and legally own. The original game "
        "and all related trademarks are the property of their respective rights "
        "holders; this project is not affiliated with, endorsed by, or sponsored "
        "by any of them.");
    ImGui::PopTextWrapPos();

    ui::Gap(ui::kGapM);
    ui::TextSubtleWrapped(
        "F1 opens in-game settings. F10 toggles the FPS readout. F11 or "
        "Alt+Enter toggles fullscreen.");
}

}  // namespace

LauncherAction Launcher::draw(AppHost &host) {
    state_.hostWindow = host.window();
    phoneParty_->service(static_cast<uint64_t>(SDL_GetTicks64()));
    LauncherAction action;
    const int panelAtFrameStart = active_;
    for (int i = 0; i < kPanelCount; ++i) g_smokeTopTabValid[i] = false;
    // Same one-frame lifetime as the tab rectangles above: the settings scroll
    // viewport is only real while the Settings panel is drawing. Leaving the
    // last value latched let Launcher_smokeSettingsScrollRect hand a touch or
    // capture gate a rectangle belonging to a panel that is no longer on screen.
    g_smokeSettingsScrollValid = false;
    g_smokePanelScrollValid = false;

    // Design-review / CI hook: MDKR_APP_PANEL=<index|name> opens a specific panel
    // on the first frame so a screenshot gate can capture it without input.
    if (!panelEnvChecked_) {
        panelEnvChecked_ = true;
        selectPanelFromEnvironment(active_);
    }

    // A file dropped on the window always means "use this ROM", whichever panel
    // is showing; switch to the ROM panel so the verdict is visible.
    acceptDroppedRom(host, state_, active_);
    // Navigation carries the global readiness/action state, so initialize the
    // remembered ROM even when a design-review hook opens another panel first.
    RomPanel_ensureInit(state_);
    RomPanel_serviceValidation(state_);
    if (state_.romPlayValidationPassed) {
        state_.romPlayValidationPassed = false;
        action.type = LauncherActionType::Play;
        fillBootConfig(state_, action.boot);
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    // This root is a fixed viewport shell; its panel children own scrolling.
    // Selecting an off-axis compact tab must not let ImGui auto-scroll the
    // whole launcher and move the brand, Quit, or persistent Play row away.
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);

    const bool compactNavigation =
        vp->Size.x < 860.0f * AppTheme::uiScale() ||
        vp->Size.y < 620.0f * AppTheme::uiScale();
    if (compactNavigation) {
        drawTopNavigation(active_, state_, action);
    } else {
        drawNavigation(active_, state_, action);
        ImGui::SameLine();
    }
    drawActivePanel(active_, state_, action);
    ImGui::End();

    // Consume deferred navigation only after this frame has rendered from one
    // immutable selection snapshot.
    if (state_.requestTab >= 0 && state_.requestTab < kPanelCount) {
        active_ = state_.requestTab;
    }
    state_.requestTab = -1;
    state_.requestTabPriority = 0;

    if (panelAtFrameStart == kLauncherPanelSettings &&
        active_ != kLauncherPanelSettings) {
        Settings_cancelAudioPreview();
    }

    return action;
}

void Launcher::setBootError(const char *message) {
    if (message == nullptr || message[0] == '\0') return;
    std::snprintf(state_.bootError, sizeof(state_.bootError), "%s", message);
    state_.bootErrorVisible = true;
    active_ = 0;
    std::fprintf(stderr, "[app] boot recovery visible: %s\n", state_.bootError);
}

void Launcher::requestPlayValidationForSmoke() {
    RomPanel_requestPlayValidation(state_);
}
