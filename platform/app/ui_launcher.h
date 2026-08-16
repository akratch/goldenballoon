// ui_launcher.h — the pre-boot launcher: nav rail + table-driven panel router.
//
// Panels are free functions over a shared LauncherState, so the router is one
// table and a new panel is one row.
//
// DKR ADAPTATION: mgb64's launcher carries Launch (level/difficulty/players)
// and Modes (engine toggle hatches) panels. Neither exists here — DKR's engine
// exposes no level or difficulty CLI, and its presentation modes are already
// first-class schema keys — so the panel set is Play / Online Room / Settings /
// Diagnostics / About rather than mgb64's six. Panels DKR has no analogue for
// are absent, not present and inert.
//
// Panel 0 is Play: the home, which carries the ROM onboarding as a STATE
// rather than as a separate destination named after a file format. Online Room
// is compiled for development only with MDKR_ENABLE_ONLINE_ROOM_PREVIEW=ON and
// then drawn under MDKR_ONLINE_ROOM_PREVIEW=1 (see panelVisible in
// ui_launcher.cpp), so a shipped build offers exactly what the release offers;
// its INDEX is unconditional either way, because the constants below are a
// public smoke contract.
#ifndef MDKR64_UI_LAUNCHER_H
#define MDKR64_UI_LAUNCHER_H

#include "engine_entry.h"   // MdkrBootConfig
#include "rom_validate.h"   // RomInfo

#include <string>
#include <memory>

/* GetModuleFileNameW permits 32,767 UTF-16 code units; UTF-8 needs at most
 * four bytes per unit. Keep a finite UI/input contract below that OS boundary.
 * The preference writer reserves enough escaped-line capacity for this value. */
constexpr size_t kLauncherRomPathMaxBytes = 32767u * 4u;
constexpr int kLauncherPanelPlay = 0;
constexpr int kLauncherPanelOnlineRoom = 1;
constexpr int kLauncherPanelSettings = 2;
constexpr int kLauncherPanelDiagnostics = 3;
constexpr int kLauncherPanelAbout = 4;
constexpr int kLauncherPanelCount = 5;

struct SDL_Window;
class AppHost;
class MdkrNativePartyHost;
class MdkrPartyTransport;

enum class LauncherActionType { None, Play, Quit };

struct LauncherAction {
    LauncherActionType type = LauncherActionType::None;
    MdkrBootConfig boot = {};   // valid when type == Play
};

// State shared across launcher panels.
struct LauncherState {
    /* Borrowed for the current draw only. Window-mode settings use it for the
     * SDL transition; Launcher never owns or destroys the host window. */
    SDL_Window *hostWindow = nullptr;
    /* Process-owned by Launcher; borrowed by the Play Here panel. */
    MdkrNativePartyHost *phoneParty = nullptr;
    // Paths stay dynamically owned by the launcher until engine boot. The UTF-8
    // filesystem boundary supports Windows extended paths, so a fixed MAX_PATH
    // or 4 KiB UI buffer must never turn a valid picked file into another path.
    std::string romPath;
    RomInfo romInfo{};
    // A rejected replacement is kept separately so the active, proven ROM and
    // Play action remain intact until a candidate validates and persists.
    std::string romCandidatePath;
    RomInfo romCandidateInfo{};
    char    romCandidateError[1024] = {0};  // carries RomInfo.message verbatim
    bool    romCandidateVisible = false;
    // Non-fatal: a first ROM can remain playable for this process even when
    // its path could not be remembered. Existing playable state is never
    // displaced on the same persistence failure.
    char    romPersistenceWarning[512] = {0};
    bool    romInitialized = false;
    // Full-image verification runs on a dedicated worker. The selected active
    // ROM remains usable while a replacement is checked; Play has a separate
    // final-check flag so no filesystem work blocks an ImGui frame.
    std::string romValidationPath;
    unsigned romValidationBytes = 0;
    unsigned romValidationTotal = 0;
    bool romValidationPending = false;
    bool romPlayValidationPending = false;
    bool romPlayValidationPassed = false;
    // No discovery state: the launcher never searches the disk. The ROM arrives
    // by drag-and-drop, a native open-panel, a typed path, or the remembered
    // choice in the app's own prefs. See ui_rom.cpp's header for why.

