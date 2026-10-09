// ROM-free real-ImGui navigation/draw-list test. Device rendering is separate.
#include "file_browser_ui.h"
#include "ui_common.h"
#include "imgui.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

namespace ui {
std::string lastFocusedName;
ImVec2 kBtnSecondary() { return ImVec2(190, ImGui::GetFrameHeight()); }
void SpeakFocusedItem(const char *name, const char *, const char *) {
    if (ImGui::IsItemFocused()) lastFocusedName = name;
}
}

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("mdkr-browser-ui-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
    fs::create_directory(root / "Folder");
    const fs::path expected = root / "Folder" / "game##literal.Z64";
    const fs::path expectedZip = root / "Folder" / "textures##literal.ZIP";
    std::ofstream(expected).put('x');
    std::ofstream(expectedZip).put('x');
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 480);
    io.DeltaTime = 1.0f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char *pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (float scale : {1.0f, 2.0f}) {
        io.FontGlobalScale = scale;
        FileBrowserUi browser;
        browser.open((root / "remembered.z64").string(), BrowserFileKind::Rom);
        std::string selection;
        bool chosen = false;
        int drawnVertices = 0;
        auto frame = [&] {
            ImGui::NewFrame();
            ImGui::Begin("Host");
            chosen |= browser.draw("Choose ROM", selection);
            ImGui::End();
            ImGui::Render();
            const auto *data = ImGui::GetDrawData();
            // ImGui may hide the initial auto-fit frame while measuring.
            drawnVertices += data->TotalVtxCount;
            for (const auto *list : data->CmdLists)
                for (const auto &vertex : list->VtxBuffer)
                    assert(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y));
        };
        auto idle = [&] { for (int i = 0; i < 4; ++i) frame(); };
        auto press = [&](ImGuiKey key) {
            io.AddKeyEvent(key, true); frame();
            io.AddKeyEvent(key, false); idle();
        };
        idle();
        assert(drawnVertices > 0);
        assert(ui::lastFocusedName == "Folder");
        press(ImGuiKey_GamepadFaceDown); // first item: Folder
        assert(!chosen);
        assert(ui::lastFocusedName == "game##literal.Z64");
        press(ImGuiKey_GamepadFaceDown); // first item within: ROM
        assert(chosen && selection == expected.u8string());
        browser.open(expected.u8string(), BrowserFileKind::Rom);
        idle();
        chosen = false;
        press(ImGuiKey_Escape);
        press(ImGuiKey_GamepadFaceDown);
        assert(!chosen); // closed, no accidental selection
        assert(selection == expected.u8string()); // cancellation did not rewrite output

        browser.open(expectedZip.u8string(), BrowserFileKind::ZipPack);
        chosen = false;
        idle();
        assert(ui::lastFocusedName == "textures##literal.ZIP");
        press(ImGuiKey_GamepadFaceRight); // B goes to parent, not selection
        assert(!chosen);
        assert(ui::lastFocusedName == "Folder");
        press(ImGuiKey_GamepadFaceDown);
        assert(!chosen);
        assert(ui::lastFocusedName == "textures##literal.ZIP");
        press(ImGuiKey_GamepadFaceDown);
        assert(chosen && selection == expectedZip.u8string());

        // Root navigation is observed using "/" by the app's controller
        // journey, while the browser stores a drive-qualified root on Windows.
        browser.open((fs::absolute("/") / "mdkr-root-observer.z64").u8string(),
                     BrowserFileKind::Rom);
        idle();
        assert(browser.atDirectory("/"));
        assert(!browser.atDirectory(nullptr));
        assert(!browser.atDirectory(expected.parent_path().u8string().c_str()));
        press(ImGuiKey_GamepadFaceRight);
        assert(!browser.visible());
    }
    ImGui::DestroyContext();
    std::puts("file browser UI: ROM/ZIP selection, controller parent navigation and cancel passed at 100%/200%");
}
