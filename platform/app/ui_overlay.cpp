// ui_overlay.cpp — see ui_overlay.h.
//
// DKR ADAPTATIONS, and what each one costs:
//
//  1. PAUSE. mgb64's overlay genuinely freezes the simulation: its engine zeroes
//     g_ClockTimer in lvlManageMpGame when platformOverlayWantsInput() is set.
//     That is a GoldenEye-specific hook in GAME code. mdkr64 has no established
//     pause seam, and adding one would be a separate gameplay-semantic change.
//     So the overlay does NOT pause — it swallows
//     input so the kart gets a neutral pad and coasts instead of steering itself
//     into a wall, and the footer SAYS the race keeps running. Claiming "Paused"
//     over a still-running race would be the dishonest option. Wiring a real
//     pause is tracked as the one remaining parity gap.
//
//  2. FPS READOUT. mgb64 flips its engine's Video.FpsOverlay config key. mdkr64
//     has no such key and no FPS display at all, so F10 toggles a readout this
//     overlay draws itself. Shell-owned, so it needs nothing from the engine.
//
//  3. REBINDING. mgb64 stores the toggle keys in its engine config registry.
//     mdkr64 has none, so they live in the app's own prefs (mdkr64_app.ini)
//     with the same F1 / F10 / gamepad-Back defaults.
#include "ui_overlay.h"
#include "app_brand.h"
#include "app_config.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "engine_entry.h"   // AppOverlayHooks, platformSetOverlayHooks
#include "ui_common.h"
#include "ui_settings.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include "platform_os.h"    // mdkr_render_backend

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

namespace {

enum class LastInputDevice { KeyboardMouse, Gamepad };
enum class ConfirmAction { None, ReturnToLauncher, QuitToDesktop };

// The overlay is installed through C callbacks, so it has process lifetime.
// Keep all of that callback-owned state in one place: this makes its lifecycle
// and transitions visible without scattering independent globals across the
// file. The app still installs exactly one overlay per engine boot.
struct OverlayState {
    bool open = false;
    bool showSettings = false;
    bool showFps = false;
    ConfirmAction confirm = ConfirmAction::None;
    bool justOpened = false;
    SDL_Window *window = nullptr;
    bool launcherReturnRequested = false;
    bool previousRelativeMouse = false;
    LastInputDevice lastInputDevice = LastInputDevice::KeyboardMouse;

    // Scripted render-proof state. Keeping it here (instead of function-local
    // statics) makes every bit of persistent overlay state explicit.
    bool testScheduleLoaded = false;
    long testOpenFrame = -1;
    long testCloseFrame = -1;
    long testFrame = 0;
    bool testFpsOnly = false;
    bool testFpsRenderReported = false;

