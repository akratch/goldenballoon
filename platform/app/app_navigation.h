// app_navigation.h -- the launcher's keyboard and controller navigation setup.
#ifndef MDKR64_APP_NAVIGATION_H
#define MDKR64_APP_NAVIGATION_H

#include "imgui.h"
#include "imgui_internal.h"

// Both AppHost backends (OpenGL and WebGPU) create the launcher's one Dear
// ImGui context. They configure navigation through this single function so the
// two paths cannot drift, and tests/test_app_navigation.cpp drives a
// launcher-shaped frame through the same call.
inline void AppNavigation_configure(ImGuiIO &io) {
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // An Escape (or controller B) that nothing else claims keeps the focused
    // control. Dear ImGui's default forgets it but leaves the navigation
    // cursor visible, and a forward Tab from that state has no control to
    // move past, so it never moves again. Among the launcher panels only the
    // Character Workshop claims Escape, and not while a character job runs;
    // everywhere else one press left a keyboard or screen-reader player
    // unable to Tab to anything. Escape still cancels text entry and closes
    // popups first.
    io.ConfigNavEscapeClearFocusItem = false;
}

// Call once per frame after the last window ends and before ImGui::Render():
// AppHost::endFrame for the launcher, endImGuiFrame for the in-game overlay.
//
// Dear ImGui remembers focus as the id of the focused control. When a screen
// stops drawing that control while its window stays open -- a confirmation
// closed by Escape, B or Cancel; a list closed with focus inside it; a button
// whose label (and so id) changes when pressed -- the id names nothing. With
// the highlight showing, a forward Tab looks for that control to move past,
// never finds it, and does nothing until another navigation key or a click
// moves focus. That is a dead end for a keyboard or screen-reader player, and
// any screen added later can create it again by removing a control.
//
// So when the highlight is showing but focus is on nothing, or on a control
// that was not drawn this frame, forget it and hide the highlight: the next
// Tab, arrow or D-pad press then starts at the window's first control, the
// state Dear ImGui gives a freshly opened window. It stays out of the way
// whenever Dear ImGui is still settling focus itself (a move, Tab or focus
// request, or a window picking its default control resolves at the next
// NewFrame), while a widget drawn this frame is active (text entry, a held
// slider), and when the focused window was not drawn at all (Dear ImGui
// refocuses the topmost window on its own then). A button pressed with Enter
// stays "active" for one frame after it disappears; that does not count, or
// a Tab pressed straight after Enter on Cancel would still hit the dead end.
// Returns true when it reset focus.
inline bool AppNavigation_recoverLostFocus() {
    ImGuiContext *context = ImGui::GetCurrentContext();
    if (context == nullptr) return false;
    ImGuiContext &g = *context;
    ImGuiWindow *window = g.NavWindow;
    if (!g.NavCursorVisible || window == nullptr || !window->Active ||
        (window->Flags & ImGuiWindowFlags_NoNavInputs) != 0) {
        return false;
    }
    if (g.NavId != 0 && g.NavIdIsAlive) return false;
    if (g.NavMoveSubmitted || g.NavMoveForwardToNextFrame ||
        g.NavInitRequest || g.NavInitResult.ID != 0 ||
        g.NavWindowingTarget != nullptr ||
        (g.ActiveId != 0 && g.ActiveIdIsAlive == g.ActiveId)) {
        return false;
    }
    ImGui::SetNavID(0, ImGuiNavLayer_Main, window->NavRootFocusScopeId,
                    ImRect());
    ImGui::SetNavCursorVisible(false);
    return true;
}

// Where keyboard/controller focus stands at the end of a frame: on a control
// that was drawn this frame, on one that was not, or on nothing.
struct AppNavigationFocusState {
    bool focused;        // a control id holds focus
    bool drawn;          // ...and that control was submitted this frame
    bool cursorVisible;  // the keyboard/controller highlight is showing
};

inline AppNavigationFocusState AppNavigation_focusState() {
    const ImGuiContext *context = ImGui::GetCurrentContext();
    if (context == nullptr) return {false, false, false};
    return {context->NavId != 0, context->NavId != 0 && context->NavIdIsAlive,
            context->NavCursorVisible};
}

#endif
