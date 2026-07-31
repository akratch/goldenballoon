// ui_overlay.cpp — see ui_overlay.h.
//
// DKR ADAPTATIONS, and what each one costs:
//
//  1. PAUSE. mgb64's overlay genuinely freezes the simulation: its engine zeroes
//     g_ClockTimer in lvlManageMpGame when platformOverlayWantsInput() is set.
//     That is a GoldenEye-specific hook in GAME code. mdkr64 has no equivalent
//     seam and inventing one would mean editing game/src, which this work is
//     explicitly meant to avoid. So the overlay does NOT pause — it swallows
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

#if !defined(_WIN32)
#include <unistd.h>   // execvp
#endif

namespace {

bool        g_open = false;
bool        g_showSettings = false;
bool        g_showFps = false;
int         g_confirm = 0;          // 0 none, 1 return-to-launcher, 2 quit
bool        g_justOpened = false;
SDL_Window *g_window = nullptr;
char        g_argv0[1024] = {0};
bool        g_prevRelMouse = false;

enum LastInputDevice { DEV_KBM, DEV_PAD };
LastInputDevice g_lastInputDevice = DEV_KBM;

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
    if (open == g_open) return;
    g_open = open;
    g_confirm = 0;
    if (g_open) {
        g_justOpened = true;   // give pad/keyboard nav an anchor
        g_prevRelMouse = (SDL_GetRelativeMouseMode() == SDL_TRUE);
        SDL_SetRelativeMouseMode(SDL_FALSE);   // free the cursor for the overlay
    } else {
        g_showSettings = false;
        SDL_SetRelativeMouseMode(g_prevRelMouse ? SDL_TRUE : SDL_FALSE);
    }
}

void quitToDesktop() {
    SDL_Event q{};        // zero-init: never queue uninitialized bytes
    q.type = SDL_QUIT;
    SDL_PushEvent(&q);
}

void returnToLauncher() {
#if !defined(_WIN32)
    if (g_argv0[0]) {
        char *argv[] = {g_argv0, nullptr};
        execvp(g_argv0, argv);   // replaces the process image -> a fresh launcher
    }
#endif
    quitToDesktop();   // fallback, and the Windows path
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
            g_lastInputDevice = DEV_KBM;
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_JOYBUTTONDOWN:
            g_lastInputDevice = DEV_PAD;
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (e->caxis.value > 8000 || e->caxis.value < -8000) g_lastInputDevice = DEV_PAD;
            break;
        default:
            break;
    }

    if (e->type == SDL_KEYDOWN && !e->key.repeat &&
        e->key.keysym.sym == menuToggleKey()) {
        setOpen(!g_open);
        return;
    }
    // FPS readout quick-toggle — deliberately does NOT open the menu.
    if (e->type == SDL_KEYDOWN && !e->key.repeat &&
        e->key.keysym.sym == fpsToggleKey()) {
        g_showFps = !g_showFps;
        return;
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        if ((int)e->cbutton.button == Overlay_gamepadToggleButton()) {
            setOpen(!g_open);
        } else if (g_open && e->cbutton.button == SDL_CONTROLLER_BUTTON_B) {
            // B = back one level. Skip while ImGui itself is consuming B (an
            // open combo/popup), so its own nav-cancel closes that first.
            if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                if (g_confirm) g_confirm = 0;
                else if (g_showSettings) g_showSettings = false;
                else setOpen(false);
            }
        }
    }
}

// The input-swallowing contract: while the overlay is up, the engine's pump
// drops input events instead of feeding them to the pad.
int onWantsInput() { return g_open ? 1 : 0; }

// Headless proof hook: scripts the overlay open/close by frame ordinal so a gate
// can render it without a human at the keyboard.
void overlayTestFrameTick() {
    static long s_openFrame = -2;   // -2 = env not yet read
    static long s_closeFrame = -2;
    static long s_frame = 0;

    if (s_openFrame == -2) {
        const char *o = std::getenv("MDKR_TEST_OVERLAY_OPEN_FRAME");
        const char *c = std::getenv("MDKR_TEST_OVERLAY_CLOSE_FRAME");
        s_openFrame  = o ? std::strtol(o, nullptr, 10) : -1;
        s_closeFrame = c ? std::strtol(c, nullptr, 10) : -1;
    }
    if (s_openFrame < 0) { ++s_frame; return; }

    if (s_frame == s_openFrame) {
        setOpen(true);
        std::fprintf(stderr, "[overlay-test] opened at frame %ld\n", s_frame);
    }
    if (s_closeFrame >= 0 && s_frame == s_closeFrame) {
        setOpen(false);
        std::fprintf(stderr, "[overlay-test] closed at frame %ld\n", s_frame);
    }
    ++s_frame;
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
        if (g_window) SDL_GetWindowSize(g_window, &lw, &lh);
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

void endImGuiFrame() {
    ImGui::Render();
#ifdef MDKR_WEBGPU_BACKEND
    if (usingWebGpu()) {
        void *pass = gfx_webgpu_current_overlay_pass();
        int sw = 0, sh = 0;
        gfx_webgpu_current_overlay_size(&sw, &sh);
        if (pass != nullptr) {
            gfx_webgpu_imgui_render(ImGui::GetDrawData(), pass, sw, sh);
        }
        return;
    }
#endif
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
        const ImGuiIO &io = ImGui::GetIO();
        ImGui::Text("%.0f fps   %.2f ms", (double)io.Framerate,
                    io.Framerate > 0.0f ? 1000.0 / (double)io.Framerate : 0.0);
    }
    ImGui::End();
}

