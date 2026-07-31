// main_app.cpp — entry point for the native mdkr64 app shell.
//
// Owns main(). A bare invocation opens the ImGui launcher; anything with
// arguments delegates verbatim to mdkr64_headless_main() (the engine's original
// main() body, renamed under -DMDKR_APP), so every scripted invocation behaves
// exactly as it did before this shell existed. See arg_triage.h for why the rule
// is deny-by-default rather than mgb64's automation allow-list.
#include "app_brand.h"
#include "app_host.h"
#include "app_theme.h"
#include "arg_triage.h"
#include "diag_log.h"
#include "file_dialog.h"
#include "engine_entry.h"
#include "rom_validate.h"
#include "ui_launcher.h"
#include "ui_overlay.h"

#include "video_config.h"   // mdkr_video_config_init / _schema (settings panel)

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <unistd.h>   // chdir
#endif

namespace {

#if defined(__APPLE__)
// Inside a .app launched from Finder the working directory is "/", so the
// CWD-relative resources the engine looks for (gamecontrollerdb.txt) would not
// resolve and the ROM scan's "current directory" entry would list the root
// filesystem. The retired launcher shim solved this by cd'ing into
// Contents/Resources before exec'ing the engine; do the same thing here so
// retiring the shim loses none of its behavior.
//
// Only fires when argv[0] really is inside a bundle, so a plain CLI build run
// from a terminal keeps the user's actual working directory.
void chdirToBundleResources(const char *argv0) {
    if (argv0 == nullptr) return;
    const char *marker = std::strstr(argv0, ".app/Contents/MacOS/");
    if (marker == nullptr) return;
    std::string resources(argv0, (size_t)(marker - argv0));
    resources += ".app/Contents/Resources";
    if (chdir(resources.c_str()) != 0) {
        std::fprintf(stderr, "[app] could not enter %s; continuing in the current directory\n",
                     resources.c_str());
    }
}
#endif

// Non-interactive schema self-check. Runs fully headless (the config registry
// needs no window and no GPU), so CI can assert the settings panel's data source
// round-trips without opening anything. Mirrors mgb64's MGB64_APP_DUMP_SCHEMA.
int dumpSchema() {
    mdkr_video_config_init(0, nullptr);

    int live = 0, restart = 0;
    std::printf("[app] config schema: %d settings\n", MDKR_VIDEO_KEY_COUNT);
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoSchema *s = mdkr_video_schema((MdkrVideoKey)i);
        if (s == nullptr) {
            std::fprintf(stderr, "[app] schema hole at index %d\n", i);
            return 1;
        }
        const char *cat = mdkr_video_category_name(s->category);
        if (cat == nullptr) {
            std::fprintf(stderr, "[app] %s has no category\n", s->name);
            return 1;
        }
        // Every key the panel renders must round-trip by name, or the panel is
        // showing a control that cannot write back.
        if (mdkr_video_key_from_name(s->name) != (MdkrVideoKey)i) {
            std::fprintf(stderr, "[app] %s does not round-trip by name\n", s->name);
            return 1;
        }
        if (s->scope == MDKR_VIDEO_SCOPE_RESTART) ++restart; else ++live;
        std::printf("[app] %-28s cat=%-12s scope=%s env=%s\n",
                    s->name, cat,
                    s->scope == MDKR_VIDEO_SCOPE_RESTART ? "RESTART" : "LIVE",
                    s->env);
    }
    std::printf("[app] scopes: live=%d restart=%d\n", live, restart);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    // Non-interactive schema self-check, before any window or tee.
    if (std::getenv("MDKR_APP_DUMP_SCHEMA")) {
        return dumpSchema();
    }

    // File-picker self-check: open the native panel once, print what came back,
    // and exit. This exists because the picker is the one acquisition path that
    // cannot be exercised without a real OS dialog — validating it through the
    // launcher means depending on a synthetic click landing on an ImGui button,
    // which is exactly the kind of proof that silently stops proving anything.
    // Prints the chosen path (or "cancelled"), and the ROM verdict for it, so a
    // run either shows a real user-selected file being validated or it does not.
    if (std::getenv("MDKR_APP_FILEDIALOG_SELFTEST")) {
        if (!filedialog::isAvailable()) {
            std::printf("[filedialog] unavailable on this platform\n");
            return 0;
        }
        // Bring up SDL video AND a real window before opening the panel. On
        // macOS the NSApplication is only finished being set up once a window
        // exists; without one the process stays a non-activating background app
        // and runModal returns immediately, so this "test" would report a cancel
        // that never happened. The real launcher always calls the picker with
        // its window already on screen, so creating one here keeps the self-test
        // on the same footing as the product path instead of a weaker one.
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "[filedialog] SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        SDL_Window *w = SDL_CreateWindow(MDKR_BRAND_NAME " — file picker self-test",
                                         SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         640, 200, SDL_WINDOW_SHOWN);
        if (w == nullptr) {
            std::fprintf(stderr, "[filedialog] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return 1;
        }
        SDL_PumpEvents();
        std::string picked;
        if (!filedialog::openRom(picked)) {
            std::printf("[filedialog] cancelled\n");
            return 0;
        }
        std::printf("[filedialog] picked: %s\n", picked.c_str());
        RomInfo info = mdkr_validate_rom(picked.c_str());
        std::printf("[filedialog] valid=%d revision=%s build=%s\n",
                    info.valid, info.revision[0] ? info.revision : "(none)",
                    info.build[0] ? info.build : "(none)");
        std::printf("[filedialog] %s\n", info.message);
        return info.valid ? 0 : 3;
    }

