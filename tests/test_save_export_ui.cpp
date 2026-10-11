#include "ui_save_export.h"
#include "file_dialog.h"
#include "ui_common.h"
#include "../platform/user_paths.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <iostream>

static std::string saveDirectory, exportPath;
static int chooserCalls;
static bool cancel;
static void check(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
extern "C" int mdkr_user_save_directory(char *out, size_t size) {
    return std::snprintf(out, size, "%s", saveDirectory.c_str()) < static_cast<int>(size);
}
namespace filedialog {
bool isAvailable() { return true; }
bool saveGameProgress(std::string &out, const std::string &name) {
    ++chooserCalls; check(name.find(".mdkr-save") != std::string::npos, "default extension");
    if (cancel) return false; out = exportPath; return true;
}
bool revealInFileManager(const std::string &) { return true; }
}
namespace ui {
void GroupHeader(const char *label, const char *subtitle) { ImGui::TextUnformatted(label); ImGui::TextWrapped("%s", subtitle); }
void TextSubtleWrapped(const char *fmt, ...) { va_list a; va_start(a,fmt); ImGui::TextV(fmt,a); va_end(a); }
void TextSubtle(const char *fmt, ...) { va_list a; va_start(a,fmt); ImGui::TextV(fmt,a); va_end(a); }
void TextSubtleUnformattedWrapped(const char *text) { ImGui::TextWrapped("%s",text); }
void SpeakFocusedItem(const char *, const char *, const char *) {}
ImVec2 kBtnWide() { return ImVec2(150,40); }
void Gap(float amount) { ImGui::Dummy(ImVec2(0,amount)); }
}
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("mdkr-save-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{root};
    saveDirectory=root.u8string(); exportPath=(root/"export.mdkr-save").u8string();
    ImGui::CreateContext(); auto &io=ImGui::GetIO(); io.IniFilename=nullptr;
    io.ConfigNavCursorVisibleAlways=true;
    io.DisplaySize=ImVec2(640,480); io.DeltaTime=1.0f/60;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;io.BackendFlags|=ImGuiBackendFlags_HasGamepad;
    unsigned char *pixels; int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    // Focus only the control under test; activation still travels through real ImGui
    // gamepad events. This fixture does not model the surrounding Settings navigation.
    auto frame=[&](bool focus=false) { ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);ImGui::Begin("Host");if(focus) { auto *window = ImGui::GetCurrentWindow(); ImGui::FocusWindow(window); ImGui::SetNavID(window->GetID("Export save"), ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect(ImVec2(0,0), ImVec2(150,40))); }SaveExport_draw();ImGui::End();ImGui::Render();
        const auto *data = ImGui::GetDrawData();
        for (const auto *list : data->CmdLists)
            for (const auto &vertex : list->VtxBuffer)
                check(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "invalid UI geometry");
    };
    auto press=[&] {io.AddKeyEvent(ImGuiKey_GamepadFaceDown,true);frame();io.AddKeyEvent(ImGuiKey_GamepadFaceDown,false);frame();frame();};
    frame();frame(true);frame();press();check(chooserCalls==0,"empty-save button active");
    uint8_t empty[512]={};std::ofstream(root/"eeprom.bin",std::ios::binary).write(reinterpret_cast<char*>(empty),sizeof(empty));
    frame(true);frame();press();check(chooserCalls==1 && fs::exists(fs::u8path(exportPath)),"controller export failed");
    cancel=true;frame(true);frame();press();check(chooserCalls==2,"cancel chooser not reached");
    check(fs::file_size(root/"eeprom.bin")==512,"source changed");
    ImGui::DestroyContext();std::cout<<"save export UI: disabled empty state, controller export and dialog cancel PASS\n";
}
