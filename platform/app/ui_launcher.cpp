// ui_launcher.cpp — nav rail, panel router, Play button, About.
#include "ui_launcher.h"
#include "app_host.h"
#include "app_theme.h"
#include "app_brand.h"
#include "ui_common.h"
#include "ui_settings.h"

#include "imgui.h"

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

void fillBootConfig(const LauncherState &state, MdkrBootConfig &boot) {
    boot = MdkrBootConfig{};
    boot.rom_path = state.romPath[0] ? state.romPath : nullptr;
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

void drawNavigation(int &activePanel, LauncherAction &action) {
    ImGui::BeginChild("##nav", ImVec2(ui::kNavWidth(), 0), true);
    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
    ImGui::TextUnformatted(MDKR_BRAND_NAME);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(AppTheme::fonts().small);
    ui::TextSubtle("v%s", AppVersion());
    ImGui::PopFont();

    ui::Gap(ui::kGapM);
    ImGui::Separator();
    ui::Gap(ui::kGapM);

    for (int i = 0; i < kPanelCount; ++i) {
        if (ImGui::Selectable(kPanels[i].label, activePanel == i, 0,
                              ImVec2(0, 30 * AppTheme::uiScale()))) {
            activePanel = i;
        }
    }

    // Keep Quit pinned to the bottom of the rail.
    const float quitHeight = ui::kBtnSecondary().y + ui::kGapM;
    const float remaining = ImGui::GetContentRegionAvail().y - quitHeight;
    if (remaining > 0) ui::Gap(remaining);
    if (ImGui::Button("Quit", ImVec2(-1, ui::kBtnSecondary().y))) {
        action.type = LauncherActionType::Quit;
    }
    ImGui::EndChild();
}

void drawTopNavigation(int &activePanel, LauncherAction &action) {
    const float scale = AppTheme::uiScale();
    ImGui::BeginChild("##topnav", ImVec2(0, 96.0f * scale), true);
    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
    ImGui::TextUnformatted(MDKR_BRAND_NAME);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushFont(AppTheme::fonts().small);
    ui::TextSubtle("v%s", AppVersion());
    ImGui::PopFont();

    const float quitWidth = 92.0f * scale;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - quitWidth);
    if (ImGui::Button("Quit", ImVec2(quitWidth, 30.0f * scale))) {
        action.type = LauncherActionType::Quit;
    }

    if (ImGui::BeginTabBar("##panels", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < kPanelCount; ++i) {
            const ImGuiTabItemFlags flags = activePanel == i
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(kPanels[i].label, nullptr, flags)) {
                activePanel = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void drawActivePanel(int activePanel, LauncherState &state, LauncherAction &action) {
    ImGui::BeginChild("##content", ImVec2(0, 0), false);
    if (activePanel >= 0 && activePanel < kPanelCount) {
        kPanels[activePanel].draw(state, action);
    }
    ImGui::EndChild();
}

}  // namespace

void PlayButton_draw(LauncherState &s, LauncherAction &out) {
    const bool ready = s.romPath[0] != '\0' && s.romInfo.valid;
    const char *label = Settings_restartPending() ? "Play with changes" : "Play";

    if (!ready) ImGui::BeginDisabled();
    if (ui::PrimaryButton(label, ui::kBtnPrimary())) {
        out.type = LauncherActionType::Play;
        fillBootConfig(s, out.boot);
    }
    if (!ready) ImGui::EndDisabled();

    if (!ready) {
        if (ImGui::GetContentRegionAvail().x > 220.0f * AppTheme::uiScale()) {
            ImGui::SameLine();
        }
        ui::TextSubtleWrapped(s.romPath[0]
                                  ? "That ROM is not one this build supports."
                                  : "Choose a ROM first.");
        // Let the hint act: clicking it takes the player where they need to be.
        if (ImGui::IsItemClicked()) s.requestTab = 0;
    }
}

namespace {

void drawSettingsPanel(LauncherState &s, LauncherAction &out) {
    ui::SectionHeader("Settings",
                      "Changes save automatically. Items marked \"restart required\" "
                      "apply when you next press Play.");

    const bool restartPending = Settings_restartPending();
    if (restartPending) {
        ui::CardBegin("##settingsready", AppTheme::accent(), 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextUnformatted("Changes ready");
        ImGui::PopStyleColor();
        ui::TextSubtle("Saved. Play starts the game with these settings.");
        PlayButton_draw(s, out);
        ui::CardEnd();
    } else {
        PlayButton_draw(s, out);
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
    ui::Gap(ui::kGapM);
    ImGui::BeginChild("##settingsscroll", ImVec2(0, 0), false);
    Settings_draw(/*compact=*/false);
    ImGui::EndChild();
}

void drawAboutPanel(LauncherState &s, LauncherAction &out) {
    (void)s;
    (void)out;

    ui::SectionHeader("About " MDKR_BRAND_NAME,
                      "An unofficial fan project: a decompilation-based native "
                      "source port, for research, preservation and education.");

    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
    ImGui::TextUnformatted(MDKR_BRAND_NAME);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ui::TextSubtle("%s", AppBrandVersionLine());

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
    ui::TextSubtle("Press F1 in-game for settings; F10 toggles the FPS readout.");
}

}  // namespace

LauncherAction Launcher::draw(AppHost &host) {
    LauncherAction action;

    // Design-review / CI hook: MDKR_APP_PANEL=<index|name> opens a specific panel
    // on the first frame so a screenshot gate can capture it without input.
    if (!panelEnvChecked_) {
        panelEnvChecked_ = true;
        selectPanelFromEnvironment(active_);
    }

    // A file dropped on the window always means "use this ROM", whichever panel
    // is showing; switch to the ROM panel so the verdict is visible.
    acceptDroppedRom(host, state_, active_);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    const bool compactNavigation =
        vp->Size.x < 860.0f * AppTheme::uiScale();
    if (compactNavigation) {
        drawTopNavigation(active_, action);
    } else {
        drawNavigation(active_, action);
        ImGui::SameLine();
    }
    drawActivePanel(active_, state_, action);
    ImGui::End();

    // A panel asked to change tabs (e.g. the disabled-Play hint).
    if (state_.requestTab >= 0 && state_.requestTab < kPanelCount) {
        active_ = state_.requestTab;
    }
    state_.requestTab = -1;

    return action;
}
