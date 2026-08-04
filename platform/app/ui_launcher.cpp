// ui_launcher.cpp — nav rail, panel router, Play button, About.
#include "ui_launcher.h"
#include "app_host.h"
#include "app_ui_policy.h"
#include "app_theme.h"
#include "app_brand.h"
#include "ui_common.h"
#include "ui_settings.h"

#include "imgui.h"

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
    {"Game ROM",    RomPanel_draw},
    {"Settings",    drawSettingsPanel},
    {"Diagnostics", DiagPanel_draw},
    {"About",       drawAboutPanel},
};
constexpr int kPanelCount = (int)(sizeof(kPanels) / sizeof(kPanels[0]));
static_assert(kPanelCount == kLauncherPanelCount,
              "launcher panel count must match the public smoke contract");
ImVec2 g_smokeTopTabMin[kPanelCount];
ImVec2 g_smokeTopTabMax[kPanelCount];
bool g_smokeTopTabValid[kPanelCount] = {};
ImVec2 g_smokeSettingsScrollMin;
ImVec2 g_smokeSettingsScrollMax;
float g_smokeSettingsScrollY = 0.0f;
bool g_smokeSettingsScrollValid = false;

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
        if (index >= 0 && index < kPanelCount) activePanel = index;
        return;
    }

    for (int i = 0; i < kPanelCount; ++i) {
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
    activePanel = 0;   // show the validation verdict
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
    if (!pressed) return;

    if (ready) {
        preparePlay(state);
    } else {
        // A disabled-looking dead Play button gave first-run players no useful
        // action. Open the native picker where one exists, then show the ROM
        // panel for its validation result or its typed-path/drop alternatives.
        RomPanel_chooseRom(state);
        state.requestTab = 0;
    }
}

ImVec4 selectedNavigationColor() {
    const ImVec4 primary = AppTheme::primary();
    return ImVec4(primary.x, primary.y, primary.z, 1.0f);
}

ImVec4 neutralNavigationColor() {
    return AppTheme::surface();
}

ImVec4 neutralNavigationHoverColor() {
    return AppTheme::hex(0x3A3A3D);
}

bool drawRailPanelItem(const char *label, bool selected) {
    ImGui::PushStyleColor(
        ImGuiCol_Header,
        selected ? selectedNavigationColor() : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(
        ImGuiCol_HeaderHovered,
        selected ? selectedNavigationColor()
                 : neutralNavigationHoverColor());
    ImGui::PushStyleColor(
        ImGuiCol_HeaderActive,
        selected ? AppTheme::hex(0x284B7C) : neutralNavigationColor());
    const bool pressed = ImGui::Selectable(
        label, selected, 0, ImVec2(0, ui::kTouchRowHeight()));
    if (selected) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            min, ImVec2(min.x + 3.0f * AppTheme::uiScale(), max.y),
            ImGui::GetColorU32(AppTheme::accent()),
            2.0f * AppTheme::uiScale());
    }
    ImGui::PopStyleColor(3);
    return pressed;
}

bool drawTopPanelTab(const char *label, bool selected) {
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        selected ? selectedNavigationColor() : neutralNavigationColor());
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        selected ? selectedNavigationColor()
                 : neutralNavigationHoverColor());
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        selected ? AppTheme::hex(0x284B7C) : AppTheme::hex(0x424247));
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
    for (int i = 0; i < kPanelCount; ++i) {
        if (i != 0) ImGui::SameLine();
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
    if (requestedPanel != selectedPanel) state.requestTab = requestedPanel;
    ImGui::Separator();
}

