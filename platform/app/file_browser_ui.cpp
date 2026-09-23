#include "file_browser_ui.h"
#include "ui_common.h"
#include "imgui.h"
#include <algorithm>
#include <cstdlib>

void FileBrowserUi::open(const std::string &remembered, BrowserFileKind kind) {
    browser_.open(remembered, kind);
    open_ = true;
    focusFirst_ = true;
}

bool FileBrowserUi::draw(const char *title, std::string &selection) {
    if (!open_) return false;
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    const ImVec2 viewport = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(
        ImVec2((std::min)(760.0f, viewport.x * 0.92f),
               (std::min)(640.0f, viewport.y * 0.90f)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings))
        return false;
    bool selected = false;
    ImGui::TextWrapped("Folder: %s", browser_.directory().u8string().c_str());
    const float width = (std::max)(1.0f,
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3);
    const ImVec2 navSize(width, ui::kBtnSecondary().y);
    if (ImGui::Button("Up", navSize)) { browser_.up(); focusFirst_ = true; }
    if (focusFirst_ && browser_.entries().empty()) {
        ImGui::SetItemDefaultFocus();
        ImGui::SetKeyboardFocusHere(-1);
        ImGui::SetNavCursorVisible(true);
        focusFirst_ = false;
    }
    ui::SpeakFocusedItem("Up", nullptr, "Go to the parent folder.");
    ImGui::SameLine();
    if (ImGui::Button("Home", navSize)) {
        const char *homePath = std::getenv("HOME");
        browser_.goTo(homePath && homePath[0] ? homePath : "/");
        focusFirst_ = true;
    }
    ui::SpeakFocusedItem("Home", nullptr, "Go to your home folder.");
    ImGui::SameLine();
    if (ImGui::Button("Root /", navSize)) { browser_.goTo("/"); focusFirst_ = true; }
    ui::SpeakFocusedItem("Root", nullptr, "Find mounted drives from the filesystem root.");

    // Reserve the actual scaled footer height, including the navigation hint.
    const float footer = ImGui::GetFrameHeightWithSpacing() +
                         ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##file-list", ImVec2(0, -footer),
                     ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    if (!browser_.error().empty()) ImGui::TextWrapped("%s", browser_.error().c_str());
    if (browser_.entries().empty()) ImGui::TextWrapped("No matching files. Choose another folder.");
    for (size_t i = 0; i < browser_.entries().size(); ++i) {
        const auto &entry = browser_.entries()[i];
        // Filename bytes are display text, not ImGui label/ID syntax.
        ImGui::PushID(static_cast<int>(i));
        const std::string name = entry.path.filename().u8string();
        const bool activated = ImGui::Selectable("##file", false,
                                                  ImGuiSelectableFlags_NoAutoClosePopups,
                                                  ImVec2(0, ImGui::GetFrameHeight()));
        const ImVec2 row = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(row, ImGui::GetColorU32(ImGuiCol_Text),
            (std::string(entry.directory ? "[Folder] " : "") + name).c_str());
        ui::SpeakFocusedItem(name.c_str(), entry.directory ? "Folder" : "File",
                             entry.directory ? "Open this folder." : "Choose this file.");
        if (focusFirst_ && i == 0) {
            ImGui::SetItemDefaultFocus(); ImGui::SetKeyboardFocusHere(-1);
            ImGui::SetNavCursorVisible(true);
            focusFirst_ = false;
        }
        ImGui::PopID();
        if (activated) {
            // Copy before navigating: goTo replaces the backing entry vector.
            const auto path = entry.path;
            if (entry.directory) { browser_.goTo(path); focusFirst_ = true; }
            else { selection = path.u8string(); selected = true; open_ = false; }
            break;
        }
    }
    ImGui::EndChild();
    ImGui::TextUnformatted("A: open   B: parent folder   D-pad/stick: move");
    if (ImGui::Button("Cancel", ui::kBtnSecondary()) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        open_ = false;
    ui::SpeakFocusedItem("Cancel", nullptr, "Close without selecting a file.");
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        if (browser_.directory() == browser_.directory().root_path()) open_ = false;
        else { browser_.up(); focusFirst_ = true; }
    }
    if (!open_) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return selected;
}
