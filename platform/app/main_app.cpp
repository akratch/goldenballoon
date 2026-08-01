// main_app.cpp — entry point for the native mdkr64 app shell.
//
// Owns main(). A bare invocation opens the ImGui launcher; anything with
// arguments delegates verbatim to mdkr64_headless_main() (the engine's original
// main() body, renamed under -DMDKR_APP), so every scripted invocation behaves
// exactly as it did before this shell existed. See arg_triage.h for why the rule
// is deny-by-default rather than mgb64's automation allow-list.
#include "app_brand.h"
#include "app_config.h"
#include "app_host.h"
#include "app_relaunch.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "app_version.h"
#include "arg_triage.h"
#include "diag_log.h"
#include "file_dialog.h"
#include "engine_entry.h"
#include "rom_validate.h"
#include "ui_launcher.h"
#include "ui_overlay.h"
#include "ui_settings.h"
#include "user_paths.h"

#include "video_config.h"   // mdkr_video_config_init / _schema (settings panel)

#include <SDL.h>

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#if defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#endif

namespace {

class DiagLogScope {
public:
    DiagLogScope() { (void)DiagLog_install(); }
    ~DiagLogScope() { DiagLog_shutdown(); }
    DiagLogScope(const DiagLogScope &) = delete;
    DiagLogScope &operator=(const DiagLogScope &) = delete;
};

bool createSmokeDirectory(const char *path) {
    if (!path || !path[0]) return false;
#if defined(_WIN32)
    const int result = _mkdir(path);
#else
    const int result = mkdir(path, 0700);
#endif
    return result == 0 || errno == EEXIST;
}

int relaunchLauncher(const std::string &executablePath) {
    if (executablePath.empty()) {
        std::fprintf(stderr, "[app] cannot return to launcher: executable path is empty\n");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                 MDKR_BRAND_NAME " — Relaunch Error",
                                 "The game closed safely, but the launcher executable "
                                 "path could not be resolved. Please reopen the app.",
                                 nullptr);
        return 1;
    }

    const int relaunchError = AppRelaunch_replace(executablePath.c_str());
    std::fprintf(stderr, "[app] could not relaunch %s (error %d: %s)\n",
                 executablePath.c_str(), relaunchError,
                 std::strerror(relaunchError));
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             MDKR_BRAND_NAME " — Relaunch Error",
                             "The game closed safely, but the launcher could not be "
                             "reopened. Please start the app again.", nullptr);
    return 1;
}

#if defined(__APPLE__)
std::string applicationExecutablePath(const char *fallback) {
    char     stackPath[PATH_MAX];
    uint32_t size = sizeof(stackPath);
    if (_NSGetExecutablePath(stackPath, &size) == 0) return stackPath;

    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) == 0) return path.data();
    return fallback ? fallback : "";
}

#endif

