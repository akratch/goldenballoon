// ui_overlay.cpp — see ui_overlay.h.
//
// DKR ADAPTATIONS, and what each one costs:
//
//  1. PAUSE. The engine keeps polling/rendering while the menu is open but gives
//     time-based simulation, menu, audio, and transition consumers a zero update
//     rate; is_game_paused() also includes this overlay state. Input remains
//     swallowed, so neither the game nor its authored pause menu consumes the
//     keys/buttons used to operate ImGui.
//
//  2. FPS READOUT. mgb64 flips its engine's Video.FpsOverlay config key. mdkr64
//     has no such key and no FPS display at all, so F10 toggles a readout this
//     overlay draws itself. Shell-owned, so it needs nothing from the engine.
//
//  3. REBINDING. mgb64 stores the toggle keys in its engine config registry.
//     mdkr64 has none, so the toggles are READ from the app's own prefs
//     (mdkr64_app.ini) with the same F1 / F10 / gamepad-Back defaults. There is
//     no rebinding widget and nothing here writes those keys back: editing the
//     file is currently the only way to change them.
#include "ui_overlay.h"
#include "a11y_model.h"     // mdkr_a11y_announce: the in-game notice surface
#include "a11y_speech.h"    // the drain worker's per-frame pump
#include "user_paths.h"     // mdkr_user_paths_save_write_failed (issue #54)
#include "app_brand.h"
#include "app_config.h"
#include "app_navigation.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "app_window.h"
#include "dev_tools.h"      // DevTools_draw: the in-game diagnostic surface
#include "engine_entry.h"   // AppOverlayHooks, platformSetOverlayHooks
#include "ui_phone_party.h"
#include "party/native_party_host.h"
#include "ui_common.h"
#include "ui_settings.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include "platform_os.h"    // mdkr_render_backend
#include "present_sched.h"  // authoritative test-schedule tick
#include "modern_character_studio_bridge.h"

#ifdef MDKR_WEBGPU_BACKEND
#include "gfx_webgpu_imgui.h"
// The surface overlay pass opened by gfx_webgpu.c for exactly this purpose.
extern "C" void *gfx_webgpu_current_overlay_pass(void);
extern "C" void  gfx_webgpu_current_overlay_size(int *w, int *h);
#endif

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum class LastInputDevice { KeyboardMouse, Gamepad, Touch };
enum class OverlayMode { GameMenu, CharacterReview, CharacterStudio };
enum class ConfirmAction {
    None,
    RestartGame,
    ReturnToLauncher,
    ReturnToWorkshop,
    QuitToDesktop,
};

// The overlay is installed through C callbacks, so it has process lifetime.
// Keep all of that callback-owned state in one place: this makes its lifecycle
// and transitions visible without scattering independent globals across the
// file. The app still installs exactly one overlay per engine boot.
struct OverlayState {
    OverlayMode mode = OverlayMode::GameMenu;
    bool open = false;
    bool showSettings = false;
    bool showFps = false;
    ConfirmAction confirm = ConfirmAction::None;
    bool justOpened = false;
    bool audioPaused = false;
    bool pauseAllowed = true;
    SDL_Window *window = nullptr;
    OverlayExitRequest exitRequest = OverlayExitRequest::None;
    bool previousRelativeMouse = false;
    LastInputDevice lastInputDevice = LastInputDevice::KeyboardMouse;
    std::string studioPackage;
    MdkrCharacterPreviewContext studioContext =
        MDKR_CHARACTER_PREVIEW_NONE;
    bool studioReturnPending = false;
    uint64_t studioReturnRequestedFrame = 0u;
    uint64_t studioReturnRequestedMillis = 0u;
    uint64_t renderFrame = 0u;
    long studioTestReturnFrame = -1;
    bool studioRenderReported = false;
    bool studioControlsCollapsed = false;
    bool studioEditorInitialized = false;

    // Scripted render-proof state. Keeping it here (instead of function-local
    // statics) makes every bit of persistent overlay state explicit.
    bool testScheduleLoaded = false;
    long testOpenFrame = -1;
    long testCloseFrame = -1;
    long testEscapeOpenFrame = -1;
    long testEscapeCloseFrame = -1;
    long testFrame = 0;
    bool testEscapeOpenQueued = false;
    bool testEscapeCloseQueued = false;
    bool testFpsOnly = false;
    bool testFpsRenderReported = false;
    // Scripted accessibility walk. Simulation is stopped while the overlay is
    // open, so this counts RENDER callbacks: a schedule hung off the
    // authoritative tick would never advance past the frame that paused it.
    bool testA11yWalk = false;
    long testA11yRenderFrame = 0;
    // Scripted player keystrokes and controller presses, one per step, in
    // rendered frames for the same reason (MDKR_TEST_OVERLAY_NAV_SCRIPT).
    std::vector<int> testNavScript;
    size_t testNavStep = 0;
    uint64_t testNavStepFrame = 0;
    bool testNavStarted = false;
    bool testNavComplete = false;

    uint64_t fpsLastSurfaceFrame = 0;
    uint64_t fpsLastCounter = 0;
    double surfaceFps = 0.0;
};

OverlayState g_overlay;
MdkrNativePartyHost *g_phonePartyHost = nullptr;

// --- Bindings (app prefs, defaults matching mgb64) --------------------------
int prefInt(const char *key, int fallback) {
    std::string v = AppConfig::get(key, "");
    if (v.empty()) return fallback;
    char *end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    return (end && *end == '\0') ? (int)n : fallback;
}

int menuToggleKey() { return prefInt("menu_toggle_key", SDLK_F1); }
int fpsToggleKey()  { return prefInt("fps_toggle_key",  SDLK_F10); }

void setOpen(bool open) {
    if (g_overlay.mode == OverlayMode::CharacterStudio && !open) return;
    if (open == g_overlay.open) return;
    g_overlay.open = open;
    g_overlay.confirm = ConfirmAction::None;
    if (g_overlay.open) {
        g_overlay.justOpened = true;   // give pad/keyboard nav an anchor
        g_overlay.previousRelativeMouse = (SDL_GetRelativeMouseMode() == SDL_TRUE);
        SDL_SetRelativeMouseMode(SDL_FALSE);   // free the cursor for the overlay
        mdkr64_engine_overlay_audio_pause();
        g_overlay.audioPaused = true;
    } else {
        Settings_cancelAudioPreview();
        if (g_overlay.audioPaused) {
            mdkr64_engine_overlay_audio_resume();
            g_overlay.audioPaused = false;
        }
        g_overlay.showSettings = false;
        SDL_SetRelativeMouseMode(g_overlay.previousRelativeMouse ? SDL_TRUE : SDL_FALSE);
    }
}

void quitToDesktop() {
    SDL_Event q{};        // zero-init: never queue uninitialized bytes
    q.type = SDL_QUIT;
    SDL_PushEvent(&q);
}

void returnToLauncher() {
    // The engine owns live GPU/audio state and the application owns a joined
    // diagnostic tee. Request a normal engine unwind; main_app consumes the
    // transition only after all borrowed host objects have been released.
    g_overlay.exitRequest = OverlayExitRequest::ReturnToLauncher;
    quitToDesktop();
}

void returnToLauncherWithoutMeasurement() {
    g_overlay.exitRequest =
        OverlayExitRequest::ReturnToLauncherWithoutMeasurement;
    quitToDesktop();
}

void restartGame() {
    /* Renderer/pacing resources intentionally latch at engine boot. Unwind all
     * borrowed GPU/audio state first; main_app then exec-replaces the process
     * with the same validated ROM and newly saved settings. */
    g_overlay.exitRequest = OverlayExitRequest::RestartGame;
    quitToDesktop();
}