    // Automation/CLI invocations run the unchanged engine path. Deliberately
    // BEFORE the bundle chdir below: a script that passes a relative --rom path
    // must keep resolving it against the directory the script is running in.
    if (mdkr_is_automation_invocation(argc, argv)) {
        return mdkr64_headless_main(argc, argv);
    }

#if defined(__APPLE__)
    chdirToBundleResources(argv[0]);
#endif

    // Tee stdout/stderr into the in-app console + mdkr64.log BEFORE host.init, so
    // its fatal init diagnostics are captured even under the macOS .app bundle
    // and the Windows GUI subsystem, where there is no console to catch them.
    // Interactive path only — automation returned above and is never redirected.
    DiagLog_install();

    AppHost host;
    if (!host.init(MDKR_BRAND_NAME, 1280, 800)) {
        return 1;
    }

    // Resolve the video/gameplay config so the settings panel can enumerate and
    // edit it before a game boots. Idempotent with the engine's own boot-time
    // init, which re-resolves from the same ini this panel writes.
    mdkr_video_config_init(argc, argv);

    Launcher launcher;

    // Boot the engine into the shell's window: adopt the window (and, on WebGPU,
    // the device/surface) so the game renders into the SAME window the launcher
    // used, then run the shared engine boot path. Blocks until the game exits.
    // Returns the engine's status so a failed boot cannot become app exit 0.
    auto play = [&](const MdkrBootConfig &cfg) -> int {
        platformSetHostWindow(host.window(), host.glContext());
        if (host.usingWebGpu()) {
            platformSetHostWebGpu(host.wgpuInstance(), host.wgpuAdapter(),
                                  host.wgpuDevice(), host.wgpuQueue(),
                                  host.wgpuSurface(), host.wgpuFormat());
        }
        Overlay_install(host.window(), argv[0]);   // in-game F1 overlay
        return mdkr64_engine_boot(&cfg);
    };

    // Headless shell smoke (CI + design review): render a fixed number of
    // launcher frames regardless of window events, optionally capturing the last
    // one as a BMP. This is what makes the shell gate honest — it proves pixels
    // reached a framebuffer, not merely that main() returned 0.
    if (const char *smoke = std::getenv("MDKR_APP_SMOKE_FRAMES")) {
        int frames = std::atoi(smoke);
        if (frames < 1) frames = 1;
        const char *shot = std::getenv("MDKR_APP_SMOKE_SHOT");
        bool sawQuit = false;
        bool captureOk = true;
        for (int i = 0; i < frames; ++i) {
            if (host.pumpAndShouldQuit()) sawQuit = true;   // drain, don't exit
            host.beginFrame();
            launcher.draw(host);
            bool ok = host.endFrame((i == frames - 1) ? shot : nullptr);
            if (i == frames - 1) captureOk = ok;
        }
        std::printf("[app] smoke: rendered %d frames, drawable %dx%d, sawQuit=%d\n",
                    frames, host.drawableWidth(), host.drawableHeight(), sawQuit ? 1 : 0);
        host.shutdown();
        // A requested capture that was not written must fail the run, so a CI
        // smoke can never pass without its image.
        if (shot && shot[0] && !captureOk) {
            std::fprintf(stderr, "[app] smoke: requested capture %s was not produced\n", shot);
            return 1;
        }
        return 0;
    }

    // Validation/CI: boot straight into the shell window, proving the
    // launcher -> engine handoff non-interactively.
    if (std::getenv("MDKR_APP_AUTOPLAY")) {
        MdkrBootConfig cfg{};
        cfg.rom_path = std::getenv("MDKR_ROM");
        cfg.video_mode = -1;
        int rc = play(cfg);
        host.shutdown();
        return rc;
    }

    bool running = true;
    int  exitCode = 0;   // a failed interactive Play boot propagates to process exit
    while (running) {
        if (host.pumpAndShouldQuit()) break;
        host.beginFrame();
        LauncherAction action = launcher.draw(host);
        host.endFrame();
        if (action.type == LauncherActionType::Quit) {
            running = false;
        } else if (action.type == LauncherActionType::Play) {
            exitCode = play(action.boot);   // blocks; the game renders into this window
            running = false;
        }
    }

    host.shutdown();
    return exitCode;
}
