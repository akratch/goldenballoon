// ui_launcher.h — the pre-boot launcher: nav rail + table-driven panel router.
//
// Panels are free functions over a shared LauncherState, so the router is one
// table and a new panel is one row.
//
// DKR ADAPTATION: mgb64's launcher carries Launch (level/difficulty/players)
// and Modes (engine toggle hatches) panels. Neither exists here — DKR's engine
// exposes no level or difficulty CLI, and its presentation modes are already
// first-class schema keys — so the panel set is ROM / Settings / Diagnostics /
// About rather than mgb64's six. Panels DKR has no analogue for are absent, not
// present and inert.
#ifndef MDKR64_UI_LAUNCHER_H
#define MDKR64_UI_LAUNCHER_H

#include "engine_entry.h"   // MdkrBootConfig
#include "rom_validate.h"   // RomInfo

#include <string>

class AppHost;

enum class LauncherActionType { None, Play, Quit };

struct LauncherAction {
    LauncherActionType type = LauncherActionType::None;
    MdkrBootConfig boot = {};   // valid when type == Play
};

// State shared across launcher panels.
struct LauncherState {
    char    romPath[1024] = {0};
    RomInfo romInfo{};
    bool    romInitialized = false;
    // No discovery state: the launcher never searches the disk. The ROM arrives
    // by drag-and-drop, a native open-panel, a typed path, or the remembered
    // choice in the app's own prefs. See ui_rom.cpp's header for why.

    // A panel can ask the shell to switch tabs (the disabled-Play hint jumps to
    // Game ROM). -1 = no request; the shell consumes it after drawing.
    int     requestTab = -1;
};

// Panels.
void RomPanel_ensureInit(LauncherState &s);                  // load the remembered ROM
void RomPanel_draw(LauncherState &s, LauncherAction &out);
void RomPanel_setRom(LauncherState &s, const char *path);    // drag-and-drop entry (validates)
void SettingsPanel_draw(LauncherState &s, LauncherAction &out);
void DiagPanel_draw(LauncherState &s, LauncherAction &out);
void AboutPanel_draw(LauncherState &s, LauncherAction &out);

// Shared helpers.
void PlayButton_draw(LauncherState &s, LauncherAction &out);  // primary Play (or disabled hint)
void fillBoot(const LauncherState &s, MdkrBootConfig &boot);  // ROM + staged restart settings

class Launcher {
public:
    LauncherAction draw(AppHost &host);

private:
    LauncherState state_;
    int  active_ = 0;               // index into the panel table
    bool panelEnvChecked_ = false;  // MDKR_APP_PANEL design-review/CI hook
};

#endif  // MDKR64_UI_LAUNCHER_H