int runFileDialogSelfTest() {
    if (!filedialog::isAvailable()) {
        std::printf("[filedialog] unavailable on this platform\n");
        return 0;
    }

    /* The real launcher opens the picker with a live window. Reproduce that
     * activation state here so macOS cannot silently turn the check into an
     * immediate background cancellation. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[filedialog] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow(
        MDKR_BRAND_NAME " — file picker self-test",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        640,
        200,
        SDL_WINDOW_SHOWN);
    if (window == nullptr) {
        std::fprintf(stderr, "[filedialog] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_PumpEvents();
    std::string picked;
    if (!filedialog::openRom(picked)) {
        std::printf("[filedialog] cancelled\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    std::printf("[filedialog] picked: %s\n", picked.c_str());
    const RomInfo info = mdkr_validate_rom(picked.c_str());
    std::printf("[filedialog] valid=%d revision=%s build=%s\n",
                info.valid,
                info.revision[0] ? info.revision : "(none)",
                info.build[0] ? info.build : "(none)");
    std::printf("[filedialog] %s\n", info.message);
    const int result = info.valid ? 0 : 3;
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}

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
        if (s->scope == MDKR_VIDEO_SCOPE_RESTART)
            ++restart;
        else
            ++live;
        std::printf("[app] %-28s cat=%-12s scope=%s env=%s\n",
                    s->name,
                    cat,
                    s->scope == MDKR_VIDEO_SCOPE_RESTART ? "RESTART" : "LIVE",
                    s->env);
    }
    std::printf("[app] scopes: live=%d restart=%d\n", live, restart);
    Settings_dumpSchemaContract();
    return 0;
}

int applyAutoplayVideoSetting() {
    const char *pair = std::getenv("MDKR_APP_AUTOPLAY_VIDEO_SET");
    if (pair == nullptr) return 1;

    const char *equals = std::strchr(pair, '=');
    if (equals == nullptr || equals == pair || equals[1] == '\0') {
        std::fprintf(stderr,
                     "[app] invalid MDKR_APP_AUTOPLAY_VIDEO_SET=%s "
                     "(expected Video.Name=value)\n",
                     pair);
        return 0;
    }
    std::string        name(pair, static_cast<size_t>(equals - pair));
    const MdkrVideoKey key = mdkr_video_key_from_name(name.c_str());
    if (key == MDKR_VIDEO_KEY_COUNT) {
        std::fprintf(stderr,
                     "[app] autoplay video setting has an unknown key: %s\n",
                     pair);
        return 0;
    }
    const MdkrVideoRuntimeResult result =
        mdkr_video_config_runtime_set(key, equals + 1);
    if (result != MDKR_VIDEO_RUNTIME_LIVE &&
        result != MDKR_VIDEO_RUNTIME_RESTART) {
        std::fprintf(stderr,
                     "[app] autoplay video setting rejected: %s result=%d\n",
                     pair,
                     static_cast<int>(result));
        return 0;
    }
    std::fprintf(stderr, "[app] autoplay video setting %s: %s\n",
                 result == MDKR_VIDEO_RUNTIME_RESTART ? "staged" : "applied",
                 pair);
    return 1;
}

int recoverAppHostWebGpu(void *userdata, int phase) {
    AppHost *host = static_cast<AppHost *>(userdata);
    return host != nullptr ? host->recoverWebGpuRoots(phase) : 0;
}

int runEngineSession(AppHost &host, const MdkrBootConfig &config, bool *launcherReturnRequested) {
    platformSetHostWindow(host.window(), host.glContext());
    if (host.usingWebGpu()) {
        platformSetHostWebGpu(host.wgpuInstance(), host.wgpuAdapter(),
                              host.wgpuDevice(), host.wgpuQueue(),
                              host.wgpuSurface(), host.wgpuFormat());
        platformSetHostWebGpuRecovery(recoverAppHostWebGpu, &host);
    }
    Overlay_install(host.window());
    const int result = mdkr64_engine_boot(&config);

    /* The engine has dropped every borrowed child. Clear the registries before
     * AppHost releases their roots so no process-lifetime seam keeps a dangling
     * window, device, or overlay callback after this boot. */
    platformSetOverlayHooks(nullptr);
    platformSetHostWebGpuRecovery(nullptr, nullptr);
    platformSetHostWebGpu(nullptr, nullptr, nullptr, nullptr, nullptr, 0);
    platformSetHostWindow(nullptr, nullptr);
    if (launcherReturnRequested != nullptr) {
        *launcherReturnRequested = Overlay_consumeLauncherReturnRequest();
    }

    if (result != 0 && !host.webGpuRecoveryError().empty()) {
        std::fprintf(stderr, "[app] durable WebGPU recovery error: %s\n", host.webGpuRecoveryError().c_str());
        if (std::getenv("MDKR_APP_AUTOPLAY") == nullptr) {
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR,
                MDKR_BRAND_NAME " — Graphics Recovery Error",
                host.webGpuRecoveryError().c_str(),
                host.window());
        }
    }
    return result;
}