void beginStudioReturn() {
    g_overlay.confirm = ConfirmAction::None;
    if (!Settings_commitCharacterOffsetStudio(
            g_overlay.studioPackage.c_str(), g_overlay.studioContext)) {
        return;
    }
    g_overlay.studioReturnPending = true;
    g_overlay.studioReturnRequestedFrame = g_overlay.renderFrame;
    g_overlay.studioReturnRequestedMillis = SDL_GetTicks64();
}

void navigateBack(OverlayBackInput input, bool keyRepeat) {
    if (g_overlay.mode == OverlayMode::CharacterStudio) {
        if (!keyRepeat &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            g_overlay.studioControlsCollapsed = false;
            g_overlay.confirm = ConfirmAction::ReturnToWorkshop;
        }
        return;
    }
    const OverlayBackState current = {
        g_overlay.open,
        g_overlay.showSettings,
        g_overlay.confirm != ConfirmAction::None,
    };
    const OverlayBackState next = AppUi_overlayBackTransition(
        current, input,
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId), keyRepeat);
    if (current.confirmation && !next.confirmation) {
        g_overlay.confirm = ConfirmAction::None;
    } else if (current.settings && !next.settings) {
        g_overlay.showSettings = false;
    } else if (current.open && !next.open) {
        setOpen(false);
    }
}

/* AppOverlayHooks members are declared with C language linkage (engine_entry.h
 * is shared with real C translation units), so the functions installed in them
 * must have C-linkage TYPES too -- assigning an ordinary C++ function to a
 * C-linkage function pointer is ill-formed even where it happens to work.
 * `static` inside a linkage-specification keeps each one internal to this TU
 * while giving its type C linkage, which is the whole fix: no exported symbol,
 * no change to how any of them is called. */
extern "C" {

static int onProcessEvent(const void *ev) {
    const SDL_Event *e = (const SDL_Event *)ev;
    if (AppWindow_handleEvent(g_overlay.window, *e)) {
        return 1;
    }
    // Undo macOS natural-scrolling on a copy before ImGui; the device-tracking
    // switch below still reads the original event. Without this the F1 overlay
    // scrolls backwards to the game under it on any Mac trackpad.
    SDL_Event forwarded = *e;
    AppWindow_normalizeWheel(forwarded);
    ImGui_ImplSDL2_ProcessEvent(&forwarded);

    // Track the active device from decisive events only, so the control hints
    // follow what the player is actually holding without flip-flopping on idle
    // stick drift or mouse jitter.
    switch (e->type) {
        case SDL_KEYDOWN:
            g_overlay.lastInputDevice = LastInputDevice::KeyboardMouse;
            break;
        case SDL_MOUSEBUTTONDOWN:
            g_overlay.lastInputDevice =
                e->button.which == SDL_TOUCH_MOUSEID
                    ? LastInputDevice::Touch
                    : LastInputDevice::KeyboardMouse;
            break;
        case SDL_MOUSEWHEEL:
            g_overlay.lastInputDevice =
                e->wheel.which == SDL_TOUCH_MOUSEID
                    ? LastInputDevice::Touch
                    : LastInputDevice::KeyboardMouse;
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_JOYBUTTONDOWN:
            g_overlay.lastInputDevice = LastInputDevice::Gamepad;
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (e->caxis.value > 8000 || e->caxis.value < -8000) {
                g_overlay.lastInputDevice = LastInputDevice::Gamepad;
            }
            break;
        default:
            break;
    }

    if (e->type == SDL_KEYDOWN && !e->key.repeat &&
        e->key.keysym.sym == menuToggleKey()) {
        setOpen(!g_overlay.open);
        return 1;
    }
    // FPS readout quick-toggle — deliberately does NOT open the menu.
    if (e->type == SDL_KEYDOWN && !e->key.repeat &&
        e->key.keysym.sym == fpsToggleKey()) {
        g_overlay.showFps = !g_overlay.showFps;
        return 1;
    }
    if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_ESCAPE &&
        (e->key.keysym.mod &
         (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT | KMOD_GUI)) == 0) {
        // ImGui owns Escape while a combo or popup is open. On the following
        // frame it closes that popup; only an otherwise-unclaimed Escape walks
        // our confirmation -> Settings -> overlay back-stack.
        if (g_overlay.open) {
            navigateBack(OverlayBackInput::Escape, e->key.repeat != 0);
        } else if (!e->key.repeat) {
            // Escape is pause/back in an active engine session. Desktop exit is
            // available only through the overlay's explicit confirmation.
            setOpen(true);
        }
        if (g_overlay.testEscapeOpenFrame >= 0) {
            std::fprintf(stderr,
                         "[overlay-test] Escape handled; overlay=%s tick=%d\n",
                         g_overlay.open ? "open" : "closed", g_simTickCounter);
        }
        return 1;
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        if ((int)e->cbutton.button == Overlay_gamepadToggleButton()) {
            setOpen(!g_overlay.open);
            return 1;
        } else if (g_overlay.open && e->cbutton.button == SDL_CONTROLLER_BUTTON_B) {
            // B = back one level. Skip while ImGui itself is consuming B (an
            // open combo/popup), so its own nav-cancel closes that first.
            navigateBack(OverlayBackInput::ControllerB, false);
            return 1;
        }
    }
    return 0;
}

// The input-swallowing contract: while the overlay is up, the engine's pump
// drops input events instead of feeding them to the pad.
static int onWantsInput(void) { return g_overlay.open ? 1 : 0; }
static int onWantsPause(void) {
    if (g_overlay.mode == OverlayMode::CharacterStudio) return 0;
    return (g_overlay.open && g_overlay.pauseAllowed) ? 1 : 0;
}
static int studioWantsFrame(void) {
    return g_overlay.mode == OverlayMode::CharacterStudio ? 1 : 0;
}
// Tools.Enabled joins the render predicate but NOT the input one. The overlay
// swallows the pad because it is a menu the player is operating; a diagnostic
// window is an observer, and a surface that took input away from the game would
// be changing the race by being visible -- exactly what the purity gate exists
// to forbid.
static int onWantsRender(void) {
    return (g_overlay.open || g_overlay.showFps || DevTools_wantsFrame() ||
            studioWantsFrame()) ? 1 : 0;
}

// The engine's per-frame service call, which during a race is the ONLY place
// the shell still runs on the main thread. The launcher pumps the speech worker
// from AppHost::pumpAndShouldQuit(); once the engine has the window that loop is
// gone, and without this line the overlay would announce rows that were never
// spoken. Same thread, same single-owner rule: see platform/a11y_speech.h.
static void onService(void) {
    AppWindow_servicePending();
    /* Issue #54: the engine latches a save-write failure the moment it happens
     * (during a blocking race the app shell is otherwise idle). Speak it once,
     * on the same thread that pushes announcements and just before the pump
     * below drains them, so a player who cannot see the launcher still learns
     * their progress was not saved. */
    static bool s_saveFailureAnnounced = false;
    if (!s_saveFailureAnnounced && mdkr_user_paths_save_write_failed()) {
        s_saveFailureAnnounced = true;
        mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_CRITICAL,
                           kSavePersistFailedNotice);
        SDL_Log("[app] %s", kSavePersistFailedNotice);
    }
    mdkr_a11y_speech_service_pump();
    if (g_phonePartyHost != nullptr) {
        g_phonePartyHost->service(static_cast<uint64_t>(SDL_GetTicks64()));
    }
}

}  // extern "C"