void drawNavigation(int &activePanel, LauncherState &state,
                    LauncherAction &action) {
    ImGui::BeginChild("##nav", ImVec2(ui::kNavWidth(), 0), true,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    const float footerHeight =
        ui::kBtnPrimary().y + ui::kBtnSecondary().y +
        ImGui::GetTextLineHeightWithSpacing() * 5.0f +
        ImGui::GetStyle().ItemSpacing.y * 3.0f;
    const float bodyHeight =
        (std::max)(1.0f, ImGui::GetContentRegionAvail().y - footerHeight);
    ImGui::BeginChild("##nav-body", ImVec2(0, bodyHeight), false,
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
        if (drawRailPanelItem(kPanels[i].label, selectedPanel == i)) {
            state.requestTab = i;
        }
    }

    ImGui::EndChild();

    // This measured footer is a separate non-scrolling region. Readiness and
    // Play therefore stay put while another panel scrolls independently.
    ImGui::BeginChild("##nav-footer", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);

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

    if (ImGui::Button("Quit", ImVec2(-1, ui::kBtnSecondary().y))) {
        action.type = LauncherActionType::Quit;
    }
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
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##topnav",
                      // The non-dense header owns the wordmark, brand rule,
                      // 44 px tabs, status, and 48 px primary action. Its fixed
                      // height keeps every rounded edge inside the child at
                      // high DPI while leaving the content panel independent.
                      ImVec2(0, dense ? denseHeight : 196.0f * scale),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 sectionMin, sectionMax, quitMin, quitMax;
    ImVec2 statusMin, statusMax, playMin, playMax;
    const float quitWidth = dense ? 96.0f * scale : 92.0f * scale;
    if (dense) {
        ImGui::SetNextItemWidth(
            (std::max)(1.0f, ImGui::GetContentRegionAvail().x - quitWidth -
                                 ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::BeginCombo("##compact-section",
                              kPanels[activePanel].label)) {
            for (int i = 0; i < kPanelCount; ++i) {
                const bool selected = activePanel == i;
                if (ImGui::Selectable(
                        kPanels[i].label, selected, 0,
                        ImVec2(0.0f, ui::kTouchRowHeight()))) {
                    state.requestTab = i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        sectionMin = ImGui::GetItemRectMin();
        sectionMax = ImGui::GetItemRectMax();
        ImGui::SameLine();
        if (ImGui::Button(
                "Quit", ImVec2(quitWidth, ui::kBtnSecondary().y))) {
            action.type = LauncherActionType::Quit;
        }
        quitMin = ImGui::GetItemRectMin();
        quitMax = ImGui::GetItemRectMax();
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
    const bool traceDenseLayout =
        dense && std::getenv("MDKR_APP_UI_TRACE") != nullptr;
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

void drawActivePanel(int activePanel, LauncherState &state, LauncherAction &action) {
    ImGui::BeginChild("##content", ImVec2(0, 0), false);
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
                140.0f * AppTheme::uiScale());
            const ImVec2 first(actions.firstWidth, ui::kBtnSecondary().y);
            const ImVec2 second(actions.secondWidth, ui::kBtnSecondary().y);
            if (ImGui::Button("View Diagnostics", first)) {
                state.requestTab = 2;
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
    ImGui::EndChild();
}

}  // namespace

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

namespace {

void drawSettingsPanel(LauncherState &s, LauncherAction &out) {
    (void)out;
    // One page, one scroll owner. Keeping the introduction outside this child
    // left no reachable settings viewport at the supported 640x480 / 2.00x
    // extreme and wasted scarce height on 7-inch handhelds.
    ImGui::BeginChild("##settingsscroll", ImVec2(0, 0), false);
    ui::SectionHeader("Settings",
                      "Changes save automatically. Items marked “restart required” "
                      "apply when you next press Play.");

    const bool restartPending = Settings_restartPending();
    if (restartPending) {
        ui::CardBegin("##settingsready", AppTheme::accent(), 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextUnformatted("Changes Ready");
        ImGui::PopStyleColor();
        ui::TextSubtle(
            "Saved. Press Play with Changes to start with these settings.");
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
    LauncherAction action;
    const int panelAtFrameStart = active_;
    for (int i = 0; i < kPanelCount; ++i) g_smokeTopTabValid[i] = false;

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

    if (panelAtFrameStart == 1 && active_ != 1) {
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