int runShellSmoke(AppHost &host, Launcher &launcher, AppUiSmokeInputMode smokeInputMode, const char *smoke) {
    int frames = std::atoi(smoke);
    if (frames < 1) frames = 1;
    const bool smokeUsesGamepad =
        smokeInputMode == AppUiSmokeInputMode::Gamepad;
    if (std::getenv("MDKR_APP_SMOKE_SELECT_FRAME_LIMIT") &&
        frames < (smokeUsesGamepad ? 24 : 18)) {
        frames = smokeUsesGamepad ? 24 : 18;
    }
    const char *shot      = std::getenv("MDKR_APP_SMOKE_SHOT");
    // Drag-and-drop coverage (Q2): the picker's NSOpenPanel and path-field
    // paths run through the same RomPanel_setRom() the C++ unit tests already
    // exercise directly, but the SDL_DROPFILE handler — used by a real
    // Finder/Explorer drag — had no automated coverage. MDKR_APP_SMOKE_DROP=
    // <path> queues exactly that event type inside AppHost partway through
    // the smoke run, so it travels the live handler route
    // (AppHost::pumpAndShouldQuit -> Launcher::draw -> takeDroppedFile ->
    // RomPanel_setRom) instead of calling the panel function directly. It
    // is materialized after SDL's platform translation boundary because
    // reserved platform events cannot be portably round-tripped through
    // SDL_PushEvent (notably through SDL2-on-SDL3 compatibility layers).
    const char *smokeDrop = std::getenv("MDKR_APP_SMOKE_DROP");
    const int   dropFrame = (frames > 1) ? 1 : 0;
    bool        sawQuit   = false;
    bool        captureOk = true;
    bool        renderOk  = true;
    const char *smokeFrameLimit =
        std::getenv("MDKR_APP_SMOKE_SELECT_FRAME_LIMIT");
    const bool expectSaveFailure =
        std::getenv("MDKR_APP_SMOKE_EXPECT_SAVE_FAILURE") != nullptr;
    const char *restoreConfigDirectory =
        std::getenv("MDKR_APP_SMOKE_RESTORE_CONFIG_DIR");
    const bool retryAfterRestore = restoreConfigDirectory &&
                                   restoreConfigDirectory[0] && expectSaveFailure;
    bool       retryScheduled    = false;
    const bool useGamepad        = smokeUsesGamepad;
    if (smokeFrameLimit && std::strcmp(smokeFrameLimit, "240") != 0) {
        std::fprintf(stderr,
                     "[app] smoke: unsupported scripted frame limit %s "
                     "(only 240 is defined)\n",
                     smokeFrameLimit);
        host.shutdown();
        return 2;
    }
    for (int i = 0; i < frames; ++i) {
        if (smokeDrop && smokeDrop[0] && i == dropFrame) {
            host.queueDropFileForSmoke(smokeDrop);
        }
        if (host.pumpAndShouldQuit()) sawQuit = true;
        host.beginFrame();
        launcher.draw(host);
        const bool ok = host.endFrame((i == frames - 1) ? shot : nullptr);
        renderOk      = renderOk && ok;
        if (i == frames - 1) captureOk = ok;
        /* A renderer failure is terminal. Starting another ImGui frame and
         * returning before ImGui::Render leaves dynamic texture state
         * half-transitioned and can make renderer shutdown release an
         * invalid atlas handle. Stop at the first failed presentation. */
        if (!ok) break;

        // Save-failure recovery stays in this process and uses the visible
        // Retry widget. Its appearance proves the rejected enum value was
        // retained; the click still traverses SDL -> ImGui -> commitEdit.
        if (retryAfterRestore && !retryScheduled) {
            int x = 0, y = 0;
            if (Settings_smokeFrameLimitRetryCenter(&x, &y)) {
                if (!createSmokeDirectory(restoreConfigDirectory)) {
                    std::fprintf(
                        stderr,
                        "[app-ui-test] could not restore config directory %s\n",
                        restoreConfigDirectory);
                    renderOk = false;
                } else {
                    host.queueMouseClickForSmoke(x, y);
                    retryScheduled = true;
                    std::fprintf(
                        stderr,
                        "[app-ui-test] same-process Retry save click queued "
                        "at %d,%d\n",
                        x,
                        y);
                }
            }
        }

        // Real widget navigation: frame 0 focuses the actual combo, then
        // these complete SDL key presses open it, move from isolated
        // first-run Original to 240, and activate that selectable.
        if (smokeFrameLimit) {
            if (i == 0 && !useGamepad) {
                int x = 0, y = 0;
                if (Settings_smokeFrameLimitCenter(&x, &y)) {
                    host.queueMouseClickForSmoke(x, y);
                } else {
                    std::fprintf(
                        stderr,
                        "[app-ui-test] Frame limit combo was not rendered\n");
                    renderOk = false;
                }
            } else if (useGamepad && i == 2) {
                renderOk = host.queueGamepadPressForSmoke(
                               SDL_CONTROLLER_BUTTON_DPAD_DOWN) &&
                           renderOk;
            } else if (useGamepad && i == 4) {
                renderOk = host.queueGamepadPressForSmoke(
                               SDL_CONTROLLER_BUTTON_DPAD_UP) &&
                           renderOk;
            } else if (useGamepad && i == 6) {
                renderOk = host.queueGamepadPressForSmoke(
                               SDL_CONTROLLER_BUTTON_A) &&
                           renderOk;
            } else if (i >= (useGamepad ? 8 : 2) &&
                       i <= (useGamepad ? 18 : 12) && (i % 2) == 0) {
                if (useGamepad) {
                    renderOk = host.queueGamepadPressForSmoke(
                                   SDL_CONTROLLER_BUTTON_DPAD_DOWN) &&
                               renderOk;
                } else {
                    host.queueKeyPressForSmoke(SDLK_DOWN);
                }
            } else if (i == (useGamepad ? 20 : 14)) {
                if (useGamepad) {
                    renderOk = host.queueGamepadPressForSmoke(
                                   SDL_CONTROLLER_BUTTON_A) &&
                               renderOk;
                } else {
                    host.queueKeyPressForSmoke(SDLK_RETURN);
                }
            }
        }
    }

    /* Release-candidate verification must prove compositor presentation in
     * addition to the window-independent capture. Give a newly activated
     * macOS app a bounded opportunity to obtain its first CAMetalLayer
     * drawable; ordinary smoke/drop tests omit this opt-in and retain their
     * exact fixed-frame behavior in occluded automation environments. */
    const bool requirePresent =
        std::getenv("MDKR_APP_REQUIRE_PRESENT") != nullptr;
    const std::uint64_t requiredPresents =
        requirePresent ? static_cast<std::uint64_t>(frames) : 0u;
    if (requirePresent && host.presentedFrames() < requiredPresents && renderOk) {
        const Uint64 presentDeadline = SDL_GetTicks64() + 2000u;
        while (host.presentedFrames() < requiredPresents &&
               SDL_GetTicks64() < presentDeadline && renderOk) {
            if (host.pumpAndShouldQuit()) sawQuit = true;
            host.beginFrame();
            launcher.draw(host);
            renderOk = host.endFrame();
            if (host.presentedFrames() < requiredPresents) SDL_Delay(1);
        }
    }
    std::printf("[app] smoke: rendered %d frames, drawable %dx%d, sawQuit=%d\n",
                frames,
                host.drawableWidth(),
                host.drawableHeight(),
                sawQuit ? 1 : 0);
    std::printf("[app] smoke: surface presents=%llu\n",
                (unsigned long long)host.presentedFrames());
    if (smokeDrop && smokeDrop[0]) {
        // Machine-parseable verdict for tests/check_shell_dropfile.py: proves
        // the SAME path/valid pair the picker would have produced for this
        // file reached the launcher's ROM state via the drop event, and that
        // rendering kept going afterward either way (no crash on garbage).
        const LauncherState &state            = launcher.state();
        const bool           showingCandidate = state.romCandidateVisible;
        std::printf("[app] smoke: drop requested=%s got=%s valid=%d\n",
                    smokeDrop,
                    showingCandidate ? state.romCandidatePath
                                     : (state.romPath[0] ? state.romPath : "(none)"),
                    showingCandidate ? state.romCandidateInfo.valid
                                     : state.romInfo.valid);
        std::printf("[app] smoke: drop message=%s\n",
                    showingCandidate
                        ? (state.romCandidateError[0]
                               ? state.romCandidateError
                               : state.romCandidateInfo.message)
                        : (state.romInfo.message[0]
                               ? state.romInfo.message
                               : "(none)"));
        std::printf(
            "[app] smoke: drop transaction active=%s activeValid=%d "
            "candidateVisible=%d cancelAvailable=%d\n",
            state.romPath[0] ? state.romPath : "(none)",
            state.romInfo.valid,
            state.romCandidateVisible ? 1 : 0,
            (state.romInfo.valid && state.romCandidateVisible) ? 1 : 0);
    }
    if (smokeFrameLimit) {
        const MdkrVideoConfig *desiredConfig = mdkr_video_config_desired();
        const char            *actual        = desiredConfig
                                                   ? desiredConfig->values[MDKR_VIDEO_FRAME_LIMIT].text
                                                   : "";
        const bool             selected      = std::strcmp(actual, smokeFrameLimit) == 0;
        std::printf(
            "[app-ui-test] input=%s frame-limit requested=%s actual=%s "
            "saveFailureExpected=%d\n",
            useGamepad ? "gamepad" : "keyboard",
            smokeFrameLimit,
            actual[0] ? actual : "(none)",
            expectSaveFailure ? 1 : 0);
        const bool expectedSelected = !expectSaveFailure || retryAfterRestore;
        if (selected != expectedSelected ||
            (retryAfterRestore && !retryScheduled)) {
            std::fprintf(stderr,
                         "[app-ui-test] scripted frame-limit verdict did not "
                         "match expected persistence outcome\n");
            renderOk = false;
        }
    }
    const bool presentOk = !requirePresent ||
                           host.presentedFrames() >= requiredPresents;
    host.shutdown();
    // A requested capture that was not written must fail the run, so a CI
    // smoke can never pass without its image.
    if (shot && shot[0] && !captureOk) {
        std::fprintf(stderr,
                     "[app] smoke: requested capture %s was not produced\n",
                     shot);
        return 1;
    }
    if (!renderOk) {
        std::fprintf(stderr,
                     "[app] smoke: host renderer entered an unrecoverable state\n");
        return 1;
    }
    if (!presentOk) {
        std::fprintf(stderr,
                     "[app] smoke: required WebGPU surface present did not occur\n");
        return 1;
    }
    return 0;
}