/*
 * Scripted player input for the open menu: MDKR_TEST_OVERLAY_NAV_SCRIPT is a
 * comma-separated list of keys (tab, shift-tab, enter, escape, up, down, left,
 * right) and controller presses (pad-b). Each step is one press and release,
 * then quiet frames until focus settles, so the steps never queue behind one
 * another. Keys enter through SDL exactly as a keyboard's do, carrying this
 * window's id so both ImGui and the back-stack above see them. pad-b presses
 * the B button of a virtual controller bound to ImGui the way the launcher's
 * gamepad smoke binds its own, so ImGui reads it as a held button and the
 * back-stack receives SDL's button event -- both halves of a real press.
 *
 * Every step prints a marker before its press and a "settled" line after it,
 * on stdout beside the [SPEAK] trace, so a check can tell which announcement
 * each step produced and where focus ended up. The menu closes when the
 * script ends.
 */
enum OverlayNavKey {
    kNavTab, kNavShiftTab, kNavEnter, kNavEscape,
    kNavUp, kNavDown, kNavLeft, kNavRight, kNavPadB,
};

struct OverlayNavKeyName {
    const char *name;
    OverlayNavKey key;
};

constexpr OverlayNavKeyName kOverlayNavKeys[] = {
    {"tab", kNavTab},     {"shift-tab", kNavShiftTab}, {"enter", kNavEnter},
    {"escape", kNavEscape}, {"up", kNavUp},            {"down", kNavDown},
    {"left", kNavLeft},   {"right", kNavRight},        {"pad-b", kNavPadB},
};

constexpr size_t kOverlayNavMaxSteps = 32;
constexpr uint64_t kOverlayNavSettleFrames = 6;
constexpr uint64_t kOverlayNavStepFrames = 8;

const char *overlayNavKeyName(int key) {
    for (const OverlayNavKeyName &entry : kOverlayNavKeys) {
        if (entry.key == key) return entry.name;
    }
    return "?";
}

bool parseOverlayNavScript(const char *text, std::vector<int> *steps) {
    steps->clear();
    const std::string script = text != nullptr ? text : "";
    size_t start = 0;
    while (start <= script.size()) {
        size_t end = script.find(',', start);
        if (end == std::string::npos) end = script.size();
        const std::string token = script.substr(start, end - start);
        bool known = false;
        for (const OverlayNavKeyName &entry : kOverlayNavKeys) {
            if (token == entry.name) {
                steps->push_back(entry.key);
                known = true;
                break;
            }
        }
        if (!known || steps->size() > kOverlayNavMaxSteps) {
            steps->clear();
            return false;
        }
        start = end + 1;
    }
    return !steps->empty();
}

#if SDL_VERSION_ATLEAST(2, 0, 14)
// Process lifetime once attached: ImGui's backend keeps polling it.
SDL_GameController *g_overlayNavPad = nullptr;
#endif

bool attachOverlayNavPad() {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (g_overlayNavPad != nullptr) return true;
    // A hidden test window never holds focus, and SDL ignores a joystick's
    // presses while no window does unless told otherwise (as app_host.cpp
    // does for the launcher's virtual controller).
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1",
                            SDL_HINT_OVERRIDE);
    const int device = SDL_JoystickAttachVirtual(
        SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
        SDL_CONTROLLER_BUTTON_MAX, 0);
    if (device < 0 || !SDL_IsGameController(device)) return false;
    g_overlayNavPad = SDL_GameControllerOpen(device);
    if (g_overlayNavPad == nullptr) return false;
    SDL_GameController *pads[] = {g_overlayNavPad};
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, pads, 1);
    return true;
#else
    return false;
#endif
}

void pushOverlayNavKey(SDL_Keycode sym, SDL_Scancode scancode, Uint16 mod,
                       bool down) {
    SDL_Event key{};
    key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    key.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    key.key.keysym.sym = sym;
    key.key.keysym.scancode = scancode;
    key.key.keysym.mod = mod;
    key.key.timestamp = SDL_GetTicks();
    // ImGui's SDL2 backend drops key events for windows it does not know.
    key.key.windowID = SDL_GetWindowID(g_overlay.window);
    (void)SDL_PushEvent(&key);
}

void applyOverlayNavStep(int step, bool down) {
    switch (step) {
        case kNavTab:
            pushOverlayNavKey(SDLK_TAB, SDL_SCANCODE_TAB, KMOD_NONE, down);
            break;
        case kNavShiftTab:
            pushOverlayNavKey(SDLK_TAB, SDL_SCANCODE_TAB,
                              down ? KMOD_LSHIFT : KMOD_NONE, down);
            break;
        case kNavEnter:
            pushOverlayNavKey(SDLK_RETURN, SDL_SCANCODE_RETURN, KMOD_NONE,
                              down);
            break;
        case kNavEscape:
            pushOverlayNavKey(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, KMOD_NONE,
                              down);
            break;
        case kNavUp:
            pushOverlayNavKey(SDLK_UP, SDL_SCANCODE_UP, KMOD_NONE, down);
            break;
        case kNavDown:
            pushOverlayNavKey(SDLK_DOWN, SDL_SCANCODE_DOWN, KMOD_NONE, down);
            break;
        case kNavLeft:
            pushOverlayNavKey(SDLK_LEFT, SDL_SCANCODE_LEFT, KMOD_NONE, down);
            break;
        case kNavRight:
            pushOverlayNavKey(SDLK_RIGHT, SDL_SCANCODE_RIGHT, KMOD_NONE,
                              down);
            break;
        case kNavPadB:
#if SDL_VERSION_ATLEAST(2, 0, 14)
            if (g_overlayNavPad != nullptr) {
                (void)SDL_JoystickSetVirtualButton(
                    SDL_GameControllerGetJoystick(g_overlayNavPad),
                    SDL_CONTROLLER_BUTTON_B,
                    down ? SDL_PRESSED : SDL_RELEASED);
            }
#endif
            break;
        default:
            break;
    }
}

void loadOverlayNavScript() {
    const char *script = std::getenv("MDKR_TEST_OVERLAY_NAV_SCRIPT");
    if (script == nullptr) return;
    if (!parseOverlayNavScript(script, &g_overlay.testNavScript)) {
        std::printf("[overlay-nav] script rejected: \"%s\"\n", script);
        std::fflush(stdout);
        return;
    }
    bool pad = false;
    for (int step : g_overlay.testNavScript) pad = pad || step == kNavPadB;
    if (pad && !attachOverlayNavPad()) {
        std::printf("[overlay-nav] script rejected: no virtual controller (%s)\n",
                    SDL_GetError());
        std::fflush(stdout);
        g_overlay.testNavScript.clear();
        return;
    }
    std::printf("[overlay-nav] script armed steps=%zu pad=%d\n",
                g_overlay.testNavScript.size(), pad ? 1 : 0);
    std::fflush(stdout);
}

