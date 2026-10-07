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
// later press too, until another navigation key or a click moves focus. A
// keyboard or screen-reader player who pressed Escape while the Character
// Workshop ran a background job (it stops claiming Escape then) or on any
// panel that never claims it could no longer Tab forward.
//
// The same dead end has a second cause the configuration cannot reach: the
// shell itself removing the focused control while its window stays open (the
// in-game menu's confirmation and Settings list). The overlay-shaped cases
// below pin AppNavigation_recoverLostFocus(), which both frame ends run, and
// that it stays out of the way of Dear ImGui's own focus moves.
#include "app_navigation.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL app navigation: %s\n", message);
        std::exit(1);
    }
}

void createContext() {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->AddFontDefault();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    expect(pixels != nullptr && width > 0 && height > 0, "CPU font atlas built");
    io.Fonts->SetTexID(static_cast<ImTextureID>(1)); // draw-list token only
}

constexpr ImGuiWindowFlags kRootFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoSavedSettings;

struct Shell {
    ImGuiID section = 0, quit = 0, acknowledge = 0, browse = 0, rescan = 0;

    const bool launcherNavigation;

    explicit Shell(bool launcherNavigation)
        : launcherNavigation(launcherNavigation) {
        createContext();
        ImGuiIO &io = ImGui::GetIO();
        if (launcherNavigation) {
            AppNavigation_configure(io);
        } else {
            // Dear ImGui's own defaults with keyboard navigation switched on:
            // the configuration the launcher used to run with.
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        }
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
        // As AppHost::endFrame does. The Escape cases below must hold with
        // it: keeping the focused control is what makes Tab carry on, where
        // the recovery would only start it over at the first control.
        if (launcherNavigation) AppNavigation_recoverLostFocus();
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

// A one-window frame for the cases below. `recover` mirrors the end-of-frame
// AppNavigation_recoverLostFocus() call both AppHost and the overlay make.
struct Ui {
    std::function<void()> draw;
    bool recover;

    Ui(bool recover, std::function<void()> drawWindow)
        : draw(std::move(drawWindow)), recover(recover) {
        createContext();
        ImGuiIO &io = ImGui::GetIO();
        AppNavigation_configure(io);
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;  // controller B below
        frame();
        frame();
    }
    ~Ui() { ImGui::DestroyContext(); }
    Ui(const Ui &) = delete;
    Ui &operator=(const Ui &) = delete;

    void frame() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(560.0f, 400.0f));
        ImGui::Begin("##overlay", nullptr, kRootFlags);
        draw();
        ImGui::End();
        if (recover) AppNavigation_recoverLostFocus();
        ImGui::Render();
    }

    void press(ImGuiKey key) {
        ImGuiIO &io = ImGui::GetIO();
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        frame();
    }

    // The overlay handles Escape and B in its SDL event handler, before the
    // frame in which ImGui sees the same key: the screen changes first.
    void backOut(ImGuiKey key, const std::function<void()> &appBack) {
        appBack();
        press(key);
    }

    ImGuiID focused() const { return ImGui::GetCurrentContext()->NavId; }
    bool cursorVisible() const {
        return ImGui::GetCurrentContext()->NavCursorVisible;
    }

    bool tabTo(ImGuiID target) {
        for (int count = 0; count < 12; ++count) {
            press(ImGuiKey_Tab);
            if (focused() == target) return true;
        }
        return false;
    }
};

// The in-game menu's shape: Resume, a Settings button whose label changes,
// the Settings list (a child window that is ONE Tab stop, entered with Enter),
// Quit to Desktop, and a confirmation that replaces them.
struct Menu {
    bool confirming = false;
    bool settings = false;
    ImGuiID resume = 0, settingsButton = 0, list = 0, row = 0, quit = 0;
    ImGuiID confirm = 0, cancel = 0;