int runAutoplay(AppHost &host, Launcher &launcher, bool *launcherReturnRequested) {
    MdkrBootConfig config{};
    config.rom_path   = std::getenv("MDKR_ROM");
    config.video_mode = -1;
    if (!applyAutoplayVideoSetting()) {
        host.shutdown();
        return 2;
    }
    if (const char *ticks = std::getenv("MDKR_APP_AUTOPLAY_TICKS")) {
        char      *end    = nullptr;
        const long parsed = std::strtol(ticks, &end, 10);
        if (end == ticks || *end != '\0' || parsed < 1 || parsed > 1000000) {
            std::fprintf(stderr,
                         "[app] invalid MDKR_APP_AUTOPLAY_TICKS=%s "
                         "(expected 1..1000000)\n",
                         ticks);
            host.shutdown();
            return 2;
        }
        config.automation_ticks = static_cast<int>(parsed);
    }

    /* Match the interactive handoff: do not let the engine adopt a newly
     * created surface until the host has actually presented it once. A
     * fixed frame delay is compositor-scheduling dependent, so retry with
     * a hard deadline and expose the result to the regression gate. */
    const std::uint64_t initialPresents = host.presentedFrames();
    const Uint64        warmupDeadline  = SDL_GetTicks64() + 2000u;
    int                 warmupAttempts  = 0;
    while (host.presentedFrames() == initialPresents &&
           SDL_GetTicks64() < warmupDeadline) {
        if (host.pumpAndShouldQuit()) {
            std::fprintf(stderr, "[app] autoplay: quit during surface warm-up\n");
            host.shutdown();
            return 1;
        }
        host.beginFrame();
        launcher.draw(host);
        if (!host.endFrame()) {
            std::fprintf(stderr,
                         "[app] autoplay: host renderer failed during surface "
                         "warm-up\n");
            host.shutdown();
            return 1;
        }
        ++warmupAttempts;
        if (host.presentedFrames() == initialPresents) SDL_Delay(1);
    }
    if (host.presentedFrames() == initialPresents &&
        !host.lastSurfaceWasOccluded()) {
        std::fprintf(stderr,
                     "[app] autoplay: host surface did not present within 2000 ms\n");
        host.shutdown();
        return 1;
    }
    if (host.presentedFrames() != initialPresents) {
        std::fprintf(stderr,
                     "[app] autoplay: host surface ready presents=%llu attempts=%d\n",
                     (unsigned long long)(host.presentedFrames() - initialPresents),
                     warmupAttempts);
    } else {
        /* Native automation can run behind the invoking terminal and wgpu
         * reports that state precisely as Occluded. The engine's qualified
         * offscreen scene path is independent of a drawable, so continue
         * only for that explicit status; all other acquisition failures
         * remain fatal above. The presentation counter is intentionally
         * still zero rather than claiming a compositor present occurred. */
        std::fprintf(stderr,
                     "[app] autoplay: host surface occluded attempts=%d; "
                     "continuing with offscreen engine frames\n",
                     warmupAttempts);
    }
    const int result =
        runEngineSession(host, config, launcherReturnRequested);
    host.shutdown();
    return result;
}