void overlayTestNavTick() {
    if (g_overlay.testNavScript.empty() || g_overlay.testNavComplete) return;
    const uint64_t frame = g_overlay.renderFrame;
    if (!g_overlay.testNavStarted) {
        // Wait for the menu, then give its opening focus a few frames to land.
        if (!g_overlay.open) return;
        g_overlay.testNavStarted = true;
        g_overlay.testNavStepFrame = frame + kOverlayNavSettleFrames;
        return;
    }
    if (frame < g_overlay.testNavStepFrame) return;
    const uint64_t phase = frame - g_overlay.testNavStepFrame;
    const int step = g_overlay.testNavScript[g_overlay.testNavStep];
    // A controller button is held across two frames so ImGui's per-frame poll
    // of the pad sees it down; a key event is queued and cannot be missed.
    const uint64_t releasePhase = step == kNavPadB ? 3u : 1u;
    if (phase == 0) {
        std::printf("[overlay-nav] step=%zu key=%s\n", g_overlay.testNavStep,
                    overlayNavKeyName(step));
        std::fflush(stdout);
        applyOverlayNavStep(step, true);
    } else if (phase == releasePhase) {
        applyOverlayNavStep(step, false);
    } else if (phase == kOverlayNavStepFrames - 1) {
        // Read before this frame's NewFrame: the state the previous, complete
        // frame left behind.
        // input= is the device ImGui's own navigation last read. It turns to
        // "gamepad" only when ImGui saw a controller button held, so a check
        // can tell a B press that reached ImGui from one that only reached
        // the back-stack above.
        const AppNavigationFocusState focus = AppNavigation_focusState();
        const ImGuiInputSource source =
            ImGui::GetCurrentContext()->NavInputSource;
        std::printf(
            "[overlay-nav] settled step=%zu focus=%s cursor=%d open=%d "
            "confirm=%d settings=%d input=%s\n",
            g_overlay.testNavStep,
            focus.drawn ? "drawn" : (focus.focused ? "stale" : "none"),
            focus.cursorVisible ? 1 : 0, g_overlay.open ? 1 : 0,
            g_overlay.confirm != ConfirmAction::None ? 1 : 0,
            g_overlay.showSettings ? 1 : 0,
            source == ImGuiInputSource_Gamepad    ? "gamepad"
            : source == ImGuiInputSource_Keyboard ? "keyboard"
            : source == ImGuiInputSource_Mouse    ? "mouse"
                                                  : "none");
        std::fflush(stdout);
        ++g_overlay.testNavStep;
        g_overlay.testNavStepFrame += kOverlayNavStepFrames;
        if (g_overlay.testNavStep == g_overlay.testNavScript.size()) {
            g_overlay.testNavComplete = true;
            std::printf("[overlay-nav] script complete steps=%zu\n",
                        g_overlay.testNavScript.size());
            std::fflush(stdout);
            setOpen(false);
        }
    }
}

// Headless proof hook: scripts the overlay open/close at authoritative ticks so
// a gate can render it without a human at the keyboard. Presentation count is
// deliberately independent of simulation count: Original cadence, GPU
// backpressure, and high-rate interpolation can all make the number of renderer
// callbacks differ from g_simTickCounter. Scheduling on the old render ordinal
// therefore made the pause proof unreachable after the clocks were separated.
void overlayTestFrameTick() {
    if (!g_overlay.testScheduleLoaded) {
        g_overlay.testScheduleLoaded = true;
        const char *o = std::getenv("MDKR_TEST_OVERLAY_OPEN_FRAME");
        const char *c = std::getenv("MDKR_TEST_OVERLAY_CLOSE_FRAME");
        const char *escapeOpen =
            std::getenv("MDKR_TEST_OVERLAY_ESCAPE_OPEN_FRAME");
        const char *escapeClose =
            std::getenv("MDKR_TEST_OVERLAY_ESCAPE_CLOSE_FRAME");
        g_overlay.testOpenFrame = o ? std::strtol(o, nullptr, 10) : -1;
        g_overlay.testCloseFrame = c ? std::strtol(c, nullptr, 10) : -1;
        g_overlay.testEscapeOpenFrame = escapeOpen
            ? std::strtol(escapeOpen, nullptr, 10) : -1;
        g_overlay.testEscapeCloseFrame = escapeClose
            ? std::strtol(escapeClose, nullptr, 10) : -1;
        g_overlay.testA11yWalk =
            std::getenv("MDKR_TEST_OVERLAY_A11Y_WALK") != nullptr;
        loadOverlayNavScript();
    }
    overlayTestNavTick();
    if (g_overlay.testOpenFrame < 0 &&
        g_overlay.testEscapeOpenFrame < 0) return;

    g_overlay.testFrame = g_simTickCounter;

    /*
     * The overlay half of the accessibility walk. It opens the Settings
     * sub-panel and then holds down Tab, one press per rendered overlay frame,
     * so the rows the overlay draws -- the very same drawKey() rows the
     * launcher draws -- are proven to speak over a running race and not only
     * in the launcher. Tab is counted in RENDERED frames rather than
     * authoritative ticks because the overlay stops the simulation clock the
     * moment it opens.
     *
     * It lets go of Settings once the scripted close is due: Escape walks the
     * back-stack one level at a time, so a run that kept re-opening Settings
     * would spend its single scripted Escape leaving the sub-panel and never
     * close the overlay at all.
     */
    const bool closeDue = g_overlay.testEscapeCloseFrame >= 0 &&
                          g_overlay.testFrame >= g_overlay.testEscapeCloseFrame;
    if (g_overlay.testA11yWalk && g_overlay.open) {
        g_overlay.showSettings = !closeDue;
        if (!closeDue) {
            SDL_Event tab{};
            const bool press = (g_overlay.testA11yRenderFrame % 2) == 0;
            tab.type = press ? SDL_KEYDOWN : SDL_KEYUP;
            tab.key.state = press ? SDL_PRESSED : SDL_RELEASED;
            tab.key.keysym.sym = SDLK_TAB;
            tab.key.keysym.scancode = SDL_SCANCODE_TAB;
            tab.key.keysym.mod = KMOD_NONE;
            // The window id is load-bearing, unlike in the Escape hooks above:
            // those are read by this file's own handler, while Tab has to be
            // accepted by the ImGui SDL2 backend, which drops any key event
            // whose window it does not recognise.
            tab.key.timestamp = SDL_GetTicks();
            tab.key.windowID = SDL_GetWindowID(g_overlay.window);
            (void)SDL_PushEvent(&tab);
            ++g_overlay.testA11yRenderFrame;
        }
    }
    if (!g_overlay.testEscapeOpenQueued &&
        g_overlay.testEscapeOpenFrame >= 0 &&
        !g_overlay.open &&
        g_overlay.testFrame >= g_overlay.testEscapeOpenFrame) {
        SDL_Event escape{};
        escape.type = SDL_KEYDOWN;
        escape.key.state = SDL_PRESSED;
        escape.key.keysym.sym = SDLK_ESCAPE;
        escape.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        escape.key.keysym.mod = KMOD_NONE;
        g_overlay.testEscapeOpenQueued = SDL_PushEvent(&escape) == 1;
        std::fprintf(stderr,
                     "[overlay-test] queued Escape-open at tick %ld result=%d\n",
                     g_overlay.testFrame,
                     g_overlay.testEscapeOpenQueued ? 1 : 0);
    }
    if (!g_overlay.testEscapeCloseQueued &&
        g_overlay.testEscapeCloseFrame >= 0 &&
        g_overlay.open &&
        g_overlay.testFrame >= g_overlay.testEscapeCloseFrame) {
        SDL_Event escape{};
        escape.type = SDL_KEYDOWN;
        escape.key.state = SDL_PRESSED;
        escape.key.keysym.sym = SDLK_ESCAPE;
        escape.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        escape.key.keysym.mod = KMOD_NONE;
        g_overlay.testEscapeCloseQueued = SDL_PushEvent(&escape) == 1;
        std::fprintf(stderr,
                     "[overlay-test] queued Escape-close at tick %ld result=%d\n",
                     g_overlay.testFrame,
                     g_overlay.testEscapeCloseQueued ? 1 : 0);
    }
    if (g_overlay.testOpenFrame >= 0 && !g_overlay.open &&
        g_overlay.testFrame >= g_overlay.testOpenFrame &&
        (g_overlay.testCloseFrame < 0 ||
         g_overlay.testFrame < g_overlay.testCloseFrame)) {
        setOpen(true);
        std::fprintf(stderr,
                     "[overlay-test] opened at frame %ld (authoritative tick %ld)\n",
                     g_overlay.testOpenFrame, g_overlay.testFrame);
    }
    if (g_overlay.testCloseFrame >= 0 &&
        g_overlay.testFrame >= g_overlay.testCloseFrame && g_overlay.open) {
        setOpen(false);
        std::fprintf(stderr,
                     "[overlay-test] closed at frame %ld (authoritative tick %ld)\n",
                     g_overlay.testCloseFrame, g_overlay.testFrame);
    }
}