    void draw() {
        if (confirming) {
            ImGui::TextUnformatted("Quit to desktop? This ends the current race.");
            ImGui::Button("Quit");
            confirm = ImGui::GetItemID();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) confirming = false;
            cancel = ImGui::GetItemID();
            return;
        }
        ImGui::Button("Resume");
        resume = ImGui::GetItemID();
        ImGui::SameLine();
        if (ImGui::Button(settings ? "Hide Settings###settings"
                                   : "Settings###settings")) {
            settings = !settings;
        }
        settingsButton = ImGui::GetItemID();
        if (settings) {
            ImGui::BeginChild("##ovsettings", ImVec2(0.0f, 120.0f),
                              ImGuiChildFlags_Borders);
            ImGui::Button("Display");
            row = ImGui::GetItemID();
            ImGui::Button("Audio");
            ImGui::EndChild();
            list = ImGui::GetItemID();
        }
        if (ImGui::Button("Quit to Desktop")) confirming = true;
        quit = ImGui::GetItemID();
    }
};

// One of the overlay's dead ends, then Tab. Without the recovery Tab must stay
// stuck (the fixture reproduces the defect); with it, Tab must land on Resume.
void confirmationClosedByBack(bool recover, ImGuiKey key) {
    Menu menu;
    menu.confirming = true;
    Ui ui(recover, [&menu] { menu.draw(); });
    expect(ui.tabTo(menu.cancel), "Tab reaches Cancel in the confirmation");
    ui.backOut(key, [&menu] { menu.confirming = false; });
    expect(!menu.confirming, "the back-out closed the confirmation");
    ui.press(ImGuiKey_Tab);
    if (!recover) {
        expect(ui.focused() == menu.cancel,
               "fixture reproduces the confirmation trap: Tab does not move");
        return;
    }
    expect(ui.focused() == menu.resume,
           "Tab after closing the confirmation lands on the first control");
    expect(ui.cursorVisible(), "...with the highlight on it");
    expect(ui.tabTo(menu.quit), "...and Tab keeps going");
}

void confirmationClosedByCancel(bool recover) {
    Menu menu;
    menu.confirming = true;
    Ui ui(recover, [&menu] { menu.draw(); });
    expect(ui.tabTo(menu.cancel), "Tab reaches Cancel in the confirmation");
    ui.press(ImGuiKey_Enter);
    expect(!menu.confirming, "Enter on Cancel closed the confirmation");
    ui.press(ImGuiKey_Tab);
    if (!recover) {
        expect(ui.focused() == menu.cancel,
               "fixture reproduces the Cancel trap: Tab does not move");
        return;
    }
    expect(ui.focused() == menu.resume && ui.cursorVisible(),
           "Tab after Cancel lands on the first control");
}

void confirmationOpened(bool recover) {
    Menu menu;
    Ui ui(recover, [&menu] { menu.draw(); });
    expect(ui.tabTo(menu.quit), "Tab reaches Quit to Desktop");
    ui.press(ImGuiKey_Enter);
    expect(menu.confirming, "Enter opened the confirmation");
    ui.press(ImGuiKey_Tab);
    if (!recover) {
        expect(ui.focused() == menu.quit,
               "fixture reproduces the opening trap: Tab does not move");
        return;
    }
    expect(ui.focused() == menu.confirm && ui.cursorVisible(),
           "Tab in a freshly opened confirmation lands on its first button");
}

void settingsListClosed(bool recover, ImGuiKey key) {
    Menu menu;
    Ui ui(recover, [&menu] { menu.draw(); });
    expect(ui.tabTo(menu.settingsButton), "Tab reaches Settings");
    ui.press(ImGuiKey_Enter);
    expect(menu.settings, "Enter opened the Settings list");
    expect(ui.focused() == menu.settingsButton,
           "a fixed id keeps focus on the relabelled Settings button");
    expect(ui.tabTo(menu.list), "Tab reaches the Settings list");
    ui.press(ImGuiKey_Enter);
    expect(ui.focused() == menu.row, "Enter moves into the list");
    ui.backOut(key, [&menu] { menu.settings = false; });
    expect(!menu.settings, "the back-out closed the Settings list");
    ui.press(ImGuiKey_Tab);
    if (!recover) {
        // Dear ImGui's "leave the child window" step put focus on the list.
        expect(ui.focused() == menu.list,
               "fixture reproduces the Settings list trap: Tab does not move");
        return;
    }
    expect(ui.focused() == menu.resume && ui.cursorVisible(),
           "Tab after closing the Settings list lands on the first control");
}