    uint64_t fpsLastSurfaceFrame = 0;
    uint64_t fpsLastCounter = 0;
    double surfaceFps = 0.0;
};

OverlayState g_overlay;

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
    if (open == g_overlay.open) return;
    g_overlay.open = open;
    g_overlay.confirm = ConfirmAction::None;
    if (g_overlay.open) {
        g_overlay.justOpened = true;   // give pad/keyboard nav an anchor
        g_overlay.previousRelativeMouse = (SDL_GetRelativeMouseMode() == SDL_TRUE);
        SDL_SetRelativeMouseMode(SDL_FALSE);   // free the cursor for the overlay
    } else {
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
    g_overlay.launcherReturnRequested = true;
    quitToDesktop();
}

void navigateBack(OverlayBackInput input, bool keyRepeat) {
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

void onProcessEvent(const void *ev) {
    const SDL_Event *e = (const SDL_Event *)ev;
    ImGui_ImplSDL2_ProcessEvent(e);

    // Track the active device from decisive events only, so the control hints
    // follow what the player is actually holding without flip-flopping on idle
    // stick drift or mouse jitter.
    switch (e->type) {
        case SDL_KEYDOWN:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEWHEEL:
            g_overlay.lastInputDevice = LastInputDevice::KeyboardMouse;
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
        return;
    }
    // FPS readout quick-toggle — deliberately does NOT open the menu.
    if (e->type == SDL_KEYDOWN && !e->key.repeat &&
        e->key.keysym.sym == fpsToggleKey()) {
        g_overlay.showFps = !g_overlay.showFps;
        return;
    }
    if (e->type == SDL_KEYDOWN && g_overlay.open &&
        e->key.keysym.sym == SDLK_ESCAPE) {
        // ImGui owns Escape while a combo or popup is open. On the following
        // frame it closes that popup; only an otherwise-unclaimed Escape walks
        // our confirmation -> Settings -> overlay back-stack.
        navigateBack(OverlayBackInput::Escape, e->key.repeat != 0);
        return;
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        if ((int)e->cbutton.button == Overlay_gamepadToggleButton()) {
            setOpen(!g_overlay.open);
        } else if (g_overlay.open && e->cbutton.button == SDL_CONTROLLER_BUTTON_B) {
            // B = back one level. Skip while ImGui itself is consuming B (an
            // open combo/popup), so its own nav-cancel closes that first.
            navigateBack(OverlayBackInput::ControllerB, false);
        }
    }
}

// The input-swallowing contract: while the overlay is up, the engine's pump
// drops input events instead of feeding them to the pad.
int onWantsInput() { return g_overlay.open ? 1 : 0; }
int onWantsRender() { return (g_overlay.open || g_overlay.showFps) ? 1 : 0; }

// Headless proof hook: scripts the overlay open/close by frame ordinal so a gate
// can render it without a human at the keyboard.
void overlayTestFrameTick() {
    if (!g_overlay.testScheduleLoaded) {
        g_overlay.testScheduleLoaded = true;
        const char *o = std::getenv("MDKR_TEST_OVERLAY_OPEN_FRAME");
        const char *c = std::getenv("MDKR_TEST_OVERLAY_CLOSE_FRAME");
        g_overlay.testOpenFrame = o ? std::strtol(o, nullptr, 10) : -1;
        g_overlay.testCloseFrame = c ? std::strtol(c, nullptr, 10) : -1;
    }
    if (g_overlay.testOpenFrame < 0) {
        ++g_overlay.testFrame;
        return;
    }

    if (g_overlay.testFrame == g_overlay.testOpenFrame) {
        setOpen(true);
        std::fprintf(stderr, "[overlay-test] opened at frame %ld\n", g_overlay.testFrame);
    }
    if (g_overlay.testCloseFrame >= 0 && g_overlay.testFrame == g_overlay.testCloseFrame) {
        setOpen(false);
        std::fprintf(stderr, "[overlay-test] closed at frame %ld\n", g_overlay.testFrame);
    }
    ++g_overlay.testFrame;
}

const char *menuKeyName() {
    const char *n = SDL_GetKeyName((SDL_Keycode)menuToggleKey());
    return (n && n[0]) ? n : "F1";
}

const char *menuButtonName() {
    switch (Overlay_gamepadToggleButton()) {
        case SDL_CONTROLLER_BUTTON_BACK:  return "View";
        case SDL_CONTROLLER_BUTTON_START: return "Start";
        case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide";
        case SDL_CONTROLLER_BUTTON_A:     return "A";
        case SDL_CONTROLLER_BUTTON_B:     return "B";
        case SDL_CONTROLLER_BUTTON_X:     return "X";
        case SDL_CONTROLLER_BUTTON_Y:     return "Y";
        default:                          return "Menu";
    }
}

bool usingWebGpu() {
#ifdef MDKR_WEBGPU_BACKEND
    return mdkr_render_backend() == MDKR_BACKEND_WEBGPU;
#else
    return false;
#endif
}

void beginImGuiFrame() {
    float framebufferScale = 1.0f;
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
    AppTheme::refreshFramebufferScale(framebufferScale);
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

    // Honest footer: the race is NOT paused (see the header note). Say so, and
    // say that the controls are held so the kart coasts rather than steering.
    const bool usingGamepad = g_overlay.lastInputDevice == LastInputDevice::Gamepad;
    const char *resume = usingGamepad ? menuButtonName() : menuKeyName();
    const char *nav = usingGamepad
        ? "D-pad move  \xE2\x80\xA2  A select  \xE2\x80\xA2  B back"
        : "arrows move  \xE2\x80\xA2  Enter select  \xE2\x80\xA2  Esc back";
    ui::TextSubtle("Race keeps running (controls held)  \xE2\x80\xA2  %s to resume  \xE2\x80\xA2  %s",
                   resume, nav);
    ui::Gap(ui::kGapS);
    ImGui::Separator();
    ui::Gap(ui::kGapM);
}

void drawConfirmation() {
    const bool returning = g_overlay.confirm == ConfirmAction::ReturnToLauncher;
    ui::TextSubtle(returning
                       ? "Return to the launcher? This ends the current race."
                       : "Quit to desktop? This ends the current race.");
    ui::Gap(ui::kGapM);
    if (ui::PrimaryButton(returning ? "Return to Launcher" : "Quit", ui::kBtnWide())) {
        if (returning) returnToLauncher();
        else quitToDesktop();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ui::kBtnSecondary())) {
        g_overlay.confirm = ConfirmAction::None;
    }
}

void drawOverlayMenu(float uiScale) {
    if (ui::PrimaryButton("Resume", ui::kBtnSecondary())) setOpen(false);
    ImGui::SameLine();
    if (ImGui::Button(g_overlay.showSettings ? "Hide Settings" : "Settings",
                      ui::kBtnSecondary())) {
        g_overlay.showSettings = !g_overlay.showSettings;
    }

    if (g_overlay.showSettings) {
        ui::Gap(ui::kGapS);
        ImGui::BeginChild("##ovsettings", ImVec2(0, 340 * uiScale), true);
        // Compact: the per-key help paragraphs belong in the launcher, not
        // over a running race. Each edit is already atomic in the config
        // layer, so there is no Apply/Cancel to stage here.
        Settings_draw(/*compact=*/true);
        ImGui::EndChild();
    }

    ui::Gap(ui::kGapM);
    if (ImGui::Button("Return to Launcher", ui::kBtnWide())) {
        g_overlay.confirm = ConfirmAction::ReturnToLauncher;
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit to Desktop", ui::kBtnWide())) {
        g_overlay.confirm = ConfirmAction::QuitToDesktop;
    }
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

    if (g_overlay.confirm != ConfirmAction::None) drawConfirmation();
    else drawOverlayMenu(uiScale);

    ImGui::End();
}

int onRender() {
    overlayTestFrameTick();

    // Nothing to draw: skip the whole ImGui frame rather than building and
    // discarding one every frame of every race.
    if (!g_overlay.open && !g_overlay.showFps) return 1;

    beginImGuiFrame();
    if (g_overlay.showFps) drawFpsReadout();
    if (g_overlay.open) drawOverlay();
    return endImGuiFrame() ? 1 : 0;
}

}  // namespace

int Overlay_gamepadToggleButton() {
    return prefInt("menu_toggle_button", SDL_CONTROLLER_BUTTON_BACK);
}

void Overlay_install(SDL_Window *window) {
    g_overlay.window = window;
    g_overlay.launcherReturnRequested = false;
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
    hooks.wants_input   = onWantsInput;
    hooks.wants_render  = onWantsRender;
    hooks.render        = onRender;
    platformSetOverlayHooks(&hooks);
}

bool Overlay_consumeLauncherReturnRequest() {
    const bool requested = g_overlay.launcherReturnRequested;
    g_overlay.launcherReturnRequested = false;
    return requested;
}
