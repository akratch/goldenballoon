// ui_rom.cpp — the launcher's "Game ROM" panel: the user hands us a file.
//
// ============================================================================
//  There is NO automatic ROM discovery here, deliberately
// ============================================================================
//
// The first version of this panel scanned the working directory, $HOME,
// Downloads, Documents and Desktop for *.z64 and auto-selected the first match.
// Three things were wrong with that, all reported from live macOS validation:
//
//   1. It picked the WRONG ROM. "First match in scan order" is not "the one the
//      player wants" when several dumps are on disk, and the selection happened
//      before the player had seen a single frame of UI.
//   2. The in-app folder browser it came with could not reach anything. It
//      started at the working directory ("." from defaultScanDirs) and
//      parentOf(".") returns empty, so no ".." row was ever emitted and there
//      was no way to navigate out. From the .app bundle the working directory
//      is Contents/Resources, so the browser opened on an empty list with no
//      exit — the root cause of "the file browser doesn't work".
//   3. Worst of all: opendir() on Downloads/Documents/Desktop trips macOS TCC
//      and raises a system permission prompt before the player has asked for
//      anything. An unsigned fan-made emulator asking for your Documents folder
//      on first launch is alarming, and rightly so.
//
// So discovery is gone, root and branch — rom_scan.{h,cpp} is deleted, not
// disabled. This panel never enumerates a directory, which is what makes "no
// permission prompt can ever fire" a property of the code rather than a
// promise. Three manual, permission-free paths remain:
//
//   a) drag-and-drop onto the window (SDL_DROPFILE — user-initiated, no TCC),
//   b) a native open-panel (NSOpenPanel / GetOpenFileNameW; a user-selected
//      file is TCC-exempt), and
//   c) a typed path.
//
// The one path we DO read without being asked is the remembered ROM in the
// app's own preferences file, which the app owns and which needs no permission.
#include "ui_launcher.h"
#include "app_brand.h"
#include "app_config.h"
#include "app_theme.h"
#include "file_dialog.h"
#include "ui_common.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

char g_pathInput[1024] = {0};

// Set when the player asks to replace a ROM that is already working, so the
// acquisition controls stay behind an explicit "Change ROM" rather than
// cluttering the ready state.
bool g_changing = false;

// Non-fatal note shown under the acquisition controls (e.g. a cancelled panel).
std::string g_note;