// A launcher card (ui::CardBegin: a flattened child) scrolled fully out of view
// by the wheel while a control inside it holds keyboard focus. Dear ImGui then
// stops drawing the card's controls, but re-submits them as soon as a Tab is
// pending, so stock Tab carries on inside the card. The recovery must not take
// that for a vanished control and send Tab back to the window's first stop.
void scrolledCardKeepsTab(bool recover) {
    ImGuiID top = 0, first = 0, second = 0;
    ImGuiWindow *card = nullptr;
    Ui ui(recover, [&] {
        ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_NavFlattened);
        ImGui::Button("Top");
        top = ImGui::GetItemID();
        ImGui::BeginChild("##card", ImVec2(0.0f, 80.0f),
                          ImGuiChildFlags_Borders |
                              ImGuiChildFlags_NavFlattened);
        card = ImGui::GetCurrentWindow();
        ImGui::Button("First in card");
        first = ImGui::GetItemID();
        ImGui::Button("Second in card");
        second = ImGui::GetItemID();
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(10.0f, 1200.0f));
        ImGui::Button("Bottom");
        ImGui::EndChild();
    });
    expect(ui.tabTo(first), "Tab reaches the first control in the card");
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(100.0f, 100.0f);
    for (int notch = 0; notch < 10; ++notch) {
        io.AddMouseWheelEvent(0.0f, -3.0f);
        ui.frame();
    }
    ui.frame();
    ui.frame();
    // The fixture must really reach the state: card drawn but its controls
    // skipped, focus on one of them, highlight still showing.
    expect(card != nullptr && card->Active && card->SkipItems,
           "fixture scrolls the card fully out of view");
    expect(!ImGui::GetCurrentContext()->NavIdIsAlive,
           "...so the focused control is not drawn");
    ui.press(ImGuiKey_Tab);
    expect(ui.focused() != top,
           "Tab from a scrolled-away card does not start over at the top");
    expect(ui.focused() == second,
           "Tab from a scrolled-away card carries on inside it");
}

// A live control can become disabled while it holds focus (a job starts or
// availability changes). It remains a traversal reference, never a destination.
void disabledFocusContinuesTab() {
    bool disabled = false;
    ImGuiID middle = 0, last = 0;
    Ui ui(true, [&] {
        ImGui::Button("Before disabled");
        ImGui::BeginDisabled(disabled);
        ImGui::Button("Becomes disabled");
        middle = ImGui::GetItemID();
        ImGui::EndDisabled();
        ImGui::Button("After disabled");
        last = ImGui::GetItemID();
    });
    expect(ui.tabTo(middle), "fixture focuses the control before disabling");
    disabled = true;
    ui.frame();
    ui.press(ImGuiKey_Tab);
    expect(ui.focused() == last, "Tab advances past a newly disabled control");
    ui.press(ImGuiKey_Tab);
    expect(ui.focused() != middle, "disabled control is never a Tab destination");
}

