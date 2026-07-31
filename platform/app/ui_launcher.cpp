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

const Panel kPanels[] = {
    {"Game ROM",    RomPanel_draw},
    {"Settings",    SettingsPanel_draw},
    {"Diagnostics", DiagPanel_draw},
    {"About",       AboutPanel_draw},
};
const int kPanelCount = (int)(sizeof(kPanels) / sizeof(kPanels[0]));

}  // namespace

void fillBoot(const LauncherState &s, MdkrBootConfig &boot) {
    boot = MdkrBootConfig{};
    boot.rom_path = s.romPath[0] ? s.romPath : nullptr;
    // -1: let the engine resolve the mode from the ini the settings panel wrote,
    // rather than the launcher second-guessing it with a preset flag that would
    // re-expand over the individual keys the player just staged.
    boot.video_mode = -1;
    boot.override_count =
        Settings_collectStagedOverrides(boot.overrides, MDKR_BOOT_MAX_OVERRIDES);
}

void PlayButton_draw(LauncherState &s, LauncherAction &out) {
    const bool ready = s.romPath[0] != '\0' && s.romInfo.valid;

    if (!ready) ImGui::BeginDisabled();
    if (ui::PrimaryButton("Play", ui::kBtnPrimary())) {
        out.type = LauncherActionType::Play;
        fillBoot(s, out.boot);
    }
    if (!ready) ImGui::EndDisabled();

    if (!ready) {
        ImGui::SameLine();
        ui::TextSubtle(s.romPath[0] ? "That ROM is not one this build supports."
                                    : "Choose a ROM first.");
        // Let the hint act: clicking it takes the player where they need to be.
        if (ImGui::IsItemClicked()) s.requestTab = 0;
    }
}

void SettingsPanel_draw(LauncherState &s, LauncherAction &out) {
    (void)out;
    ui::SectionHeader("Settings",
                      "Everything " MDKR_BRAND_NAME " can change about how the game looks and runs. "
                      "Items marked \"restart\" apply the next time the game starts.");
    ImGui::BeginChild("##settingsscroll", ImVec2(0, 0), false);
    Settings_draw(/*compact=*/false);
    ImGui::EndChild();
    (void)s;
}

void AboutPanel_draw(LauncherState &s, LauncherAction &out) {
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

LauncherAction Launcher::draw(AppHost &host) {
    LauncherAction action;

    // Design-review / CI hook: MDKR_APP_PANEL=<index|name> opens a specific panel
    // on the first frame so a screenshot gate can capture it without input.
    if (!panelEnvChecked_) {
        panelEnvChecked_ = true;
        if (const char *p = std::getenv("MDKR_APP_PANEL")) {
            if (p[0] >= '0' && p[0] <= '9') {
                int idx = std::atoi(p);
                if (idx >= 0 && idx < kPanelCount) active_ = idx;
            } else {
                for (int i = 0; i < kPanelCount; ++i) {
                    if (std::strcmp(kPanels[i].label, p) == 0) { active_ = i; break; }
                }
            }
        }
    }

    // A file dropped on the window always means "use this ROM", whichever panel
    // is showing; switch to the ROM panel so the verdict is visible.
    std::string dropped = host.takeDroppedFile();
    if (!dropped.empty()) {
        RomPanel_ensureInit(state_);
        RomPanel_setRom(state_, dropped.c_str());
        active_ = 0;
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

    // --- Nav rail -----------------------------------------------------------
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
        if (ImGui::Selectable(kPanels[i].label, active_ == i, 0,
                              ImVec2(0, 30 * AppTheme::uiScale()))) {
            active_ = i;
        }
    }

    // Quit pinned to the bottom of the rail.
    float quitH = ui::kBtnSecondary().y + ui::kGapM;
    float remaining = ImGui::GetContentRegionAvail().y - quitH;
    if (remaining > 0) ui::Gap(remaining);
    if (ImGui::Button("Quit", ImVec2(-1, ui::kBtnSecondary().y))) {
        action.type = LauncherActionType::Quit;
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // --- Content ------------------------------------------------------------
    ImGui::BeginChild("##content", ImVec2(0, 0), false);
    if (active_ >= 0 && active_ < kPanelCount) {
        kPanels[active_].draw(state_, action);
    }
    ImGui::EndChild();

    ImGui::End();

    // A panel asked to change tabs (e.g. the disabled-Play hint).
    if (state_.requestTab >= 0 && state_.requestTab < kPanelCount) {
        active_ = state_.requestTab;
    }
    state_.requestTab = -1;

    return action;
}