void drawDropZone(bool haveRom) {
    // The drop target is the whole window (SDL_DROPFILE is window-wide), so this
    // is an invitation, not a hit-box. Mirrors the web shell's drop-zone
    // language so the two front-ends read the same.
    const ImVec4 accent = AppTheme::accent();
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
    ImGui::BeginChild("##dropzone", ImVec2(0, 86 * AppTheme::uiScale()), true);
    {
        ImGui::PushFont(AppTheme::fonts().title);
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        ImGui::TextUnformatted(haveRom ? "Drop a different ROM here"
                                       : "Drag your ROM file here");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ui::TextSubtle("...or use the buttons below.  Accepts .z64, .n64 and .v64 files.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// The acquisition controls: native panel (where one exists), and a typed path.
void drawAcquisition(LauncherState &s) {
    if (filedialog::isAvailable()) {
        if (ui::PrimaryButton("Choose ROM File...", ui::kBtnWide())) {
            std::string picked;
            if (filedialog::openRom(picked)) {
                g_note.clear();
                RomPanel_setRom(s, picked.c_str());
            } else {
                g_note = "No file chosen.";
            }
        }
        ImGui::SameLine();
        ui::TextSubtle("Opens your system's file picker.");
        ui::Gap(ui::kGapM);
    }

    ui::TextSubtle("Or paste the full path to your ROM:");
    ImGui::SetNextItemWidth(ui::kControlWidth(1.6f));
    const bool entered = ImGui::InputText("##rompath", g_pathInput, sizeof(g_pathInput),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    const bool stackAction = ImGui::GetContentRegionAvail().x <
                             ui::kBtnSecondary().x + ImGui::GetStyle().ItemSpacing.x;
    if (!stackAction) ImGui::SameLine();
    const bool pressed = ImGui::Button("Use This Path", ui::kBtnSecondary());
    if (entered || pressed) {
        if (g_pathInput[0] == '\0') {
            g_note = "Type a path first, or use the file picker.";
        } else {
            g_note.clear();
            RomPanel_setRom(s, g_pathInput);
        }
    }

    if (!g_note.empty()) {
        ui::Gap(ui::kGapS);
        ui::TextSubtle("%s", g_note.c_str());
    }
}

}  // namespace

void RomPanel_setRom(LauncherState &s, const char *path) {
    if (path == nullptr || path[0] == '\0') return;
    char candidatePath[sizeof(s.romPath)];
    std::snprintf(candidatePath, sizeof(candidatePath), "%s", path);
    const RomInfo candidateInfo = mdkr_validate_rom(candidatePath);
    std::snprintf(g_pathInput, sizeof(g_pathInput), "%s", candidatePath);

    if (!candidateInfo.valid) {
        std::snprintf(s.romCandidatePath, sizeof(s.romCandidatePath), "%s",
                      candidatePath);
        s.romCandidateInfo = candidateInfo;
        s.romCandidateError[0] = '\0';
        s.romCandidateVisible = true;
        if (s.romInfo.valid) g_changing = true;
        return;
    }

    // A candidate becomes active only after its preference transaction lands.
    // Failed storage therefore cannot strand the user without their working ROM.
    if (!AppConfig::setAndSave("rom_path", candidatePath)) {
        std::snprintf(s.romCandidatePath, sizeof(s.romCandidatePath), "%s",
                      candidatePath);
        s.romCandidateInfo = candidateInfo;
        std::snprintf(s.romCandidateError, sizeof(s.romCandidateError),
                      "This ROM is valid, but the preference file could not be "
                      "saved. Your current ROM was kept.");
        s.romCandidateVisible = true;
        if (s.romInfo.valid) g_changing = true;
        return;
    }

    std::snprintf(s.romPath, sizeof(s.romPath), "%s", candidatePath);
    s.romInfo = candidateInfo;
    s.romCandidatePath[0] = '\0';
    s.romCandidateInfo = RomInfo{};
    s.romCandidateError[0] = '\0';
    s.romCandidateVisible = false;
    g_changing = false;
}

void RomPanel_ensureInit(LauncherState &s) {
    if (s.romInitialized) return;
    s.romInitialized = true;

    AppConfig::load();
    std::string remembered = AppConfig::get("rom_path", "");
    if (remembered.empty()) return;   // first run: the player will hand us one

    // Validating the remembered path touches exactly one file the player chose
    // themselves on a previous run. No directory is enumerated.
    std::snprintf(s.romPath, sizeof(s.romPath), "%s", remembered.c_str());
    s.romInfo = mdkr_validate_rom(s.romPath);
    std::snprintf(g_pathInput, sizeof(g_pathInput), "%s", s.romPath);
}

void RomPanel_draw(LauncherState &s, LauncherAction &out) {
    RomPanel_ensureInit(s);

    ui::SectionHeader("Game ROM",
                      MDKR_BRAND_NAME " needs a copy of the original game that you "
                      "supply and legally own. No game data is included, and "
                      "nothing on your disk is searched.");

    const bool haveRom = s.romPath[0] != '\0';
    const bool ready   = haveRom && s.romInfo.valid;

    // ---- The identified ROM, stated plainly before Play -------------------
    // A wrong pick has to be VISIBLE, so the revision the validator actually
    // recognised is the most prominent thing on the card, not a footnote.
    if (haveRom) {
        const ImVec4 border = ready ? AppTheme::good() : AppTheme::bad();
        if (ui::CardBegin("##romcard", border, 0.0f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, border);
            ImGui::PushFont(AppTheme::fonts().title);
            if (ready && s.romInfo.revision[0]) {
                ImGui::TextUnformatted(s.romInfo.revision);
            } else {
                ImGui::TextUnformatted(ready ? "Ready" : "This ROM cannot be used");
            }
            ImGui::PopFont();
            ImGui::PopStyleColor();

            if (ready && s.romInfo.build[0]) {
                ui::TextSubtle("%s  \xE2\x80\xA2  %s  \xE2\x80\xA2  CRC %08X %08X%s",
                               s.romInfo.build, s.romInfo.byte_order,
                               s.romInfo.crc1, s.romInfo.crc2,
                               s.romInfo.matched_by_crc ? "" : "  (matched without CRC)");
            }

            ui::Gap(ui::kGapXS);
            ImGui::PushTextWrapPos(0.0f);
            if (!ready) {
                // The validator's own sentence: it names the revision it found
                // and why this build refuses it.
                ImGui::TextWrapped("%s", s.romInfo.message);
                ui::Gap(ui::kGapXS);
            }
            ui::TextSubtle("%s", s.romPath);
            ImGui::PopTextWrapPos();
        }
        ui::CardEnd();

        if (!ready) {
            ui::Gap(ui::kGapS);
            ui::TextSubtle("Supported: %s", mdkr_supported_rom_list());
        }
        ui::Gap(ui::kGapM);
    }

    if (s.romCandidateVisible) {
        const char *message = s.romCandidateError[0]
            ? s.romCandidateError : s.romCandidateInfo.message;
        if (ui::CardBegin("##romcandidate", AppTheme::bad(), 0.0f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ImGui::TextUnformatted(s.romCandidateInfo.valid
                                       ? "Replacement was not saved"
                                       : "Replacement ROM cannot be used");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", message);
            ui::TextSubtleWrapped("%s", s.romCandidatePath);
        }
        ui::CardEnd();
        ui::Gap(ui::kGapM);
    }

    // ---- Play, and the way back to changing your mind ---------------------
    if (ready) {
        PlayButton_draw(s, out);
        if (ImGui::GetContentRegionAvail().x > ui::kBtnSecondary().x +
                                               ImGui::GetStyle().ItemSpacing.x) {
            ImGui::SameLine();
        }
        if (ImGui::Button(g_changing ? "Cancel" : "Change ROM...", ui::kBtnSecondary())) {
            if (g_changing) {
                g_changing = false;
                s.romCandidateVisible = false;
                s.romCandidatePath[0] = '\0';
                s.romCandidateError[0] = '\0';
                std::snprintf(g_pathInput, sizeof(g_pathInput), "%s", s.romPath);
            } else {
                g_changing = true;
            }
            g_note.clear();
        }
        ui::Gap(ui::kGapL);
    }

    // ---- Acquisition ------------------------------------------------------
    // Shown whenever there is nothing usable yet, or the player asked to change.
    if (!ready || g_changing) {
        if (ready) {
            ImGui::Separator();
            ui::Gap(ui::kGapM);
        }
        drawDropZone(haveRom);
        ui::Gap(ui::kGapM);
        drawAcquisition(s);
    }
}
