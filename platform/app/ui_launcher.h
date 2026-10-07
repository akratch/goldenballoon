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
// is always visible in MDKR_ENABLE_ONLINE_BETA builds (including the 1.7 native
// release packages), visible under MDKR_ONLINE_ROOM_PREVIEW=1 in a beta-OFF
// development-preview build, and absent when both compile-time gates are off
// (see panelVisible in ui_launcher.cpp). Its INDEX is unconditional in every
// state because the constants below are a public smoke contract.
#ifndef MDKR64_UI_LAUNCHER_H
#define MDKR64_UI_LAUNCHER_H

#include "launcher_panels.h" // kLauncherPanel* (public smoke contract)
#include "engine_entry.h"   // MdkrBootConfig
#include "rom_validate.h"   // RomInfo
#include "ui_phone_party.h" // PhonePartyLanControls
#include "party/lan_party_transport.h" // MdkrLanPartyTransportConfig

#include <cstdint>
#include <string>
#include <memory>

/* GetModuleFileNameW permits 32,767 UTF-16 code units; UTF-8 needs at most
 * four bytes per unit. Keep a finite UI/input contract below that OS boundary.
 * The preference writer reserves enough escaped-line capacity for this value. */
constexpr size_t kLauncherRomPathMaxBytes = 32767u * 4u;

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
    /* Local-play (no internet) control surface for the Phone Party card. The
     * launcher fills these before the card draws and reads `request` back after
     * the frame -- switching to/from the LAN transport rebuilds `phoneParty`, so
     * it must happen at a safe point, not mid-draw. */
    PhonePartyLanControls lanParty;
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
    // A Workshop preview uses the same mandatory final ROM check as Play. It
    // remains pending only for that asynchronous check, then is copied into the
    // one-shot boot config or cleared on cancellation/failure.
    std::string characterPreviewPackage;
    std::string characterPreviewSourceSha256;
    std::string characterPreviewFitSha256;
    std::string characterPreviewPresentationSha256;
    MdkrCharacterPreviewContext characterPreviewContext =
        MDKR_CHARACTER_PREVIEW_NONE;
    MdkrCharacterPreviewScene characterPreviewScene =
        MDKR_CHARACTER_PREVIEW_SCENE_BASELINE;
    int characterPreviewPlayers = 0;
    MdkrCharacterPreviewPose characterPreviewPose =
        MDKR_CHARACTER_PREVIEW_POSE_LIVE;
    unsigned characterPreviewPosePhaseMilli = 0u;
    MdkrCharacterPreviewPose characterPreviewTransitionFromPose =
        MDKR_CHARACTER_PREVIEW_POSE_LIVE;
    unsigned characterPreviewTransitionFromPhaseMilli = 0u;
    int characterPreviewViewYawDegrees = 0;
    int characterPreviewViewPitchDegrees = 0;
    MdkrWorkshopPreviewLighting characterPreviewLighting =
        MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
    std::string characterPreviewCapturePng;
    MdkrCharacterPreviewCaptureKind characterPreviewCaptureKind =
        MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE;
    bool characterPreviewAutoReturn = false;
    bool characterPreviewCaptureLauncherOwned = false;
    bool characterPreviewPortraitSourceHandoff = false;
    bool characterPreviewInteractiveStudio = false;
    bool characterPreviewRepresentativeMotionReview = false;
    bool characterPreviewDonorReference = false;
    bool characterPreviewDispatched = false;
    MdkrCharacterPreviewResult characterPreviewResult{};
    MdkrCharacterMotionReviewResult characterMotionReviewResult{};
    // Play was pressed while a replacement Selection check was still running.
    // The check keeps running on the candidate file; once it resolves, the
    // mandatory final Play check runs against whatever ROM that resolution
    // leaves active (the new one on success, the previous one on failure), so
    // Play can never publish a decision against a ROM the player just
    // replaced out from under it.
    bool romPlayAwaitingReplacement = false;
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
    // A normal Quit/window-close request waits for the current transactional
    // character operation to publish while the launcher remains visible and
    // responsive. The player can cancel this request from the progress card.
    bool    quitRequested = false;
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

struct LauncherNetworkShutdown;

class Launcher {
public:
    Launcher();
    ~Launcher();
    LauncherAction draw(AppHost &host);
    void setBootError(const char *message);
    void requestQuit();
    bool quitRequested() const;
    bool quitReady() const;
    // Non-rendering completion publication, including returned preview results.
    // Called even while minimized, before deciding whether Quit can proceed.
    void serviceCharacterWork();
    // Exceptional exits cannot draw progress but still publish completed work
    // before launcher/global owners are destroyed. Does not abandon workers.
    void finishCharacterWorkForExit();
    // Irreversible app-exit phase, after Workshop/engine work has settled.
    // Retires room and phone owners before observing process-wide RTC cleanup.
    void beginNetworkShutdown();
    // Terminal includes failure; inspect networkShutdownFailed() before success.
    bool pollNetworkShutdown();
    bool networkShutdownFailed() const;
    // Exceptional/non-renderable exit backstop; waits, returning success only
    // after every owner and the global cleanup have completed successfully.
    bool finishNetworkShutdownForExit();
    // Closing-only surface: no panel service, drop intake, Play or room actions.
    void drawOnlineClosing(bool delayed);

