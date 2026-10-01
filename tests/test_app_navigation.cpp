// Keyboard navigation regression: Escape must not end Tab traversal.
//
// CPU draw commands and input events only: no platform backend, native window,
// GPU or ROM. The frame below has the launcher's real window shape -- a root
// window holding flattened child regions for the header and the content, and a
// flattened card inside the content (ui::CardBegin) -- and the context is set
// up by the same AppNavigation_configure() both AppHost backends call.
//
// The defect: when no control claims Escape, Dear ImGui's default
// (ConfigNavEscapeClearFocusItem) forgets the focused control but leaves the
// navigation cursor visible. A forward Tab from that state looks for the
// focused control to move past, never finds it, and does nothing -- on every
// later press too. A keyboard or screen-reader player who pressed Escape while
// the Character Workshop ran a background job (it stops claiming Escape then)
// or on any panel that never claims it could no longer Tab to anything.
#include "app_navigation.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstdlib>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL app navigation: %s\n", message);
        std::exit(1);
    }
}

struct Shell {
    ImGuiID section = 0, quit = 0, acknowledge = 0, browse = 0, rescan = 0;

    explicit Shell(bool launcherNavigation) {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(640.0f, 480.0f);
        io.DeltaTime = 1.0f / 60.0f;
        if (launcherNavigation) {
            AppNavigation_configure(io);
        } else {
            // Dear ImGui's own defaults with keyboard navigation switched on:
            // the configuration the launcher used to run with.
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        }
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->AddFontDefault();
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        expect(pixels != nullptr && width > 0 && height > 0,
               "CPU font atlas built");
        io.Fonts->SetTexID(static_cast<ImTextureID>(1)); // draw-list token only
        frame();
        frame();
    }

    ~Shell() { ImGui::DestroyContext(); }
    Shell(const Shell &) = delete;
    Shell &operator=(const Shell &) = delete;

    void frame() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##launcher", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::BeginChild("##topnav", ImVec2(0.0f, 60.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::Button("Section");
        section = ImGui::GetItemID();
        ImGui::SameLine();
        ImGui::Button("Quit");
        quit = ImGui::GetItemID();
        ImGui::EndChild();
        ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_NavFlattened);
        ImGui::BeginChild("##status", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_NavFlattened);
        ImGui::TextWrapped("Checking private failed-import metadata in the background.");
        ImGui::SmallButton("Acknowledge");
        acknowledge = ImGui::GetItemID();
        ImGui::EndChild();
        ImGui::Button("Browse for another source");
        browse = ImGui::GetItemID();
        ImGui::Button("Rescan installed characters");
        rescan = ImGui::GetItemID();
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
    }

    // Down and up on separate frames, as a person types them.
    void press(ImGuiKey key) {
        ImGuiIO &io = ImGui::GetIO();
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        frame();
    }

    ImGuiID focused() const { return ImGui::GetCurrentContext()->NavId; }

    // Tab until `target` holds keyboard focus; false if it never does.
    bool tabTo(ImGuiID target) {
        for (int press = 0; press < 12; ++press) {
            this->press(ImGuiKey_Tab);
            if (focused() == target) return true;
        }
        return false;
    }
};

// Escape on a control nothing claims Escape for, then Tab: where does focus go?
ImGuiID tabAfterEscape(bool launcherNavigation, ImGuiID Shell::*control,
                       ImGuiID Shell::*expectedNext) {
    Shell shell(launcherNavigation);
    expect(shell.tabTo(shell.*control), "Tab reaches the control under test");
    shell.press(ImGuiKey_Escape);
    shell.press(ImGuiKey_Tab);
    const ImGuiID after = shell.focused();
    if (launcherNavigation) {
        expect(after == shell.*expectedNext,
               "Tab after Escape moves to the next control");
        // ...and keeps going: every control is still reachable in order.
        expect(shell.tabTo(shell.rescan), "Tab after Escape reaches the last control");
        shell.press(ImGuiKey_Tab);
        expect(shell.focused() == shell.section, "Tab after Escape wraps to the first control");
    }
    return after;
}

}  // namespace

int main() {
    // The fixture must reproduce the trap with Dear ImGui's default Escape
    // handling, or the launcher assertions below prove nothing. If a future
    // Dear ImGui stops trapping Tab here, re-evaluate AppNavigation_configure.
    expect(tabAfterEscape(false, &Shell::acknowledge, &Shell::browse) == 0,
           "fixture reproduces the default Escape trap inside a card");
    expect(tabAfterEscape(false, &Shell::quit, &Shell::acknowledge) == 0,
           "fixture reproduces the default Escape trap in the header");

    // The launcher keeps the focused control on an unclaimed Escape, inside a
    // card (the Workshop's Acknowledge button) and in the header alike.
    tabAfterEscape(true, &Shell::acknowledge, &Shell::browse);
    tabAfterEscape(true, &Shell::quit, &Shell::acknowledge);
    tabAfterEscape(true, &Shell::browse, &Shell::rescan);

    std::puts("app navigation: PASS");
    return 0;
}
