// app_navigation.h -- the launcher's keyboard and controller navigation setup.
#ifndef MDKR64_APP_NAVIGATION_H
#define MDKR64_APP_NAVIGATION_H

#include "imgui.h"

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

#endif