const char *menuKeyName() {
    const char *n = SDL_GetKeyName((SDL_Keycode)menuToggleKey());
    return (n && n[0]) ? n : "F1";
}

const char *menuButtonName() {
    switch (Overlay_gamepadToggleButton()) {
        case SDL_CONTROLLER_BUTTON_BACK:       return "View";
        case SDL_CONTROLLER_BUTTON_START:      return "Start";
        case SDL_CONTROLLER_BUTTON_GUIDE:      return "Guide";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:  return "Left stick click";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Right stick click";
        case SDL_CONTROLLER_BUTTON_A:          return "A";
        case SDL_CONTROLLER_BUTTON_B:          return "B";
        case SDL_CONTROLLER_BUTTON_X:          return "X";
        case SDL_CONTROLLER_BUTTON_Y:          return "Y";
        default:                               return "Menu";
    }
}

// The OpenGL-only build compiles out every caller, so compile out the helper as
// well rather than leaving an unused function under the strict warning gate.
#ifdef MDKR_WEBGPU_BACKEND
bool usingWebGpu() {
    return mdkr_render_backend() == MDKR_BACKEND_WEBGPU;
}
#endif

void beginImGuiFrame() {
    /* Zero means "not measurable this frame". A minimized window and an
     * occluded WebGPU surface both report no drawable, and the 1.0 ratio that
     * would imply is not the display's: feeding it to the DPI transition
     * rebuilds the font atlas at the wrong scale from inside the engine's
     * render callback, and again on restore. */
    float framebufferScale = 0.0f;
    if (g_overlay.window) {
        int logicalWidth = 0, logicalHeight = 0;
        int drawableWidth = 0, drawableHeight = 0;
        SDL_GetWindowSize(g_overlay.window, &logicalWidth, &logicalHeight);
#if defined(MDKR_WEBGPU_BACKEND)
        if (usingWebGpu()) {
            gfx_webgpu_current_overlay_size(&drawableWidth, &drawableHeight);
        } else
#endif
        {
            SDL_GL_GetDrawableSize(g_overlay.window, &drawableWidth, &drawableHeight);
        }
        if (logicalWidth > 0 && drawableWidth > 0) {
            framebufferScale = static_cast<float>(drawableWidth) /
                               static_cast<float>(logicalWidth);
        }
    }
    AppTheme::applyPendingUiScale();
    if (framebufferScale > 0.0f) AppTheme::refreshFramebufferScale(framebufferScale);
#ifdef MDKR_WEBGPU_BACKEND
    if (usingWebGpu()) {
        gfx_webgpu_imgui_new_frame();
        ImGui_ImplSDL2_NewFrame();
        // On a Metal window imgui_impl_sdl2's SDL_GL_GetDrawableSize returns the
        // LOGICAL size, so the high-DPI scale has to come from the real surface.
        // DisplaySize stays logical so the panel lays out at point sizes.
        ImGuiIO &io = ImGui::GetIO();
        int sw = 0, sh = 0, lw = 0, lh = 0;
        gfx_webgpu_current_overlay_size(&sw, &sh);
        if (g_overlay.window) SDL_GetWindowSize(g_overlay.window, &lw, &lh);
        if (lw > 0 && lh > 0 && sw > 0 && sh > 0) {
            io.DisplaySize = ImVec2((float)lw, (float)lh);
            io.DisplayFramebufferScale = ImVec2((float)sw / (float)lw, (float)sh / (float)lh);
        }
        ImGui::NewFrame();
        return;
    }
#endif
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

bool endImGuiFrame() {
    // The same end-of-frame focus recovery the launcher runs: backing out of a
    // confirmation or the Settings list removes the control focus was on.
    (void)AppNavigation_recoverLostFocus();
    ImGui::Render();
#ifdef MDKR_WEBGPU_BACKEND
    if (usingWebGpu()) {
        void *pass = gfx_webgpu_current_overlay_pass();
        int sw = 0, sh = 0;
        gfx_webgpu_current_overlay_size(&sw, &sh);
        if (pass != nullptr) {
            ImDrawData *drawData = ImGui::GetDrawData();
            const bool rendered =
                gfx_webgpu_imgui_render(drawData, pass, sw, sh);
            if (rendered && drawData != nullptr &&
                drawData->TotalVtxCount > 0 && drawData->TotalIdxCount > 0 &&
                g_overlay.mode == OverlayMode::CharacterStudio &&
                !g_overlay.studioRenderReported &&
                std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
                std::fprintf(
                    stderr,
                    "[app-ui] exact-character-studio overlay-rendered "
                    "vertices=%d indices=%d context=%d\n",
                    drawData->TotalVtxCount, drawData->TotalIdxCount,
                    static_cast<int>(g_overlay.studioContext));
                g_overlay.studioRenderReported = true;
            }
            if (rendered && drawData != nullptr &&
                drawData->TotalVtxCount > 0 && drawData->TotalIdxCount > 0 &&
                g_overlay.testFpsOnly && !g_overlay.open &&
                !g_overlay.testFpsRenderReported) {
                std::fprintf(stderr,
                             "[overlay-test] FPS-only WebGPU pass rendered "
                             "vertices=%d indices=%d\n",
                             drawData->TotalVtxCount, drawData->TotalIdxCount);
                g_overlay.testFpsRenderReported = true;
            }
            return rendered;
        }
        return true;  // no drawable: per-frame overlay state still advanced
    }
#endif
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return true;
}

void drawFpsReadout() {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 12.0f, vp->Pos.y + 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
        const uint64_t now = SDL_GetPerformanceCounter();
        const uint64_t frequency = SDL_GetPerformanceFrequency();
        if (g_overlay.fpsLastCounter == 0) {
            g_overlay.fpsLastCounter = now;
            g_overlay.fpsLastSurfaceFrame = g_surfaceFrameCounter;
        } else if (frequency > 0 && now >= g_overlay.fpsLastCounter + frequency / 2) {
            const double seconds =
                (double)(now - g_overlay.fpsLastCounter) / (double)frequency;
            g_overlay.surfaceFps =
                (double)(g_surfaceFrameCounter - g_overlay.fpsLastSurfaceFrame) /
                seconds;
            g_overlay.fpsLastCounter = now;
            g_overlay.fpsLastSurfaceFrame = g_surfaceFrameCounter;
        }
        ImGui::Text("%.0f visual fps   %.2f ms", g_overlay.surfaceFps,
                    g_overlay.surfaceFps > 0.0
                        ? 1000.0 / g_overlay.surfaceFps : 0.0);
    }
    ImGui::End();
}

