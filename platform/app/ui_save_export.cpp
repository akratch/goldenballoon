#include "ui_save_export.h"
#include "save_export.h"
#include "file_dialog.h"
#include "ui_common.h"
#include "../user_paths.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

void SaveExport_draw() {
    static char destination[4096] = {};
    static std::string note, exported;
    ui::GroupHeader("Saves", "Export your progress and records for backup or transfer.");
    ui::TextSubtleWrapped("Includes all three adventure slots, unlocks and time records. Ghosts and launcher settings are stored separately.");
    char directory[4096] = {};
    const bool located = mdkr_user_save_directory(directory, sizeof(directory)) != 0;
    std::error_code error;
    const bool haveSave = located && std::filesystem::is_regular_file(
        std::filesystem::u8path(directory) / "eeprom.bin", error);
    const bool native = filedialog::isAvailable();
    if (!native) {
        if (!destination[0]) {
            const char *home = std::getenv("HOME");
            const auto path = std::filesystem::u8path(home && home[0] ? home : directory) /
                              SaveExport::suggestedFilename();
            std::snprintf(destination, sizeof(destination), "%s", path.u8string().c_str());
        }
        ui::TextSubtle("Destination");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##save-export-destination", destination, sizeof(destination));
        ui::SpeakFocusedItem("Save export destination", nullptr, "Choose the path for your portable save file.");
    }
    ImGui::BeginDisabled(!haveSave);
    if (ImGui::Button("Export save", ui::kBtnWide())) {
        std::string path = destination;
        if (!native || filedialog::saveGameProgress(path, SaveExport::suggestedFilename())) {
            if (SaveExport::progress(directory, path, MDKR_APP_VERSION_STR, note)) {
                exported = path;
                note = "Save exported: " + path;
            } else {
                exported.clear();
            }
        }
    }
    ui::SpeakFocusedItem("Export save", nullptr, "Save a portable copy of your progress and records as a .mdkr-save file.");
    ImGui::EndDisabled();
    if (!haveSave) ui::TextSubtleWrapped("No saved progress yet. Play and save first, then return here to export.");
    if (!note.empty()) ui::TextSubtleUnformattedWrapped(note.c_str());
    if (!exported.empty() && native && ImGui::Button("Show exported save", ui::kBtnWide())) {
        if (!filedialog::revealInFileManager(exported)) note = "Save exported to " + exported + ". Could not open the file manager.";
    }
    if (!exported.empty() && native) ui::SpeakFocusedItem("Show exported save", nullptr, "Reveal the exported file in your file manager.");
    ui::Gap(ui::kGapM);
}