void onRender() {
    overlayTestFrameTick();

    // Nothing to draw: skip the whole ImGui frame rather than building and
    // discarding one every frame of every race.
    if (!g_open && !g_showFps) return;

    beginImGuiFrame();

    if (g_showFps) drawFpsReadout();

    if (!g_open) {
        endImGuiFrame();
        return;
    }

    const float uiS = AppTheme::uiScale();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
        IM_COL32(8, 9, 11, 180));

    // 560 (not mgb64's 440): the footer line carries the resume binding AND the
    // nav hints, which clipped at 440 in the first hands-on capture.
    float w = (g_showSettings ? 720.0f : 560.0f) * uiS;
    float h = (g_confirm ? 250.0f : (g_showSettings ? 560.0f : 300.0f)) * uiS;
    if (w > vp->Size.x) w = vp->Size.x;
    if (h > vp->Size.y) h = vp->Size.y;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::Begin("##overlay", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
    ImGui::TextUnformatted(MDKR_BRAND_NAME);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // Honest footer: the race is NOT paused (see the header note). Say so, and
    // say that the controls are held so the kart coasts rather than steering.
    const char *resume = (g_lastInputDevice == DEV_PAD) ? menuButtonName() : menuKeyName();
    const char *nav = (g_lastInputDevice == DEV_PAD)
        ? "D-pad move  \xE2\x80\xA2  A select  \xE2\x80\xA2  B back"
        : "arrows move  \xE2\x80\xA2  Enter select  \xE2\x80\xA2  Esc back";
    ui::TextSubtle("Race keeps running (controls held)  \xE2\x80\xA2  %s to resume  \xE2\x80\xA2  %s",
                   resume, nav);
    ui::Gap(ui::kGapS);
    ImGui::Separator();
    ui::Gap(ui::kGapM);

    if (g_justOpened) {
        ImGui::SetKeyboardFocusHere();
        g_justOpened = false;
    }

    if (g_confirm) {
        ui::TextSubtle(g_confirm == 1
                           ? "Return to the launcher? This ends the current race."
                           : "Quit to desktop? This ends the current race.");
        ui::Gap(ui::kGapM);
        if (ui::PrimaryButton(g_confirm == 1 ? "Return to Launcher" : "Quit",
                              ui::kBtnWide())) {
            if (g_confirm == 1) returnToLauncher();
            else quitToDesktop();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ui::kBtnSecondary())) g_confirm = 0;
    } else {
        if (ui::PrimaryButton("Resume", ui::kBtnSecondary())) setOpen(false);
        ImGui::SameLine();
        if (ImGui::Button(g_showSettings ? "Hide Settings" : "Settings",
                          ui::kBtnSecondary())) {
            g_showSettings = !g_showSettings;
        }

        if (g_showSettings) {
            ui::Gap(ui::kGapS);
            ImGui::BeginChild("##ovsettings", ImVec2(0, 340 * uiS), true);
            // Compact: the per-key help paragraphs belong in the launcher, not
            // over a running race. Each edit is already atomic in the config
            // layer, so there is no Apply/Cancel to stage here.
            Settings_draw(/*compact=*/true);
            ImGui::EndChild();
        }

        ui::Gap(ui::kGapM);
        // "Return to Launcher" re-execs the process, which is not wired on
        // Windows; hide it there rather than mislabel a silent quit.
#if !defined(_WIN32)
        if (ImGui::Button("Return to Launcher", ui::kBtnWide())) g_confirm = 1;
        ImGui::SameLine();
#endif
        if (ImGui::Button("Quit to Desktop", ui::kBtnWide())) g_confirm = 2;
    }

    ImGui::End();
    endImGuiFrame();
}

}  // namespace

int Overlay_gamepadToggleButton() {
    return prefInt("menu_toggle_button", SDL_CONTROLLER_BUTTON_BACK);
}

void Overlay_install(SDL_Window *window, const char *argv0) {
    g_window = window;
    if (argv0) std::snprintf(g_argv0, sizeof(g_argv0), "%s", argv0);
    if (std::getenv("MDKR_APP_OVERLAY_TEST")) g_open = true;   // headless render proof
    static AppOverlayHooks hooks;
    hooks.process_event = onProcessEvent;
    hooks.wants_input   = onWantsInput;
    hooks.render        = onRender;
    platformSetOverlayHooks(&hooks);
}