    // Test-only entry point for the shell smoke.  It uses the exact same
    // asynchronous final recheck as the Play widget, while leaving the smoke
    // in the launcher instead of booting a game session.
    void requestPlayValidationForSmoke();

    // "Skip the launcher" (Launcher.SkipWhenReady, issue #60). main() makes
    // the launch decision -- the setting, and what was being held when the app
    // opened -- and arms this before the first frame. Once armed, the first
    // frame on which the remembered ROM has settled and verified presses the
    // SAME Play the player would have pressed: the mandatory final ROM check
    // still runs, and only its verdict publishes a boot. Nothing is skipped
    // except the waiting.
    void armSkipWhenReady();
    // Read-only witnesses for tests/check_launcher_skip.py.
    bool skipArmedForSmoke() const { return skipArmed_; }
    bool skipDispatchedForSmoke() const { return skipDispatched_; }

    // Read-only view of the shared panel state (ROM path/verdict). Exists for
    // the headless shell smoke (MDKR_APP_SMOKE_DROP) to observe the outcome of
    // a smoke-generated SDL_DROPFILE the same way a screenshot proves a
    // synthetic frame: by reading what the launcher itself ended up holding.
    const LauncherState &state() const { return state_; }

    // Read-only test witness for the intermediate-width navigation smoke.
    int activePanelForSmoke() const { return active_; }

    // Which relay backs the one live Phone Party transport. Cloud is the
    // shipped default (the compiled https Party service); Lan is the embedded
    // zero-internet server+room. Runtime mutual-exclusion is enforced by
    // selectPartyTransport(): exactly one is ever live. Task 5 owns the UI that
    // flips this at runtime.
    enum class PartyTransportKind { Cloud, Lan };

private:
    // Tears the current transport (and the host bound to it) down before
    // building the requested one, so a cloud and a LAN transport can never be
    // live at once. Called at construction with the shipped default, and by the
    // party card's deferred Start/Stop. `config` seeds the LAN transport
    // (advertised host, controller manifest, port); ignored for Cloud.
    void selectPartyTransport(PartyTransportKind kind,
                              MdkrLanPartyTransportConfig config = {});
    // Deferred local-play actions from the party card, applied after the frame.
    void applyLanStart();
    void applyLanStop();
    // Refresh lanParty.available/active/note for the card (cheap; throttled).
    void refreshLanControls();

    std::unique_ptr<LauncherNetworkShutdown> networkShutdown_;
    std::unique_ptr<MdkrPartyTransport> partyTransport_;
    std::unique_ptr<MdkrNativePartyHost> phoneParty_;
    PartyTransportKind partyKind_ = PartyTransportKind::Cloud;
    // Owns the storage the card's `note` pointer borrows (a start failure).
    std::string lanNote_;
    bool lanAvailable_ = false;      // cached host + controller-asset presence
    bool lanHostReachable_ = false;  // cached advertised-host presence (reason)
    bool lanChecked_ = false;
    uint64_t lanCheckedMs_ = 0u;     // last availability check (SDL ticks)
    LauncherState state_;
    // The launch decision, and whether this launch has already asked to boot.
    // Once per launch: a boot that fails its final check leaves the player in
    // the launcher looking at the reason, and must not be retried behind them.
    bool skipArmed_ = false;
    bool skipDispatched_ = false;
    // Samples of the launch hold taken by this launcher. main()'s pre-frame
    // sample is 0, so these continue from 1; the count is what lets the test
    // seam make a hold appear partway through the window.
    unsigned holdSamples_ = 0u;
    int  active_ = 0;               // index into the panel table
    bool panelEnvChecked_ = false;  // MDKR_APP_PANEL design-review/CI hook
};

// Returns the last rendered logical-window rectangle center for a top
// navigation destination. Test-only; false when the responsive layout is using
// the wide rail or dense dropdown instead of top tabs.
bool Launcher_smokeChooseRomFocused();
bool Launcher_smokePackInstallButtonFocused();
bool Launcher_smokePackBrowserOpen();
bool Launcher_smokePackBrowserFocusedZip(const char *path);
bool Launcher_smokePackInstalled();
bool Launcher_smokePackInstallPending();
bool RomPanel_smokeBrowserOpen();
bool RomPanel_smokeBrowserFocusedRom(const char *path);
bool RomPanel_smokeBrowserFocusedFolder(const char *path);
bool RomPanel_smokeBrowserAtDirectory(const char *path);
bool RomPanel_smokeBrowserRootFocused();
bool RomPanel_smokeBrowserUpFocused();
bool RomPanel_smokeBrowserScrolled();
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