int runInteractiveLauncher(AppHost &host, Launcher &launcher, bool *launcherReturnRequested) {
    bool running  = true;
    int  exitCode = 0;
    while (running) {
        const bool drawableAvailable =
            host.drawableWidth() > 0 && host.drawableHeight() > 0;
        const AppUiIdleDecision idle = AppUi_idleDecision(
            drawableAvailable,
            host.lastSurfaceWasOccluded());
        if (idle.waitMilliseconds > 0) {
            // Occluded WebGPU surfaces are retried at a bounded 40 Hz. A truly
            // minimized zero-drawable window skips ImGui construction entirely
            // until restore, while close/quit remains event-driven and prompt.
            if (host.waitAndPump(static_cast<int>(idle.waitMilliseconds))) break;
            if (!idle.buildFrame) continue;
        } else if (host.pumpAndShouldQuit()) {
            break;
        }
        host.beginFrame();
        const LauncherAction action = launcher.draw(host);
        if (!host.endFrame()) {
            char message[512];
            std::snprintf(
                message,
                sizeof(message),
                "The %s presentation path failed. The app stopped%s.\n\n"
                "See mdkr64.log for details.",
                host.usingWebGpu() ? "WebGPU" : "OpenGL",
                host.usingWebGpu()
                    ? " instead of switching to the diagnostic OpenGL backend"
                    : "");
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR,
                MDKR_BRAND_NAME " — Graphics Error",
                message,
                host.window());
            exitCode = 1;
            running  = false;
            continue;
        }
        if (action.type == LauncherActionType::Quit) {
            running = false;
        } else if (action.type == LauncherActionType::Play) {
            // Blocks while the game renders into the launcher's host window.
            exitCode =
                runEngineSession(host, action.boot, launcherReturnRequested);
            running = false;
        }
    }
    return exitCode;
}

} // namespace