    // A panel or responsive navigation control can request a destination.
    // -1 = no request; the shell consumes it after drawing.
    int     requestTab = -1;
    // Two independent writers stage navigation within one frame, and the ROM
    // validation service is one of them -- it runs twice per frame (once from
    // Launcher::draw, once from RomPanel_draw), so its second pass lands AFTER
    // the navigation controls have already recorded the player's click. Plain
    // last-writer-wins therefore let an in-flight Play verdict silently discard
    // that click. Requests carry a priority instead: a service request never
    // replaces the player's, while a later player request still supersedes an
    // earlier one (the responsive tab strip depends on that).
    int     requestTabPriority = 0;

    // A failed engine attempt is carried across the deliberate clean-process
    // relaunch and shown inline. The user's ROM/settings remain intact and the
    // Diagnostics panel is one action away.
    /* Holds a composed sentence AROUND a full ROM-validation verdict, which
     * is itself 1024 bytes (rom_validate.h); sized so the frame never
     * re-truncates what the verdict buffers were widened to carry. */
    char    bootError[1280] = {0};
    bool    bootErrorVisible = false;
};

// Deferred navigation. `priority` orders the frame's competing writers; use
// kLauncherTabPlayer for anything a person just pressed and
// kLauncherTabService for a background verdict redirecting the view.
enum {
    kLauncherTabService = 1,
    kLauncherTabPlayer  = 2
};
void Launcher_requestTab(LauncherState &s, int panel, int priority);

// Panels.
void RomPanel_ensureInit(LauncherState &s);                  // load the remembered ROM
void RomPanel_serviceValidation(LauncherState &s);
void RomPanel_requestPlayValidation(LauncherState &s);
// Opens the native file picker where one exists. False means unavailable or
// cancelled.
bool RomPanel_chooseRom(LauncherState &s);
void RomPanel_draw(LauncherState &s, LauncherAction &out);
void RomPanel_setRom(LauncherState &s, const char *path);    // drag-and-drop entry (validates)
void DiagPanel_draw(LauncherState &s, LauncherAction &out);

class Launcher {
public:
    Launcher();
    ~Launcher();
    LauncherAction draw(AppHost &host);
    void setBootError(const char *message);

    // Test-only entry point for the shell smoke.  It uses the exact same
    // asynchronous final recheck as the Play widget, while leaving the smoke
    // in the launcher instead of booting a game session.
    void requestPlayValidationForSmoke();

    // Read-only view of the shared panel state (ROM path/verdict). Exists for
    // the headless shell smoke (MDKR_APP_SMOKE_DROP) to observe the outcome of
    // a smoke-generated SDL_DROPFILE the same way a screenshot proves a
    // synthetic frame: by reading what the launcher itself ended up holding.
    const LauncherState &state() const { return state_; }

    // Read-only test witness for the intermediate-width navigation smoke.
    int activePanelForSmoke() const { return active_; }

private:
    std::unique_ptr<MdkrPartyTransport> partyTransport_;
    std::unique_ptr<MdkrNativePartyHost> phoneParty_;
    LauncherState state_;
    int  active_ = 0;               // index into the panel table
    bool panelEnvChecked_ = false;  // MDKR_APP_PANEL design-review/CI hook
};

// Returns the last rendered logical-window rectangle center for a top
// navigation destination. Test-only; false when the responsive layout is using
// the wide rail or dense dropdown instead of top tabs.
bool Launcher_smokeTopTabCenter(int panel, int *x, int *y);

// Last rendered Settings scroll viewport and position. These read-only seams
// let the dedicated handheld smoke begin a touch gesture on real panel content
// and prove that the production scroll owner moved.
bool Launcher_smokeSettingsScrollRect(int *minX, int *minY,
                                      int *maxX, int *maxY);
float Launcher_smokeSettingsScrollY();

// Active panel scroll owner. Settings has a nested owner, while every other
// panel uses the shared content child; this seam resolves that distinction so
// a production-input touch gate can qualify any launcher destination.
bool Launcher_smokePanelScrollRect(int *minX, int *minY,
                                   int *maxX, int *maxY);
float Launcher_smokePanelScrollY();

#endif  // MDKR64_UI_LAUNCHER_H