void positionOverlayWindow(const ImGuiViewport &viewport, float uiScale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        viewport.Pos,
        ImVec2(viewport.Pos.x + viewport.Size.x, viewport.Pos.y + viewport.Size.y),
        IM_COL32(8, 9, 11, 180));

    // 560 (not mgb64's 440): the footer line carries the resume binding AND the
    // nav hints, which clipped at 440 in the first hands-on capture.
    const bool confirming = g_overlay.confirm != ConfirmAction::None;
    float width = (g_overlay.showSettings ? 720.0f : 560.0f) * uiScale;
    float height = (confirming ? 250.0f : (g_overlay.showSettings ? 560.0f : 300.0f))
                   * uiScale;
    if (width > viewport.Size.x) width = viewport.Size.x;
    if (height > viewport.Size.y) height = viewport.Size.y;
    ImGui::SetNextWindowPos(
        ImVec2(viewport.Pos.x + viewport.Size.x * 0.5f,
               viewport.Pos.y + viewport.Size.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
}

void drawOverlayHeader() {
    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
    ImGui::TextUnformatted(MDKR_BRAND_NAME);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    const bool usingGamepad =
        g_overlay.lastInputDevice == LastInputDevice::Gamepad;
    const bool usingTouch = g_overlay.lastInputDevice == LastInputDevice::Touch;
    // Settings can set the pad toggle to None (-1). Naming a controller
    // button then would tell the player to press one that does nothing, so
    // fall back to the keyboard key, which always works.
    const char *resume = (usingGamepad && Overlay_gamepadToggleButton() >= 0)
                             ? menuButtonName()
                             : menuKeyName();
    const char *nav = usingTouch
        ? "Swipe to scroll  \xE2\x80\xA2  tap an action"
        : usingGamepad
            ? "D-pad move  \xE2\x80\xA2  A select  \xE2\x80\xA2  B back"
            : "arrows move  \xE2\x80\xA2  Enter select  \xE2\x80\xA2  Esc back";
    // Keep the state/action and navigation guidance independently wrappable.
    // At 640px with 2x UI scale the overlay intentionally caps at the viewport;
    // a single non-wrapping status sentence would cut off the controls needed
    // to leave it.
    const bool review = g_overlay.mode == OverlayMode::CharacterReview;
    if (usingTouch) {
        ui::TextSubtleWrapped(
            review
                ? "Character review paused  \xE2\x80\xA2  tap Resume to continue"
                : "Game paused  \xE2\x80\xA2  tap Resume to continue");
    } else {
        ui::TextSubtleWrapped(
            review ? "Character review paused  \xE2\x80\xA2  %s to resume"
                   : "Game paused  \xE2\x80\xA2  %s to resume",
            resume);
    }
    ui::TextSubtleWrapped("%s", nav);
    ui::Gap(ui::kGapS);
    ImGui::Separator();
    ui::Gap(ui::kGapM);
}

struct OverlayButtonPair {
    ImVec2 first;
    ImVec2 second;
    bool sameLine;
};

OverlayButtonPair fitButtonPair(const ImVec2 &first,
                                const ImVec2 &second,
                                float uiScale) {
    const AppUiButtonPairLayout layout = AppUi_fitButtonPair(
        ImGui::GetContentRegionAvail().x, ImGui::GetStyle().ItemSpacing.x,
        first.x, second.x, 140.0f * uiScale);
    return {ImVec2(layout.firstWidth, first.y),
            ImVec2(layout.secondWidth, second.y), layout.sameLine};
}

void drawConfirmation(float uiScale) {
    const bool workshop =
        g_overlay.confirm == ConfirmAction::ReturnToWorkshop;
    const bool returning =
        g_overlay.confirm == ConfirmAction::ReturnToLauncher || workshop;
    const bool restarting = g_overlay.confirm == ConfirmAction::RestartGame;
    const bool stoppingReview =
        returning && !workshop &&
        g_overlay.mode == OverlayMode::CharacterReview;
    const char *description;
    const char *primary;
    if (restarting) {
        description =
            "Restart and apply saved settings? This ends the current race.";
        primary = "Restart & Apply";
    } else if (workshop) {
        description =
            "Return to the Workshop? Current fit changes will be saved.";
        primary = "Return to Workshop";
    } else if (stoppingReview) {
        description =
            "Stop this course review? Its incomplete measurements will be "
            "discarded; completed courses remain current and resumable.";
        primary = "Stop Review";
    } else if (returning) {
        description = "Return to the launcher? This ends the current race.";
        primary = "Return to Launcher";
    } else {
        description = "Quit to desktop? This ends the current race.";
        primary = "Quit";
    }
    ui::TextSubtleWrapped("%s", description);
    ui::Gap(ui::kGapM);
    const OverlayButtonPair actions =
        fitButtonPair(ui::kBtnWide(), ui::kBtnSecondary(), uiScale);
    if (ui::PrimaryButton(primary, actions.first)) {
        if (restarting) restartGame();
        else if (workshop) {
            beginStudioReturn();
        } else if (returning) returnToLauncher();
        else quitToDesktop();
    }
    // The question itself is plain text, which focus never lands on, so the
    // action that answers it carries it as its help.
    ui::SpeakFocusedItem(primary, nullptr, description);
    if (actions.sameLine) ImGui::SameLine();
    if (ImGui::Button("Cancel", actions.second)) {
        g_overlay.confirm = ConfirmAction::None;
    }
    ui::SpeakFocusedItem(
        "Cancel", nullptr,
        workshop ? "Goes back to the fit controls and changes nothing."
                 : "Goes back to the menu and changes nothing.");
}

void drawOverlayMenu(float uiScale) {
    const bool review = g_overlay.mode == OverlayMode::CharacterReview;
    const OverlayButtonPair primaryActions =
        fitButtonPair(ui::kBtnSecondary(), ui::kBtnSecondary(), uiScale);
    // The menu's own buttons speak like every launcher control. Without this a
    // screen-reader player heard the settings rows and nothing else here: not
    // the way back to the race, and not the confirmation that ends it.
    if (ui::PrimaryButton("Resume", primaryActions.first)) setOpen(false);
    ui::SpeakFocusedItem(
        "Resume", nullptr,
        review ? "Closes this menu and carries on with the course review."
               : "Closes this menu and carries on with the race.");
    if (primaryActions.sameLine) ImGui::SameLine();
    // One id for both labels: Dear ImGui derives an id from the label, so a
    // relabelled button is a different control and pressing it would leave
    // focus on the one that just disappeared. With a fixed id, focus stays on
    // the button the player pressed and announces its new label.
    if (ImGui::Button(g_overlay.showSettings
                          ? "Hide Settings###overlay-settings"
                          : "Settings###overlay-settings",
                      primaryActions.second)) {
        if (g_overlay.showSettings) {
            Settings_cancelAudioPreview();
        }
        g_overlay.showSettings = !g_overlay.showSettings;
    }
    ui::SpeakFocusedItem(
        g_overlay.showSettings ? "Hide Settings" : "Settings", nullptr,
        g_overlay.showSettings
            ? "Closes the settings list."
            : "Shows the settings you can change without leaving the game.");

    if (g_overlay.showSettings) {
        ui::Gap(ui::kGapS);
        // NavFlattened only under the scripted walk, for the reason given on
        // AppUi_a11yWalkArmed(): Tab does not leave a child window's focus
        // scope, so without it the walk can never step off the overlay's
        // buttons and onto the settings rows it is there to check.
        ImGui::BeginChild(
            "##ovsettings", ImVec2(0, 340 * uiScale),
            ImGuiChildFlags_Borders |
                (AppUi_a11yWalkArmed() ? ImGuiChildFlags_NavFlattened : 0));
        // Compact: the per-key help paragraphs belong in the launcher, not
        // over a running race. Each edit is already atomic in the config
        // layer, so there is no Apply/Cancel to stage here.
        Settings_draw(g_overlay.window, /*compact=*/true);
        ui::TouchScrollCurrentWindow();
        ImGui::EndChild();
        // In play the list is one stop in the menu's Tab order rather than
        // flattened into it, so the stop itself says what it is and how to
        // get in. (Under the scripted walk it is flattened and not a stop.)
        ui::SpeakFocusedItem(
            "Settings list", nullptr,
            "Press Enter or A to move into the list. Escape or B closes "
            "Settings.");
        if (Settings_restartPending()) {
            ui::Gap(ui::kGapS);
            if (ui::PrimaryButton("Restart & Apply", ui::kBtnWide())) {
                g_overlay.confirm = ConfirmAction::RestartGame;
            }
            ui::SpeakFocusedItem(
                "Restart & Apply", nullptr,
                "Restarts the game so the changes that need it take effect. "
                "Asks first, because it ends the current race.");
        }
    }

    /* No compiled pairing origin means no Phone Party in this build; the
     * in-game surface stays absent to match the launcher (see ui_rom.cpp). */
    if (g_phonePartyHost != nullptr &&
        PhoneParty_availableInBuild(MDKR_PARTY_ORIGIN)) {
        PhoneParty_drawOverlay(*g_phonePartyHost, MDKR_PARTY_ORIGIN);
    }

    ui::Gap(ui::kGapM);
    const OverlayButtonPair exitActions =
        fitButtonPair(ui::kBtnWide(), ui::kBtnWide(), uiScale);
    const char *returnLabel =
        review ? "Stop Review & Return" : "Return to Launcher";
    if (ImGui::Button(returnLabel, exitActions.first)) {
        g_overlay.confirm = ConfirmAction::ReturnToLauncher;
    }
    ui::SpeakFocusedItem(
        returnLabel, nullptr,
        review ? "Asks first, then stops this course review and goes back to "
                 "the launcher."
               : "Asks first, then ends the race and goes back to the "
                 "launcher.");
    if (exitActions.sameLine) ImGui::SameLine();
    if (ImGui::Button("Quit to Desktop", exitActions.second)) {
        g_overlay.confirm = ConfirmAction::QuitToDesktop;
    }
    ui::SpeakFocusedItem(
        "Quit to Desktop", nullptr,
        "Asks first, then ends the race and closes the game.");
}

void drawOverlay() {
    const float uiScale = AppTheme::uiScale();
    const ImGuiViewport &viewport = *ImGui::GetMainViewport();
    positionOverlayWindow(viewport, uiScale);

    ImGui::Begin("##overlay", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    drawOverlayHeader();

    if (g_overlay.justOpened) {
        ImGui::SetKeyboardFocusHere();
        g_overlay.justOpened = false;
    }

    if (g_overlay.confirm != ConfirmAction::None) drawConfirmation(uiScale);
    else drawOverlayMenu(uiScale);

    ImGui::End();
}

void positionCharacterStudioWindow(const ImGuiViewport &viewport,
                                   float uiScale) {
    const float margin = 12.0f * uiScale;
    if (g_overlay.studioControlsCollapsed) {
        ImGui::SetNextWindowPos(
            ImVec2(viewport.Pos.x + viewport.Size.x - margin,
                   viewport.Pos.y + viewport.Size.y - margin),
            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(
            ImVec2(std::min(460.0f * uiScale,
                            viewport.Size.x - margin * 2.0f),
                   std::min(92.0f * uiScale,
                            viewport.Size.y - margin * 2.0f)),
            ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.94f);
        return;
    }
    const bool sidePanel = viewport.Size.x >= 900.0f * uiScale;
    if (sidePanel) {
        const float width = std::min(500.0f * uiScale,
                                     viewport.Size.x * 0.46f);
        ImGui::SetNextWindowPos(
            ImVec2(viewport.Pos.x + viewport.Size.x - margin,
                   viewport.Pos.y + margin),
            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(
            ImVec2(width, viewport.Size.y - margin * 2.0f),
            ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowPos(
            ImVec2(viewport.Pos.x, viewport.Pos.y + viewport.Size.y),
            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(
            ImVec2(viewport.Size.x,
                   std::min(viewport.Size.y,
                            std::max(280.0f * uiScale,
                                     viewport.Size.y * 0.62f))),
            ImGuiCond_Always);
    }
    ImGui::SetNextWindowBgAlpha(0.94f);
}

void drawCharacterStudio() {
    const float uiScale = AppTheme::uiScale();
    const ImGuiViewport &viewport = *ImGui::GetMainViewport();
    positionCharacterStudioWindow(viewport, uiScale);
    ImGui::Begin(
        "##exact-character-offset-studio", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::TextUnformatted("Exact Offset Studio");
    ImGui::PopFont();
    if (g_overlay.studioControlsCollapsed) {
        ImGui::SameLine();
        if (ImGui::Button("Show controls")) {
            g_overlay.studioControlsCollapsed = false;
        }
        ui::SpeakFocusedItem(
            "Show Offset Studio controls", nullptr,
            "Restores the fit controls over the exact game scene.");
        ui::TextSubtleWrapped(
            "Inspecting the unobstructed exact scene. Gameplay input remains locked.");
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    const float hideWidth = ImGui::CalcTextSize("Hide controls").x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(
        ImGui::GetCursorPosX(),
        ImGui::GetWindowContentRegionMax().x - hideWidth));
    if (ImGui::Button("Hide controls")) {
        g_overlay.studioControlsCollapsed = true;
    }
    ui::SpeakFocusedItem(
        "Hide Offset Studio controls", nullptr,
        "Collapses this panel so the exact game scene can be inspected without obstruction. Gameplay input stays locked.");
    ui::TextSubtleWrapped(
        "The scene behind this panel is the game—not a substitute preview. Gameplay input is locked while you fit the character.");
    ImGui::Separator();

    if (g_overlay.justOpened) {
        ImGui::SetKeyboardFocusHere();
        g_overlay.justOpened = false;
    }
    if (g_overlay.confirm != ConfirmAction::None) {
        drawConfirmation(uiScale);
    } else if (g_overlay.studioReturnPending) {
        const bool timedOut =
            SDL_GetTicks64() >
                g_overlay.studioReturnRequestedMillis + 10000u;
        ImGui::TextColored(
            timedOut ? AppTheme::bad() : AppTheme::accent(),
            timedOut ? "The exact renderer did not confirm the saved fit."
                     : "Rendering the saved fit once more…");
        ui::TextSubtleWrapped(
            timedOut
                ? "You can keep editing and recover the scene, or return safely without recording a current measurement. Your saved fit settings are kept."
                : "This short handoff prevents the Workshop from receiving measurements from the frame before your last edit.");
        if (timedOut) {
            if (ImGui::Button("Keep editing", ui::kBtnSecondary())) {
                g_overlay.studioReturnPending = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Return without measurement",
                              ui::kBtnWide())) {
                returnToLauncherWithoutMeasurement();
            }
        }
    } else {
        ImGui::BeginChild(
            "##exact-character-offset-studio-content", ImVec2(0, 0),
            ImGuiChildFlags_None);
        const SettingsCharacterStudioFrame frame =
            Settings_drawCharacterOffsetStudio(
                g_overlay.window, g_overlay.studioPackage.c_str(),
                g_overlay.studioContext,
                !g_overlay.studioEditorInitialized);
        g_overlay.studioEditorInitialized = true;
        if (frame.returnRequested) {
            beginStudioReturn();
        }
        ui::TouchScrollCurrentWindow();
        ImGui::EndChild();
    }
    ImGui::End();
}

bool anyDevToolOpen() {
    for (int id = 0; id < MDKR_TOOL_COUNT; ++id) {
        if (DevTools_isOpen(static_cast<MdkrDevToolId>(id))) return true;
    }
    return false;
}

// Issue #45: the OS cursor is entirely delegated to ImGui's SDL2 backend
// (imgui_impl_sdl2.cpp's UpdateMouseCursor(), called from NewFrame()), which
// only runs on a frame onRender() actually builds -- something ordinary
// racing skips outright, so nothing ever calls SDL_ShowCursor and the pointer
// sits in SDL's default-visible state for the whole race. onRender() is the
// one choke point both render backends funnel through, so it is the one place
// that needs to own SDL_ShowCursor, overriding whatever ImGui decided this
// frame: NewFrame() unconditionally re-shows the cursor whenever it wants any
// shape (the default Arrow included), so this must run after beginImGuiFrame()
// in the frame-built branch or a pre-emptive hide would be immediately undone.
//
// The SDL call itself runs every frame a decision is possible; the stderr
// marker is edge-triggered so a race's worth of steady frames does not spam
// the log -- the same shape as the [overlay-input] capture/release marker in
// platform_sdl_min.c's overlay_capture_sync().
void syncCursorVisibility() {
    const bool wanted = AppUi_cursorVisible(g_overlay.open, anyDevToolOpen());
    static bool visible = true;  // SDL's real default before this ever runs
    static bool announced = false;
    SDL_ShowCursor(wanted ? SDL_ENABLE : SDL_DISABLE);
    if (!announced || wanted != visible) {
        std::fprintf(stderr, "[app-cursor] %s at tick %d\n",
                     wanted ? "shown" : "hidden", g_simTickCounter);
        visible = wanted;
        announced = true;
    }
}

/* C linkage, same reason as the input hooks above. */
extern "C" {

static int onRender(void) {
    ++g_overlay.renderFrame;
    overlayTestFrameTick();

    // Nothing to draw: skip the whole ImGui frame rather than building and
    // discarding one every frame of every race. Tools.Enabled joins this
    // condition rather than "is a tool open", because the hotkey that opens a
    // tool is dispatched from inside DevTools_draw() -- a frame built only once
    // something is open could never see the key that opens it.
    if (!g_overlay.open && !g_overlay.showFps && !DevTools_wantsFrame() && !studioWantsFrame()) {
        // ImGui::NewFrame() is the only consumer of the event queue that
        // onProcessEvent keeps filling, and the only thing that releases a key
        // ImGui still believes is held. Neither runs while the overlay is
        // hidden, so draining here is what keeps two things true:
        //
        //  * a race's worth of keystrokes cannot accumulate and then be
        //    trickled into the menu the moment it opens, navigating and
        //    activating its buttons on the player's behalf; and
        //  * the button that closed the menu -- Enter or a nav A/Cross on
        //    Resume -- cannot stay latched while it is held across the
        //    transition and re-activate whatever the menu shows next time.
        //
        // The overlay always reopens from a neutral input state. Mouse
        // position and gamepad state are re-read from SDL by the backend's
        // next NewFrame(), so nothing durable is lost.
        ImGuiIO &io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        // Nothing ran ImGui's cursor logic this frame, so nothing else will
        // hide the OS cursor either.
        syncCursorVisibility();
        return 1;
    }

    beginImGuiFrame();
    if (g_overlay.showFps) drawFpsReadout();
    if (g_overlay.mode == OverlayMode::CharacterStudio) {
        drawCharacterStudio();
        if (!g_overlay.studioReturnPending &&
            g_overlay.studioTestReturnFrame >= 0 &&
            g_overlay.renderFrame >=
                static_cast<uint64_t>(g_overlay.studioTestReturnFrame)) {
            beginStudioReturn();
        }
        if (g_overlay.studioReturnPending &&
            g_overlay.renderFrame > g_overlay.studioReturnRequestedFrame) {
            MdkrModernCharacterFitDiagnostics diagnostics{};
            const int context = static_cast<int>(g_overlay.studioContext) -
                static_cast<int>(MDKR_CHARACTER_PREVIEW_SELECT);
            if (g_mdkrCharacterPreviewResult != nullptr &&
                g_mdkrCharacterPreviewResult->warmup_complete &&
                context >= 0 && context < MDKR_CHARACTER_CONTEXT_COUNT &&
                mdkr_modern_character_player_fit_diagnostics(
                    0, static_cast<MdkrModernCharacterContext>(context),
                    &diagnostics)) {
                returnToLauncher();
            }
        }
    } else if (g_overlay.open) {
        drawOverlay();
    }
    // After the overlay, so a diagnostic window never draws over the menu the
    // player is operating. Self-gating on Tools.Enabled: this call is a no-op
    // and costs one comparison in every normal session.
    DevTools_draw();
    // After beginImGuiFrame() and DevTools_draw(): ImGui's own cursor logic
    // already ran this frame (possibly re-showing the cursor for the FPS
    // readout or a Tools.Enabled session with nothing open), and a hotkey
    // inside DevTools_draw() may have just opened or closed a tool window.
    // This call is the one that gets the final say.
    syncCursorVisibility();
    return endImGuiFrame() ? 1 : 0;
}

}  // extern "C"

}  // namespace

int Overlay_gamepadToggleButton() {
    return prefInt("menu_toggle_button", SDL_CONTROLLER_BUTTON_BACK);
}

void Overlay_install(SDL_Window *window) {
    g_overlay = OverlayState{};
    g_overlay.window = window;
    if (std::getenv("MDKR_APP_OVERLAY_TEST")) {
        g_overlay.open = true;   // headless render proof
    }
    if (std::getenv("MDKR_APP_FPS_TEST")) {
        g_overlay.open = false;
        g_overlay.showFps = true;
        g_overlay.testFpsOnly = true;
        std::fprintf(stderr,
                     "[overlay-test] FPS-only state wantsRender=%d wantsInput=%d\n",
                     onWantsRender(), onWantsInput());
    }
    static AppOverlayHooks hooks;
    hooks.process_event = onProcessEvent;
    hooks.service       = onService;
    hooks.wants_input   = onWantsInput;
    hooks.wants_pause   = onWantsPause;
    hooks.wants_render  = onWantsRender;
    hooks.render        = onRender;
    platformSetOverlayHooks(&hooks);
}

void Overlay_installCharacterReview(SDL_Window *window) {
    Overlay_install(window);
    g_overlay.mode = OverlayMode::CharacterReview;
    g_overlay.pauseAllowed = true;
}

void Overlay_installCharacterStudio(
    SDL_Window *window, const char *packageId,
    MdkrCharacterPreviewContext context) {
    g_overlay = OverlayState{};
    g_overlay.mode = OverlayMode::CharacterStudio;
    g_overlay.open = true;
    g_overlay.justOpened = true;
    g_overlay.pauseAllowed = false;
    g_overlay.window = window;
    g_overlay.studioPackage = packageId != nullptr ? packageId : "";
    g_overlay.studioContext = context;
    g_overlay.previousRelativeMouse =
        SDL_GetRelativeMouseMode() == SDL_TRUE;
    SDL_SetRelativeMouseMode(SDL_FALSE);
    const char *testToken = std::getenv(
        "MDKR_APP_SMOKE_CHARACTER_STUDIO_TOKEN");
    const char *testReturn = std::getenv(
        "MDKR_APP_SMOKE_CHARACTER_STUDIO_RETURN_FRAME");
    if (testToken != nullptr &&
        std::strcmp(testToken, "mdkr64-character-studio-v1") == 0 &&
        testReturn != nullptr) {
        char *end = nullptr;
        const long parsed = std::strtol(testReturn, &end, 10);
        if (end != testReturn && *end == '\0' && parsed >= 1 &&
            parsed <= 10000) {
            g_overlay.studioTestReturnFrame = parsed;
        }
    }

    static AppOverlayHooks hooks;
    hooks.process_event = onProcessEvent;
    hooks.service       = onService;
    hooks.wants_input   = onWantsInput;
    hooks.wants_pause   = onWantsPause;
    hooks.wants_render  = onWantsRender;
    hooks.render        = onRender;
    platformSetOverlayHooks(&hooks);
}

void Overlay_setPauseAllowed(bool allowed) {
    g_overlay.pauseAllowed = allowed;
}

void Overlay_setPhonePartyHost(MdkrNativePartyHost *host) {
    g_phonePartyHost = host;
}

OverlayExitRequest Overlay_consumeExitRequest() {
    const OverlayExitRequest requested = g_overlay.exitRequest;
    g_overlay.exitRequest = OverlayExitRequest::None;
    return requested;
}