int main(int argc, char **argv) {
    /* Exact informational invocations neither consume nor create user data.
     * In particular, release verification runs the executable from a checkout
     * that can contain legacy mdkr64.ini/save files: initializing packaged
     * paths here used to migrate those files merely to print --version. */
    if (mdkr_is_side_effect_free_info_invocation(argc, argv)) {
        if (std::strcmp(argv[1], "--version") == 0) {
            std::printf("mdkr64 %s\n", AppVersion());
            return 0;
        }
        return mdkr64_headless_main(argc, argv);
    }

    std::string executablePath = argv[0] ? argv[0] : "";
#if defined(__APPLE__)
    executablePath = applicationExecutablePath(argv[0]);
#endif
    /* Register a real bundle before any config/save/resource access, including
     * automation launched through Contents/MacOS. This does not change CWD:
     * relative ROM/script paths remain owned by their caller, while immutable
     * Resources and mutable per-user state resolve through separate policies. */
    if (mdkr_user_paths_init(executablePath.c_str()) < 0) {
        const char *message =
            "Golden Balloon could not open its per-user data directory. "
            "The signed app bundle was left untouched.";
        std::fprintf(stderr, "[app] %s\n", message);
        if (!mdkr_is_automation_invocation(argc, argv)) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                     MDKR_BRAND_NAME " — Data Directory Error",
                                     message, nullptr);
        }
        return 1;
    }

    // Non-interactive schema self-check, before any window or tee.
    if (std::getenv("MDKR_APP_DUMP_SCHEMA")) {
        return dumpSchema();
    }

    // Exercise the native picker through the same live-window activation state
    // as the launcher, then print its selection and ROM verdict.
    if (std::getenv("MDKR_APP_FILEDIALOG_SELFTEST")) {
        return runFileDialogSelfTest();
    }

    // Automation/CLI invocations run the unchanged engine path. Deliberately
    // BEFORE the bundle chdir below: a script that passes a relative --rom path
    // must keep resolving it against the directory the script is running in.
    if (mdkr_is_automation_invocation(argc, argv)) {
        return mdkr64_headless_main(argc, argv);
    }

    const AppUiSmokeInputMode smokeInputMode = AppUi_smokeInputMode();
    if (smokeInputMode == AppUiSmokeInputMode::Invalid) {
        std::fprintf(
            stderr,
            "[app-ui-test] rejected partial or stale synthetic-input contract\n");
        return 2;
    }

    // Tee stdout/stderr into the in-app console + mdkr64.log BEFORE host.init, so
    // its fatal init diagnostics are captured even under the macOS .app bundle
    // and the Windows GUI subsystem, where there is no console to catch them.
    // Interactive path only — automation returned above and is never redirected.
    DiagLogScope diagnosticLog;

    AppHost host;
    if (!host.init(MDKR_BRAND_NAME, 1280, 800)) {
        if (std::getenv("MDKR_APP_AUTOPLAY") == nullptr &&
            std::getenv("MDKR_APP_SMOKE_FRAMES") == nullptr) {
            char message[2048];
            std::snprintf(
                message, sizeof(message),
                "%s could not start the %s graphics backend.%s"
                "\n\nDiagnostic log:\n%s",
                MDKR_BRAND_NAME, host.usingWebGpu() ? "WebGPU" : "OpenGL",
                host.usingWebGpu()
                    ? " The app did not switch silently to OpenGL because that "
                      "diagnostic backend does not yet have visual parity."
                    : "",
                DiagLog_path()[0] ? DiagLog_path() : "(log unavailable)");
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                     MDKR_BRAND_NAME " — Graphics Error",
                                     message, host.window());
        }
        host.shutdown();
        return 1;
    }

    AppConfig::load();
    Settings_loadUiScalePreference();

    if (const char *windowSize = std::getenv("MDKR_APP_SMOKE_WINDOW_SIZE")) {
        int width = 0, height = 0;
        char trailing = '\0';
        if (std::sscanf(windowSize, "%dx%d%c", &width, &height, &trailing) != 2 ||
            width < 640 || height < 480 || width > 7680 || height > 4320) {
            std::fprintf(stderr,
                         "[app] invalid MDKR_APP_SMOKE_WINDOW_SIZE=%s "
                         "(expected 640x480..7680x4320)\n", windowSize);
            host.shutdown();
            return 2;
        }
        SDL_SetWindowSize(host.window(), width, height);
        std::fprintf(stderr, "[app-ui-test] window size=%dx%d\n", width, height);
    }

    // Resolve the video/gameplay config so the settings panel can enumerate and
    // edit it before a game boots. Idempotent with the engine's own boot-time
    // init, which re-resolves from the same ini this panel writes.
    mdkr_video_config_init(argc, argv);

    Launcher launcher;

    bool launcherReturnRequested = false;

    // Headless shell smoke (CI + design review): render a bounded launcher
    // sequence and optionally capture the last frame.
    if (const char *smoke = std::getenv("MDKR_APP_SMOKE_FRAMES")) {
        return runShellSmoke(host, launcher, smokeInputMode, smoke);
    }

    // Validation/CI: boot straight into the shell window, proving the
    // launcher -> engine handoff non-interactively.
    if (std::getenv("MDKR_APP_AUTOPLAY")) {
        return runAutoplay(host, launcher, &launcherReturnRequested);
    }
    const int exitCode =
        runInteractiveLauncher(host, launcher, &launcherReturnRequested);
    host.shutdown();
    if (launcherReturnRequested) {
        // End the tee before exec: fd 1/2 are restored, its pipe reader is
        // joined, and no private descriptor can leak into the replacement.
        DiagLog_shutdown();
        return relaunchLauncher(executablePath);
    }
    return exitCode;
}