void collapsedCardRecoversTab() {
    bool collapsed = false;
    ImGuiID inside = 0;
    ImGuiWindow *card = nullptr;
    Ui ui(true, [&] {
        ImGui::Button("Before collapsed card");
        ImGui::BeginChild("##collapsing-card", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
        card = ImGui::GetCurrentWindow();
        if (!collapsed) {
            ImGui::Button("Vanishing control");
            inside = ImGui::GetItemID();
        }
        ImGui::EndChild();
        ImGui::Button("After collapsed card");
    });
    expect(ui.tabTo(inside), "fixture focuses the auto-sized card");
    collapsed = true;
    ui.frame();
    ui.frame();
    ui.frame();
    expect(card->ContentSize.y == 0.0f, "fixture collapses the card content");
    ui.press(ImGuiKey_Tab);
    expect(ui.focused() != inside && ImGui::GetCurrentContext()->NavIdIsAlive,
           "Tab recovers after a flattened auto-sized card collapses");
}

// The recovery must not undo Dear ImGui's own focus moves: a popup opened from
// the keyboard focuses its first item with the highlight showing, and Escape
// closes it back onto the control that opened it (the launcher configuration
// keeps that). Tab then carries on.
void popupKeepsFocus() {
    ImGuiID opener = 0, first = 0, next = 0;
    bool popupOpen = false;
    Ui ui(true, [&] {
        if (ImGui::Button("Choose")) ImGui::OpenPopup("choices");
        opener = ImGui::GetItemID();
        popupOpen = false;
        if (ImGui::BeginPopup("choices")) {
            popupOpen = true;
            ImGui::Button("First choice");
            first = ImGui::GetItemID();
            ImGui::Button("Second choice");
            ImGui::EndPopup();
        }
        ImGui::Button("Next");
        next = ImGui::GetItemID();
    });
    expect(ui.tabTo(opener), "Tab reaches the popup's opener");
    ui.press(ImGuiKey_Enter);
    expect(popupOpen, "Enter opened the popup");
    expect(ui.focused() == first && ui.cursorVisible(),
           "the popup's first item takes focus with the highlight showing");
    ui.press(ImGuiKey_Escape);
    expect(!popupOpen, "Escape closes a non-modal popup");
    expect(ui.focused() == opener && ui.cursorVisible(),
           "...and returns focus to the control that opened it");
    ui.press(ImGuiKey_Tab);
    expect(ui.focused() == next, "Tab carries on from the opener");
}

// Escape still ends text entry first, and focus stays on the field.
void textEntryKeepsFocus() {
    ImGuiID before = 0, field = 0, after = 0;
    static char name[32] = "Banjo";
    Ui ui(true, [&] {
        ImGui::Button("Before");
        before = ImGui::GetItemID();
        ImGui::InputText("Name", name, sizeof(name));
        field = ImGui::GetItemID();
        ImGui::Button("After");
        after = ImGui::GetItemID();
    });
    expect(ui.tabTo(before), "Tab reaches the control before the text field");
    ui.press(ImGuiKey_Tab);
    expect(ui.focused() == field &&
               ImGui::GetCurrentContext()->ActiveId == field,
           "Tab onto a text field starts editing it");
    ImGui::GetIO().AddInputCharacter('!');
    ui.frame();
    ui.press(ImGuiKey_Escape);
    expect(ImGui::GetCurrentContext()->ActiveId == 0,
           "Escape ends text entry");
    expect(std::strcmp(name, "Banjo") == 0, "...and drops the unfinished edit");
    expect(ui.focused() == field && ui.cursorVisible(),
           "...with focus still on the field");
    ui.press(ImGuiKey_Tab);
    expect(ui.focused() == after, "Tab carries on from the field");
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

    // The in-game menu's dead ends: first without the end-of-frame recovery
    // (each fixture must reproduce its trap), then with it.
    for (const bool recover : {false, true}) {
        confirmationClosedByBack(recover, ImGuiKey_Escape);
        confirmationClosedByBack(recover, ImGuiKey_GamepadFaceRight);
        confirmationClosedByCancel(recover);
        confirmationOpened(recover);
        settingsListClosed(recover, ImGuiKey_Escape);
        settingsListClosed(recover, ImGuiKey_GamepadFaceRight);
    }
    // Stock Dear ImGui carries on here; the recovery must leave it so.
    scrolledCardKeepsTab(false);
    scrolledCardKeepsTab(true);
    disabledFocusContinuesTab();
    collapsedCardRecoversTab();
    popupKeepsFocus();
    textEntryKeepsFocus();

    std::puts("app navigation: PASS");
    return 0;
}
