// main_app.cpp — entry point for the native mdkr64 app shell.
//
// Owns main(). A bare invocation opens the ImGui launcher; anything with
// arguments delegates verbatim to mdkr64_headless_main() (the engine's original
// main() body, renamed under -DMDKR_APP), so every scripted invocation behaves
// exactly as it did before this shell existed. See arg_triage.h for why the rule
// is deny-by-default rather than mgb64's automation allow-list.
#include "app_brand.h"
#include "app_activation.h"
#include "app_config.h"
#include "app_host.h"
#include "app_launch_hold.h"
#include "app_relaunch.h"
#include "app_restart.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "app_version.h"
#include "arg_triage.h"
#include "crash_screen.h"
#include "dev_tools.h"
#include "diag_log.h"
#include "file_dialog.h"
#include "fs_utf8.h"
#include "engine_entry.h"
#include "rom_validate.h"
#include "session_runtime.h"
#include "net/match_input_runtime.h"
#include "net/match_input_bundle.h"
#include "net/match_input_packet.h"
#include "net/net_clock.h"
#include "net/net_impairment.h"
#include "net/net_roster_runtime.h"
#include "online/lobby_view_model.h"
#if MDKR_ENABLE_ONLINE_BETA
#include "online/match_live_adapter.h"  // visible-engine race-boot handoff
#include "net/online_race_results.h"    // finished-race placements poll (one-shot)
#include "net/party_link.h"             // single-endpoint note (launcher)
#endif
#include "platform_os.h"
#include "present_sched.h"
#include "ui_launcher.h"
#include "ui_online_room.h"
#include "ui_overlay.h"
#include "ui_settings.h"
#include "user_paths.h"

#include "imgui.h"          // the accessibility Escape walk's key observation
#include "video_config.h"   // mdkr_video_config_init / _schema (settings panel)

#include <SDL.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#if MDKR_ENABLE_ONLINE_BETA
/* Re-arm probe condition toggle: defined in online_live_wiring.cpp. Declared
 * locally (not in match_live_adapter.h) to keep this seam scoped to the
 * launcher/wiring TUs + tests. Beta-only, exactly like the match_live_adapter.h
 * include above (IMdkrOnlineAdapter is beta-gated, so this block must be too or the
 * OFF build cannot name the type). Drive the loopback room OUT of the room-ready
 * takeover window (park a finished race in RESULTS => condition false) and back
 * (leader REMATCH => SELECTING => condition true) -- the mode-independent, RESULTS-
 * park-faithful replacement for the old mode-flip toggle, which stopped changing
 * OnlineRoom_roomReadyConditionHolds once single race also takes the native path. */
bool OnlineRoom_testParkRoomInResults(IMdkrOnlineAdapter *leader,
                                      IMdkrOnlineAdapter *peer);
bool OnlineRoom_testReturnRoomToSelecting(IMdkrOnlineAdapter *leader,
                                          IMdkrOnlineAdapter *peer);
#endif /* MDKR_ENABLE_ONLINE_BETA */

namespace {

class DiagLogScope {
public:
    DiagLogScope() { (void)DiagLog_install(); }
    ~DiagLogScope() { DiagLog_shutdown(); }
    DiagLogScope(const DiagLogScope &) = delete;
    DiagLogScope &operator=(const DiagLogScope &) = delete;
};

void shutdownLauncherHost(AppHost &host, Launcher &launcher) {
    // Only post-init paths call this helper. Restart can reach it both inside
    // its session runner and again in main; never service work after SDL quit.
    if (host.window() == nullptr) return;
    // Every post-construction terminal path uses this order, including paths
    // that never enter the interactive loop. The launcher joins all network
    // owners and global RTC cleanup while host services and logging still live.
    launcher.finishCharacterWorkForExit();
    Overlay_setPhonePartyHost(nullptr);
    const bool networkClean = launcher.finishNetworkShutdownForExit();
    if (!networkClean) {
        std::fprintf(stderr,
                     "[app] network shutdown failed; exiting with failure and refusing relaunch\n");
    }
    AppLaunchHold_release();
    host.shutdown();
}

/* RAII latch for the epoch-scoped NTSC source identity (rom_io.c).  Every
 * online engine boot wraps its blocking mdkr64_engine_boot() in one of these:
 * the constructor arms the override iff the epoch is online, and the destructor
 * clears it on EVERY exit from the scope.  Using the guard instead of a bare
 * arm/clear pair means a future early return inserted between the boot and the
 * clear can never leak an NTSC latch into the process's next offline epoch. */
class OnlineNtscIdentityScope {
public:
    explicit OnlineNtscIdentityScope(bool online) : armed_(online) {
        if (armed_) platform_source_set_ntsc_identity_override(1);
    }
    ~OnlineNtscIdentityScope() {
        if (armed_) platform_source_set_ntsc_identity_override(0);
    }
    OnlineNtscIdentityScope(const OnlineNtscIdentityScope &) = delete;
    OnlineNtscIdentityScope &operator=(const OnlineNtscIdentityScope &) = delete;

private:
    bool armed_;
};

bool createSmokeDirectory(const char *path) {
    if (!path || !path[0]) return false;
    const int result = mdkr_mkdir_utf8(path);
    return result == 0 || errno == EEXIST;
}

/* The final-Play smoke must prove that the production recheck notices a ROM
 * changed after selection.  This hook is deliberately opt-in, operates only
 * on the smoke's explicitly supplied writable copy, and flips one body byte
 * after the initial async validation has settled. */
bool mutateSmokeRomForFinalCheck(const char *path) {
    if (!path || !path[0]) return false;
    FILE *file = mdkr_fopen_utf8(path, "rb+");
    if (!file) return false;
    const long offset = static_cast<long>(DKR_ROM_SIZE_BYTES / 2u);
    bool ok = std::fseek(file, offset, SEEK_SET) == 0;
    const int original = ok ? std::fgetc(file) : EOF;
    ok = ok && original != EOF && std::fseek(file, offset, SEEK_SET) == 0;
    if (ok) ok = std::fputc(original ^ 0x01, file) != EOF;
    if (ok) ok = std::fflush(file) == 0;
    if (std::fclose(file) != 0) ok = false;
    return ok;
}

int relaunchApplication(const std::string &executablePath) {
    if (executablePath.empty()) {
        std::fprintf(stderr, "[app] cannot restart application: executable path is empty\n");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                 MDKR_BRAND_NAME " — Relaunch Error",
                                 "The game closed safely, but the application executable "
                                 "path could not be resolved. Please reopen the app.",
                                 nullptr);
        return 1;
    }

    const int relaunchError = AppRelaunch_replace(executablePath.c_str());
    std::fprintf(stderr, "[app] could not restart %s (error %d: %s)\n",
                 executablePath.c_str(), relaunchError,
                 std::strerror(relaunchError));
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             MDKR_BRAND_NAME " — Relaunch Error",
                             "The game closed safely, but the application could not be "
                             "reopened. Please start the app again.", nullptr);
    return 1;
}

/* One-shot handoffs all travel the app_restart environment seam: on Windows a
 * recovery message can name a non-ASCII ROM path, which only the wide
 * environment can carry losslessly. */
void setBootRecoveryEnvironment(const std::string &message) {
    (void)AppRestart_setEnv("MDKR_APP_BOOT_RECOVERY", message.c_str());
}

void clearBootRecoveryEnvironment() {
    (void)AppRestart_setEnv("MDKR_APP_BOOT_RECOVERY", "");
}

void prepareRestartRecoverySmokeForTest() {
    /* The restart integration gate must observe the normal launcher recovery
     * process and then finish without synthetic input. This opt-in is ignored
     * until a recovery transition has actually been selected. */
    std::string frames;
    if (!AppRestart_getEnv("MDKR_APP_TEST_RESTART_RECOVERY_FRAMES", frames) ||
        frames.empty()) {
        return;
    }
    (void)AppRestart_setEnv("MDKR_APP_SMOKE_FRAMES", frames.c_str());
}

int recoverRestartToLauncher(const std::string &executablePath,
                             const char *message) {
    /* A failed replacement must never inherit either one-shot control. In
     * particular, MDKR_APP_AUTOPLAY would turn the recovery replacement into
     * another failing boot rather than the interactive launcher. */
    AppRestart_clear();
    setBootRecoveryEnvironment(message);
    prepareRestartRecoverySmokeForTest();
    std::fprintf(stderr, "[app] Restart & Apply recovery: %s\n", message);
    DiagLog_shutdown();
    return relaunchApplication(executablePath);
}

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

    int live = 0, level = 0, restart = 0;
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
        const char *scope = "LIVE";
        if (s->scope == MDKR_VIDEO_SCOPE_RESTART) {
            scope = "RESTART";
            ++restart;
        } else if (s->scope == MDKR_VIDEO_SCOPE_LEVEL) {
            scope = "LEVEL";
            ++level;
        } else {
            ++live;
        }
        std::printf("[app] %-28s cat=%-12s scope=%s env=%s\n",
                    s->name, cat, scope, s->env);
    }
    std::printf("[app] scopes: live=%d level=%d restart=%d\n",
                live, level, restart);
    Settings_dumpSchemaContract();
    return 0;
}

// Non-interactive developer-tool registry dump, the same shape and the same
// pre-window position as the schema self-check above. This is how
// tests/check_dev_tools_purity.py learns which tools exist: the gate does not
// carry its own list, it enumerates the table, so a tool added later is born
// covered instead of waiting for someone to extend the test.
//
// Deliberately touches no config, no ROM and no GPU. An argument rather than an
// environment variable because it selects a whole mode of the process, and the
// interception has to sit above mdkr_is_automation_invocation() -- which classes
// any argument at all as "run the engine" -- for the flag to be seen.
bool argvRequestsToolTableDump(int argc, char **argv) {
    if (argv == nullptr) return false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr &&
            std::strcmp(argv[i], "--dump-tool-table") == 0) {
            return true;
        }
    }
    return false;
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
    if (!mdkr_video_runtime_result_applied(result)) {
        std::fprintf(stderr,
                     "[app] autoplay video setting rejected: %s result=%d\n",
                     pair,
                     static_cast<int>(result));
        return 0;
    }
    std::fprintf(stderr, "[app] autoplay video setting %s: %s\n",
                 result == MDKR_VIDEO_RUNTIME_RESTART ? "staged" :
                 result == MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED
                     ? "applied; durability unconfirmed" : "applied",
                 pair);
    return 1;
}

/*
 * "Skip the launcher" (issue #60): the FIRST sample of what the player is
 * holding. Deliberately not the only one -- AppLaunchHold_sample()'s header
 * explains why a single sample taken here cannot see a Shift that was already
 * down, and Launcher::draw() keeps sampling until the direct boot dispatches.
 * This one exists so the common case (a hold that is already down and stays
 * down) is decided before a single frame is built.
 *
 * The launch decision itself, made once and logged in full. Logged even when
 * it is "open the launcher", because "why did it not boot straight in" is the
 * first question a player who turned this on will ask, and the answer --
 * setting off, or something held -- has to be in mdkr64.log.
 */
bool armSkipLauncher(Launcher &launcher) {
    const MdkrVideoConfig *config = mdkr_video_config_desired();
    const bool settingEnabled =
        config != nullptr &&
        config->values[MDKR_APP_SKIP_LAUNCHER].number != 0.0f;
    const AppUiLauncherHold hold = AppLaunchHold_sample(0u);
    const bool holdOpensLauncher = AppUi_launcherHoldOpensLauncher(hold);
    const bool armed = AppUi_launcherSkipArmed(settingEnabled, holdOpensLauncher);
    std::fprintf(stderr,
                 "[app] skip-launcher setting=%d shift=%d shoulderL=%d "
                 "shoulderR=%d holdOpensLauncher=%d armed=%d\n",
                 settingEnabled ? 1 : 0, hold.shift ? 1 : 0,
                 hold.leftShoulder ? 1 : 0, hold.rightShoulder ? 1 : 0,
                 holdOpensLauncher ? 1 : 0, armed ? 1 : 0);
    if (armed) launcher.armSkipWhenReady();
    /*
     * Give back whatever sample 0 borrowed, armed or not. main() takes exactly
     * one sample and then hands the window to whichever launcher path runs
     * next, and every one of those calls host.shutdown() itself -- so handles
     * kept across that boundary are handles some later teardown has to
     * remember to release. The launcher reopens them on its first frame, for
     * as long as it is actually sampling.
     */
    AppLaunchHold_release();
    return armed;
}

int recoverAppHostWebGpu(void *userdata, int phase) {
    AppHost *host = static_cast<AppHost *>(userdata);
    return host != nullptr ? host->recoverWebGpuRoots(phase) : 0;
}

struct EngineSessionTransition {
    OverlayExitRequest request = OverlayExitRequest::None;
    std::string romPath;
};

struct MatchInputProviderContext {
    SessionRuntime *session = nullptr;
    bool canonicalMirrorForTest = false;
    bool delayInputForTest = false;
    bool haveDelayedInput = false;
    std::uint32_t delayedTick = 0u;
    MdkrPadSample delayedSample{};
    MdkrNetImpairment impairment{};
    MdkrNetClock clock{};
    MdkrNetImpairmentProfileName profile = MDKR_NET_PROFILE_COUNT;
    const char *profileName = nullptr;
    std::uint64_t decodedPackets = 0u;
    std::uint64_t rejectedPackets = 0u;
    std::uint32_t profileStartTick = 1u;
    std::uint32_t historyTick[MDKR_MATCH_INPUT_BUNDLE_FRAMES]{};
    MdkrPadSample history[MDKR_MATCH_INPUT_BUNDLE_FRAMES]
                         [MDKR_SESSION_MAX_PLAYERS]{};
    bool historyValid[MDKR_MATCH_INPUT_BUNDLE_FRAMES]{};
    MdkrMatchRecovery recovery{};
};

MdkrMatchTransportIngressResult ingressCarrierBytes(
    MatchInputProviderContext *context, std::uint8_t authenticatedMask,
    const std::uint8_t *bytes, std::size_t length) {
    MdkrMatchInputPacket decoded{};
    if (context == nullptr || context->session == nullptr ||
        !mdkr_match_input_packet_decode(bytes, length, &decoded)) {
        if (context != nullptr) context->rejectedPackets++;
        return MDKR_MATCH_INGRESS_INVALID;
    }
    context->decodedPackets++;
    const MdkrPadSample decodedSample = {
        decoded.buttons, decoded.stick_x, decoded.stick_y, 1u};
    return context->session->receiveRemoteInput(
        decoded.match_epoch, authenticatedMask, decoded.canonical_slot,
        decoded.tick, decodedSample);
}

MdkrMatchTransportIngressResult ingressLoopbackPacket(
    MatchInputProviderContext *context, std::uint32_t epoch,
    std::uint8_t authenticatedMask, unsigned slot, std::uint32_t tick,
    MdkrPadSample sample) {
    const MdkrMatchInputPacket packet = {
        epoch, tick, static_cast<std::uint8_t>(slot),
        sample.buttons, sample.stick_x, sample.stick_y};
    std::uint8_t bytes[MDKR_MATCH_INPUT_PACKET_BYTES]{};
    if (context == nullptr || context->session == nullptr ||
        !mdkr_match_input_packet_encode(&packet, bytes, sizeof(bytes))) {
        return MDKR_MATCH_INGRESS_INVALID;
    }
    /* authenticatedMask comes from the launcher peer/session binding, never
     * from decoded carrier bytes. MatchTransport performs the final ownership
     * and exact-epoch check before accepting the sample. */
    return ingressCarrierBytes(
        context, authenticatedMask, bytes, sizeof(bytes));
}

bool parseNetworkProfile(const char *text,
                         MdkrNetImpairmentProfileName *profile) {
    static const char *const names[MDKR_NET_PROFILE_COUNT] = {
        "lan", "regional-good", "regional-variable", "poor",
        "two-second-outage", "adversarial"};
    if (text == nullptr || text[0] == '\0' || profile == nullptr) return false;
    for (unsigned index = 0u; index < MDKR_NET_PROFILE_COUNT; index++) {
        if (std::strcmp(text, names[index]) == 0) {
            *profile = static_cast<MdkrNetImpairmentProfileName>(index);
            return true;
        }
    }
    return false;
}

bool drainImpairedLoopback(
    MatchInputProviderContext *context, std::uint32_t epoch,
    std::uint32_t tick,
    const MdkrPadSample physical[MDKR_SESSION_MAX_PLAYERS],
    const MdkrNetRoster *roster, std::uint8_t remote) {
    if (tick < context->profileStartTick) {
        for (unsigned slot = 0u; slot < roster->canonical_player_count; slot++) {
            const std::uint8_t bit = static_cast<std::uint8_t>(1u << slot);
            if ((remote & bit) == 0u) continue;
            const MdkrMatchTransportIngressResult result = ingressLoopbackPacket(
                context, epoch, remote, slot, tick, physical[slot]);
            if (result != MDKR_MATCH_INGRESS_ACCEPTED &&
                result != MDKR_MATCH_INGRESS_DUPLICATE) return false;
        }
        return true;
    }
    const std::uint32_t profileTick = tick - context->profileStartTick + 1u;
    const unsigned historyIndex = tick % MDKR_MATCH_INPUT_BUNDLE_FRAMES;
    context->historyTick[historyIndex] = tick;
    context->historyValid[historyIndex] = true;
    for (unsigned slot = 0u; slot < roster->canonical_player_count; slot++) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << slot);
        context->history[historyIndex][slot] =
            (remote & bit) != 0u ? physical[slot] : MdkrPadSample{};
    }
    const MdkrNetClockStep clockStep = mdkr_net_clock_step(&context->clock);
    if (clockStep.tick_offered) {
        MdkrMatchInputBundle bundle{};
        std::uint8_t bytes[MDKR_MATCH_INPUT_BUNDLE_BYTES]{};
        bundle.match_epoch = epoch;
        bundle.newest_tick = tick;
        bundle.slot_mask = remote;
        for (unsigned age = 0u; age < MDKR_MATCH_INPUT_BUNDLE_FRAMES; age++) {
            if (tick < age) break;
            const std::uint32_t authoredTick = tick - age;
            const unsigned index = authoredTick % MDKR_MATCH_INPUT_BUNDLE_FRAMES;
            if (!context->historyValid[index] ||
                context->historyTick[index] != authoredTick) break;
            for (unsigned slot = 0u;
                 slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
                bundle.frames[age][slot] = context->history[index][slot];
            }
            bundle.frame_count++;
        }
        if (!mdkr_match_input_bundle_encode(&bundle, bytes, sizeof(bytes)) ||
            !mdkr_net_impairment_send(
                &context->impairment, profileTick, 0u, 0u,
                bytes, sizeof(bytes))) return false;
    }
    MdkrNetSimPacket wire{};
    while (mdkr_net_impairment_receive(
               &context->impairment, profileTick, 0u, &wire)) {
        MdkrMatchInputBundle bundle{};
        if (!mdkr_match_input_bundle_decode(
                wire.bytes, wire.length, &bundle) ||
            (bundle.slot_mask & static_cast<std::uint8_t>(~remote)) != 0u) {
            context->rejectedPackets++;
            continue;
        }
        context->decodedPackets++;
        /* Oldest first prevents a redundant bundle from invalidating a newer
         * prediction suffix twice in one authenticated ingress transaction. */
        for (unsigned age = bundle.frame_count; age-- > 0u;) {
            const std::uint32_t authoredTick = bundle.newest_tick - age;
            for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
                const std::uint8_t bit = static_cast<std::uint8_t>(1u << slot);
                if ((bundle.slot_mask & bit) == 0u) continue;
                const MdkrMatchTransportIngressResult result =
                    context->session->receiveRemoteInput(
                        bundle.match_epoch, remote, slot, authoredTick,
                        bundle.frames[age][slot]);
                if (result != MDKR_MATCH_INGRESS_ACCEPTED &&
                    result != MDKR_MATCH_INGRESS_CORRECTED &&
                    result != MDKR_MATCH_INGRESS_DUPLICATE &&
                    result != MDKR_MATCH_INGRESS_OUT_OF_WINDOW &&
                    result != MDKR_MATCH_INGRESS_TAKEN_OVER) return false;
            }
        }
    }
    return true;
}

bool drainMatchInputProvider(
    void *opaque, std::uint32_t epoch, std::uint32_t tick,
    const MdkrPadSample physical[MDKR_SESSION_MAX_PLAYERS], unsigned count,
    MdkrInputSet *out) {
    MatchInputProviderContext *context =
        static_cast<MatchInputProviderContext *>(opaque);
    if (context == nullptr || context->session == nullptr || out == nullptr) {
        return false;
    }
    if (context->canonicalMirrorForTest) {
        const MdkrSessionBridge &bridge = context->session->bridge();
        const MdkrNetRoster *roster = mdkr_session_bridge_roster(&bridge);
        if (roster == nullptr) return false;
        const std::uint8_t active = static_cast<std::uint8_t>(
            (1u << roster->canonical_player_count) - 1u);
        const std::uint8_t remote = static_cast<std::uint8_t>(
            active & ~mdkr_session_bridge_local_slot_mask(&bridge));
        if (context->profile != MDKR_NET_PROFILE_COUNT) {
            if (!drainImpairedLoopback(
                    context, epoch, tick, physical, roster, remote)) return false;
            const bool drained = context->session->engineDrainInputs(
                epoch, tick, physical, count, *out, true);
            MdkrMatchRecovery recovery{};
            if (drained &&
                context->session->engineRecovery(epoch, recovery) &&
                context->recovery.reason == MDKR_MATCH_RECOVERY_NONE) {
                context->recovery = recovery;
                std::fprintf(
                    stderr,
                    "[NET-RECOVERY] reason=%s slot=%u first=%u observed=%u "
                    "action=return-to-launcher\n",
                    recovery.reason == MDKR_MATCH_RECOVERY_INPUT_GAP
                        ? "input-gap" : "late-input",
                    static_cast<unsigned>(recovery.canonical_slot),
                    static_cast<unsigned>(recovery.first_unrecoverable_tick),
                    static_cast<unsigned>(recovery.observed_at_tick));
                platform_request_exit(0);
            }
            return drained;
        }
        if (context->haveDelayedInput && tick == context->delayedTick + 4u) {
            const MdkrMatchTransportIngressResult result = ingressLoopbackPacket(
                context, epoch, remote, 0u, context->delayedTick,
                context->delayedSample);
            if (result != MDKR_MATCH_INGRESS_CORRECTED) return false;
            std::fprintf(stderr,
                         "[NET-INPUT-TEST] delivered slot=0 tick=%u at=%u\n",
                         context->delayedTick, tick);
            context->haveDelayedInput = false;
        }
        for (unsigned slot = 0u; slot < roster->canonical_player_count; ++slot) {
            const std::uint8_t bit = static_cast<std::uint8_t>(1u << slot);
            if ((remote & bit) == 0u) continue;
            if (context->delayInputForTest && slot == 0u && tick == 131u) {
                context->delayedTick = tick;
                context->delayedSample = physical[slot];
                context->haveDelayedInput = true;
                std::fprintf(stderr,
                             "[NET-INPUT-TEST] withheld slot=0 tick=%u "
                             "deliver=%u\n", tick, tick + 4u);
                continue;
            }
            const MdkrMatchTransportIngressResult result = ingressLoopbackPacket(
                context, epoch, remote, slot, tick, physical[slot]);
            if (result != MDKR_MATCH_INGRESS_ACCEPTED &&
                result != MDKR_MATCH_INGRESS_DUPLICATE &&
                result != MDKR_MATCH_INGRESS_TAKEN_OVER) {
                return false;
            }
        }
    }
    return context->session->engineDrainInputs(
            epoch, tick, physical, count, *out,
            context->canonicalMirrorForTest);
}

bool readMatchInputProvider(void *opaque, std::uint32_t epoch,
                            std::uint32_t tick, MdkrInputSet *out) {
    MatchInputProviderContext *context =
        static_cast<MatchInputProviderContext *>(opaque);
    return context != nullptr && context->session != nullptr && out != nullptr &&
        context->session->engineInputsForTick(epoch, tick, *out);
}

bool takeDirtyMatchInputProvider(void *opaque, std::uint32_t epoch,
                                 std::uint32_t *tick) {
    MatchInputProviderContext *context =
        static_cast<MatchInputProviderContext *>(opaque);
    return context != nullptr && context->session != nullptr && tick != nullptr &&
        context->session->engineTakeDirty(epoch, *tick);
}

bool aiMaskMatchInputProvider(void *opaque, std::uint32_t epoch,
                              std::uint32_t tick, std::uint8_t *slotMask) {
    MatchInputProviderContext *context =
        static_cast<MatchInputProviderContext *>(opaque);
    return context != nullptr && context->session != nullptr &&
        slotMask != nullptr &&
        context->session->engineAiMaskForTick(epoch, tick, *slotMask);
}

bool parseTestOnlineMask(const char *name, std::uint8_t *out) {
    const char *text = std::getenv(name);
    if (text == nullptr || text[0] == '\0' || out == nullptr) return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > 0x0fu) {
        return false;
    }
    *out = static_cast<std::uint8_t>(value);
    return true;
}

bool makeTestOnlineLaunch(std::uint32_t epoch, std::uint8_t localMask,
                          std::uint8_t viewportMask,
                          MdkrSessionLaunchV2 *out) {
    if (out == nullptr || epoch == 0u || localMask == 0u ||
        (viewportMask & static_cast<std::uint8_t>(~localMask)) != 0u) {
        return false;
    }
    MdkrSessionLaunchV2 launch{};
    launch.version = MDKR_SESSION_LAUNCH_VERSION;
    launch.size = sizeof(launch);
    launch.match.match_epoch = epoch;
    launch.match.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    for (std::size_t index = 0; index < sizeof(launch.match.build_id); ++index) {
        launch.match.build_id[index] = static_cast<std::uint8_t>(0x40u + index);
    }
    for (std::size_t index = 0;
         index < sizeof(launch.match.gameplay_digest); ++index) {
        launch.match.gameplay_digest[index] =
            static_cast<std::uint8_t>(0x80u + index);
    }
    for (unsigned slot = 0u; slot < MDKR_MATCH_SLOTS; ++slot) {
        launch.match.slot_owner[slot] = UINT64_C(0x1000000000000001) + slot;
    }
    launch.match.rng_seed = UINT64_C(0x4d444b5236340001);
    launch.match.track_id = 5u;
    if (const char *trackText =
            std::getenv("MDKR_APP_TEST_ONLINE_MANIFEST_TRACK")) {
        char *end = nullptr;
        errno = 0;
        const unsigned long track = std::strtoul(trackText, &end, 10);
        if (errno != 0 || end == trackText || *end != '\0' || track > 65u) {
            return false;
        }
        launch.match.track_id = static_cast<std::uint16_t>(track);
    }
    launch.match.rom_revision = MDKR_ROM_US_11;
    launch.match.cadence_hz = 30u;
    launch.match.slot_count = MDKR_MATCH_SLOTS;
    launch.match.rules = 1u;
    /* The default test route loads Ancient Lake (id 5), whose ROM-authored
     * player-vehicle capability mask is car/hovercraft/plane. A different
     * manifest track must supply its own exact mask through the test override
     * below, just as production matchmaking derives it from the validated ROM. */
    launch.match.vehicle_mask = 7u;
    if (const char *maskText =
            std::getenv("MDKR_APP_TEST_ONLINE_MANIFEST_VEHICLE_MASK")) {
        char *end = nullptr;
        errno = 0;
        const unsigned long mask = std::strtoul(maskText, &end, 0);
        if (errno != 0 || end == maskText || *end != '\0' ||
            mask == 0u || mask > 7u) {
            return false;
        }
        launch.match.vehicle_mask = static_cast<std::uint8_t>(mask);
    }
    launch.match.input_delay = 2u;
    launch.local_slot_mask = localMask;
    launch.viewport_slot_mask = viewportMask;
    *out = launch;
    return true;
}

bool makeTestOnlineLaunchV3(std::uint32_t epoch, std::uint8_t localMask,
                            std::uint8_t viewportMask,
                            MdkrSessionLaunchV3 *out) {
    MdkrSessionLaunchV2 legacy{};
    if (out == nullptr ||
        !makeTestOnlineLaunch(epoch, localMask, viewportMask, &legacy)) {
        return false;
    }
    MdkrSessionLaunchV3 launch{};
    launch.version = MDKR_SESSION_LAUNCH_V3_VERSION;
    launch.size = sizeof(launch);
    launch.match.version = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
    launch.match.manifest = legacy.match;
    for (unsigned slot = 0u; slot < MDKR_MATCH_SLOTS; ++slot) {
        launch.match.selections[slot].selection_revision = slot + 1u;
        launch.match.selections[slot].character_id =
            static_cast<std::uint8_t>(slot);
        launch.match.selections[slot].vehicle_id = 0u;
    }
    launch.local_slot_mask = localMask;
    launch.viewport_slot_mask = viewportMask;
    *out = launch;
    return true;
}

void logNetworkRoster(const MdkrMatchManifestV1 &manifest,
                      const MdkrNetRoster &roster,
                      std::uint8_t localMask,
                      std::uint8_t viewportMask) {
    std::fprintf(stderr,
                 "[NET-ROSTER] epoch=%u manifest=%016llx players=%u "
                 "local-mask=0x%02x viewport-mask=0x%02x local-map=",
                 static_cast<unsigned>(manifest.match_epoch),
                 static_cast<unsigned long long>(
                     mdkr_match_manifest_digest(&manifest)),
                 static_cast<unsigned>(roster.canonical_player_count),
                 static_cast<unsigned>(localMask),
                 static_cast<unsigned>(viewportMask));
    for (unsigned index = 0u; index < roster.local_seat_count; ++index) {
        std::fprintf(stderr, "%s%u", index == 0u ? "" : ",",
                     static_cast<unsigned>(roster.local_to_canonical[index]));
    }
    std::fprintf(stderr, " viewport-map=");
    for (unsigned index = 0u; index < roster.viewport_count; ++index) {
        std::fprintf(stderr, "%s%u", index == 0u ? "" : ",",
                     static_cast<unsigned>(roster.viewport_to_canonical[index]));
    }
    std::fprintf(stderr, "\n");
}

int runEngineSession(AppHost &host, SessionRuntime &session,
                     const MdkrBootConfig &config,
                     EngineSessionTransition *transition) {
    const std::string sessionRom =
        config.rom_path && config.rom_path[0]
            ? config.rom_path : AppConfig::get("rom_path", "");
    /* The runtime lives above this blocking engine invocation. Transition it
     * before lending the window/device so overlay and recovery policy come
     * from one executable state authority rather than from boot call sites. */
    bool sessionReady = true;
    if (session.state().scene == MDKR_SCENE_HOME) {
        sessionReady = session.beginLocal();
    }
    if (sessionReady && session.state().engine == MDKR_ENGINE_STOPPED &&
        (session.state().scene == MDKR_SCENE_LOCAL_SETUP ||
         (session.state().intent == MDKR_INTENT_ONLINE_PRIVATE &&
          session.state().scene == MDKR_SCENE_LOADING))) {
        sessionReady = session.requestRace();
    } else if (sessionReady && session.state().scene == MDKR_SCENE_RESULTS) {
        sessionReady = session.rematch();
    }
    if (sessionReady && session.state().engine == MDKR_ENGINE_BOOTING) {
        sessionReady = session.enginePhase(MDKR_ENGINE_READY) &&
                       session.enginePhase(MDKR_ENGINE_RACING);
    }
    if (!sessionReady || session.state().engine != MDKR_ENGINE_RACING) {
        std::fprintf(stderr,
                     "[session] refused engine boot scene=%u engine=%u error=%u\n",
                     static_cast<unsigned>(session.state().scene),
                     static_cast<unsigned>(session.state().engine),
                     static_cast<unsigned>(session.lastStep().error));
        return 2;
    }

    const bool online =
        session.state().intent == MDKR_INTENT_ONLINE_PRIVATE;
#if MDKR_ENABLE_ONLINE_BETA
    /* Beach-ball guard (net_roster ownership): a local-Play boot must never
     * inherit an online session's process-global roster -- that stray global
     * would flip the engine into online-race mode and stall forever waiting for
     * network input a local race never sends. Historically the roster was only
     * cleared on engine EXIT and never checked on ENTRY. Force-clear any roster
     * this local boot does not own BEFORE booting, not only after. */
    if (!online && OnlineRoom_guardRosterOwner(0u)) {
        std::fprintf(stderr,
                     "[session] discarded a stray online roster before a local "
                     "boot (ownership guard)\n");
    }
#endif
    const MdkrMatchManifestV1 *networkManifest = online
        ? mdkr_session_bridge_manifest(&session.bridge()) : nullptr;
    const MdkrNetRoster *networkRoster = online
        ? mdkr_session_bridge_roster(&session.bridge()) : nullptr;
    const MdkrMatchLaunchDescriptorV1 *networkLaunch = online
        ? mdkr_session_bridge_launch_descriptor(&session.bridge()) : nullptr;
    if (online &&
        (networkManifest == nullptr || networkRoster == nullptr ||
         networkManifest->match_epoch != session.state().match_epoch)) {
        std::fprintf(stderr,
                     "[session] refused missing/stale online launch "
                     "expected-epoch=%u actual-epoch=%u\n",
                     static_cast<unsigned>(session.state().match_epoch),
                     networkManifest != nullptr
                         ? static_cast<unsigned>(networkManifest->match_epoch)
                         : 0u);
        (void)session.enginePhase(MDKR_ENGINE_FAILED);
        return 2;
    }
    if (networkRoster != nullptr &&
        !(networkLaunch != nullptr
              ? mdkr_net_roster_runtime_install_launch(
                    networkLaunch, networkRoster)
              : mdkr_net_roster_runtime_install(
                    networkManifest, networkRoster))) {
        std::fprintf(stderr,
                     "[session] refused mismatched manifest/network roster\n");
        (void)session.enginePhase(MDKR_ENGINE_FAILED);
        return 2;
    }
#if MDKR_ENABLE_ONLINE_BETA
    /* Tag the roster this online session installed so a subsequent local-Play
     * boot's ownership guard clears it rather than silently inheriting it. */
    if (networkRoster != nullptr) {
        OnlineRoom_setRosterOwner(session.state().session_id);
    }
#endif
    MatchInputProviderContext matchInputContext{};
    bool matchInputInstalled = false;
    if (networkManifest != nullptr && networkRoster != nullptr) {
        logNetworkRoster(*networkManifest, *networkRoster,
                         mdkr_session_bridge_local_slot_mask(&session.bridge()),
                         mdkr_session_bridge_viewport_slot_mask(&session.bridge()));
        if (networkLaunch != nullptr) {
            std::fprintf(stderr,
                         "[NET-LAUNCH] epoch=%u descriptor=%016llx "
                         "track=%u selections=0:%u/%u,1:%u/%u,2:%u/%u,3:%u/%u\n",
                         static_cast<unsigned>(networkManifest->match_epoch),
                         static_cast<unsigned long long>(
                             mdkr_match_launch_descriptor_digest(networkLaunch)),
                         static_cast<unsigned>(networkManifest->track_id),
                         networkLaunch->selections[0].character_id,
                         networkLaunch->selections[0].vehicle_id,
                         networkLaunch->selections[1].character_id,
                         networkLaunch->selections[1].vehicle_id,
                         networkLaunch->selections[2].character_id,
                         networkLaunch->selections[2].vehicle_id,
                         networkLaunch->selections[3].character_id,
                         networkLaunch->selections[3].vehicle_id);
        }
        matchInputContext.session = &session;
        matchInputContext.canonicalMirrorForTest =
            std::getenv("MDKR_APP_TEST_ONLINE_LOOPBACK_INPUTS") != nullptr;
        matchInputContext.delayInputForTest =
            std::getenv("MDKR_APP_TEST_ONLINE_DELAY_INPUT") != nullptr;
        const char *networkProfile =
            std::getenv("MDKR_APP_TEST_NET_PROFILE");
        if (networkProfile != nullptr) {
            MdkrNetImpairmentProfile impairmentProfile{};
            MdkrNetClockProfile clockProfile{};
            std::uint8_t endpoint = 0u;
            if (!matchInputContext.canonicalMirrorForTest ||
                matchInputContext.delayInputForTest ||
                !parseNetworkProfile(
                    networkProfile, &matchInputContext.profile) ||
                !mdkr_net_impairment_named_profile(
                    matchInputContext.profile, networkManifest->cadence_hz,
                    &impairmentProfile) ||
                !mdkr_net_roster_runtime_local_to_canonical(0u, &endpoint) ||
                !mdkr_net_clock_named_profile(
                    matchInputContext.profile, networkManifest->cadence_hz,
                    endpoint, &clockProfile) ||
                !mdkr_net_clock_init(&matchInputContext.clock, &clockProfile)) {
                std::fprintf(stderr,
                             "[session] invalid named network profile: %s\n",
                             networkProfile);
                mdkr_net_roster_runtime_clear();
                (void)session.enginePhase(MDKR_ENGINE_FAILED);
                return 2;
            }
            mdkr_net_impairment_init(
                &matchInputContext.impairment,
                mdkr_match_manifest_digest(networkManifest) ^ endpoint,
                impairmentProfile);
            matchInputContext.profileName = networkProfile;
            if (const char *startText =
                    std::getenv("MDKR_APP_TEST_NET_PROFILE_START_TICK")) {
                char *end = nullptr;
                errno = 0;
                const unsigned long parsed = std::strtoul(startText, &end, 10);
                if (errno != 0 || end == startText || *end != '\0' ||
                    parsed == 0u || parsed > 1000000u) {
                    std::fprintf(stderr,
                                 "[session] invalid network profile start: %s\n",
                                 startText);
                    mdkr_net_roster_runtime_clear();
                    (void)session.enginePhase(MDKR_ENGINE_FAILED);
                    return 2;
                }
                matchInputContext.profileStartTick =
                    static_cast<std::uint32_t>(parsed);
            }
        }
        if (matchInputContext.delayInputForTest &&
            !matchInputContext.canonicalMirrorForTest) {
            std::fprintf(stderr,
                         "[session] delayed-input test requires loopback input\n");
            mdkr_net_roster_runtime_clear();
            (void)session.enginePhase(MDKR_ENGINE_FAILED);
            return 2;
        }
        const MdkrMatchInputSource source = {
            MDKR_MATCH_INPUT_SOURCE_VERSION,
            networkManifest->match_epoch,
            &matchInputContext,
            drainMatchInputProvider,
            readMatchInputProvider,
            takeDirtyMatchInputProvider,
            aiMaskMatchInputProvider,
        };
        matchInputInstalled = mdkr_match_input_runtime_install(&source);
        if (!matchInputInstalled) {
            std::fprintf(stderr,
                         "[session] refused duplicate/invalid match input provider\n");
            mdkr_net_roster_runtime_clear();
            (void)session.enginePhase(MDKR_ENGINE_FAILED);
            return 2;
        }
        const char *takeoverSlotText =
            std::getenv("MDKR_APP_TEST_AI_TAKEOVER_SLOT");
        const char *takeoverTickText =
            std::getenv("MDKR_APP_TEST_AI_TAKEOVER_TICK");
        if ((takeoverSlotText == nullptr) != (takeoverTickText == nullptr)) {
            std::fprintf(stderr,
                         "[session] AI takeover test requires slot and tick\n");
            mdkr_match_input_runtime_clear();
            mdkr_net_roster_runtime_clear();
            (void)session.enginePhase(MDKR_ENGINE_FAILED);
            return 2;
        }
        if (takeoverSlotText != nullptr) {
            char *slotEnd = nullptr;
            char *tickEnd = nullptr;
            errno = 0;
            const unsigned long slot =
                std::strtoul(takeoverSlotText, &slotEnd, 10);
            const unsigned long activation =
                std::strtoul(takeoverTickText, &tickEnd, 10);
            const bool parsed = errno == 0 && slotEnd != takeoverSlotText &&
                *slotEnd == '\0' && tickEnd != takeoverTickText &&
                *tickEnd == '\0' && slot < MDKR_SESSION_MAX_PLAYERS &&
                activation > 0u && activation <= UINT32_MAX;
            const MdkrMatchTakeoverResult scheduled = parsed
                ? session.scheduleAiTakeover(
                    networkManifest->match_epoch,
                    static_cast<unsigned>(slot),
                    static_cast<std::uint32_t>(activation))
                : MDKR_MATCH_TAKEOVER_INVALID;
            if (scheduled != MDKR_MATCH_TAKEOVER_ACCEPTED) {
                std::fprintf(stderr,
                             "[session] invalid AI takeover slot=%s tick=%s "
                             "result=%u\n",
                             takeoverSlotText, takeoverTickText,
                             static_cast<unsigned>(scheduled));
                mdkr_match_input_runtime_clear();
                mdkr_net_roster_runtime_clear();
                (void)session.enginePhase(MDKR_ENGINE_FAILED);
                return 2;
            }
            std::fprintf(stderr,
                         "[NET-TAKEOVER] epoch=%u slot=%lu tick=%lu "
                         "policy=ai-no-handback\n",
                         static_cast<unsigned>(networkManifest->match_epoch),
                         slot, activation);
        }
    }

    platformSetHostWindow(host.window(), host.glContext());
    if (host.usingWebGpu()) {
        platformSetHostWebGpu(host.wgpuInstance(), host.wgpuAdapter(),
                              host.wgpuDevice(), host.wgpuQueue(),
                              host.wgpuSurface(), host.wgpuFormat());
        platformSetHostWebGpuRecovery(recoverAppHostWebGpu, &host);
    }
    if (config.character_preview_studio) {
        Overlay_installCharacterStudio(
            host.window(), config.character_preview_package,
            config.character_preview_context);
    } else if (config.character_motion_review) {
        Overlay_installCharacterReview(host.window());
    } else {
        Overlay_setPauseAllowed(session.overlayMayPause());
        Overlay_install(host.window());
    }
    /* Online epochs present the NTSC source identity to the engine
     * (rom_io.c): the accepted ROM payloads are byte-identical, so a PAL
     * endpoint authoring the manifest's 30 Hz cadence is bit-identical to a
     * US endpoint -- the cross-region convergence requirement.  Epoch-scoped:
     * armed only across this bounded blocking boot and cleared the moment it
     * returns, so a later offline epoch re-latches the ROM's true clock. */
    int result;
    {
        OnlineNtscIdentityScope ntscIdentityScope(online);
        result = mdkr64_engine_boot(&config);
    }

    if (matchInputContext.profile != MDKR_NET_PROFILE_COUNT) {
        std::fprintf(stderr,
                     "[NET-PROFILE] name=%s sent=%llu dropped=%llu duplicate=%llu "
                     "reordered=%llu corrupted=%llu outage=%llu throttled=%llu "
                     "overflow=%llu decoded=%llu rejected=%llu offers=%u "
                     "skipped=%u long=%u sleep=%u\n",
                     matchInputContext.profileName,
                     (unsigned long long)matchInputContext.impairment.sent,
                     (unsigned long long)matchInputContext.impairment.dropped,
                     (unsigned long long)matchInputContext.impairment.duplicated,
                     (unsigned long long)matchInputContext.impairment.reordered,
                     (unsigned long long)matchInputContext.impairment.corrupted,
                     (unsigned long long)matchInputContext.impairment.outage_dropped,
                     (unsigned long long)matchInputContext.impairment.throttled,
                     (unsigned long long)matchInputContext.impairment.overflow,
                     (unsigned long long)matchInputContext.decodedPackets,
                     (unsigned long long)matchInputContext.rejectedPackets,
                     matchInputContext.clock.authored_offers,
                     matchInputContext.clock.skipped_offers,
                     matchInputContext.clock.long_frames,
                     matchInputContext.clock.sleep_skips);
    }
    if (networkManifest != nullptr) {
        const MdkrMatchTransportStats *stats = session.transportStats();
        MdkrMatchRecovery recovery{};
        const bool recovering = session.engineRecovery(
            networkManifest->match_epoch, recovery);
        if (stats != nullptr) {
            std::fprintf(
                stderr,
                "[NET-TRANSPORT] epoch=%u accepted=%u corrected=%u "
                "duplicate=%u invalid=%u stale=%u unauthorized=%u conflict=%u "
                "outWindow=%u drained=%u drainRejected=%u recovery=%u "
                "takeoverStarted=%u takeoverIgnored=%u\n",
                static_cast<unsigned>(networkManifest->match_epoch),
                stats->accepted, stats->corrected, stats->duplicates,
                stats->invalid, stats->stale_epoch, stats->unauthorized,
                stats->conflicts, stats->out_of_window, stats->drained,
                stats->drain_rejected,
                recovering ? static_cast<unsigned>(recovery.reason) : 0u,
                stats->takeover_started,
                stats->takeover_ignored_inputs);
        }
    }

    /* mdkr64_engine_boot is blocking and returns only after every engine
     * worker has joined. The launcher's frozen roster can now be retired. */
    if (matchInputInstalled) mdkr_match_input_runtime_clear();
    mdkr_net_roster_runtime_clear();

    /* Presentation/settings consumers have now released their per-engine
     * latches. Re-arm exactly one launcher handoff for a later match epoch. */
    if (!mdkr_video_config_engine_session_complete()) {
        std::fprintf(stderr,
                     "[session] video-config engine epoch did not close cleanly\n");
    }

    if (session.state().engine == MDKR_ENGINE_RACING) {
        if (matchInputContext.recovery.reason != MDKR_MATCH_RECOVERY_NONE) {
            (void)session.enginePhase(MDKR_ENGINE_FAILED);
            (void)session.recover(MDKR_SESSION_ERROR_CONNECTION_LOST);
            std::fprintf(stderr,
                         "[SESSION-RECOVERY] scene=%u engine=%u error=%u "
                         "epoch=%u\n",
                         static_cast<unsigned>(session.state().scene),
                         static_cast<unsigned>(session.state().engine),
                         static_cast<unsigned>(session.state().last_error),
                         static_cast<unsigned>(session.state().match_epoch));
        } else {
            (void)session.enginePhase(
                result == 0 ? MDKR_ENGINE_FINISHED : MDKR_ENGINE_FAILED);
        }
    }

    /* The engine has dropped every borrowed child. Clear the registries before
     * AppHost releases their roots so no process-lifetime seam keeps a dangling
     * window, device, or overlay callback after this boot. */
    platformSetOverlayHooks(nullptr);
    platformSetHostWebGpuRecovery(nullptr, nullptr);
    platformSetHostWebGpu(nullptr, nullptr, nullptr, nullptr, nullptr, 0);
    platformSetHostWindow(nullptr, nullptr);
    if (transition != nullptr) {
        transition->request = Overlay_consumeExitRequest();
        if (transition->request == OverlayExitRequest::RestartGame) {
            transition->romPath = sessionRom;
        }
    }

    if (result != 0 && !host.webGpuRecoveryError().empty()) {
        std::fprintf(stderr, "[app] durable WebGPU recovery error: %s\n", host.webGpuRecoveryError().c_str());
    }
    return result;
}

#if MDKR_ENABLE_ONLINE_BETA
/* ======================================================================== *
 * Boot the VISIBLE 3D engine for an ONLINE race driven by the LIVE
 * adapter transport (make-or-break).
 *
 * The engine's per-tick canonical input runs through the process-global
 * MdkrMatchInputSource (platform/net/match_input_runtime.h). runEngineSession's
 * online branch backs that source with a launcher SessionRuntime fed by a
 * LOOPBACK simulator; this path backs it with the LIVE adapter's race transport
 * instead -- the same transport that drains real opened INPUT envelopes from the
 * mesh. The engine roster is already installed process-globally by the adapter's
 * install(); we only publish the input source, wire the host, and boot.
 *
 * `peer` is non-null only on the in-process loopback proof (MDKR_APP_TEST_
 * ONLINE_LIVE): it is the second endpoint, cross-pumped one tick per drain so it
 * seals real input over the mesh for the visible endpoint. In production `peer`
 * is null and the real remote process supplies that input.
 * ======================================================================== */
/* Why the online-live engine session ended, so post-session handling can route
 * the survivor to the right recovery card instead of a ghost race. Completed is
 * the normal engine exit (race finished or the player left through the race
 * chrome); the rest are the abnormal drain exits liveDrainMatchInput forces. */
enum class LiveRaceEndReason {
    Completed = 0,        /* engine exited normally */
    AdvanceFailed,        /* race_advance() failed (pre-existing hard exit) */
    OpponentLeft,         /* peer-loss latch fired mid-race */
    OpponentNeverStarted, /* start barrier aborted before the first authored tick */
};

struct LiveMatchInputContext {
    IMdkrOnlineAdapter *visible = nullptr;
    IMdkrOnlineAdapter *peer = nullptr;   /* loopback proof only; null in prod */
    std::uint32_t epoch = 0u;
    std::uint8_t activeMask = 0u;
    std::uint32_t racedTicks = 0u;        /* highest authored tick drained */
    std::uint64_t drainCalls = 0u;
    bool advanceFailed = false;
    /* Set by liveDrainMatchInput when it forces an abnormal session end (peer
     * loss mid-race, or a start-barrier abort). runOnlineLiveEngineSession reads
     * it to route the post-race view and to suppress fabricated results. */
    LiveRaceEndReason endReason = LiveRaceEndReason::Completed;
    /* Cross-process test-only real-time pacing (see runOnlineLiveEngineSession's
     * paceAdvanceHz parameter, default 0 == disabled/current behavior). Headless
     * autoplay drains authored ticks as fast as the CPU allows; interactive play
     * is already naturally paced by vsync, and the in-process loopback proof
     * cross-pumps its peer synchronously, so neither needs this. A REAL
     * two-process race has no such synchronous peer to force along, so its
     * confirmed frontier can never catch an unthrottled drain frontier over a
     * genuine network round trip. Zero leaves liveDrainMatchInput's `peer ==
     * nullptr` branch exactly as before. */
    unsigned paceAdvanceHz = 0u;
    std::uint64_t lastAdvanceMs = 0u;
    /* TEST-ONLY transport-sever seam state (see liveTestSeverPeerAtTick).
     * Once severed, the in-process peer is a CORPSE: never serviced again
     * (its mesh stops ponging/draining -- exactly a SIGKILLed remote as the
     * transport sees it), its loopback signal presence is dropped (what the
     * real signal service broadcasts when the killed process's socket dies),
     * and the drain takes the production predict path. severRig is the
     * loopback rig whose hub delivers the presence drop; null in production
     * and in every lane that leaves the seam env unset. */
    bool peerSevered = false;
    /* TEST-ONLY room-only departure seam state (see
     * liveTestRoomDepartureAtTick): the peer's loopback presence has been
     * dropped once, and the peer is otherwise untouched. */
    bool peerRoomDeparted = false;
    MdkrOnlineTestLoopbackRace *severRig = nullptr;
    /* Test-only (MDKR_APP_TEST_ONLINE_LIVE_PREDICT): on the in-process loopback
     * proof the visible endpoint normally spins until the peer's input for the
     * tick it is about to commit has arrived, so it never rolls back. Set this
     * to a positive count to instead advance the FIRST N active-race ticks with
     * PREDICTED input (skip that wait), exactly as the production peer==nullptr
     * path does, so the peer's real input lands one drain later and the engine's
     * network-input rollback reconciles it -- a deterministic, cloud-free
     * reproduction of a joiner rolling back on a viewport_count=1 endpoint.
     * Zero leaves the loopback proof's confirmed-input behavior unchanged. */
    unsigned predictWindowTicks = 0u;
    unsigned predictTicksDone = 0u;
};

/* Serviced every engine frame (menu-nav included) via the overlay service hook,
 * so the mesh's application-level ping never lapses during the long headless
 * menu walk that precedes the race level. Single instance per boot. */
LiveMatchInputContext *g_liveMatchInput = nullptr;

/* RESIDENT LIVE coordinator (set only by the resident-live lane; null for
 * every existing lane). Driven each engine frame from liveOverlayService -- the
 * ONLY launcher code that runs while the engine is resident -- it OWNS the one-
 * shot results poll (PUBLISH_RESULTS mid-residency), pumps BOTH party_link feeds
 * so the native RESULTS screen fronts on the real reducer feed, and drives the
 * per-round re-cycle (REMATCH via the reverse feed -> re-Ready -> START ->
 * re-install the match-input source with the fresh match_epoch) so race N+1 boots
 * IN THE SAME engine process. */
struct LiveResidentState {
    IMdkrOnlineAdapter *visible = nullptr;
    IMdkrOnlineAdapter *peer = nullptr;
    LiveMatchInputContext *ctx = nullptr;
    std::uint8_t raceIndex = 0u; /* reducer race_index at the last PUBLISH_RESULTS */
    /* Results -> Advancing -> Racing. The round transition is driven
     * FRAME-BY-FRAME through the resumable OnlineRoom_residentAdvanceStep, so the
     * RESULTS->next-race gap never freezes the launcher's per-frame service path. */
    enum class Phase { Racing, Results, Advancing, Done } phase = Phase::Racing;
    MdkrResidentAdvanceState advance{}; /* frame-stepped advance coordinator */
    /* SINGLE-ENDPOINT residency (a real 2-process room). When set, the
     * per-round advance drives ONLY the local (visible) endpoint -- the remote
     * readies itself over the transport -- so `advance.singleEndpoint` is armed and
     * no peer adapter is ever poked. Production sets this with peer == nullptr. */
    bool singleEndpoint = false;
    /* TEST-ONLY: with two loopback adapters, drive the PEER as a stand-in
     * REMOTE process (re-Ready its own local endpoint each round) SEPARATELY from
     * the single-endpoint advance step, so the headless lane proves the advance step
     * itself never pokes the peer. Never set in production (peer == nullptr there). */
    bool remoteSim = false;
    unsigned joinerCharacter = 1u; /* remote-sim: != host native pick Pipsy(2) */
    /* WEDGE (test-only): once the session reaches RESULTS, STOP pumping the
     * reverse-feed intent so the host's REMATCH never reaches the reducer -- the room
     * parks in RESULTS and the engine's RESULTS-hold WALL-CLOCK watchdog
     * must fire + route to a clean ERROR exit, never hang. None in normal runs. */
    bool wedgeResultsHold = false;
    /* WEDGE (test-only): during round 2's re-cycle, once the advance
     * has driven the room to LOADING, CANCEL it (leader RETURN_TO_LOBBY) so the room
     * regresses to LOBBY -- the engine's per-round re-wait must UNWIND + re-front
     * CHARSELECT (never park). None in normal runs. */
    bool wedgeCancelRound2 = false;
    /* WEDGE (test-only): after a round advance completes (room left LOBBY,
     * fresh race-ready epoch) NEVER re-arm the match-input, so the engine's per-round
     * re-wait wall-clock watchdog trips at "per-round re-wait" (the deterministic
     * per-round proof). None in normal runs. */
    bool wedgeSkipRearm = false;
    /* OBSERVE-ONLY re-cycle (single-race replay + the tournament FINAL wrap).
     * A single race's REMATCH keeps race_index (only a tournament advances it),
     * so the tournament "race_index advanced" re-cycle trigger never fires for a
     * single race; and the tournament FINAL's REMATCH wrap RESETS race_index to
     * 0 (a fresh series -- lobby_core.c reset_tournament_series), which that
     * same `advanced` trigger also misses. In both cases the room LEAVES
     * RESULTS back to LOBBY with the ENGINE owning the re-drive (the native
     * re-selection screen the chooser routed to, or the LOBBY_WAIT auto-start
     * for RACE AGAIN), so arm this OBSERVE-ONLY re-cycle instead of the
     * auto-driving mid-cup advance: the launcher only clears the stale
     * roster/match-input and re-arms the match-input once the room reaches a
     * FRESH race-ready transport. NOT auto-driving is what lets the engine's
     * re-selection own the new config without the launcher racing it to START
     * on the old config. */
    bool singleObserve = false;
    unsigned singleObserveFrames = 0u; /* observe-only re-cycle watchdog */
    /* state-hash witness (log-only): the epoch whose confirmed-input
     * fold hash was already emitted, so each race logs exactly one fold line
     * (the Racing phase in liveResidentServiceStep). */
    std::uint32_t foldEmittedEpoch = 0u;
};
LiveResidentState *g_liveResident = nullptr;
static void liveResidentServiceStep(void);

/* LOBBY-START coordinator (set only by the lobby-start lane; null for
 * every existing lane). Serviced from liveOverlayService each engine frame while
 * the DESCRIPTOR-LESS session fronts its native CHARSELECT/TRACKSELECT: it pumps
 * BOTH party_link feeds for the visible (host) endpoint so the native screens'
 * selection/ready/START intents drive the REAL adapter, drives the joiner (peer)
 * toward ready, and -- once the host's START has built the descriptor + installed
 * the roster + stood up the race transport -- installs the match-input source so
 * the engine's race-1 readiness gate passes and race 1 boots. */
struct LiveLobbyStartState {
    IMdkrOnlineAdapter *visible = nullptr; /* host (A) -- the native screens */
    IMdkrOnlineAdapter *peer = nullptr;    /* joiner (B) -- driven toward ready */
    LiveMatchInputContext *ctx = nullptr;
    unsigned joinerCharacter = 1u; /* host native picks Pipsy(2); joiner != 2 */
    enum class Phase { Lobby, Racing, Done } phase = Phase::Lobby;
    /* When non-null (the TOURNAMENT lobby-start lane), the race-1 arm
     * HANDS OFF to the resident coordinator (g_liveResident) so races 2..4 re-cycle
     * in-process via the SAME machinery the resident lane uses -- the lobby-start
     * coordinator only fronts race 1. Null for the single-race lobby-start lane
     * (byte-behaviour-unchanged: it just arms race 1 and lets it race + exit). */
    LiveResidentState *resident = nullptr;
    /* SINGLE-ENDPOINT (real 2-process) session -- the handoff arms the
     * resident coordinator's single-endpoint advance (peer == nullptr in
     * production). remoteSim is TEST-ONLY (two loopback adapters, peer drives the
     * stand-in remote). */
    bool singleEndpoint = false;
    bool remoteSim = false;
    /* Propagated to the resident coordinator at handoff (RESULTS-phase /
     * round-2 wedges that fire after race 1, not Lobby-phase wedges below). */
    bool wedgeResultsHold = false;
    bool wedgeCancelRound2 = false;
    bool wedgeSkipRearm = false;
    /* WEDGE sub-tests (MDKR_APP_TEST_ONLINE_LOBBY_WEDGE): prove the engine
     * safety paths (watchdog + unwind) fire cleanly, never hang. None in normal runs. */
    enum class Wedge { None, DescriptorNeverBuilds, CancelLoading } wedge = Wedge::None;
    bool wedgeCancelDone = false; /* CancelLoading: cancel exactly once, then recover */
};
LiveLobbyStartState *g_liveLobbyStart = nullptr;
static void liveLobbyStartServiceStep(void);

extern "C" {
static void liveOverlayService(void) {
    LiveMatchInputContext *ctx = g_liveMatchInput;
    if (ctx == nullptr) return;
    if (ctx->visible != nullptr) ctx->visible->service();
    if (ctx->peer != nullptr && !ctx->peerSevered) ctx->peer->service();
    if (g_liveResident != nullptr) liveResidentServiceStep();
    if (g_liveLobbyStart != nullptr) liveLobbyStartServiceStep();
}
static int liveOverlayProcessEvent(const void * /*sdl_event*/) { return 0; }
static int liveOverlayWantsInput(void) { return 0; }
static int liveOverlayWantsPause(void) {
    /* TEST-ONLY (beta rigs): MDKR_APP_TEST_ONLINE_APP_OVERLAY_PAUSE="A-B"
     * answers the engine's wants-pause query with 1 over the authored-tick
     * window [A,B) -- byte-for-byte the FACT the real app overlay reports
     * while open with pause allowed (ui_overlay.cpp onWantsPause). The live
     * sessions bind these minimal hooks, so the real F1/Escape overlay is not
     * constructible here; this injects only that app-boundary input, and
     * everything the engine does with it is production code. Inert (constant
     * 0, the shipped behavior) without the env. */
    static long openTick = -1;
    static long closeTick = -1;
    static int resolved = 0;
    static int announced = 0;
    if (!resolved) {
        resolved = 1;
        if (const char *env =
                std::getenv("MDKR_APP_TEST_ONLINE_APP_OVERLAY_PAUSE")) {
            char *end = nullptr;
            const long open = std::strtol(env, &end, 10);
            if (end != env && *end == '-' && open > 0) {
                const char *rest = end + 1;
                const long close = std::strtol(rest, &end, 10);
                if (end != rest && *end == '\0' && close > open) {
                    openTick = open;
                    closeTick = close;
                }
            }
        }
    }
    if (openTick < 0) return 0;
    const long tick = static_cast<long>(g_simTickCounter);
    if (tick < openTick || tick >= closeTick) {
        if (announced == 1 && tick >= closeTick) {
            announced = 2;
            std::fprintf(stderr,
                         "[overlay-test] app overlay pause window CLOSED "
                         "tick=%ld\n", tick);
        }
        return 0;
    }
    if (announced == 0) {
        announced = 1;
        std::fprintf(stderr,
                     "[overlay-test] app overlay pause window OPEN tick=%ld "
                     "(wants-pause asserted; window %ld-%ld)\n",
                     tick, openTick, closeTick);
    }
    return 1;
}
static int liveOverlayWantsRender(void) { return 0; }
static int liveOverlayRender(void) { return 1; }
}  // extern "C"

/* Overlay hooks shared by all three visible live-engine sessions (loopback live,
 * lobby-start loopback, lobby-start production). */
static const AppOverlayHooks sLiveOverlayHooks = {
    liveOverlayProcessEvent, liveOverlayService, liveOverlayWantsInput,
    liveOverlayWantsPause, liveOverlayWantsRender, liveOverlayRender,
};

/* Bind the live overlay + host window/WebGPU to the platform for the duration of a
 * visible online boot; liveEngineHostUnbind() reverses it. Shared by all three
 * visible live-engine sessions. */
static void liveEngineHostBind(AppHost &host) {
    platformSetOverlayHooks(&sLiveOverlayHooks);
    platformSetHostWindow(host.window(), host.glContext());
    if (host.usingWebGpu()) {
        platformSetHostWebGpu(host.wgpuInstance(), host.wgpuAdapter(),
                              host.wgpuDevice(), host.wgpuQueue(),
                              host.wgpuSurface(), host.wgpuFormat());
        platformSetHostWebGpuRecovery(recoverAppHostWebGpu, &host);
    }
}

static void liveEngineHostUnbind(void) {
    platformSetOverlayHooks(nullptr);
    platformSetHostWebGpuRecovery(nullptr, nullptr);
    platformSetHostWebGpu(nullptr, nullptr, nullptr, nullptr, nullptr, 0);
    platformSetHostWindow(nullptr, nullptr);
}

/* TEST-ONLY (beta) race-start peer-loss seam. When
 * MDKR_APP_TEST_ONLINE_DROP_RACE_START_INPUT is set, liveDrainMatchInput refuses
 * the FIRST authored tick's drain -- exactly what the production race-start
 * barrier does when the peer LOST before ever delivering tick-1 (ICE failed /
 * opponent vanished; see the barrier at drainTick == firstTick below). The engine
 * then reaches its tick-1 canonical-input boundary with no input, which is the P0
 * crash's trigger: rollback's validate_boundary reports the RECOVERABLE
 * "online bootstrap input unavailable tick=1" starvation. Inert (resolved once)
 * in every normal run. */
static bool liveTestDropRaceStartInput(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *env =
            std::getenv("MDKR_APP_TEST_ONLINE_DROP_RACE_START_INPUT");
        cached = (env != nullptr && std::strtoul(env, nullptr, 10) > 0ul) ? 1 : 0;
    }
    return cached != 0;
}

/* TEST-ONLY (beta) MID-RACE peer-loss seam. When
 * MDKR_APP_TEST_ONLINE_DROP_INPUT_AT_TICK=<N> (N > firstTick) is set,
 * liveDrainMatchInput refuses the drain for that ONE authored tick -- standing in
 * for a peer/console that dropped cleanly MID-RACE (the real "console drops
 * mid-race" case). The engine's prepare_tick canonical-input drain for tick N then
 * fails ([ROLLBACK] launcher input provider rejected tick=N) -> the sibling of the
 * race-start crash. 0 / unset == off. Resolved once. */
static std::uint32_t liveTestDropInputAtTick(void) {
    static long cached = -1;
    if (cached < 0) {
        const char *env = std::getenv("MDKR_APP_TEST_ONLINE_DROP_INPUT_AT_TICK");
        const long parsed = (env != nullptr) ? std::strtol(env, nullptr, 10) : 0;
        cached = (parsed > 0) ? parsed : 0;
    }
    return static_cast<std::uint32_t>(cached);
}

/* TEST-ONLY (beta) mid-race TRANSPORT-SEVER seam. When
 * MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK=<N> (N > firstTick) is set on a
 * loopback rig, the drain HARD-severs the in-process peer at authored tick N:
 * the peer adapter is never serviced again (a frozen corpse -- its mesh stops
 * ponging and draining, exactly what a SIGKILLed remote process looks like to
 * the transport) and the loopback signal hub broadcasts its presence=false
 * (what the real signal service does when the killed process's socket dies).
 * Unlike the DROP_* input seams this REFUSES NOTHING: the race keeps
 * predicting on the production path and every detection must come from the
 * transport's own liveness ladders (control-ping stale -> [MESH] peer LOST ->
 * the mid-race latch). 0 / unset == off. */
static std::uint32_t liveTestSeverPeerAtTick(void) {
    static long cached = -1;
    if (cached < 0) {
        const char *env = std::getenv("MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK");
        const long parsed = (env != nullptr) ? std::strtol(env, nullptr, 10) : 0;
        cached = (parsed > 0) ? parsed : 0;
    }
    return static_cast<std::uint32_t>(cached);
}

/* TEST-ONLY sibling of the tick sever (MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_
 * RESULTS): HARD-sever the in-process peer the moment the resident
 * coordinator enters its RESULTS hold -- the near-finish kill shape, where
 * the loss lands DURING the post-race waits after a genuine captured finish.
 * The identical fault as the tick seam (pump frozen + loopback presence
 * dropped), refusing nothing: detection must come from the transport's own
 * ladders. Unset == off. */
static bool liveTestSeverPeerAtResults(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *env =
            std::getenv("MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_RESULTS");
        cached = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

/* TEST-ONLY variant selector for the tick sever
 * (MDKR_APP_TEST_ONLINE_SEVER_LINGER_PRESENCE=1): keep the frozen peer's
 * loopback signal PRESENCE asserted (the Worker was slow to drop it, or is
 * itself down -- the real-cloud lingering-presence kill signature) and force
 * the ICE-down promptly through the mesh's existing kill-channels seam, so
 * the survivor's loss resolves through the presence-asserted restart ladder
 * (ConnectTimeout), never the presence-drop vanish/ping path. Unset == the
 * original presence-dropping sever. */
static bool liveTestSeverLingerPresence(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *env =
            std::getenv("MDKR_APP_TEST_ONLINE_SEVER_LINGER_PRESENCE");
        cached = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

/* TEST-ONLY (beta) ROOM-ONLY departure seam. When
 * MDKR_APP_TEST_ONLINE_ROOM_DEPARTURE_AT_TICK=<N> (N > firstTick) is set on a
 * loopback rig, the loopback signal hub broadcasts the in-process peer's
 * presence=false at authored tick N -- and NOTHING else. The peer keeps being
 * pumped, keeps sealing input and keeps answering pings, so the survivor sees
 * exactly the room-service wobble the departure grace exists for: the room
 * says a member left while that member is plainly still racing. The sever seam
 * above is the opposite signature (the peer really is gone), and the two can
 * be armed together to drop the room's verdict first and kill the peer later.
 * 0 / unset == off. */
static std::uint32_t liveTestRoomDepartureAtTick(void) {
    static long cached = -1;
    if (cached < 0) {
        const char *env =
            std::getenv("MDKR_APP_TEST_ONLINE_ROOM_DEPARTURE_AT_TICK");
        const long parsed =
            (env != nullptr) ? std::strtol(env, nullptr, 10) : 0;
        cached = (parsed > 0) ? parsed : 0;
    }
    return static_cast<std::uint32_t>(cached);
}

/* Advance the visible endpoint's race transport up to `tick` (idempotent), then
 * copy the canonical frame for `tick`. Authored ticks are 1-based and align with
 * the adapter's raceFirstTick (1), so one drain == one race_advance. */
bool liveDrainMatchInput(void *opaque, std::uint32_t /*epoch*/,
                         std::uint32_t tick,
                         const MdkrPadSample *physical, unsigned count,
                         MdkrInputSet *out) {
    LiveMatchInputContext *ctx = static_cast<LiveMatchInputContext *>(opaque);
    if (ctx == nullptr || ctx->visible == nullptr || out == nullptr) return false;
    ctx->drainCalls++;
    /* Feed the REAL local controller into the visible endpoint before it seals
     * and drains, so the committed canonical frame is the player's input rather
     * than the raceLocalSample fixture. The adapter ignores this when a test
     * seam has selected synthetic input (see runOnlineLiveEngineSession). */
    mdkr_online_live_adapter_race_set_local_input(ctx->visible, physical, count);
    MdkrOnlineLiveRaceInfo info{};
    if (!mdkr_online_live_adapter_race_info(ctx->visible, &info) || !info.ready) {
        return false;
    }
    /* Keep the in-process peer this many authored ticks ahead of the visible
     * endpoint's drain frontier, so its sealed input for the tick we are about
     * to commit has already crossed the loopback mesh. */
    const std::uint32_t lead = static_cast<std::uint32_t>(info.inputDelay) + 2u;
    for (unsigned guard = 0u; info.nextTick <= tick && guard < 100000u; ++guard) {
        const std::uint32_t drainTick = info.nextTick;
        /* TEST-ONLY: drop the FIRST authored tick's remote/bootstrap input to
         * stand in for a peer that vanished at race start (the production barrier
         * below reaches this same "aborting to the room" verdict on a real peer
         * loss). The engine's tick-1 boundary then starves -> the P0 crash lane's
         * exact trigger. Reason mirrors the barrier's OpponentNeverStarted. */
        if (drainTick == info.firstTick && liveTestDropRaceStartInput()) {
            ctx->endReason = LiveRaceEndReason::OpponentNeverStarted;
            std::fprintf(stderr,
                         "[online-live] TEST: race-start tick-%u remote input "
                         "UNAVAILABLE (peer-loss seam); aborting to the room\n",
                         drainTick);
            return false;
        }
        /* End the visible race the instant a roster peer is lost mid-race,
         * rather than silently predicting against a frozen ghost all the way to
         * the finish line. The start barrier (drainTick == firstTick) reports
         * its own OpponentNeverStarted reason below, so only a loss AFTER the
         * opening tick counts as OpponentLeft here. Serviced every engine frame
         * by liveOverlayService, so the latch is at most one frame stale. */
        if (drainTick > info.firstTick &&
            mdkr_online_live_adapter_race_peer_lost(ctx->visible)) {
            ctx->endReason = LiveRaceEndReason::OpponentLeft;
            std::fprintf(stderr,
                         "[online-live] peer lost mid-race at tick %u; ending "
                         "session (no ghost race)\n",
                         drainTick);
            return false;
        }
        /* TEST-ONLY: drop this ONE mid-race authored tick's remote input to stand
         * in for a peer that dropped cleanly mid-race (same verdict the real
         * mid-race peer-loss latch above reaches). The engine's prepare_tick drain
         * for this tick then starves -> the sibling crash's exact trigger. */
        {
            const std::uint32_t dropAt = liveTestDropInputAtTick();
            if (dropAt != 0u && drainTick == dropAt &&
                drainTick > info.firstTick) {
                ctx->endReason = LiveRaceEndReason::OpponentLeft;
                std::fprintf(stderr,
                             "[online-live] TEST: mid-race tick-%u remote input "
                             "UNAVAILABLE (peer-loss seam); ending session\n",
                             drainTick);
                return false;
            }
        }
        /* TEST-ONLY (beta): drop ONLY the in-process peer's room presence at
         * this authored tick (see liveTestRoomDepartureAtTick). The peer is
         * left running, so the survivor must decide between the room's
         * verdict and a peer that is still sending. */
        {
            const std::uint32_t departAt = liveTestRoomDepartureAtTick();
            if (departAt != 0u && !ctx->peerRoomDeparted &&
                drainTick >= departAt && drainTick > info.firstTick &&
                ctx->severRig != nullptr) {
                ctx->peerRoomDeparted = true;
                OnlineRoom_testLoopbackSeverPeerPresence(ctx->severRig);
                std::fprintf(stderr,
                             "[online-live] TEST: peer ROOM PRESENCE dropped "
                             "at tick %u (peer still racing)\n",
                             drainTick);
            }
        }
        /* TEST-ONLY (beta): HARD-SEVER the in-process peer's transport at
         * this authored tick (see liveTestSeverPeerAtTick). Refuses nothing;
         * the drain falls through to the production predict branch and the
         * transport's own liveness ladders must surface the loss. */
        {
            const std::uint32_t severAt = liveTestSeverPeerAtTick();
            if (severAt != 0u && !ctx->peerSevered && drainTick >= severAt &&
                drainTick > info.firstTick) {
                ctx->peerSevered = true;
                const bool linger = liveTestSeverLingerPresence();
                if (!linger && ctx->severRig != nullptr) {
                    OnlineRoom_testLoopbackSeverPeerPresence(ctx->severRig);
                }
                if (linger) {
                    /* Lingering-presence variant: presence stays asserted;
                     * force the ICE-down instead so the survivor's restart
                     * ladder (not the vanish/ping path) resolves the loss. */
                    (void)mdkr_online_live_adapter_test_kill_peer_channels(
                        ctx->visible);
                }
                std::fprintf(stderr,
                             "[online-live] TEST: peer transport SEVERED at "
                             "tick %u (pump frozen + %s; "
                             "detection must come from the transport)\n",
                             drainTick,
                             linger ? "presence LINGERS + channels killed"
                                    : "presence dropped");
            }
        }
        if (ctx->peer != nullptr && !ctx->peerSevered) {
            /* Advance the peer ahead: each advance seals its deterministic input
             * for (nextTick + inputDelay) and fans it out, so ticks up to
             * drainTick+lead are already in flight. The peer's own drain uses
             * prediction for the not-yet-sent visible input and is discarded;
             * only its mesh output feeds the visible engine. */
            /* Test-only: advance a bounded window of active-race ticks with
             * PREDICTED input instead of waiting for confirmation, so the peer's
             * real input lands a drain later and the engine rolls back. Gated by
             * MDKR_APP_TEST_ONLINE_LIVE_PREDICT; only fires once the race has
             * unpaused (nextTick past the countdown), so it reproduces a
             * mid-race joiner rollback rather than a countdown correction the
             * engine rejects anyway. During the window the peer is held a few
             * ticks BEHIND the commit frontier so its sealed input for the tick
             * being committed has NOT crossed the mesh yet -- exactly the missing
             * confirmation a real network round trip creates. */
            const bool predictThisTick =
                ctx->predictWindowTicks > 0u &&
                ctx->predictTicksDone < ctx->predictWindowTicks &&
                drainTick > lead + 1u;
            const std::uint32_t peerBound =
                predictThisTick ? (drainTick - lead - 1u) : (drainTick + lead);
            MdkrOnlineLiveRaceInfo pinfo{};
            while (mdkr_online_live_adapter_race_info(ctx->peer, &pinfo) &&
                   pinfo.ready && pinfo.nextTick <= peerBound) {
                ctx->peer->service();
                if (!mdkr_online_live_adapter_race_advance(ctx->peer)) break;
            }
            if (predictThisTick) {
                ctx->visible->service();
                ctx->predictTicksDone++;
            } else {
                /* Deliver drainTick's remote input synchronously (like the
                 * loopback simulator this replaces): pump the visible endpoint
                 * until the peer's input for drainTick has been received, so the
                 * drain commits fully-confirmed input and the engine never rolls
                 * back into the paused race countdown. */
                for (unsigned spin = 0u; spin < 4000u; ++spin) {
                    ctx->visible->service();
                    if (mdkr_online_live_adapter_race_remote_ready(ctx->visible,
                                                                   drainTick)) {
                        break;
                    }
                    SDL_Delay(1u);
                }
            }
        } else {
            /* Production: the real remote process supplies input over the mesh;
             * predict now and let the engine's rollback correct via take_dirty. */
            ctx->visible->service();
            /* RACE-START BARRIER. The two machines boot their engines seconds
             * apart (loading, ACK round-trips), and an engine that commits
             * authored ticks before the peer's FIRST bundle crosses the WAN can
             * outrun the 32-snapshot rollback window in under a second at 30Hz.
             * The transport then marks those early ticks unrecoverable-late and
             * the two endpoints permanently retain different canonical input
             * history for the countdown (caught by the fold-hash gate; the sim
             * usually survives because the countdown ignores input — but the
             * histories must match). Hold the FIRST authored tick until the
             * peer's input for it has actually arrived; bounded so a dead peer
             * degrades to today's predict-and-recover behavior. */
            if (drainTick == info.firstTick) {
                /* Prime FIRST: seal + fan out our own opening window so the
                 * peer's identical barrier can release — without this each
                 * side's first seal only happens inside the drain this
                 * barrier blocks, and the two machines deadlock into their
                 * timeouts. Re-fan the (byte-identical) window every ~200ms
                 * while waiting: the peer may not even have joined the mesh
                 * when the first send goes out. */
                (void)mdkr_online_live_adapter_race_prime_start(ctx->visible);
                bool remoteArrived = false;
                bool peerLostAtBarrier = false;
                for (unsigned spin = 0u; spin < 30000u; ++spin) {
                    if (mdkr_online_live_adapter_race_remote_ready(ctx->visible,
                                                                   drainTick)) {
                        remoteArrived = true;
                        break;
                    }
                    /* The opponent's transport died before ever delivering
                     * tick-1: stop waiting immediately rather than burning the
                     * full 30 s budget. */
                    if (mdkr_online_live_adapter_race_peer_lost(ctx->visible)) {
                        peerLostAtBarrier = true;
                        break;
                    }
                    if ((spin % 200u) == 199u) {
                        (void)mdkr_online_live_adapter_race_prime_start(
                            ctx->visible);
                    }
                    ctx->visible->service();
                    SDL_Delay(1u);
                }
                if (!remoteArrived) {
                    /* NEVER author the opening tick against a peer that never
                     * started. Aborting here (before the first race_advance)
                     * keeps the local player out of a one-sided ghost race and
                     * routes them to the OPPONENT_NEVER_STARTED recovery card.
                     * The witness line is kept truthful for the test lanes. */
                    ctx->endReason = LiveRaceEndReason::OpponentNeverStarted;
                    /* Tell a slow-but-alive opponent we are aborting, so it
                     * never passes its own barrier on our primed tick-1 fan-out
                     * and races our frozen input to the flag. No-op if the peer
                     * is already gone (peerLostAtBarrier). */
                    (void)mdkr_online_live_adapter_race_send_abort(ctx->visible);
                    std::fprintf(stderr,
                                 "[START] race-start barrier: remote tick-%u "
                                 "input %s; aborting to the room\n",
                                 drainTick,
                                 peerLostAtBarrier ? "peer lost"
                                                   : "TIMED OUT");
                    return false;
                }
                std::fprintf(stderr,
                             "[START] race-start barrier: remote tick-%u input "
                             "arrived\n",
                             drainTick);
            }
            if (ctx->paceAdvanceHz > 0u) {
                /* Test-only (see LiveMatchInputContext::paceAdvanceHz): hold this
                 * authored tick's advance to roughly the authored cadence so a
                 * REAL peer process's confirmations over a genuine network round
                 * trip have real wall-clock time to land, instead of the
                 * confirmed frontier falling permanently behind an unthrottled
                 * headless drain rate it can never catch. */
                const std::uint64_t intervalMs = 1000u / ctx->paceAdvanceHz;
                if (ctx->lastAdvanceMs != 0u) {
                    while (SDL_GetTicks64() < ctx->lastAdvanceMs + intervalMs) {
                        ctx->visible->service();
                        SDL_Delay(1u);
                    }
                }
                ctx->lastAdvanceMs = SDL_GetTicks64();
            }
        }
        if (!mdkr_online_live_adapter_race_advance(ctx->visible)) {
            ctx->advanceFailed = true;
            ctx->endReason = LiveRaceEndReason::AdvanceFailed;
            return false;
        }
        (void)mdkr_online_live_adapter_race_info(ctx->visible, &info);
    }
    if (tick > ctx->racedTicks) ctx->racedTicks = tick;
    return mdkr_online_live_adapter_race_inputs_for_tick(ctx->visible, tick, out);
}

bool liveInputsForTick(void *opaque, std::uint32_t /*epoch*/,
                       std::uint32_t tick, MdkrInputSet *out) {
    LiveMatchInputContext *ctx = static_cast<LiveMatchInputContext *>(opaque);
    return ctx != nullptr && ctx->visible != nullptr && out != nullptr &&
           mdkr_online_live_adapter_race_inputs_for_tick(ctx->visible, tick, out);
}

bool liveTakeDirtyMatchInput(void *opaque, std::uint32_t /*epoch*/,
                             std::uint32_t *tick) {
    LiveMatchInputContext *ctx = static_cast<LiveMatchInputContext *>(opaque);
    return ctx != nullptr && ctx->visible != nullptr && tick != nullptr &&
           mdkr_online_live_adapter_race_take_dirty(ctx->visible, tick);
}

bool liveAiMaskMatchInput(void *opaque, std::uint32_t /*epoch*/,
                          std::uint32_t tick, std::uint8_t *slotMask) {
    LiveMatchInputContext *ctx = static_cast<LiveMatchInputContext *>(opaque);
    return ctx != nullptr && ctx->visible != nullptr && slotMask != nullptr &&
           mdkr_online_live_adapter_race_ai_mask(ctx->visible, tick, slotMask);
}

/* Fold the confirmed canonical frames one endpoint retained over [firstTick..]
 * into an FNV-1a state hash, stopping at the first tick not fully confirmed.
 * Returns how many ticks were folded. Two independent endpoints that converged
 * on the same authority produce byte-identical hashes over the same span. */
std::uint32_t foldConfirmedRace(IMdkrOnlineAdapter *adapter,
                                std::uint32_t firstTick, std::uint32_t lastTick,
                                std::uint8_t activeMask, std::uint64_t *hash) {
    std::uint64_t h = UINT64_C(1469598103934665603);
    std::uint32_t folded = 0u;
    for (std::uint32_t tick = firstTick; tick <= lastTick; ++tick) {
        MdkrInputSet frame{};
        if (!mdkr_online_live_adapter_race_inputs_for_tick(adapter, tick,
                                                           &frame)) {
            break;
        }
        if ((frame.confirmed_mask & activeMask) != activeMask) break;
        auto mix = [&h](std::uint64_t value) {
            for (unsigned b = 0u; b < 8u; ++b) {
                h ^= (value >> (b * 8u)) & 0xffu;
                h *= UINT64_C(1099511628211);
            }
        };
        mix(tick);
        for (unsigned slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; ++slot) {
            if ((activeMask & (1u << slot)) == 0u) continue;
            mix(frame.slots[slot].buttons);
            mix(static_cast<std::uint8_t>(frame.slots[slot].stick_x));
            mix(static_cast<std::uint8_t>(frame.slots[slot].stick_y));
        }
        ++folded;
    }
    if (hash != nullptr) *hash = h;
    return folded;
}

/* Install the live match-input source for `ctx` at `epoch`/`activeMask`, resetting
 * the per-race counters so a (re-)armed round starts clean. Returns the install
 * result; each caller owns its own failure log + unwind (they differ per session)
 * and a re-arm site clears the prior install before calling. */
static bool armLiveMatchInput(LiveMatchInputContext *ctx, std::uint32_t epoch,
                              std::uint8_t activeMask) {
    ctx->epoch = epoch;
    ctx->activeMask = activeMask;
    ctx->racedTicks = 0u;
    ctx->drainCalls = 0u;
    ctx->advanceFailed = false;
    ctx->endReason = LiveRaceEndReason::Completed;
    const MdkrMatchInputSource source = {
        MDKR_MATCH_INPUT_SOURCE_VERSION, epoch, ctx,
        liveDrainMatchInput,   liveInputsForTick,
        liveTakeDirtyMatchInput, liveAiMaskMatchInput,
    };
    return mdkr_match_input_runtime_install(&source);
}

int runOnlineLiveEngineSession(AppHost &host, const MdkrBootConfig &config,
                               IMdkrOnlineAdapter *visible,
                               IMdkrOnlineAdapter *peer,
                               unsigned paceAdvanceHz = 0u,
                               bool syntheticInput = false,
                               LiveRaceEndReason *endReasonOut = nullptr,
                               bool resident = false) {
    if (endReasonOut != nullptr) *endReasonOut = LiveRaceEndReason::Completed;
    if (visible == nullptr) return 2;
    if (!mdkr_net_roster_runtime_active()) {
        std::fprintf(stderr,
                     "[online-live] refused: no engine roster installed\n");
        return 2;
    }
    MdkrOnlineLiveRaceInfo info{};
    if (!mdkr_online_live_adapter_race_info(visible, &info) || !info.ready ||
        info.matchEpoch == 0u) {
        std::fprintf(stderr,
                     "[online-live] refused: visible race transport not ready\n");
        return 2;
    }
    /* Explicit ownership: tag the roster this online session installed so a
     * later local-Play boot's guard force-clears it instead of inheriting it. */
    const std::uint64_t ownerToken =
        UINT64_C(0x4f4e4c49564500) ^ static_cast<std::uint64_t>(info.matchEpoch);
    OnlineRoom_setRosterOwner(ownerToken);

    /* The transport/rollback test seams (loopback + cloud) drive the race from
     * the deterministic raceLocalSample fixture, which varies per tick to force
     * genuine corrections and converges byte-for-byte across endpoints. The
     * shipped interactive boot leaves this false, so the race commits the real
     * controller fed through liveDrainMatchInput -> race_set_local_input. */
    if (syntheticInput) {
        mdkr_online_live_adapter_race_set_synthetic_input(visible, true);
        if (peer != nullptr) {
            mdkr_online_live_adapter_race_set_synthetic_input(peer, true);
        }
    }

    LiveMatchInputContext context;
    context.visible = visible;
    context.peer = peer;
    context.paceAdvanceHz = paceAdvanceHz;
    if (const char *predict =
            std::getenv("MDKR_APP_TEST_ONLINE_LIVE_PREDICT")) {
        char *end = nullptr;
        const long parsed = std::strtol(predict, &end, 10);
        if (end != predict && *end == '\0' && parsed > 0 && parsed <= 100000) {
            context.predictWindowTicks = static_cast<unsigned>(parsed);
        }
    }

    if (!armLiveMatchInput(&context, info.matchEpoch, info.activeSlotMask)) {
        std::fprintf(stderr,
                     "[online-live] refused: duplicate match input provider\n");
        mdkr_net_roster_runtime_clear();
        return 2;
    }
    g_liveMatchInput = &context;

    /* RESIDENT LIVE: install the party_link bridge for the whole session
     * and arm the per-frame coordinator (g_liveResident) so the session spans
     * >= 2 races in THIS ONE engine boot -- the native RESULTS screen fronting on
     * the real reducer feed between races, the host advance driving a REAL REMATCH
     * via the reverse feed, and race N+1 re-cycled + booted in-process. Only the
     * resident-live lane sets `resident`; every existing lane leaves it false, so
     * the bridge is never installed and the coordinator never runs (byte-behavior
     * unchanged). */
    LiveResidentState residentState;
    if (resident) {
        OnlineRoom_installPartyLink();
        residentState.visible = visible;
        residentState.peer = peer;
        residentState.ctx = &context;
        g_liveResident = &residentState;
        std::fprintf(stderr,
                     "[online-resident-live] residency armed (party_link bridge "
                     "installed; per-round re-cycle coordinator live)\n");
    }

    liveEngineHostBind(host);

    std::fprintf(stderr,
                 "[online-live] booting visible engine epoch=%u active=0x%02x "
                 "local=0x%02x remote=0x%02x inputDelay=%u peer=%d\n",
                 static_cast<unsigned>(info.matchEpoch),
                 static_cast<unsigned>(info.activeSlotMask),
                 static_cast<unsigned>(info.localSlotMask),
                 static_cast<unsigned>(info.remoteSlotMask),
                 static_cast<unsigned>(info.inputDelay), peer != nullptr ? 1 : 0);

    /* Online epoch: the engine sees the NTSC source identity (rom_io.c) so a
     * PAL ROM races the 30 Hz manifest bit-identically to US; cleared as soon
     * as the bounded boot returns. */
    int result;
    {
        OnlineNtscIdentityScope ntscIdentityScope(true);
        result = mdkr64_engine_boot(&config);
    }

    /* Flush any in-flight input so both endpoints have folded the same recent
     * window before we compare (the peer may still owe the visible endpoint's
     * last few drained inputs). The loopback proof's peer is cross-pumped
     * synchronously during the race itself (see liveDrainMatchInput), so 100
     * iterations (~100ms) is already generous there; a REAL cross-process
     * peer's straggling confirmations for the last few authored ticks travel
     * an actual network round trip, so give paced (cross-process) sessions
     * much more real wall-clock time to land before the trailing fold window
     * is read. */
    const unsigned settleIterations = paceAdvanceHz > 0u ? 5000u : 100u;
    /* Test-only (paceAdvanceHz gate): the trailing window about to be folded
     * below, precomputed here so the settle loop can periodically re-offer
     * it. race_advance() only ever seals and sends EACH tick's bundle once (a
     * 3-frame window); on the lossy, unordered, maxRetransmits-0 state
     * channel, a single dropped datagram can leave one tick in that exact
     * window unconfirmed forever, with no in-race resend sweep (unlike
     * tests/test_online_live_transport_e2e_driver.cpp's own) to recover it. */
    std::uint32_t resendFoldStart = 0u;
    std::uint32_t resendFoldEnd = 0u;
    if (paceAdvanceHz > 0u && context.racedTicks >= info.firstTick + 8u) {
        resendFoldEnd = context.racedTicks - 6u;
        const std::uint32_t window = 60u;
        resendFoldStart = resendFoldEnd > info.firstTick + window
                              ? resendFoldEnd - window
                              : info.firstTick;
    }
    for (unsigned settle = 0u; settle < settleIterations; ++settle) {
        visible->service();
        if (peer != nullptr) peer->service();
        if (resendFoldEnd > 0u && (settle % 100u) == 0u) {
            for (std::uint32_t t = resendFoldEnd;;) {
                mdkr_online_live_adapter_race_resend(visible, t);
                if (t <= resendFoldStart) break;
                t = t >= resendFoldStart + 3u ? t - 3u : resendFoldStart;
            }
        }
        SDL_Delay(1u);
    }
    MdkrOnlineLiveRaceStats stats{};
    (void)mdkr_online_live_adapter_race_stats(visible, &stats);
    std::uint64_t hashVisible = 0u;
    std::uint64_t hashPeer = 0u;
    std::uint32_t foldVisible = 0u;
    std::uint32_t foldPeer = 0u;
    bool converged = false;
    /* Fold a recent CONFIRMED window rather than from tick 1: the net_input ring
     * only retains the last MDKR_NET_INPUT_CAPACITY (128) authored ticks, so the
     * opening ticks are long evicted after a long race. Two independent endpoints
     * that stayed converged fold the identical window to the identical hash. */
    if (context.racedTicks >= info.firstTick + 8u) {
        const std::uint32_t foldEnd = context.racedTicks - 6u;
        const std::uint32_t window = 60u;
        const std::uint32_t foldStart =
            foldEnd > info.firstTick + window ? foldEnd - window : info.firstTick;
        foldVisible = foldConfirmedRace(visible, foldStart, foldEnd,
                                        info.activeSlotMask, &hashVisible);
        if (peer != nullptr) {
            foldPeer = foldConfirmedRace(peer, foldStart, foldEnd,
                                         info.activeSlotMask, &hashPeer);
            converged = foldVisible > 0u && foldVisible == foldPeer &&
                        hashVisible == hashPeer;
        }
    }
    std::fprintf(
        stderr,
        "[ENGINE-ONLINE-LIVE] result=%d racedTicks=%u drainCalls=%llu "
        "advanceFailed=%d inputEnvelopes=%llu transportAccepted=%u "
        "transportCorrected=%u transportDrained=%u foldVisible=%u foldPeer=%u "
        "hashVisible=%016llx hashPeer=%016llx converged=%d\n",
        result, static_cast<unsigned>(context.racedTicks),
        static_cast<unsigned long long>(context.drainCalls),
        context.advanceFailed ? 1 : 0,
        static_cast<unsigned long long>(stats.inputEnvelopesReceived),
        stats.transportAccepted, stats.transportCorrected, stats.transportDrained,
        static_cast<unsigned>(foldVisible), static_cast<unsigned>(foldPeer),
        static_cast<unsigned long long>(hashVisible),
        static_cast<unsigned long long>(hashPeer), converged ? 1 : 0);

    /* Hand the end reason to the caller; the post-session view routing +
     * results decision are made together in reportOnlineRaceResults, which owns
     * the single (one-shot) results poll -- the disconnect card must NOT
     * suppress a genuinely captured finish. */
    if (endReasonOut != nullptr) *endReasonOut = context.endReason;

    liveEngineHostUnbind();
    g_liveMatchInput = nullptr;
    /* Retire the resident coordinator + party_link bridge (session end).
     * No-op when not resident. */
    if (resident) {
        g_liveResident = nullptr;
        OnlineRoom_clearPartyLink();
    }
    mdkr_match_input_runtime_clear();
    /* This boot owned the roster: retire it so nothing downstream inherits it.
     * Multi-race rooms REQUIRE this clear: the runtime install is once-only
     * while a roster is installed, and the adapter re-arms its own install
     * latch (resetRaceLatches -> installed_ = false, "the launcher clears the
     * process-global roster between races") when the lobby returns to LOBBY,
     * so the next BEGIN_LOADING re-installs a fresh roster for race 2. */
    mdkr_net_roster_runtime_clear();
    /* The engine has fully unwound (mdkr64_engine_boot is blocking). Re-arm
     * exactly one launcher video-config handoff so a LATER engine boot in this
     * same process (the per-race fallback's next race, or a later native
     * takeover) can pass mdkr_video_config_handoff_to_engine again -- the
     * handoff is one-shot per engine session, and runEngineSession's teardown
     * does the same. Without this, the SECOND boot in a process stops at
     * "[app] video-config handoff was missing or repeated". */
    if (!mdkr_video_config_engine_session_complete()) {
        std::fprintf(stderr,
                     "[session] video-config engine epoch did not close cleanly\n");
    }
    return result;
}

/* Race-end results handoff + post-session view routing (launcher -> room),
 * shared by the interactive race-boot handoff and the loopback proof. This owns
 * the single, one-shot results poll (mdkr_online_race_results_poll consumes the
 * epoch), so the publish decision and the recovery-card decision are made
 * together here.
 *
 * The completion signal is the CAPTURE, not the end reason. If the engine
 * committed a finish order (poll returns placements), those placements are
 * genuine even when the opponent then quit during the ~2.5 s post-race window;
 * publish them and show results, and do NOT front a disconnect card. Only when
 * NOTHING was captured (a mid-race peer loss / a start-barrier abort ends the
 * engine before the finish) do we suppress the publish and, for the abnormal
 * end reasons, route the survivor to the dedicated recovery card instead of the
 * generic connection-lost copy the in-race PeerLost latch mapped. */
void reportOnlineRaceResults(
    IMdkrOnlineAdapter *adapter,
    LiveRaceEndReason endReason = LiveRaceEndReason::Completed) {
    if (adapter == nullptr) return;
    MdkrOnlineLiveRaceInfo endInfo{};
    if (mdkr_online_live_adapter_race_info(adapter, &endInfo) &&
        endInfo.peerLost) {
        std::fprintf(stderr,
                     "[online-live] race ended with peerLost=1 "
                     "(endReason=%d)\n",
                     static_cast<int>(endReason));
    }
    uint8_t placements[MDKR_ONLINE_RACE_RESULT_SLOTS];
    if (mdkr_online_race_results_poll(placements)) {
        /* Genuine finish order was committed before the session ended -- even a
         * post-race-window quit does not erase it. Publish + show results; never
         * a disconnect card. */
        const bool reported =
            mdkr_online_live_adapter_report_results(adapter, placements);
        /* The same mid-race PeerLost that ended the session also latched a
         * CONNECTION_CHECK-class failure on the view; the view builder gives any
         * failure precedence over RESULTS, so clear that loss-mapped latch now
         * (and only it) so the freshly published RESULTS phase fronts.
         * Gate on a SUCCESSFUL publish -- if report_results was refused (a
         * leader's PUBLISH_RESULTS never landed, or a joiner never got the
         * snapshot) there is no RESULTS phase to front, so clearing the latch
         * would strand the player on a blank room instead of the truthful
         * recovery card. Keep the card in that case. */
        if (reported) {
            mdkr_online_live_adapter_clear_race_loss_failure(adapter);
        } else {
            /* Publish failed, so the loss-mapped recovery card is kept. Walk
             * the abandoned race's engine out of RACING so that kept card's
             * PLAY_HERE -> RETURN_HOME is ACCEPTED (the reducer refuses it while
             * still RACING), instead of a dead button. */
            mdkr_online_live_adapter_walk_engine_out_of_race(adapter);
        }
        std::fprintf(stderr,
                     "[online-live] race results reported placements=%u,%u,%u,%u "
                     "accepted=%d\n",
                     static_cast<unsigned>(placements[0]),
                     static_cast<unsigned>(placements[1]),
                     static_cast<unsigned>(placements[2]),
                     static_cast<unsigned>(placements[3]), reported ? 1 : 0);
        return;
    }
    /* Nothing captured: the race never reached the finish. Suppress the publish
     * (no fabricated placements for a vanished peer) and, when the end was an
     * opponent disconnect / never-start, front the dedicated recovery card. */
    if (endReason == LiveRaceEndReason::OpponentLeft) {
        mdkr_online_live_adapter_set_race_end_failure(
            adapter, MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT);
    } else if (endReason == LiveRaceEndReason::OpponentNeverStarted) {
        mdkr_online_live_adapter_set_race_end_failure(
            adapter, MDKR_ONLINE_VIEW_FAILURE_OPPONENT_NEVER_STARTED);
    }
    std::fprintf(stderr,
                 "[online-live] race results: no results captured "
                 "(endReason=%d)\n",
                 static_cast<int>(endReason));
}

/* Deliberate local leave (session end LEFT) with the race transport still up:
 * tell the peer NOW over the reliable control channel -- the existing
 * race_abort, the same message the race-start barrier sends -- so the survivor
 * ends its race on the immediate typed opponent-left path instead of
 * ghost-racing into the 20-30 s loss ladders. Nothing transport-level says
 * anything otherwise: the leaver returns to the ROOM, so its mesh stays up and
 * keeps answering pings. Skipped when no race transport ever armed (pre-race
 * backouts have their own vacate handling) and when the LEFT was itself caused
 * by the PEER's loss (nobody is left to notify). */
void notifyPeerOfDeliberateLeave(IMdkrOnlineAdapter *adapter,
                                 MdkrPartyLinkSessionEndReason reason) {
    if (adapter == nullptr || reason != MDKR_PARTY_LINK_SESSION_END_LEFT) {
        return;
    }
    MdkrOnlineLiveRaceInfo info{};
    if (!mdkr_online_live_adapter_race_info(adapter, &info) || !info.ready) {
        return;
    }
    if (mdkr_online_live_adapter_race_peer_lost(adapter)) return;
    if (mdkr_online_live_adapter_race_send_abort(adapter)) {
        std::fprintf(stderr,
                     "[online-live] deliberate leave: race-abort sent to the "
                     "peer\n");
    }
}

/* Session return after a mesh peer loss: latch the truthful opponent-left
 * recovery card. Covers the loss that lands in the POST-RACE window (peer
 * killed within the detection bound of the survivor's finish): the race
 * completed genuinely, the in-race mapping was demoted or cleared by the
 * lobby wrap (resetRaceLatches), and the session then unwound LEFT (the
 * vacated-seat presentation) or ERROR (a watchdog) -- either way the player
 * deserves the card that says what happened, not generic recovery copy. The
 * adapter's own no-demote rule still protects a more specific mid-race
 * breakdown (CONNECTION_UNPLAYABLE). FINISHED returns are left alone: a
 * completed tournament's wrap outranks a post-completion room departure. */
void latchOpponentLeftCardOnPeerLoss(IMdkrOnlineAdapter *adapter,
                                     MdkrPartyLinkSessionEndReason reason) {
#if MDKR_ENABLE_ONLINE_BETA
    if (adapter == nullptr ||
        (reason != MDKR_PARTY_LINK_SESSION_END_LEFT &&
         reason != MDKR_PARTY_LINK_SESSION_END_ERROR)) {
        return;
    }
    if (!OnlineRoom_partyLinkPeerLossObserved()) return;
    if (mdkr_online_live_adapter_set_race_end_failure(
            adapter, MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT)) {
        std::fprintf(stderr,
                     "[online-live] session ended after a mesh peer loss -> "
                     "opponent-left recovery card\n");
    }
#else
    (void)adapter;
    (void)reason;
#endif
}

/* Non-blocking frame budget for the SINGLE-RACE observe-only re-cycle: the max
 * serviced frames the launcher waits for the ENGINE-driven re-cycle to reach a
 * fresh race-ready transport before it gives up (logs + ends residency, never
 * hangs). Each frame does O(1) work and returns. Sized like the engine session's
 * own descriptor-less budget so the SESSION wall-clock watchdog (which also bounds
 * the LOBBY_WAIT re-wait) trips first in the normal peer-drop case; this is a
 * belt-and-suspenders launcher-side ceiling. */
static const unsigned kSingleObserveFrameBudget = 3000u;

/* state-hash witness window for the resident descriptor-less path
 * (log-only; asserted by the cloud capstone's (d)/(e)). The reducer-agreed
 * finish order proves the reducer heard the same placements; a fold hash over
 * the CONFIRMED canonical input frames proves the two endpoints simulated the
 * SAME race (the finish order is a much weaker projection of that state). The
 * window must be IDENTICAL on both endpoints to be comparable, so it is
 * anchored to the race's authored origin (firstTick) -- never to each
 * endpoint's own drain frontier, which differs by wall-clock jitter -- and it
 * is folded EARLY, once this endpoint's frontier passes the probe point, while
 * the net_input ring (last 128 authored ticks) still retains it. If a tick in
 * the window is not yet confirmed the fold retries next frame; a window that
 * never confirms emits no line and the capstone fails visibly. */
static const std::uint32_t kResidentFoldSkipTicks = 8u;    /* skip the ragged start */
static const std::uint32_t kResidentFoldWindowTicks = 60u; /* the compared span */
static const std::uint32_t kResidentFoldProbeTicks = 90u;  /* fold once nextTick
                                                            * >= firstTick + 90 */

/* RESIDENT LIVE per-frame coordinator (see LiveResidentState). Runs from
 * liveOverlayService every engine frame while g_liveResident is set. */
static void liveResidentServiceStep(void) {
    LiveResidentState *rs = g_liveResident;
    if (rs == nullptr || rs->visible == nullptr) return;

    /* Pump BOTH party_link feeds every frame -- the forward feed (reducer lobby ->
     * snapshot) the native RESULTS screen renders, and the reverse feed (engine
     * intents -> reducer commands) the host's RESULTS advance publishes REMATCH
     * on. These pumps have no other production caller; this is their live wiring. */
    OnlineRoom_pumpPartyLink(rs->visible);
    /* WEDGE (test-only): while parked in RESULTS, STOP dispatching
     * the reverse-feed intent so the host's republished REMATCH never reaches the
     * reducer -- the room stays in RESULTS and the engine's RESULTS-hold wall-clock
     * watchdog must fire. The forward feed keeps flowing so the RESULTS screen still
     * renders (and holds). Every normal run dispatches the intent. */
    if (!(rs->wedgeResultsHold && rs->phase == LiveResidentState::Phase::Results)) {
        OnlineRoom_pumpPartyLinkIntent(rs->visible);
    }

    /* TEST-ONLY REMOTE-SIM: with two loopback adapters the peer stands in
     * for the REMOTE process. Drive it toward ready every frame (its own local seat)
     * SEPARATELY from the single-endpoint advance step below, so the lane proves the
     * advance step drives ONLY the visible endpoint. Never runs in production
     * (remoteSim is false and peer == nullptr there). */
    if (rs->remoteSim && rs->peer != nullptr &&
        !(rs->ctx != nullptr && rs->ctx->peerSevered)) {
        OnlineRoom_lobbyStartServiceJoiner(rs->peer, rs->joinerCharacter);
    }

    if (rs->phase == LiveResidentState::Phase::Racing) {
        /* state-hash witness (log-only): once per epoch, once this
         * endpoint's drain frontier clears the probe point, fold the FIXED
         * firstTick-anchored confirmed-input window into the same FNV state
         * hash the per-race path emits, and log it. Two endpoints that
         * simulated the same race print the identical span AND hash; the cloud
         * capstone asserts that equality for (d)/(e) on top of the
         * reducer-agreed finish order. Retries silently until the whole window
         * is confirmed (never blocks; O(window) reads per attempt). */
        MdkrOnlineLiveRaceInfo foldInfo{};
        if (mdkr_online_live_adapter_race_info(rs->visible, &foldInfo) &&
            foldInfo.ready && foldInfo.matchEpoch != 0u &&
            foldInfo.matchEpoch != rs->foldEmittedEpoch &&
            foldInfo.nextTick >= foldInfo.firstTick + kResidentFoldProbeTicks) {
            const std::uint32_t foldStart =
                foldInfo.firstTick + kResidentFoldSkipTicks;
            const std::uint32_t foldEnd =
                foldStart + kResidentFoldWindowTicks - 1u;
            std::uint64_t hashVisible = 0u;
            const std::uint32_t folded =
                foldConfirmedRace(rs->visible, foldStart, foldEnd,
                                  foldInfo.activeSlotMask, &hashVisible);
            if (folded == kResidentFoldWindowTicks) {
                MdkrOnlineLobby foldLobby{};
                (void)mdkr_online_live_adapter_lobby(rs->visible, &foldLobby);
                rs->foldEmittedEpoch = foldInfo.matchEpoch;
                std::fprintf(stderr,
                             "[online-resident-live] race fold epoch=%u "
                             "race_index=%u span=%u..%u hash=%016llx\n",
                             static_cast<unsigned>(foldInfo.matchEpoch),
                             static_cast<unsigned>(foldLobby.race_index),
                             static_cast<unsigned>(foldStart),
                             static_cast<unsigned>(foldEnd),
                             static_cast<unsigned long long>(hashVisible));
                /* Loopback corroboration (test rigs only; production has no
                 * local peer): the peer endpoint's fold over the SAME span must
                 * match in-process too. */
                if (rs->peer != nullptr) {
                    std::uint64_t hashPeer = 0u;
                    const std::uint32_t foldedPeer =
                        foldConfirmedRace(rs->peer, foldStart, foldEnd,
                                          foldInfo.activeSlotMask, &hashPeer);
                    std::fprintf(
                        stderr,
                        "[online-resident-live] race fold peer epoch=%u "
                        "span=%u..%u hash=%016llx converged=%d\n",
                        static_cast<unsigned>(foldInfo.matchEpoch),
                        static_cast<unsigned>(foldStart),
                        static_cast<unsigned>(foldEnd),
                        static_cast<unsigned long long>(hashPeer),
                        (foldedPeer == kResidentFoldWindowTicks &&
                         hashPeer == hashVisible)
                            ? 1
                            : 0);
                }
            }
        }
        /* POLL-CONTENTION single owner: the launcher pump owns the one-shot engine
         * results poll. When THIS race captures a finish order, PUBLISH_RESULTS to
         * the reducer (-> RESULTS phase) so the native RESULTS screen fronts on the
         * real feed and mdkr_online_session_resume_results() sees RESULTS. The
         * native screen reads placements from snapshot.last_placements, NOT this
         * poll -- no collision. */
        std::uint8_t placements[MDKR_ONLINE_RACE_RESULT_SLOTS];
        if (mdkr_online_race_results_poll(placements)) {
            const bool reported =
                mdkr_online_live_adapter_report_results(rs->visible, placements);
            MdkrOnlineLobby lobby{};
            (void)mdkr_online_live_adapter_lobby(rs->visible, &lobby);
            rs->raceIndex = lobby.race_index;
            std::fprintf(stderr,
                         "[online-resident-live] race results reported "
                         "placements=%u,%u,%u,%u accepted=%d race_index=%u\n",
                         static_cast<unsigned>(placements[0]),
                         static_cast<unsigned>(placements[1]),
                         static_cast<unsigned>(placements[2]),
                         static_cast<unsigned>(placements[3]), reported ? 1 : 0,
                         static_cast<unsigned>(lobby.race_index));
            /* Re-publish the forward feed AFTER report so the snapshot carries the
             * RESULTS phase + last_placements this same frame. */
            OnlineRoom_pumpPartyLink(rs->visible);
            rs->phase = LiveResidentState::Phase::Results;
        }
        return;
    }

    if (rs->phase == LiveResidentState::Phase::Results) {
        /* TEST-ONLY: the near-finish sever (see liveTestSeverPeerAtResults)
         * fires once, on the first RESULTS-hold frame -- after the genuine
         * finish was captured and reported, before any REMATCH/advance. */
        if (liveTestSeverPeerAtResults() && rs->ctx != nullptr &&
            !rs->ctx->peerSevered) {
            rs->ctx->peerSevered = true;
            if (rs->ctx->severRig != nullptr) {
                OnlineRoom_testLoopbackSeverPeerPresence(rs->ctx->severRig);
            }
            std::fprintf(stderr,
                         "[online-live] TEST: peer transport SEVERED at "
                         "results (pump frozen + presence dropped; detection "
                         "must come from the transport)\n");
        }
        /* The native RESULTS screen fronts; the host's advance publishes rematch on
         * the reverse feed, which the intent pump above dispatches as the reducer's
         * leader-only REMATCH. Wait for it to actually land (LOBBY + race_index
         * advanced), then ARM the FRAME-STEPPED advance coordinator (no blocking
         * wait: the per-round re-cycle is driven one bounded unit per frame in the
         * Advancing phase below). */
        MdkrOnlineLobby lobby{};
        if (!mdkr_online_live_adapter_lobby(rs->visible, &lobby)) return;
        if (lobby.mode == MDKR_ONLINE_MODE_SINGLE_RACE) {
            /* SINGLE-RACE replay re-cycle. A single race's REMATCH keeps
             * race_index, so detect the re-cycle by the room LEAVING RESULTS back to
             * LOBBY -- unambiguous here because we only reach Results AFTER
             * PUBLISH_RESULTS parked the reducer in RESULTS, so an observed LOBBY is
             * the host's rematch landing. This is OBSERVE-ONLY: the engine drives the
             * re-Ready + START (a native CHANGE-picks screen or the LOBBY_WAIT
             * auto-start for RACE AGAIN); the launcher only drops the stale roster +
             * match-input so the session's boot-ready gate must see a FRESH epoch,
             * then waits in Advancing for that fresh race-ready transport and re-arms
             * the match-input. NOT auto-driving Ready/START is what lets a CHANGE-picks
             * native screen own the new config without the launcher racing it. */
            if (lobby.phase == MDKR_ONLINE_LOBBY) {
                if (rs->remoteSim) OnlineRoom_lobbyStartResetJoiner();
                /* TEST-ONLY: arm the perpetual-flap peer now (no-op unless the
                 * flap env is set), so race 1 readied normally and the flap
                 * engages only for this re-wait -- the cap scenario. */
                if (rs->remoteSim) OnlineRoom_lobbyStartArmJoinerFlap();
                mdkr_net_roster_runtime_clear();
                if (mdkr_match_input_runtime_active()) {
                    mdkr_match_input_runtime_clear();
                }
                rs->singleObserve = true;
                rs->singleObserveFrames = 0u;
                rs->phase = LiveResidentState::Phase::Advancing;
                std::fprintf(stderr,
                             "[online-resident-live] single-race replay: room left "
                             "RESULTS -> LOBBY (rematch) -> observe-only re-cycle "
                             "(engine drives selection/START; launcher re-arms "
                             "match-input on the fresh epoch)\n");
            }
            return;
        }
        if (lobby.phase == MDKR_ONLINE_LOBBY && lobby.race_index > rs->raceIndex) {
            std::fprintf(stderr,
                         "[online-resident-live] rematch observed race_index=%u "
                         "(reverse feed) -> advancing round\n",
                         static_cast<unsigned>(lobby.race_index));
            rs->advance = MdkrResidentAdvanceState{};
            rs->advance.visible = rs->visible;
            /* Single-endpoint advance drives ONLY the local endpoint (the
             * remote readies over the transport). Give the advance step a NULL peer
             * even in the test so it CANNOT poke a peer adapter -- the stand-in remote
             * (peer B) is driven separately by the remote-sim below + serviced by
             * liveOverlayService (ctx->peer). In production rs->peer is already null. */
            rs->advance.peer = rs->singleEndpoint ? nullptr : rs->peer;
            rs->advance.singleEndpoint = rs->singleEndpoint;
            /* Remote-sim (test only): reset the joiner-driver dedupe so the peer
             * re-Readies its own seat afresh for this round. */
            if (rs->remoteSim) OnlineRoom_lobbyStartResetJoiner();
            rs->phase = LiveResidentState::Phase::Advancing;
            return;
        }
        if (lobby.phase == MDKR_ONLINE_LOBBY && lobby.race_index < rs->raceIndex) {
            /* TOURNAMENT FINAL WRAP: the room left RESULTS with race_index
             * RESET (the leader REMATCH at the last cup round starts a fresh
             * series -- lobby_core.c reset_tournament_series), which the
             * `advanced` trigger above by definition misses (0 is never >
             * 3). This is the host CONTINUING in-session after a final-replay
             * chooser option (NEW TOURNAMENT / CHANGE CUP / CHANGE MODE /
             * RACE AGAIN / CHANGE CHARACTER) -- or the FINISH wrap right
             * before the session exits, in which case the residency is
             * retired at the engine return and this arm is moot. The ENGINE
             * owns the re-drive (the chooser routed it to a native
             * re-selection screen, or armed the LOBBY_WAIT auto-start), so
             * take the OBSERVE-ONLY re-cycle exactly like the single-race
             * replay: clear the stale roster/match-input and wait in
             * Advancing for the engine-driven BEGIN_LOADING to mint a fresh
             * race-ready epoch, then re-arm the match-input. Auto-driving the
             * mid-cup advance here would race the host's own re-selection to
             * START on the old config. Without this arm the coordinator
             * parked in Results forever and the continuing host's next race
             * never booted (the engine's re-wait watchdog then tripped
             * ERROR) -- caught red-first by check_online_final_replay.py. */
            if (rs->remoteSim) OnlineRoom_lobbyStartResetJoiner();
            mdkr_net_roster_runtime_clear();
            if (mdkr_match_input_runtime_active()) {
                mdkr_match_input_runtime_clear();
            }
            rs->singleObserve = true;
            rs->singleObserveFrames = 0u;
            rs->phase = LiveResidentState::Phase::Advancing;
            std::fprintf(stderr,
                         "[online-resident-live] tournament final wrap: room "
                         "left RESULTS -> LOBBY (fresh series, race_index %u -> "
                         "%u) -> observe-only re-cycle (the chooser's "
                         "re-selection owns the drive; launcher re-arms "
                         "match-input on the fresh epoch)\n",
                         static_cast<unsigned>(rs->raceIndex),
                         static_cast<unsigned>(lobby.race_index));
        }
        return;
    }

    if (rs->phase == LiveResidentState::Phase::Advancing && rs->singleObserve) {
        /* SINGLE-RACE observe-only re-cycle. Wait for the ENGINE-driven
         * re-cycle to reach a FRESH race-ready transport (BEGIN_LOADING re-installed
         * the roster + a race transport is ready on a NEW epoch), then re-install the
         * match-input so the session's live re-wait boots race N+1. We submit NO
         * Ready/START here -- the engine drives it -- so a CHANGE-picks native screen
         * is never raced. The roster was cleared in Results, so `roster active` gates
         * out the stale (just-raced) epoch until the new BEGIN_LOADING lands.
         *
         * Test remote-sim only: keep re-driving the stand-in remote (peer B) toward
         * ready WHILE the room has not yet reached BEGIN_LOADING (roster inactive) --
         * a CHANGE-picks host SET_CONFIG_TRACK re-clears the peer's ready AFTER the
         * one-shot Results reset, so a single reset would strand it un-ready. Once the
         * roster is back (all-ready -> START -> LOADING) we stop. Production
         * (remoteSim == false, peer == nullptr) never does this. */
        ++rs->singleObserveFrames;
        if (rs->singleObserveFrames > kSingleObserveFrameBudget) {
            std::fprintf(stderr,
                         "[online-resident-live] single-race re-cycle TIMEOUT "
                         "(exceeded %u-frame budget) -- ending residency\n",
                         kSingleObserveFrameBudget);
            rs->phase = LiveResidentState::Phase::Done;
            return;
        }
        /* WEDGE (test-only): a peer drop DURING the single-race re-cycle. Once the
         * engine-driven re-cycle has reached LOADING, the leader CANCELs loading so
         * the room regresses LOADING -> LOBBY -- the engine's per-round re-wait
         * mid-cancel unwind (online_session.c, singleEndpoint) must note LEFT +
         * exit(0): a CLEAN return to the room, never a hang/abort, and the launcher
         * reads LEFT so it does NOT re-arm (no extra boot). Reuses the tournament
         * cancel flag; fires once. None in normal runs. */
        if (rs->wedgeCancelRound2 &&
            OnlineRoom_lobbyStartCancelLoading(rs->visible)) {
            rs->wedgeCancelRound2 = false;
            rs->phase = LiveResidentState::Phase::Done;
            std::fprintf(stderr,
                         "[online-resident-live] WEDGE single-race re-cycle cancel: "
                         "leader CANCEL_LOADING mid re-cycle (engine must UNWIND -> "
                         "clean LEFT return)\n");
            return;
        }
        if (!mdkr_net_roster_runtime_active()) {
            if (rs->remoteSim) OnlineRoom_lobbyStartResetJoiner();
            return; /* engine has not driven BEGIN_LOADING yet -- keep waiting */
        }
        MdkrOnlineLiveRaceInfo vinfo{};
        if (!mdkr_online_live_adapter_race_info(rs->visible, &vinfo) ||
            !vinfo.ready || vinfo.matchEpoch == 0u) {
            return; /* roster installed but the transport is not race-ready yet */
        }
        /* Fresh race-ready transport: re-install the match-input on the new epoch
         * (the installed source's epoch is pinned at install; the re-cycle minted a
         * fresh one). Same discipline as the tournament ADVANCED arm. */
        mdkr_match_input_runtime_clear();
        if (!armLiveMatchInput(rs->ctx, vinfo.matchEpoch, vinfo.activeSlotMask)) {
            std::fprintf(stderr,
                         "[online-resident-live] single-race match-input reinstall "
                         "refused -- ending residency\n");
            rs->phase = LiveResidentState::Phase::Done;
            return;
        }
        /* Witness text note: "single race" is the historical label the
         * single-race replay lane pins; the SAME observe-only completion now
         * also serves the tournament FINAL-wrap re-cycle (its own distinct
         * arm-time witness names that case). */
        std::fprintf(stderr,
                     "[online-resident-live] single race race-ready epoch=%u "
                     "active=0x%02x frames=%u (observe-only re-cycle; match-input "
                     "re-installed; session LOBBY_WAIT will boot it)\n",
                     static_cast<unsigned>(vinfo.matchEpoch),
                     static_cast<unsigned>(vinfo.activeSlotMask),
                     rs->singleObserveFrames);
        rs->singleObserve = false;
        rs->singleObserveFrames = 0u;
        rs->phase = LiveResidentState::Phase::Racing;
        return;
    }

    if (rs->phase == LiveResidentState::Phase::Advancing) {
        /* WEDGE (test-only): during round 2's re-cycle, once the
         * advance has driven the room to LOADING, CANCEL it so the room regresses to
         * LOBBY -- the engine's per-round re-wait must UNWIND + re-front CHARSELECT.
         * Fire once (only round 2, raceIndex 1 after the rematch); then keep pumping
         * (the top-of-function pumps run in Done too) so the engine observes the
         * LOBBY regression and re-fronts. */
        if (rs->wedgeCancelRound2 && rs->raceIndex >= 1u &&
            OnlineRoom_lobbyStartCancelLoading(rs->visible)) {
            rs->wedgeCancelRound2 = false;
            rs->phase = LiveResidentState::Phase::Done;
            std::fprintf(stderr,
                         "[online-resident-live] WEDGE mid-tournament cancel: leader "
                         "CANCEL_LOADING submitted mid round-2 advance (engine must "
                         "UNWIND)\n");
            return;
        }
        /* Drive the round transition ONE bounded unit this frame (one pump + a
         * state check). It returns WORKING until the room is re-cycled to a fresh
         * race-ready transport -- the native RESULTS screen keeps presenting the
         * whole time; nothing blocks the service thread. */
        const MdkrResidentAdvanceStatus status =
            OnlineRoom_residentAdvanceStep(&rs->advance);
        if (status == MDKR_RESIDENT_ADVANCE_WORKING) return;
        if (status == MDKR_RESIDENT_ADVANCE_FAILED) {
            std::fprintf(stderr,
                         "[online-resident-live] round advance FAILED -- "
                         "ending residency\n");
            rs->phase = LiveResidentState::Phase::Done;
            return;
        }
        /* WEDGE (test-only): the room advanced to a fresh race-ready epoch
         * (it LEFT lobby, so no unwind fires) but we NEVER re-arm the match-input --
         * the engine's per-round re-wait `ready` predicate stays false, so its
         * WALL-CLOCK watchdog must trip at "per-round re-wait" + ERROR exit. This is
         * the deterministic per-round wall-clock proof. None in normal runs. */
        if (rs->wedgeSkipRearm) {
            std::fprintf(stderr,
                         "[online-resident-live] WEDGE skip-rearm: NOT re-installing "
                         "match-input after the round advance (engine per-round "
                         "re-wait must wall-clock TIMEOUT)\n");
            rs->phase = LiveResidentState::Phase::Done;
            return;
        }
        /* MDKR_RESIDENT_ADVANCE_ADVANCED: race N+1 is race-ready on a FRESH
         * match_epoch. Re-install the match-input source with that new epoch (the
         * installed source's epoch is pinned at install; REMATCH minted a fresh
         * one). The engine session is idling in LOBBY_WAIT, so no drain races this
         * clear/install (single-threaded launcher/engine alternation).
         * Use the epoch + active mask the advance step already resolved and
         * validated (vinfo.ready) -- NOT an unchecked race_info refetch that a
         * zero-init failure would have wedged the re-wait gate with. */
        const std::uint32_t newEpoch = rs->advance.newEpoch;
        mdkr_match_input_runtime_clear();
        if (!armLiveMatchInput(rs->ctx, newEpoch, rs->advance.newActiveMask)) {
            std::fprintf(stderr,
                         "[online-resident-live] match-input reinstall "
                         "refused -- ending residency\n");
            rs->phase = LiveResidentState::Phase::Done;
            return;
        }
        std::fprintf(stderr,
                     "[online-resident-live] next race armed: epoch=%u "
                     "active=0x%02x (match-input re-installed; session "
                     "LOBBY_WAIT will boot it)\n",
                     static_cast<unsigned>(newEpoch),
                     static_cast<unsigned>(rs->advance.newActiveMask));
        rs->phase = LiveResidentState::Phase::Racing;
        return;
    }
    /* Phase::Done -- nothing more; the tick budget ends the process. */
}

/* LOBBY-START per-frame coordinator (see LiveLobbyStartState). Runs from
 * liveOverlayService every engine frame while g_liveLobbyStart is set. */
static void liveLobbyStartServiceStep(void) {
    LiveLobbyStartState *ls = g_liveLobbyStart;
    if (ls == nullptr || ls->visible == nullptr) return;

    /* Pump BOTH party_link feeds for the VISIBLE (host) endpoint: the forward feed
     * (reducer lobby -> snapshot) the native CHARSELECT/TRACKSELECT render, and the
     * reverse feed (native intents -> reducer commands) the host's selection /
     * ready / SET_CONFIG_TRACK / START ride. This is the SAME wiring the resident
     * loop uses -- here it fronts race 1. */
    OnlineRoom_pumpPartyLink(ls->visible);
    OnlineRoom_pumpPartyLinkIntent(ls->visible);

    /* Drive the joiner (peer) toward ready so the host's native START can leave
     * LOBBY (BEGIN_LOADING needs all-ready). The host's native pick is Pipsy(2),
     * so the joiner takes a different racer. A severed peer (transport-sever
     * seam) is a corpse: never serviced. */
    if (!(ls->ctx != nullptr && ls->ctx->peerSevered)) {
        OnlineRoom_lobbyStartServiceJoiner(ls->peer, ls->joinerCharacter);
    }

    if (ls->phase != LiveLobbyStartState::Phase::Lobby) return;

    /* WEDGE sub-tests (test-only; None in every real lane). */
    if (ls->wedge == LiveLobbyStartState::Wedge::DescriptorNeverBuilds) {
        /* Let the room reach LOADING (so the engine defers boot + parks in the
         * descriptor-less re-wait) but NEVER arm the match-input source -- so the
         * engine's race-1 readiness gate never passes and its WALL-CLOCK WATCHDOG
         * must fire + exit cleanly (never hang). Keep pumping so the room progresses. */
        return;
    }
    if (ls->wedge == LiveLobbyStartState::Wedge::CancelLoading &&
        !ls->wedgeCancelDone) {
        /* Once the host's START has driven the room to LOADING (engine now parked in
         * the descriptor-less re-wait with a boot pending), the LEADER cancels
         * loading -> the room returns to SELECTING. The engine must UNWIND the
         * pending boot and re-front CHARSELECT (never park forever). After the single
         * cancel we fall through to the normal arm so race 1 still boots (proving
         * RECOVERY, not just unwind). */
        if (OnlineRoom_lobbyStartCancelLoading(ls->visible)) {
            ls->wedgeCancelDone = true;
            return;
        }
        return; /* not LOADING yet -- keep pumping until the START lands */
    }

    /* Race-1 ARM. The host's START drove BEGIN_LOADING; the adapters built the
     * descriptor, installed the process-global roster and stood up the race
     * transport. The instant the visible endpoint's race is ready on a real epoch,
     * install the match-input source with that epoch (the generalization of the
     * resident advance's ADVANCED arm to round 1). The engine's descriptor-less
     * readiness gate (online_session_boot_race) then passes and race 1 boots --
     * NEVER before this, so the NULL/stale descriptor is never dereferenced. */
    MdkrOnlineLiveRaceInfo info{};
    if (!(mdkr_net_roster_runtime_active() &&
          mdkr_online_live_adapter_race_info(ls->visible, &info) && info.ready &&
          info.matchEpoch != 0u)) {
        return; /* still selecting / loading -- keep pumping */
    }
    /* Consume any pending engine-race-boot publish so it does not leak into a
     * later poll; this coordinator arms the boot itself off the race-ready state. */
    (void)OnlineRoom_pollEngineRaceBoot();

    const std::uint64_t ownerToken =
        UINT64_C(0x4f4e4c49564500) ^ static_cast<std::uint64_t>(info.matchEpoch);
    OnlineRoom_setRosterOwner(ownerToken);

    if (!armLiveMatchInput(ls->ctx, info.matchEpoch, info.activeSlotMask)) {
        std::fprintf(stderr,
                     "[online-lobby-start] match-input install refused -- "
                     "ending lobby-start\n");
        ls->phase = LiveLobbyStartState::Phase::Done;
        return;
    }
    std::fprintf(stderr,
                 "[online-lobby-start] race-1 armed: epoch=%u active=0x%02x "
                 "(descriptor built + roster installed + match-input installed; "
                 "session LOBBY_WAIT re-wait will boot it)\n",
                 static_cast<unsigned>(info.matchEpoch),
                 static_cast<unsigned>(info.activeSlotMask));
    ls->phase = LiveLobbyStartState::Phase::Racing;

    /* COMPOSE: hand race 2..N off to the RESIDENT coordinator. The
     * lobby-start coordinator only fronts race 1; the resident coordinator
     * (g_liveResident) then OWNS the mid-residency results poll -> PUBLISH_RESULTS,
     * the host-advance REMATCH observation, and the per-round frame-stepped re-cycle
     * (OnlineRoom_residentAdvanceStep) EXACTLY as the resident-live lane does -- so a
     * descriptor-less session runs a FULL tournament in this one engine process.
     * Clear g_liveLobbyStart so only ONE coordinator pumps the feeds from the next
     * frame on (liveOverlayService checks g_liveResident BEFORE g_liveLobbyStart, and
     * g_liveResident was null this frame, so the resident step first runs next frame
     * -- no double-pump). Null resident (single-race lobby-start lane): keep the
     * existing arm-and-race behaviour, byte-unchanged. */
    if (ls->resident != nullptr) {
        ls->resident->visible = ls->visible;
        ls->resident->peer = ls->peer;
        ls->resident->ctx = ls->ctx;
        ls->resident->raceIndex = 0u;
        ls->resident->phase = LiveResidentState::Phase::Racing;
        /* Propagate the single-endpoint / remote-sim mode so races 2..N
         * re-cycle via the single-endpoint advance (production: peer == nullptr,
         * the remote readies over the transport; test: peer drives a stand-in
         * remote SEPARATELY from the advance step). */
        ls->resident->singleEndpoint = ls->singleEndpoint;
        ls->resident->remoteSim = ls->remoteSim;
        ls->resident->joinerCharacter = ls->joinerCharacter;
        ls->resident->wedgeResultsHold = ls->wedgeResultsHold;
        ls->resident->wedgeCancelRound2 = ls->wedgeCancelRound2;
        ls->resident->wedgeSkipRearm = ls->wedgeSkipRearm;
        g_liveResident = ls->resident;
        g_liveLobbyStart = nullptr;
        std::fprintf(stderr,
                     "[online-lobby-start] composed: handed off to resident "
                     "coordinator for races 2..N (in-process tournament; "
                     "singleEndpoint=%d remoteSim=%d)\n",
                     ls->singleEndpoint ? 1 : 0, ls->remoteSim ? 1 : 0);
    }
}

/* Engine->launcher FINISH/RETURN handshake. Take the engine-written
 * session end reason (one-shot) right after mdkr64_engine_boot returns and BEFORE
 * OnlineRoom_clearPartyLink() drops it, log a clear witness, and return the reason
 * so the caller/loop can route back to the Online Room. FINISHED (tournament
 * complete) and LEFT (a player backed out / a seat vacated / a mid-tournament
 * cancel) are clean returns; ERROR mirrors the nonzero watchdog rc. NONE means the
 * session ended without a native verdict (app-quit / a non-session postrace exit)
 * -- treated as before. The launcher loop already re-draws the room with the
 * adapter intact, so this needs NO teardown; it is the witness + reason surface. */
static const char *onlineSessionEndLabel(MdkrPartyLinkSessionEndReason reason) {
    switch (reason) {
    case MDKR_PARTY_LINK_SESSION_END_FINISHED: return "FINISHED";
    case MDKR_PARTY_LINK_SESSION_END_LEFT:     return "LEFT";
    case MDKR_PARTY_LINK_SESSION_END_ERROR:    return "ERROR";
    case MDKR_PARTY_LINK_SESSION_END_NONE:
    default:                                   return "NONE";
    }
}

static MdkrPartyLinkSessionEndReason onlineTakeSessionEndWitness(int result) {
    const MdkrPartyLinkSessionEndReason reason =
        mdkr_party_link_take_session_end();
    const char *label = onlineSessionEndLabel(reason);
    if (reason != MDKR_PARTY_LINK_SESSION_END_NONE) {
        std::fprintf(stderr,
                     "[online-session-end] reason=%s result=%d -> returning to "
                     "Online Room\n",
                     label, result);
    }
    return reason;
}

/* Boot the VISIBLE engine DESCRIPTOR-LESS on a lobby-start loopback
 * room. Unlike runOnlineLiveEngineSession this skips the roster/race-info gates
 * (there is no descriptor at boot), installs party_link + primes the forward feed
 * BEFORE the boot (so LOBBY_WAIT fronts native CHARSELECT, not the !haveSnap
 * direct boot), arms the lobby-start coordinator (which installs the match-input
 * source once the host's START has built race 1), and passes peer!=nullptr so the
 * eventual race reuses the two-adapter loopback drain/cross-pump. */
int runOnlineLobbyStartEngineSession(AppHost &host, const MdkrBootConfig &config,
                                     MdkrOnlineTestLoopbackRace *race) {
    IMdkrOnlineAdapter *visible = OnlineRoom_testLoopbackVisible(race);
    IMdkrOnlineAdapter *peer = OnlineRoom_testLoopbackPeer(race);
    if (visible == nullptr || peer == nullptr) return 2;

    /* Loopback drives the eventual race from the deterministic synthetic fixture
     * (same as the LIVE lane), converging byte-for-byte across both endpoints. */
    mdkr_online_live_adapter_race_set_synthetic_input(visible, true);
    mdkr_online_live_adapter_race_set_synthetic_input(peer, true);

    /* Match-input CONTEXT only (epoch 0, NO runtime install yet): the source is
     * installed by the coordinator once race 1 is ready. Setting g_liveMatchInput
     * makes liveOverlayService's early-out pass so the pumps + coordinator run
     * every engine frame from the very first LOBBY_WAIT tick. */
    LiveMatchInputContext context;
    context.visible = visible;
    context.peer = peer;
    context.epoch = 0u;
    context.activeMask = 0u;
    /* Transport-sever seam plumbing: the drain needs the rig to drop the
     * peer's loopback signal presence at sever time, and the severed race
     * must run at the AUTHORED cadence -- detection is the transport's
     * REAL-TIME liveness ladder (ping interval + stale bound), while the
     * unthrottled headless drain finishes a whole race in ~2 real seconds,
     * which no wall-clock detector could ever land inside (the real cloud
     * session is 30 Hz wall-clock). The room-only departure seam
     * (MDKR_APP_TEST_ONLINE_ROOM_DEPARTURE_AT_TICK) uses the same rig and the
     * same cadence, for the same reason. Inert unless one of the two is
     * set. */
    context.severRig = race;
    if (liveTestSeverPeerAtTick() != 0u ||
        liveTestRoomDepartureAtTick() != 0u) {
        context.paceAdvanceHz = 30u;
    }
    g_liveMatchInput = &context;

    /* Install party_link + PRIME the forward feed BEFORE the engine boots, so
     * LOBBY_WAIT's first read is a LOBBY snapshot with the local seat -> the native
     * CHARSELECT fronts (never the !haveSnap direct-boot branch). */
    OnlineRoom_installPartyLink();
    /* TEST: exercise the SINGLE-ENDPOINT advance + WALL-CLOCK watchdog on
     * the loopback rig -- the coordinator drives ONLY the visible endpoint's advance
     * while a SEPARATE remote-sim drives the peer (proving no peer poke by the
     * advance step). Notes the engine into the wall-clock + error-signal path. */
    const bool singleEndpoint =
        std::getenv("MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT") != nullptr;
    if (singleEndpoint) mdkr_party_link_note_single_endpoint(true);
    visible->service();
    OnlineRoom_pumpPartyLink(visible);
    OnlineRoom_lobbyStartResetJoiner();

    /* The TOURNAMENT lobby-start composes with the resident coordinator so
     * races 2..N re-cycle in-process (the demo's real flow). Detect it from the SAME
     * env the room builder used to pre-configure the cup. A single-race lobby-start
     * (no tournament env) keeps the resident pointer null -> arm-and-race, unchanged
     * -- UNLESS MDKR_APP_TEST_ONLINE_SINGLE_REPLAY is set (the single-replay lane), which composes
     * the resident for a SINGLE race too so its "Race Again" / "change picks" replays
     * re-cycle in-process (production runOnlineLobbyStartLiveSession always composes
     * it; this mirrors that for the loopback rig without disturbing the plain
     * single-race lobby-start lane, which sets neither env). */
    const char *modeEnv = std::getenv("MDKR_APP_TEST_ONLINE_MODE");
    const bool tournament = modeEnv != nullptr && std::strcmp(modeEnv, "tournament") == 0;
    const bool singleReplay =
        !tournament && std::getenv("MDKR_APP_TEST_ONLINE_SINGLE_REPLAY") != nullptr;
    LiveResidentState residentState; /* races 2..N (tournament) OR single-race replays */

    LiveLobbyStartState lobbyState;
    lobbyState.visible = visible;
    lobbyState.peer = peer;
    lobbyState.ctx = &context;
    lobbyState.joinerCharacter = 1u; /* host native picks Pipsy(2); joiner != 2 */
    lobbyState.phase = LiveLobbyStartState::Phase::Lobby;
    lobbyState.resident = (tournament || singleReplay) ? &residentState : nullptr;
    /* On the loopback rig the peer is the test-only remote-sim. */
    lobbyState.singleEndpoint = singleEndpoint;
    lobbyState.remoteSim = singleEndpoint;
    /* WEDGE sub-tests (test-only; unset -> Wedge::None in every real lane). */
    if (const char *wedgeEnv = std::getenv("MDKR_APP_TEST_ONLINE_LOBBY_WEDGE")) {
        if (std::strcmp(wedgeEnv, "descriptor") == 0) {
            lobbyState.wedge = LiveLobbyStartState::Wedge::DescriptorNeverBuilds;
        } else if (std::strcmp(wedgeEnv, "cancel") == 0) {
            lobbyState.wedge = LiveLobbyStartState::Wedge::CancelLoading;
        } else if (std::strcmp(wedgeEnv, "results") == 0) {
            /* After race 1 the resident coordinator parks in RESULTS
             * and never dispatches the REMATCH, so the engine's RESULTS-hold wall-clock
             * watchdog must fire. Single-endpoint only (the RESULTS-hold watchdog is
             * gated on singleEndpoint). */
            lobbyState.wedgeResultsHold = true;
        } else if (std::strcmp(wedgeEnv, "cancel2") == 0) {
            /* Cancel loading mid round-2 advance -> the engine's
             * per-round re-wait must UNWIND + re-front CHARSELECT. Single-endpoint. */
            lobbyState.wedgeCancelRound2 = true;
        } else if (std::strcmp(wedgeEnv, "perround") == 0) {
            /* Skip the per-round match-input re-arm after the advance ->
             * the engine's per-round re-wait wall-clock watchdog must trip. */
            lobbyState.wedgeSkipRearm = true;
        }
    }
    g_liveLobbyStart = &lobbyState;

    std::fprintf(stderr,
                 "[online-lobby-start] residency armed: party_link installed, no "
                 "descriptor -- native online screens own race 1 (tournament=%d "
                 "wedge=%d)\n",
                 tournament ? 1 : 0, static_cast<int>(lobbyState.wedge));

    liveEngineHostBind(host);

    /* Online epoch: the engine sees the NTSC source identity (rom_io.c) so a
     * PAL ROM races the 30 Hz manifest bit-identically to US; cleared as soon
     * as the bounded boot returns. */
    int result;
    {
        OnlineNtscIdentityScope ntscIdentityScope(true);
        result = mdkr64_engine_boot(&config);
    }

    /* Read the engine's session end reason BEFORE OnlineRoom_clearPartyLink
     * drops the party_link note (the launcher then resumes the room). This is the
     * LOOPBACK TEST path (peer != nullptr, driven by the
     * lobby-start/tournament/single-endpoint lanes); it deliberately does NOT arm the
     * room-ready re-arm -- the probe + lanes own the latch state and assert exact fire
     * counts, so arming here would pollute them. The production native path
     * (runOnlineLobbyStartLiveSession) is the sole arm site. */
    const MdkrPartyLinkSessionEndReason lobbyStartEndReason =
        onlineTakeSessionEndWitness(result);
    /* Same immediate peer notify as the production path: a deliberate mid-race
     * leave must not strand the survivor on the loss ladders. */
    notifyPeerOfDeliberateLeave(visible, lobbyStartEndReason);
    /* Same truthful-card routing as the production path. */
    latchOpponentLeftCardOnPeerLoss(visible, lobbyStartEndReason);

    liveEngineHostUnbind();
    g_liveMatchInput = nullptr;
    g_liveLobbyStart = nullptr;
    /* The tournament compose may have handed off to the resident
     * coordinator; retire it too (no-op when it was never armed). */
    g_liveResident = nullptr;
    if (mdkr_match_input_runtime_active()) mdkr_match_input_runtime_clear();
    OnlineRoom_clearPartyLink();
    mdkr_net_roster_runtime_clear();
    /* Re-arm the one-shot video-config handoff for a later boot in this
     * process (same rationale + shape as runEngineSession's teardown). */
    if (!mdkr_video_config_engine_session_complete()) {
        std::fprintf(stderr,
                     "[session] video-config engine epoch did not close cleanly\n");
    }
    return result;
}

/* PRODUCTION room-ready boot: front the NATIVE online screens for a REAL
 * human from race 1, for ANY online mode. Unlike the loopback
 * runOnlineLobbyStartEngineSession this drives a SINGLE live endpoint (peer ==
 * nullptr) -- the real remote process readies itself and supplies its input over the
 * mesh (the proven cloud/liveDrainMatchInput peer==nullptr path). The room-ready
 * trigger's former TOURNAMENT-only gate (OnlineRoom_roomReadyConditionHolds) is
 * gone, so this is now the descriptor-less native boot path for BOTH single race and
 * tournament. The resident coordinator it composes with re-cycles rounds 2..N for a
 * TOURNAMENT via the SINGLE-ENDPOINT advance; a SINGLE race boots race 1 native and
 * ends (a native single-race REPLAY re-cycle is a later follow-up -- until then a
 * single race is one race per boot, exactly like the descriptor-first path it
 * replaces).
 * Booted from runInteractiveLauncher's room-ready poll with the panel's live adapter;
 * returns the engine result (a watchdog error trip is a nonzero code the launcher
 * routes back to the room). */
int runOnlineLobbyStartLiveSession(AppHost &host, const MdkrBootConfig &config,
                                   IMdkrOnlineAdapter *visibleWrapper,
                                   MdkrPartyLinkSessionEndReason *endReasonOut) {
    if (endReasonOut != nullptr) *endReasonOut = MDKR_PARTY_LINK_SESSION_END_NONE;
    /* DIAGNOSTIC [4/5] -- native session boot entered. Logged BEFORE the resolve
     * below so even a resolve failure produces an explicit boot line (never a missing
     * bracket); paired with the launcher's boot-result line so a real-hardware
     * takeover shows a clear boot/return bracket in the log. */
    std::fprintf(stderr,
                 "[online-room-ready] native session boot entered (descriptor-less, "
                 "peer=nullptr)\n");

    /* Resolve the concrete LiveAdapter behind the panel's owning wrapper so the
     * forward-feed pump + race arm below drive the real inner adapter (and the
     * room-ready registry keyed on that raw pointer). The C accessors reach it via
     * the mdkrResolveLive hook whether handed the wrapper or the raw adapter. */
    IMdkrOnlineAdapter *visible = OnlineRoom_resolveRawLiveAdapter(visibleWrapper);
    if (visible == nullptr) {
        std::fprintf(stderr,
                     "[online-room-ready] boot aborted: adapter resolve returned "
                     "null (no concrete live adapter)\n");
        return 2;
    }

    /* Scripted PAD-INPUT injection for the race (test-only; production leaves it
     * unset). The descriptor-less native takeover normally commits the real
     * controller fed through liveDrainMatchInput -> race_set_local_input. A
     * headless two-process cloud capstone has no human at the pad, so this seam
     * stands the deterministic per-tick raceLocalSample fixture in for the local
     * controller: both processes drive their car from the identical fixture and
     * therefore converge byte-for-byte across the real mesh -- the SAME synthetic
     * race-input the loopback lobby-start + cloud lanes already use, here on the
     * production peer==nullptr path. It is INPUT only (the car's stick/buttons),
     * never a phase/route override. Set once at boot; the resident re-cycle keeps
     * the adapter flag for races 2..N (matching the loopback lobby-start lane). */
    if (std::getenv("MDKR_APP_TEST_ONLINE_SYNTH_RACE_INPUT") != nullptr) {
        mdkr_online_live_adapter_race_set_synthetic_input(visible, true);
        std::fprintf(stderr,
                     "[online-autopair] synthetic race-input injection enabled "
                     "(headless controller stand-in; production path unchanged)\n");
    }

    /* Match-input CONTEXT only (epoch 0, NO runtime install yet; the coordinator
     * installs the source once race 1 is ready). peer == nullptr: the real remote
     * supplies its input over the mesh. NO synthetic input by default -- the real
     * controller drives the race. paceAdvanceHz 0: interactive play is naturally
     * paced by vsync (the drain frontier cannot outrun the remote's
     * confirmations), unlike the headless cloud lane which needs an explicit 30 Hz
     * pace. */
    LiveMatchInputContext context;
    context.visible = visible;
    context.peer = nullptr;
    context.epoch = 0u;
    context.activeMask = 0u;
    g_liveMatchInput = &context;

    /* Install party_link + PRIME the forward feed BEFORE the engine boots (so
     * LOBBY_WAIT fronts native CHARSELECT, never the !haveSnap direct-boot branch),
     * and NOTE the single-endpoint mode so the engine session picks the WALL-CLOCK
     * watchdog + error-signal + mid-tournament-cancel-unwind path. */
    OnlineRoom_installPartyLink();
    mdkr_party_link_note_single_endpoint(true);
    visible->service();
    OnlineRoom_pumpPartyLink(visible);
    OnlineRoom_lobbyStartResetJoiner();

    LiveResidentState residentState; /* re-cycles races 2..N for a tournament, and
                                      * also re-cycles a SINGLE race's "Race Again" /
                                      * "change picks" replays in-process via the
                                      * observe-only single-race re-cycle */
    LiveLobbyStartState lobbyState;
    lobbyState.visible = visible;
    lobbyState.peer = nullptr;       /* production: no local peer to drive */
    lobbyState.ctx = &context;
    lobbyState.joinerCharacter = 1u; /* unused (no peer) */
    lobbyState.phase = LiveLobbyStartState::Phase::Lobby;
    lobbyState.resident = &residentState;
    lobbyState.singleEndpoint = true;
    lobbyState.remoteSim = false;    /* the REAL remote readies itself */
    g_liveLobbyStart = &lobbyState;

    std::fprintf(stderr,
                 "[online-lobby-start] PRODUCTION residency armed: party_link "
                 "installed, no descriptor, peer=nullptr -- native online screens "
                 "own race 1 (single-endpoint, any mode)\n");

    liveEngineHostBind(host);

    /* Online epoch: the engine sees the NTSC source identity (rom_io.c) so a
     * PAL ROM races the 30 Hz manifest bit-identically to US; cleared as soon
     * as the bounded boot returns. */
    int result;
    {
        OnlineNtscIdentityScope ntscIdentityScope(true);
        result = mdkr64_engine_boot(&config);
    }

    /* Engine->launcher FINISH/RETURN handshake: read WHY the native session
     * ended (FINISHED / LEFT / ERROR) BEFORE OnlineRoom_clearPartyLink() drops the
     * note, so the launcher loop resumes the Online Room with the reason logged.
     * The room-ready block below `continue`s on any return; the panel still owns
     * the adapter/room (no teardown here), so the human is back in the room. */
    const MdkrPartyLinkSessionEndReason endReason =
        onlineTakeSessionEndWitness(result);
    if (endReasonOut != nullptr) *endReasonOut = endReason;
    /* A deliberate mid-race leave must notify the surviving peer immediately
     * (its race is still running against our now-frozen seat). */
    notifyPeerOfDeliberateLeave(visible, endReason);
    /* A session that unwound after the mesh lost the peer fronts the
     * truthful card, wherever in the session the loss landed. */
    latchOpponentLeftCardOnPeerLoss(visible, endReason);
    /* A FINISHED native session returns with the tournament-final REMATCH
     * wrap already landed (the host's FINISH dispatched it before leaving; a joiner's
     * FINISHED followed that same observed wrap), so the room is normally already
     * back at a fresh-series SELECTING+2+LOBBY. Arm the re-arm here -- the panel's
     * per-frame observer completes it immediately (the wrap's RESULTS-out-and-back
     * WAS the rising edge, it just happened while the engine owned the frames), and
     * the next poll re-takes native for session #2 on THIS endpoint; the second real
     * peer does the same on its own FINISHED return -- the automatic both-endpoint
     * re-take into the freshly wrapped room. Single-race "Race Again" / "change
     * picks" never reach here -- they re-cycle IN-PROCESS (the session stays booted;
     * see the resident coordinator's single-race observe-only re-cycle), so this arm
     * is purely the whole-new-session path, still one arm per FINISHED return.
     * LEFT/ERROR/NONE land with the condition potentially still TRUE but with NO
     * completed tournament behind them, so they must NOT arm: a re-take there would
     * re-boot the session the player just left. Reason-gating here is the
     * load-bearing half of the no-loop proof (the other half: one latch clear per
     * arm, and a re-taken session parks at CHARSELECT without human input). */
    if (endReason == MDKR_PARTY_LINK_SESSION_END_FINISHED) {
        OnlineRoom_armRoomReadyRearm();
    }
    /* Record the return so the SELECTING body can offer a "Return to game" control
     * after a LEFT/ERROR return (FINISHED/NONE clear the offer). This is a pure
     * record and never re-arms -- a LEFT/ERROR return still lands with the latch SET
     * and nothing pending, so the takeover cannot re-fire until the player presses. */
    OnlineRoom_noteSessionReturn(endReason);

    liveEngineHostUnbind();
    g_liveMatchInput = nullptr;
    g_liveLobbyStart = nullptr;
    g_liveResident = nullptr; /* the compose may have handed off; retire it too */
    /* Make the room-ready vs race-boot mutual exclusion
     * STRUCTURAL, not incidental. The adapter's per-round setUpRace re-publishes the
     * race-boot handoff each round; drain any stale pending here so the launcher's
     * race-boot poll cannot fire on it after this descriptor-less session returns. */
    (void)OnlineRoom_pollEngineRaceBoot();
    if (mdkr_match_input_runtime_active()) mdkr_match_input_runtime_clear();
    OnlineRoom_clearPartyLink(); /* also clears the single-endpoint note */
    mdkr_net_roster_runtime_clear();
    /* Re-arm the one-shot video-config handoff. The FINISHED re-take boots a
     * SECOND descriptor-less native session in this same process (the automatic
     * both-endpoint re-take into the freshly wrapped room), and that boot must
     * pass mdkr_video_config_handoff_to_engine again -- the first real second
     * boot ever taken stopped at "[app] video-config handoff was missing or
     * repeated" because no online runner re-armed it (runEngineSession's
     * teardown always has). Same rationale + shape as that teardown. */
    if (!mdkr_video_config_engine_session_complete()) {
        std::fprintf(stderr,
                     "[session] video-config engine epoch did not close cleanly\n");
    }
    return result;
}
#endif /* MDKR_ENABLE_ONLINE_BETA */

int runShellSmoke(AppHost &host, Launcher &launcher, AppUiSmokeInputMode smokeInputMode, const char *smoke) {
    int frames = std::atoi(smoke);
    if (frames < 1) frames = 1;
    const char *smokeNavigationTarget =
        std::getenv("MDKR_APP_SMOKE_NAV_TARGET");
    const char *smokeNavigationToken =
        std::getenv("MDKR_APP_SMOKE_NAV_TOKEN");
    const bool anyNavigationContract =
        (smokeNavigationTarget && smokeNavigationTarget[0]) ||
        (smokeNavigationToken && smokeNavigationToken[0]);
    char *navigationEnd = nullptr;
    const long smokeNavigationPanel = smokeNavigationTarget
        ? std::strtol(smokeNavigationTarget, &navigationEnd, 10) : -1;
    const bool smokeNavigation = anyNavigationContract &&
        smokeNavigationTarget && navigationEnd &&
        navigationEnd != smokeNavigationTarget && *navigationEnd == '\0' &&
        smokeNavigationPanel >= 0 &&
        smokeNavigationPanel < kLauncherPanelCount &&
        smokeNavigationToken &&
        std::strcmp(smokeNavigationToken, "mdkr64-app-nav-v1") == 0;
    if (anyNavigationContract && !smokeNavigation) {
        std::fprintf(stderr,
                     "[app] smoke: invalid top-navigation input contract\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    if (smokeNavigation && frames < 4) frames = 4;
    bool smokeNavigationQueued = false;
    const bool smokeUsesGamepad =
        smokeInputMode == AppUiSmokeInputMode::Gamepad;
    const char *smokeFrameLimit =
        std::getenv("MDKR_APP_SMOKE_SELECT_FRAME_LIMIT");
    // Drives the real Presentation pace radio button through SDL, for the same
    // reason the Frame limit script exists: the claim under test is that ONE
    // press writes BOTH pacing keys, and only a click that travels
    // SDL -> ImGui -> the quick-choice transaction can prove it.
    const char *smokePresentationPace =
        std::getenv("MDKR_APP_SMOKE_SELECT_PRESENTATION_PACE");
    bool smokePaceClickQueued = false;
    /*
     * The scripted accessibility walk uses Tab/arrow navigation for keyboard
     * runs and the virtual controller's D-pad for gamepad runs, then lets the
     * shared row helper announce whichever production widget receives focus.
     *
     * Both routes enter through SDL -> ImGui -> the production widget. Calling
     * panel draw functions directly would prove only that rows can speak, not
     * that a player's input can reach them.
     */
    const bool smokeA11yWalk = AppUi_a11yWalkArmed();
    const char *smokeCharacterImportFocus =
        std::getenv("MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS");
    const char *smokeCharacterImportFocusToken =
        std::getenv("MDKR_APP_SMOKE_CHARACTER_IMPORT_FOCUS_TOKEN");
    const bool anyCharacterImportFocusContract =
        (smokeCharacterImportFocus && smokeCharacterImportFocus[0]) ||
        (smokeCharacterImportFocusToken &&
         smokeCharacterImportFocusToken[0]);
    const bool smokeCharacterImportFocusArmed =
        anyCharacterImportFocusContract && smokeA11yWalk &&
        smokeCharacterImportFocus &&
        std::strcmp(smokeCharacterImportFocus, "1") == 0 &&
        smokeCharacterImportFocusToken &&
        std::strcmp(smokeCharacterImportFocusToken,
                    "mdkr64-character-import-focus-v1") == 0;
    if (anyCharacterImportFocusContract &&
        !smokeCharacterImportFocusArmed) {
        std::fprintf(
            stderr,
            "[app] smoke: invalid Character Workshop import-focus contract\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    const char *smokeCharacterWorkshopReturn =
        std::getenv("MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN");
    const char *smokeCharacterWorkshopReturnToken =
        std::getenv("MDKR_APP_SMOKE_CHARACTER_WORKSHOP_RETURN_TOKEN");
    const bool anyCharacterWorkshopReturnContract =
        (smokeCharacterWorkshopReturn &&
         smokeCharacterWorkshopReturn[0]) ||
        (smokeCharacterWorkshopReturnToken &&
         smokeCharacterWorkshopReturnToken[0]);
    const bool smokeCharacterWorkshopReturnArmed =
        anyCharacterWorkshopReturnContract && smokeA11yWalk &&
        smokeCharacterWorkshopReturn &&
        std::strcmp(smokeCharacterWorkshopReturn, "1") == 0 &&
        smokeCharacterWorkshopReturnToken &&
        std::strcmp(smokeCharacterWorkshopReturnToken,
                    "mdkr64-character-workshop-return-v1") == 0;
    if (anyCharacterWorkshopReturnContract &&
        !smokeCharacterWorkshopReturnArmed) {
        std::fprintf(
            stderr,
            "[app] smoke: invalid Character Workshop return contract\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    /*
     * An Escape that nothing on the panel claims, pressed after N Tabs of the
     * keyboard walk (tests/check_keyboard_dead_ends.py). The walk stays on Tab
     * for its whole length so every control announced after the Escape was
     * reached by Tab, and the frame on which ImGui receives the Escape is
     * printed beside the speech trace, so the check can tell "carried on",
     * "started over" and "stopped" apart.
     */
    const char *smokeA11yEscape = std::getenv("MDKR_APP_SMOKE_A11Y_ESCAPE");
    const char *smokeA11yEscapeToken =
        std::getenv("MDKR_APP_SMOKE_A11Y_ESCAPE_TOKEN");
    const bool anyA11yEscapeContract =
        (smokeA11yEscape && smokeA11yEscape[0]) ||
        (smokeA11yEscapeToken && smokeA11yEscapeToken[0]);
    char *a11yEscapeEnd = nullptr;
    const long smokeA11yEscapeTabs = smokeA11yEscape
        ? std::strtol(smokeA11yEscape, &a11yEscapeEnd, 10) : 0;
    const bool smokeA11yEscapeArmed =
        anyA11yEscapeContract && smokeA11yWalk && !smokeUsesGamepad &&
        !smokeCharacterImportFocusArmed &&
        !smokeCharacterWorkshopReturnArmed && smokeA11yEscape &&
        a11yEscapeEnd != smokeA11yEscape && *a11yEscapeEnd == '\0' &&
        smokeA11yEscapeTabs >= 1 && smokeA11yEscapeTabs <= 64 &&
        smokeA11yEscapeToken &&
        std::strcmp(smokeA11yEscapeToken, "mdkr64-a11y-escape-v1") == 0;
    if (anyA11yEscapeContract && !smokeA11yEscapeArmed) {
        std::fprintf(stderr,
                     "[app] smoke: invalid accessibility Escape contract\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    const char *smokeUiScale =
        std::getenv("MDKR_APP_SMOKE_UI_SCALE_DRAG");
    const char *smokeTouchScroll =
        std::getenv("MDKR_APP_SMOKE_TOUCH_SCROLL");
    const char *smokeTouchToken =
        std::getenv("MDKR_APP_SMOKE_TOUCH_TOKEN");
    const char *smokeWheelScroll =
        std::getenv("MDKR_APP_SMOKE_WHEEL_SCROLL");
    const char *smokeWheelToken =
        std::getenv("MDKR_APP_SMOKE_WHEEL_TOKEN");
    const char *smokeOnlineActionValue =
        std::getenv("MDKR_APP_SMOKE_ONLINE_ACTION");
    const char *smokeOnlineFocusValue =
        std::getenv("MDKR_APP_ONLINE_FOCUS_ACTION");
    const char *smokeOnlineActionToken =
        std::getenv("MDKR_APP_ONLINE_ACTION_TOKEN");
    const bool anyOnlineActionContract =
        (smokeOnlineActionValue && smokeOnlineActionValue[0]) ||
        (smokeOnlineFocusValue && smokeOnlineFocusValue[0]) ||
        (smokeOnlineActionToken && smokeOnlineActionToken[0]);
    char *onlineActionEnd = nullptr;
    char *onlineFocusEnd = nullptr;
    const long smokeOnlineAction = smokeOnlineActionValue
        ? std::strtol(smokeOnlineActionValue, &onlineActionEnd, 10) : 0;
    const long smokeOnlineFocus = smokeOnlineFocusValue
        ? std::strtol(smokeOnlineFocusValue, &onlineFocusEnd, 10) : 0;
    const bool smokeOnlineActionArmed = anyOnlineActionContract &&
        smokeOnlineActionValue && onlineActionEnd &&
        onlineActionEnd != smokeOnlineActionValue && *onlineActionEnd == '\0' &&
        smokeOnlineFocusValue && onlineFocusEnd &&
        onlineFocusEnd != smokeOnlineFocusValue && *onlineFocusEnd == '\0' &&
        smokeOnlineAction == smokeOnlineFocus &&
        smokeOnlineAction > MDKR_ONLINE_VIEW_ACTION_NONE &&
        smokeOnlineAction <=
            MDKR_ONLINE_VIEW_ACTION_REPORT_PHRASE_MISMATCH &&
        smokeOnlineActionToken &&
        std::strcmp(smokeOnlineActionToken,
                    "mdkr64-online-action-v1") == 0 &&
        (smokeInputMode == AppUiSmokeInputMode::Keyboard ||
         smokeInputMode == AppUiSmokeInputMode::Gamepad);
    if (anyOnlineActionContract && !smokeOnlineActionArmed) {
        std::fprintf(stderr,
                     "[app] smoke: invalid Online Room action contract\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    const bool anyTouchContract =
        (smokeTouchScroll && smokeTouchScroll[0]) ||
        (smokeTouchToken && smokeTouchToken[0]);
    const bool smokeTouch = anyTouchContract && smokeTouchScroll &&
        std::strcmp(smokeTouchScroll, "1") == 0 && smokeTouchToken &&
        std::strcmp(smokeTouchToken, "mdkr64-app-touch-v1") == 0;
    if (anyTouchContract && !smokeTouch) {
        std::fprintf(stderr,
                     "[app] smoke: invalid touchscreen input contract\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    const bool anyWheelContract =
        (smokeWheelScroll && smokeWheelScroll[0]) ||
        (smokeWheelToken && smokeWheelToken[0]);
    const bool smokeWheel = anyWheelContract && smokeWheelScroll &&
        std::strcmp(smokeWheelScroll, "1") == 0 && smokeWheelToken &&
        std::strcmp(smokeWheelToken, "mdkr64-app-wheel-v1") == 0;
    if (anyWheelContract && !smokeWheel) {
        std::fprintf(stderr,
                     "[app] smoke: invalid mouse-wheel input contract\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    // In flipped mode the injected notches carry SDL_MOUSEWHEEL_FLIPPED with
    // the SAME preciseY sign as the normal case, because that is what SDL
    // delivers: macOS has already applied the natural-scrolling preference to
    // deltaY and SDL passes it through, setting the flag purely as information
    // (SDL_cocoamouse.m). The panel must therefore scroll DOWN in both runs.
    // Injecting an inverted sign here modelled a negation the platform does not
    // perform, and it agreed with a bridge that negated to match -- two wrongs
    // that kept each other green while the app scrolled backwards on every Mac
    // with the default setting.
    const float smokeWheelDelta = -0.6f;
    float smokeUiScaleTarget = 1.0f;
    const bool smokeUiScaleDrag = smokeUiScale && smokeUiScale[0];
    if (smokeUiScaleDrag &&
        (!AppUi_parseScale(smokeUiScale, &smokeUiScaleTarget) ||
         std::fabs(smokeUiScaleTarget - 2.0f) > 0.001f)) {
        std::fprintf(stderr,
                     "[app] smoke: UI-scale drag target must be 2.00\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    if ((smokeUiScaleDrag && smokeFrameLimit) ||
        (smokePresentationPace && (smokeUiScaleDrag || smokeFrameLimit)) ||
        (smokeA11yWalk && (smokeUiScaleDrag || smokeFrameLimit ||
                           smokePresentationPace || smokeTouch || smokeWheel ||
                           smokeNavigation || smokeOnlineActionArmed)) ||
        (smokeTouch && (smokeUiScaleDrag || smokeFrameLimit ||
                        smokePresentationPace || smokeNavigation || smokeWheel ||
                        smokeOnlineActionArmed)) ||
        (smokeWheel && (smokeUiScaleDrag || smokeFrameLimit ||
                        smokePresentationPace || smokeNavigation ||
                        smokeOnlineActionArmed)) ||
        (smokeOnlineActionArmed &&
         (smokeUiScaleDrag || smokeFrameLimit || smokePresentationPace ||
          smokeNavigation))) {
        std::fprintf(stderr,
                     "[app] smoke: pointer scripts cannot share one input run\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    if (smokePresentationPace &&
        std::strcmp(smokePresentationPace, "original") != 0 &&
        std::strcmp(smokePresentationPace, "smooth") != 0) {
        std::fprintf(stderr,
                     "[app] smoke: unsupported presentation pace %s "
                     "(original or smooth)\n", smokePresentationPace);
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    if (smokePresentationPace && frames < 8) frames = 8;
    if (smokeUiScaleDrag && frames < 11) frames = 11;
    if (smokeTouch && frames < 10) frames = 10;
    if (smokeWheel && frames < 12) frames = 12;
    if (smokeOnlineActionArmed && frames < 12) frames = 12;
    /* One keystroke per frame, and the walk has to get all the way round the
     * panel with room to spare -- an early stop would report controls as
     * silent that the keyboard simply never reached. */
    if (smokeA11yWalk && frames < 24) frames = 24;
    const int smokeFrameLimitSteps = smokeFrameLimit
        ? Settings_smokeFrameLimitDownSteps("original", smokeFrameLimit)
        : 0;
    const int smokeMoveStart = 8;
    /* Gamepad smoke uses the fixed focus-opening choreography below. Keyboard
     * smoke observes the rendered popup focus and moves toward the requested
     * public option, so it does not depend on first-layout frame timing. */
    const int smokeNavigationPresses = smokeFrameLimitSteps;
    const int smokeActivateFrame =
        smokeMoveStart + 2 * smokeNavigationPresses;
    /* Leave enough rendered frames for a failed persistence attempt to expose
     * Retry, for its queued SDL click to cross ImGui's next-frame boundary,
     * and for the successful same-process save verdict to be observed. */
    const int smokeMinimumFrames = smokeUsesGamepad
        ? smokeActivateFrame + 8
        : smokeFrameLimitSteps * 3 + 12;
    if (smokeFrameLimit && smokeFrameLimitSteps >= 0 &&
        frames < smokeMinimumFrames) {
        frames = smokeMinimumFrames;
    }
    const char *shot      = std::getenv("MDKR_APP_SMOKE_SHOT");
    const char *smokeCharacterWait =
        std::getenv("MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS");
    const char *smokeCharacterWaitToken =
        std::getenv("MDKR_APP_SMOKE_WAIT_CHARACTER_JOBS_TOKEN");
    const bool anyCharacterWaitContract =
        (smokeCharacterWait && smokeCharacterWait[0]) ||
        (smokeCharacterWaitToken && smokeCharacterWaitToken[0]);
    const bool waitForCharacterJobs = anyCharacterWaitContract &&
        smokeCharacterWait &&
        std::strcmp(smokeCharacterWait, "1") == 0 &&
        smokeCharacterWaitToken &&
        std::strcmp(smokeCharacterWaitToken,
                    "mdkr64-character-jobs-v1") == 0;
    if (anyCharacterWaitContract && !waitForCharacterJobs) {
        std::fprintf(stderr,
                     "[app] smoke: invalid Character Workshop wait contract\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    const char *smokeQuitDuringCharacterWork =
        std::getenv("MDKR_APP_SMOKE_QUIT_DURING_CHARACTER_WORK");
    const char *smokeQuitDuringCharacterWorkToken =
        std::getenv("MDKR_APP_SMOKE_QUIT_DURING_CHARACTER_WORK_TOKEN");
    const bool anyCharacterQuitContract =
        (smokeQuitDuringCharacterWork &&
         smokeQuitDuringCharacterWork[0]) ||
        (smokeQuitDuringCharacterWorkToken &&
         smokeQuitDuringCharacterWorkToken[0]);
    const bool quitDuringCharacterWork = anyCharacterQuitContract &&
        waitForCharacterJobs && smokeQuitDuringCharacterWork &&
        std::strcmp(smokeQuitDuringCharacterWork, "1") == 0 &&
        smokeQuitDuringCharacterWorkToken &&
        std::strcmp(smokeQuitDuringCharacterWorkToken,
                    "mdkr64-character-quit-v1") == 0;
    if (anyCharacterQuitContract && !quitDuringCharacterWork) {
        std::fprintf(
            stderr,
            "[app] smoke: invalid Character Workshop quit contract\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
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
    const bool smokeDropPlay =
        std::getenv("MDKR_APP_SMOKE_DROP_PLAY") != nullptr;
    const bool smokeDropPlayMutate =
        std::getenv("MDKR_APP_SMOKE_DROP_PLAY_MUTATE") != nullptr;
    const char *smokeReplacementPlay =
        std::getenv("MDKR_APP_SMOKE_REPLACEMENT_PLAY");
    const int   dropFrame = (frames > 1) ? 1 : 0;
    bool        sawQuit   = false;
    bool characterQuitRequested = false;
    bool characterQuitPublished = false;
    bool characterQuitPublishedWhilePending = false;
    /* Starts false whenever an image was requested: only the final frame's
     * successful write may set it. A break before that frame must not leave
     * the AUDIT-0046 capture gate reporting an image nobody produced. */
    bool        captureOk = !(shot && shot[0]);
    bool        renderOk  = true;
    const bool expectSaveFailure =
        std::getenv("MDKR_APP_SMOKE_EXPECT_SAVE_FAILURE") != nullptr;
    const char *restoreConfigDirectory =
        std::getenv("MDKR_APP_SMOKE_RESTORE_CONFIG_DIR");
    const bool retryAfterRestore = restoreConfigDirectory &&
                                   restoreConfigDirectory[0] && expectSaveFailure;
    bool       retryScheduled    = false;
    int        retryVisibleFrames = 0;
    int        retryLastX = -1, retryLastY = -1;
    bool       keyboardActivationQueued = false;
    bool       keyboardMovePending = false;
    int        keyboardMoveFromIndex = -2;
    const bool useGamepad        = smokeUsesGamepad;
    const float scaleAtDragStart = AppTheme::uiScale();
    const unsigned scaleApplicationsAtStart =
        AppTheme::uiScaleApplicationCount();
    int scaleRect[4] = {0, 0, 0, 0};
    bool scaleRectCaptured = false;
    bool scaleDragQueued = false;
    bool scaleStableWhileHeld = true;
    int touchRect[4] = {0, 0, 0, 0};
    int touchStartX = 0;
    int touchStartY = 0;
    int touchScriptStep = 0;
    float touchStartScroll = 0.0f;
    float touchFinalScroll = 0.0f;
    bool touchGestureQueued = false;
    bool touchGestureReleased = false;
    int wheelRect[4] = {0, 0, 0, 0};
    int wheelPointerX = 0;
    int wheelPointerY = 0;
    int wheelScriptStep = 0;
    float wheelStartScroll = 0.0f;
    float wheelFinalScroll = 0.0f;
    bool wheelQueued = false;
    bool onlineActionInputQueued = false;
    int smokeA11yWalkFrame = 0;
    int smokeA11yWaitFrames = 0;
    const Uint64 smokeA11yDeadline = SDL_GetTicks64() + 120000u;
    int smokePlayActions = 0;
    std::string smokePlayActionRom;
    auto observeSmokePlay = [&](const LauncherAction &action) {
        if (action.type != LauncherActionType::Play) return;
        ++smokePlayActions;
        smokePlayActionRom = action.boot.rom_path ? action.boot.rom_path : "";
    };
    auto observeSmokeQuit = [&](const LauncherAction &action) {
        if (action.type != LauncherActionType::Quit) return;
        characterQuitPublished = true;
        characterQuitPublishedWhilePending =
            characterQuitPublishedWhilePending ||
            Settings_characterWorkPending();
    };
    int characterServiceFrames = 0;
    bool characterJobsSettled = true;
    if (smokeDropPlayMutate && !smokeDropPlay) {
        std::fprintf(stderr,
                     "[app] smoke: final Play mutation requires final Play check\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    if (smokeReplacementPlay && smokeReplacementPlay[0] &&
        (!smokeDrop || !smokeDrop[0])) {
        std::fprintf(stderr,
                     "[app] smoke: replacement Play requires an initial drop\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    if (smokeFrameLimit &&
        (std::strcmp(smokeFrameLimit, "240") != 0 ||
         smokeFrameLimitSteps < 0)) {
        std::fprintf(stderr,
                     "[app] smoke: unsupported scripted frame limit %s "
                     "(only 240 is defined)\n",
                     smokeFrameLimit);
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    for (int i = 0;
         i < frames ||
         (smokeA11yWalk && smokeA11yWalkFrame < frames &&
          SDL_GetTicks64() < smokeA11yDeadline);
         ++i) {
        if (smokeDrop && smokeDrop[0] && i == dropFrame) {
            host.queueDropFileForSmoke(smokeDrop);
        }
        if (host.pumpAndShouldQuit()) {
            sawQuit = true;
            launcher.requestQuit();
        }
        host.beginFrame();
        const LauncherAction action = launcher.draw(host);
        observeSmokePlay(action);
        observeSmokeQuit(action);
        if (smokeA11yEscapeArmed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            // After the panel drew (and spoke) this frame, beside the speech
            // trace on stdout: what is spoken after this line came after the
            // Escape. The walk queues input faster than ImGui consumes it, so
            // the frame that queued the Escape is not this one.
            std::printf("[app-a11y-walk] escape reached imgui frame=%d\n",
                        smokeA11yWalkFrame);
            std::fflush(stdout);
        }
        const bool smokeSequenceFinalFrame = smokeA11yWalk
            ? !launcher.state().romValidationPending &&
                  smokeA11yWalkFrame == frames - 1
            : i == frames - 1;
        const bool ok = host.endFrame(
            (smokeSequenceFinalFrame && !waitForCharacterJobs)
                ? shot : nullptr);
        renderOk      = renderOk && ok;
        if (smokeSequenceFinalFrame && !waitForCharacterJobs) captureOk = ok;
        /* A renderer failure is terminal. Starting another ImGui frame and
         * returning before ImGui::Render leaves dynamic texture state
         * half-transitioned and can make renderer shutdown release an
         * invalid atlas handle. Stop at the first failed presentation. */
        if (!ok) break;

        if (quitDuringCharacterWork && !characterQuitRequested &&
            Settings_characterWorkPending()) {
            launcher.requestQuit();
            characterQuitRequested = true;
        }

        /* Character authoring chains multiple real subprocess publications
         * (for example ZIP conversion followed by GLB inspection).
         * Unthrottled smoke frames are not a clock and can all render before
         * either worker gets CPU. Pause scripted input while servicing the
         * same launcher/UI-thread publication route, with a hard deadline so
         * a broken compiler cannot hang CI. This also makes every following
         * scripted action operate on the settled UI a player would see. */
        if (waitForCharacterJobs) {
            const Uint64 characterDeadline = SDL_GetTicks64() + 15000u;
            while (Settings_smokeCharacterWorkPending() &&
                   SDL_GetTicks64() < characterDeadline && renderOk) {
                if (host.waitAndPump(1)) {
                    sawQuit = true;
                    launcher.requestQuit();
                }
                host.beginFrame();
                const LauncherAction serviceAction = launcher.draw(host);
                observeSmokePlay(serviceAction);
                observeSmokeQuit(serviceAction);
                renderOk = host.endFrame() && renderOk;
                ++characterServiceFrames;
            }
            if (Settings_smokeCharacterWorkPending()) {
                characterJobsSettled = false;
                renderOk = false;
            }
            if (i == frames - 1 && renderOk) {
                if (host.waitAndPump(1)) {
                    sawQuit = true;
                    launcher.requestQuit();
                }
                host.beginFrame();
                const LauncherAction settledAction = launcher.draw(host);
                observeSmokePlay(settledAction);
                observeSmokeQuit(settledAction);
                const bool settledFrameOk = host.endFrame(shot);
                renderOk = renderOk && settledFrameOk;
                captureOk = settledFrameOk;
                ++characterServiceFrames;
                if (Settings_smokeCharacterWorkPending()) {
                    characterJobsSettled = false;
                    renderOk = false;
                }
            }
            if (!renderOk) break;
        }

        if (smokeNavigation && !smokeNavigationQueued) {
            int x = 0;
            int y = 0;
            if (Launcher_smokeTopTabCenter(
                    static_cast<int>(smokeNavigationPanel), &x, &y)) {
                host.queueMouseClickForSmoke(x, y);
                smokeNavigationQueued = true;
                std::fprintf(
                    stderr,
                    "[app-ui-test] top navigation click queued target=%ld at %d,%d\n",
                    smokeNavigationPanel, x, y);
            }
        }

        if (smokeOnlineActionArmed) {
            const bool selection =
                smokeOnlineAction == MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER ||
                smokeOnlineAction == MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE ||
                smokeOnlineAction == MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK;
            // Let the token-gated SetKeyboardFocusHere request survive one
            // complete ImGui frame before activation. Selection combos then
            // need one settled popup frame around each navigation input.
            const bool activate = i == 2 || (selection && i == 8);
            const bool move = selection && i == 5;
            if (activate || move) {
                if (smokeUsesGamepad) {
                    onlineActionInputQueued = host.queueGamepadPressForSmoke(
                        move ? SDL_CONTROLLER_BUTTON_DPAD_DOWN
                             : SDL_CONTROLLER_BUTTON_A) ||
                        onlineActionInputQueued;
                } else {
                    host.queueKeyPressForSmoke(
                        move ? SDLK_DOWN
                             : SDLK_RETURN);
                    onlineActionInputQueued = true;
                }
            }
        }

        if (smokeUiScaleDrag) {
            int currentRect[4] = {0, 0, 0, 0};
            // Through frame 8 the real SDL button is held or its release is
            // being consumed by ImGui: this arm is driving the slider, so the
            // slider not being on screen is a failure to drive it.
            const bool dragUnderway = i <= 8;
            if (!Settings_smokeUiScaleRect(
                    &currentRect[0], &currentRect[1],
                    &currentRect[2], &currentRect[3])) {
                /* Afterwards the slider legitimately may leave the panel:
                 * applying 2.00x re-lays the whole panel out at double size and
                 * pushes this row below the fold of the 700pt window this arm
                 * uses. Settings_smokeUiScaleRect() reports that truthfully now
                 * rather than latching the last rectangle it ever saw, so say
                 * it and move on -- the transaction the arm actually claims
                 * (one application, stable while held, persisted) is decided
                 * after the loop and never reads this rectangle. */
                if (dragUnderway) {
                    std::fprintf(
                        stderr,
                        "[app-ui-test] UI-scale slider was not rendered\n");
                    renderOk = false;
                } else {
                    std::fprintf(
                        stderr,
                        "[app-ui-test] ui-scale drag frame=%d slider off screen "
                        "after the applied scale re-laid the panel out; the "
                        "drag transaction was already complete\n",
                        i);
                }
            } else if (!scaleRectCaptured && i >= 1) {
                // Give the launcher's responsive layout one complete warm-up
                // frame before taking the invariant rectangle. This separates
                // ordinary first-layout settling from pointer-driven motion.
                std::memcpy(scaleRect, currentRect, sizeof(scaleRect));
                scaleRectCaptured = true;
                const float normalized =
                    (scaleAtDragStart - 0.75f) / (2.0f - 0.75f);
                const int startX = scaleRect[0] + static_cast<int>(
                    normalized * static_cast<float>(scaleRect[2] - scaleRect[0]));
                const int centerY = (scaleRect[1] + scaleRect[3]) / 2;
                scaleDragQueued =
                    host.queueMouseDragStepForSmoke(startX, centerY, true);
                std::fprintf(
                    stderr,
                    "[app-ui-test] ui-scale drag begin applied=%.2f rect=%d,%d,%d,%d\n",
                    static_cast<double>(scaleAtDragStart),
                    scaleRect[0], scaleRect[1], scaleRect[2], scaleRect[3]);
            } else if (scaleRectCaptured) {
                // Neither the applied scale nor the slider geometry may move
                // while the button is held (see dragUnderway above).
                if (dragUnderway) {
                    scaleStableWhileHeld = scaleStableWhileHeld &&
                        std::fabs(AppTheme::uiScale() - scaleAtDragStart) < 0.001f &&
                        AppTheme::uiScaleApplicationCount() ==
                            scaleApplicationsAtStart &&
                        std::memcmp(scaleRect, currentRect, sizeof(scaleRect)) == 0;
                }
                const int centerY = (scaleRect[1] + scaleRect[3]) / 2;
                // Five motion frames cross the entire slider while one real
                // SDL left-button press remains held. Moving beyond the right
                // edge makes the qualified 2.00 target exact and clamped.
                if (i >= 2 && i <= 6) {
                    const float fraction = static_cast<float>(i - 1) / 5.0f;
                    const int x = scaleRect[0] + static_cast<int>(
                        fraction * static_cast<float>(scaleRect[2] - scaleRect[0] + 8));
                    scaleDragQueued = host.queueMouseDragStepForSmoke(
                                          x, centerY, true) && scaleDragQueued;
                } else if (i == 7) {
                    scaleDragQueued = host.queueMouseDragStepForSmoke(
                                          scaleRect[2] + 8, centerY, false) &&
                                      scaleDragQueued;
                }
                std::fprintf(
                    stderr,
                    "[app-ui-test] ui-scale drag frame=%d held=%d editStable=%d "
                    "applied=%.2f applications=%u rect=%d,%d,%d,%d\n",
                    i, i <= 7 ? 1 : 0, scaleStableWhileHeld ? 1 : 0,
                    static_cast<double>(AppTheme::uiScale()),
                    AppTheme::uiScaleApplicationCount() - scaleApplicationsAtStart,
                    currentRect[0], currentRect[1], currentRect[2], currentRect[3]);
            }
        }

        if (smokeTouch) {
            int currentRect[4] = {0, 0, 0, 0};
            if (!Launcher_smokePanelScrollRect(
                    &currentRect[0], &currentRect[1],
                    &currentRect[2], &currentRect[3])) {
                if (i >= 1) {
                    std::fprintf(stderr,
                                 "[app-ui-test] touch scroll viewport was not rendered\n");
                    renderOk = false;
                }
            } else {
                touchFinalScroll = Launcher_smokePanelScrollY();
                if (touchScriptStep == 0 && i >= 1) {
                    std::memcpy(touchRect, currentRect, sizeof(touchRect));
                    touchStartX = touchRect[0] +
                        (touchRect[2] - touchRect[0]) * 3 / 4;
                    touchStartY = touchRect[3] -
                        static_cast<int>(48.0f * AppTheme::uiScale());
                    touchStartScroll = touchFinalScroll;
                    touchGestureQueued = host.queueTouchDragStepForSmoke(
                        touchStartX, touchStartY, true);
                    touchScriptStep = 1;
                } else if (touchScriptStep >= 1 && touchScriptStep <= 4) {
                    const int y = touchStartY - static_cast<int>(
                        60.0f * AppTheme::uiScale() * touchScriptStep);
                    touchGestureQueued = host.queueTouchDragStepForSmoke(
                                             touchStartX, y, true) &&
                                         touchGestureQueued;
                    ++touchScriptStep;
                } else if (touchScriptStep == 5) {
                    const int y = touchStartY - static_cast<int>(
                        240.0f * AppTheme::uiScale());
                    touchGestureReleased = host.queueTouchDragStepForSmoke(
                        touchStartX, y, false);
                    touchScriptStep = 6;
                }
            }
        }

        if (smokeWheel) {
            int currentRect[4] = {0, 0, 0, 0};
            if (!Launcher_smokePanelScrollRect(
                    &currentRect[0], &currentRect[1],
                    &currentRect[2], &currentRect[3])) {
                if (i >= 1) {
                    std::fprintf(stderr,
                                 "[app-ui-test] wheel scroll viewport was not rendered\n");
                    renderOk = false;
                }
            } else {
                wheelFinalScroll = Launcher_smokePanelScrollY();
                if (wheelScriptStep == 0 && i >= 1) {
                    std::memcpy(wheelRect, currentRect, sizeof(wheelRect));
                    wheelPointerX = wheelRect[0] +
                        (wheelRect[2] - wheelRect[0]) / 2;
                    wheelPointerY = wheelRect[1] +
                        (wheelRect[3] - wheelRect[1]) / 2;
                    wheelStartScroll = wheelFinalScroll;
                    // Move onto the panel first: ImGui reports a freshly entered
                    // window as not-hovered for one frame, which would drop the
                    // opening notch and route it nowhere.
                    wheelQueued = host.queueWheelStepForSmoke(
                        wheelPointerX, wheelPointerY, 0.0f);
                    wheelScriptStep = 1;
                } else if (wheelScriptStep >= 1 && wheelScriptStep <= 6) {
                    // A MacBook trackpad two-finger scroll: the integer wheel.y
                    // truncates to zero and ONLY preciseY carries the motion.
                    // +/-0.6 per event reproduces exactly that shape (int y ==
                    // 0), and summed it clears the scroll threshold -- so a
                    // handler that reads the integer field instead of preciseY
                    // scrolls nothing here and fails, which is the whole point.
                    // In flipped mode the FLIPPED flag is set and the sign is
                    // UNCHANGED, because that is what SDL delivers on macOS: a
                    // bridge that negates on the flag scrolls up, clamps at the
                    // top, and fails here.
                    wheelQueued = host.queueWheelStepForSmoke(
                                      wheelPointerX, wheelPointerY,
                                      smokeWheelDelta) &&
                                  wheelQueued;
                    ++wheelScriptStep;
                }
            }
        }

        // Save-failure recovery stays in this process and uses the visible
        // Retry widget. Its appearance proves the rejected enum value was
        // retained; the click still traverses SDL -> ImGui -> commitEdit.
        if (retryAfterRestore && !retryScheduled) {
            int x = 0, y = 0;
            if (Settings_smokeFrameLimitRetryCenter(&x, &y)) {
                // The failed selection closes an ImGui combo after the frame
                // that first renders Retry. Wait for one more complete layout
                // so the click targets the settled button, not its transient
                // popup-relative rectangle.
                //
                // Stability, not mere validity: the error row and Retry make
                // the panel taller, and the scrollbar that arrives a few
                // frames later re-wraps a text line above, moving the button.
                // A click aimed at the pre-settle coordinates lands on the
                // widget below. Restart the wait whenever the rect moves, so
                // the click is only ever taken at a layout that has held
                // still for two frames -- which is also when a person would
                // press it.
                if (x != retryLastX || y != retryLastY) {
                    retryVisibleFrames = 0;
                    retryLastX = x;
                    retryLastY = y;
                }
                ++retryVisibleFrames;
                if (retryVisibleFrames >= 2) {
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
            } else {
                retryVisibleFrames = 0;
            }
        }

        // One press of the real radio button, once it has been laid out. The
        // rectangle is only valid after a complete frame, so this waits for it
        // rather than assuming a fixed frame index.
        if (smokePresentationPace && !smokePaceClickQueued) {
            int x = 0, y = 0;
            if (Settings_smokePresentationPaceCenter(
                    smokePresentationPace, &x, &y)) {
                host.queueMouseClickForSmoke(x, y);
                smokePaceClickQueued = true;
                std::fprintf(stderr,
                             "[app-ui-test] presentation-pace click queued "
                             "pace=%s at %d,%d\n",
                             smokePresentationPace, x, y);
            }
        }

        if (smokeA11yWalk && !launcher.state().romValidationPending) {
            /*
             * Two phases, not one interleaved stream. Keyboard gets a clean
             * linear Tab pass before arrow navigation. Gamepad gets a forward
             * right/down spatial sweep before its reverse left/up sample. The
             * boundary lets each gate distinguish its primary coverage from
             * the shorter directional-reversal phase.
             */
            const int tabFrames =
                smokeA11yEscapeArmed ? frames : frames - frames / 4;
            const int importFocusFrame = 16;
            const int workshopReturnFrame = 24;
            const bool shortcutSettle =
                (smokeCharacterImportFocusArmed &&
                 (smokeA11yWalkFrame == importFocusFrame ||
                  smokeA11yWalkFrame == importFocusFrame + 1)) ||
                (smokeCharacterWorkshopReturnArmed &&
                 (smokeA11yWalkFrame == workshopReturnFrame ||
                  smokeA11yWalkFrame == workshopReturnFrame + 1));
            if (smokeA11yWalkFrame == importFocusFrame &&
                smokeCharacterImportFocusArmed) {
                if (smokeUsesGamepad) {
                    renderOk = host.queueGamepadPressForSmoke(
                                   SDL_CONTROLLER_BUTTON_BACK) &&
                        renderOk;
                } else {
                    host.queueKeyChordForSmoke(SDLK_i, KMOD_CTRL);
                }
                std::fprintf(
                    stderr,
                    "[app-ui-test] character import-focus shortcut queued input=%s\n",
                    smokeUsesGamepad ? "gamepad" : "keyboard");
            }
            if (smokeA11yWalkFrame == workshopReturnFrame &&
                smokeCharacterWorkshopReturnArmed) {
                if (smokeUsesGamepad) {
                    renderOk = host.queueGamepadPressForSmoke(
                                   SDL_CONTROLLER_BUTTON_B) &&
                        renderOk;
                } else {
                    host.queueKeyPressForSmoke(SDLK_ESCAPE);
                }
                std::fprintf(
                    stderr,
                    "[app-ui-test] character Workshop return shortcut queued input=%s\n",
                    smokeUsesGamepad ? "gamepad" : "keyboard");
            }
            if (shortcutSettle) {
                // Leave one complete post-shortcut frame free of navigation
                // input so the newly focused source control is both rendered
                // and announced before the ordinary traversal resumes.
            } else if (smokeA11yEscapeArmed &&
                       smokeA11yWalkFrame == smokeA11yEscapeTabs) {
                // In place of one Tab, so exactly N Tabs precede it in the
                // queue whatever the frame rate.
                host.queueKeyPressForSmoke(SDLK_ESCAPE);
            } else if (smokeA11yWalkFrame < tabFrames) {
                if (smokeUsesGamepad) {
                    // Sweep across each responsive row before advancing. A
                    // D-pad-only vertical walk can skip every second radio
                    // button and all but the first cell of a grid even though
                    // those controls are reachable by a real controller.
                    const SDL_GameControllerButton direction =
                        smokeA11yWalkFrame % 4 == 3
                            ? SDL_CONTROLLER_BUTTON_DPAD_DOWN
                            : SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
                    renderOk = host.queueGamepadPressForSmoke(
                                   direction) &&
                        renderOk;
                } else {
                    host.queueKeyPressForSmoke(SDLK_TAB);
                }
            } else {
                if (smokeA11yWalkFrame == tabFrames) {
                    if (smokeUsesGamepad) {
                        std::printf(
                            "[app-a11y-walk] primary phase complete input=gamepad frame=%d\n",
                            smokeA11yWalkFrame);
                    } else {
                        std::printf(
                            "[app-a11y-walk] tab phase complete frame=%d\n",
                            smokeA11yWalkFrame);
                    }
                    std::fflush(stdout);
                }
                const bool forward =
                    ((smokeA11yWalkFrame - tabFrames) % 8) < 6;
                if (smokeUsesGamepad) {
                    const bool vertical =
                        (smokeA11yWalkFrame - tabFrames) % 4 == 3;
                    SDL_GameControllerButton direction;
                    if (vertical) {
                        direction = forward
                            ? SDL_CONTROLLER_BUTTON_DPAD_DOWN
                            : SDL_CONTROLLER_BUTTON_DPAD_UP;
                    } else {
                        direction = forward
                            ? SDL_CONTROLLER_BUTTON_DPAD_RIGHT
                            : SDL_CONTROLLER_BUTTON_DPAD_LEFT;
                    }
                    renderOk = host.queueGamepadPressForSmoke(
                                   direction) &&
                        renderOk;
                } else {
                    host.queueKeyPressForSmoke(
                        forward ? SDLK_DOWN : SDLK_UP);
                }
            }
            ++smokeA11yWalkFrame;
        } else if (smokeA11yWalk) {
            ++smokeA11yWaitFrames;
        }

        // Real widget navigation: keyboard movement follows the focused row in
        // the rendered popup; gamepad retains its explicit production-input
        // choreography. Every action still enters through SDL and ImGui.
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
            } else if (!useGamepad && !keyboardActivationQueued) {
                int focusedIndex = -1;
                if (Settings_smokeFrameLimitPopup(&focusedIndex)) {
                    if (keyboardMovePending) {
                        const bool moveObserved =
                            (keyboardMoveFromIndex < 0 && focusedIndex >= 0) ||
                            (keyboardMoveFromIndex >= 0 &&
                             focusedIndex != keyboardMoveFromIndex);
                        if (moveObserved) keyboardMovePending = false;
                    } else if (focusedIndex < 0) {
                        host.queueKeyPressForSmoke(SDLK_HOME);
                        keyboardMovePending = true;
                        keyboardMoveFromIndex = focusedIndex;
                    } else if (focusedIndex < smokeFrameLimitSteps) {
                        host.queueKeyPressForSmoke(SDLK_DOWN);
                        keyboardMovePending = true;
                        keyboardMoveFromIndex = focusedIndex;
                    } else if (focusedIndex > smokeFrameLimitSteps) {
                        host.queueKeyPressForSmoke(SDLK_UP);
                        keyboardMovePending = true;
                        keyboardMoveFromIndex = focusedIndex;
                    } else {
                        host.queueKeyPressForSmoke(SDLK_RETURN);
                        keyboardActivationQueued = true;
                    }
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
            } else if (useGamepad && i >= smokeMoveStart &&
                       i < smokeActivateFrame && (i % 2) == 0) {
                renderOk = host.queueGamepadPressForSmoke(
                               SDL_CONTROLLER_BUTTON_DPAD_DOWN) &&
                           renderOk;
            } else if (useGamepad && i == smokeActivateFrame) {
                renderOk = host.queueGamepadPressForSmoke(
                               SDL_CONTROLLER_BUTTON_A) &&
                           renderOk;
            }
            /* No keyboard tail: the popup-tracking branch above owns the
             * whole keyboard selection and sets keyboardActivationQueued when
             * it presses Return. Falling through to this fixed-frame
             * choreography afterwards queued a Down every even frame and a
             * Return at the activation frame -- keys that cancel a scripted
             * mouse press mid-click (nav steals the active widget) and then
             * activate whichever row focus had drifted to. The same-process
             * Retry arm only ever passed by winning that race. */
        }
    }

    if (smokeA11yWalk) {
        const bool completed = smokeA11yWalkFrame == frames;
        std::fprintf(
            stderr,
            "[app-ui-test] accessibility walk frames=%d/%d readiness-wait-frames=%d settled=%d\n",
            smokeA11yWalkFrame, frames, smokeA11yWaitFrames,
            launcher.state().romValidationPending ? 0 : 1);
        renderOk = renderOk && completed;
    }

    if (waitForCharacterJobs) {
        std::fprintf(
            stderr,
            "[app] smoke: Character Workshop serviceFrames=%d settled=%d\n",
            characterServiceFrames, characterJobsSettled ? 1 : 0);
    }
    if (quitDuringCharacterWork) {
        std::fprintf(
            stderr,
            "[app-ui-test] character quit lifecycle requested=%d published=%d pending-at-publication=%d settled=%d\n",
            characterQuitRequested ? 1 : 0,
            characterQuitPublished ? 1 : 0,
            characterQuitPublishedWhilePending ? 1 : 0,
            Settings_characterWorkPending() ? 0 : 1);
        renderOk = renderOk && characterQuitRequested &&
            characterQuitPublished &&
            !characterQuitPublishedWhilePending &&
            !Settings_characterWorkPending();
    }

    if (smokeTouch) {
        const float moved = touchFinalScroll - touchStartScroll;
        const ImGuiStyle &style = ImGui::GetStyle();
        const float effectiveFrameHeight =
            ImGui::GetFrameHeight() + style.TouchExtraPadding.y * 2.0f;
        const float expectedTarget = 44.0f * AppTheme::uiScale();
        const bool targetQualified =
            effectiveFrameHeight >= expectedTarget - 0.5f &&
            style.ScrollbarSize >= 20.0f * AppTheme::uiScale() - 0.5f &&
            style.GrabMinSize >= 20.0f * AppTheme::uiScale() - 0.5f;
        const bool scrollQualified = touchGestureQueued &&
            touchGestureReleased &&
            moved >= 80.0f * AppTheme::uiScale();
        std::fprintf(
            stderr,
            "[app-ui-test] touch handheld targets=%d scroll=%d "
            "effective=%.1f expected=%.1f scrollbar=%.1f grab=%.1f "
            "start=%.1f final=%.1f moved=%.1f rect=%d,%d,%d,%d\n",
            targetQualified ? 1 : 0, scrollQualified ? 1 : 0,
            static_cast<double>(effectiveFrameHeight),
            static_cast<double>(expectedTarget),
            static_cast<double>(style.ScrollbarSize),
            static_cast<double>(style.GrabMinSize),
            static_cast<double>(touchStartScroll),
            static_cast<double>(touchFinalScroll),
            static_cast<double>(moved), touchRect[0], touchRect[1],
            touchRect[2], touchRect[3]);
        renderOk = renderOk && targetQualified && scrollQualified;
    }

    if (smokeWheel) {
        const float moved = wheelFinalScroll - wheelStartScroll;
        const bool scrollQualified =
            wheelQueued && moved >= 80.0f * AppTheme::uiScale();
        std::fprintf(
            stderr,
            "[app-ui-test] wheel handheld scroll=%d start=%.1f final=%.1f "
            "moved=%.1f rect=%d,%d,%d,%d\n",
            scrollQualified ? 1 : 0,
            static_cast<double>(wheelStartScroll),
            static_cast<double>(wheelFinalScroll),
            static_cast<double>(moved), wheelRect[0], wheelRect[1],
            wheelRect[2], wheelRect[3]);
        renderOk = renderOk && scrollQualified;
    }

    if (smokeOnlineActionArmed) {
        bool actionAccepted = false;
        const bool witnessed = OnlineRoom_smokeActionResult(
            static_cast<unsigned>(smokeOnlineAction), &actionAccepted);
        std::fprintf(stderr,
                     "[app-ui-test] online action=%ld input=%s queued=%u "
                     "witnessed=%u accepted=%u\n",
                     smokeOnlineAction,
                     smokeUsesGamepad ? "gamepad" : "keyboard",
                     onlineActionInputQueued ? 1u : 0u,
                     witnessed ? 1u : 0u, actionAccepted ? 1u : 0u);
        renderOk = renderOk && onlineActionInputQueued && witnessed &&
            actionAccepted;
    }

    /* A full ROM check is deliberately asynchronous. Drop qualification keeps
     * servicing and rendering the real launcher until that worker publishes,
     * with a hard deadline so a stalled removable/network drive fails the gate
     * rather than freezing the UI or hanging CI. These service frames do not
     * change the requested smoke-frame contract printed below. */
    if (smokeDrop && smokeDrop[0] &&
        launcher.state().romValidationPending && renderOk) {
        const Uint64 validationDeadline = SDL_GetTicks64() + 5000u;
        int validationServiceFrames = 0;
        while (launcher.state().romValidationPending &&
               SDL_GetTicks64() < validationDeadline && renderOk) {
            if (host.waitAndPump(1)) sawQuit = true;
            host.beginFrame();
            const LauncherAction action = launcher.draw(host);
            observeSmokePlay(action);
            renderOk = host.endFrame();
            validationServiceFrames++;
        }
        std::fprintf(
            stderr,
            "[app] smoke: async ROM check serviceFrames=%d settled=%d\n",
            validationServiceFrames,
            launcher.state().romValidationPending ? 0 : 1);
        if (launcher.state().romValidationPending) {
            renderOk = false;
        }
    }

    /* A proven active ROM must remain playable while a replacement is being
     * checked, AND pressing Play must never throw the replacement away out
     * from under the player: begin a real second SDL drop, press Play while
     * that candidate is still mid-check, and require Play to WAIT for the
     * pending check rather than abandon it -- landing on the NEW ROM once the
     * check resolves it as valid, exactly as if Play had been pressed after
     * the check finished on its own. (A prior revision made Play cancel the
     * unresolved replacement and immediately re-affirm the ROM being
     * replaced, which silently discarded a fully valid selection -- the
     * reported "picking a different supported ROM reverts to the original"
     * bug.) */
    if (smokeReplacementPlay && smokeReplacementPlay[0] && renderOk) {
        const LauncherState &initial = launcher.state();
        const std::string activeRom = initial.romPath;
        const bool initialReady = !initial.romValidationPending &&
                                  initial.romInfo.valid && !activeRom.empty();
        bool replacementPending = false;
        bool deferred = false;
        int serviceFrames = 0;
        if (initialReady) {
            host.queueDropFileForSmoke(smokeReplacementPlay);
            if (host.pumpAndShouldQuit()) sawQuit = true;
            host.beginFrame();
            const LauncherAction replacementAction = launcher.draw(host);
            observeSmokePlay(replacementAction);
            renderOk = host.endFrame() && renderOk;
            const LauncherState &replacement = launcher.state();
            replacementPending = replacement.romValidationPending &&
                !replacement.romPlayValidationPending &&
                replacement.romInfo.valid &&
                replacement.romPath == activeRom &&
                replacement.romValidationPath == smokeReplacementPlay;
            if (replacementPending) {
                launcher.requestPlayValidationForSmoke();
                const LauncherState &afterPlay = launcher.state();
                // Play must leave the pending replacement running on the NEW
                // file untouched: still checking, still targeting the
                // candidate path, active ROM unchanged so far, and no
                // early/duplicate Play-purpose check against the OLD ROM.
                deferred = afterPlay.romValidationPending &&
                    !afterPlay.romPlayValidationPending &&
                    afterPlay.romPath == activeRom &&
                    afterPlay.romValidationPath == smokeReplacementPlay;
            }
        }
        const Uint64 deadline = SDL_GetTicks64() + 5000u;
        while (deferred &&
               (launcher.state().romValidationPending ||
                launcher.state().romPlayValidationPending) &&
               SDL_GetTicks64() < deadline && renderOk) {
            if (host.waitAndPump(1)) sawQuit = true;
            host.beginFrame();
            const LauncherAction action = launcher.draw(host);
            observeSmokePlay(action);
            renderOk = host.endFrame() && renderOk;
            ++serviceFrames;
        }
        const LauncherState &finalState = launcher.state();
        const bool settled = !finalState.romValidationPending &&
                             !finalState.romPlayValidationPending;
        std::printf(
            "[app] smoke: replacement Play candidate=%s initialReady=%d "
            "replacementPending=%d deferred=%d serviceFrames=%d actions=%d "
            "actionRom=%s settled=%d active=%s candidateVisible=%d\n",
            smokeReplacementPlay, initialReady ? 1 : 0,
            replacementPending ? 1 : 0, deferred ? 1 : 0,
            serviceFrames, smokePlayActions,
            smokePlayActionRom.empty() ? "(none)" : smokePlayActionRom.c_str(),
            settled ? 1 : 0,
            finalState.romPath.empty() ? "(none)" : finalState.romPath.c_str(),
            finalState.romCandidateVisible ? 1 : 0);
        if (!initialReady || !replacementPending || !deferred || !settled ||
            serviceFrames < 1 || smokePlayActions != 1 ||
            smokePlayActionRom != smokeReplacementPlay ||
            finalState.romPath != smokeReplacementPlay ||
            finalState.romCandidateVisible) {
            renderOk = false;
        }
    }

    /* The Play widget never trusts the earlier selection verdict: it starts a
     * second worker validation and emits a Play action only once that final
     * result still matches the active path.  Exercise that full production
     * transition without calling runAutoplay(), so this remains a short
     * launcher smoke rather than a game-session test. */
    if (smokeDropPlay && smokeDrop && smokeDrop[0] && renderOk) {
        const LauncherState &selected = launcher.state();
        const bool initialReady = !selected.romValidationPending &&
                                  selected.romInfo.valid &&
                                  selected.romPath == smokeDrop;
        // `selected` is a live reference, so reading romPath after the recheck
        // loop would print whatever the path IS, under a label promising what
        // it WAS. Copy the value the verdict was formed from; the sibling block
        // above prints the live path under the honest label "active=".
        const std::string initialPath = selected.romPath;
        bool mutationApplied = false;
        bool recheckRequested = false;
        int playServiceFrames = 0;
        if (initialReady) {
            if (smokeDropPlayMutate) {
                mutationApplied = mutateSmokeRomForFinalCheck(smokeDrop);
            }
            if (!smokeDropPlayMutate || mutationApplied) {
                launcher.requestPlayValidationForSmoke();
                recheckRequested = launcher.state().romPlayValidationPending;
            }
        }
        const Uint64 playDeadline = SDL_GetTicks64() + 5000u;
        /* Run at least one frame after the request.  Apart from giving a fast
         * worker a publish point, this proves the hash is never run by
         * blocking the ImGui frame that asked to Play. */
        while (recheckRequested && renderOk &&
               (launcher.state().romValidationPending || playServiceFrames == 0) &&
               SDL_GetTicks64() < playDeadline) {
            if (host.waitAndPump(1)) sawQuit = true;
            host.beginFrame();
            const LauncherAction action = launcher.draw(host);
            observeSmokePlay(action);
            renderOk = host.endFrame();
            ++playServiceFrames;
        }
        const LauncherState &finalState = launcher.state();
        std::printf(
            "[app] smoke: final Play recheck requested=%d initialPath=%s "
            "initialValid=%d mutationApplied=%d serviceFrames=%d actions=%d "
            "actionRom=%s finalValid=%d settled=%d\n",
            recheckRequested ? 1 : 0,
            initialPath.empty() ? "(none)" : initialPath.c_str(),
            initialReady ? 1 : 0, mutationApplied ? 1 : 0,
            playServiceFrames, smokePlayActions,
            smokePlayActionRom.empty() ? "(none)" : smokePlayActionRom.c_str(),
            finalState.romInfo.valid ? 1 : 0,
            finalState.romValidationPending ? 0 : 1);
        if (!initialReady || !recheckRequested ||
            finalState.romValidationPending || playServiceFrames == 0) {
            renderOk = false;
        }
    }

    /*
     * "Skip the launcher" (issue #60). The decision was already taken in
     * main() and, when it armed, Launcher::draw() has been pressing Play on
     * its own since the remembered ROM settled. All this arm adds is TIME:
     * hashing a 32 MiB image takes longer than the handful of frames a smoke
     * renders, so keep drawing real launcher frames until a Play action
     * arrives or the deadline passes, then report what happened.
     *
     * Deliberately no pass/fail opinion here beyond rendering. Most of the
     * cases tests/check_launcher_skip.py drives expect NO boot, and a smoke
     * that failed on "no Play action" could not express them.
     */
    if (std::getenv("MDKR_APP_SMOKE_SKIP_LAUNCHER") != nullptr && renderOk) {
        int skipServiceFrames = 0;
        const Uint64 skipDeadline = SDL_GetTicks64() + 15000u;
        while (renderOk && smokePlayActions == 0 &&
               SDL_GetTicks64() < skipDeadline &&
               (launcher.state().romValidationPending ||
                launcher.state().romPlayValidationPending ||
                skipServiceFrames < 2)) {
            if (host.waitAndPump(1)) sawQuit = true;
            host.beginFrame();
            const LauncherAction action = launcher.draw(host);
            observeSmokePlay(action);
            renderOk = host.endFrame();
            ++skipServiceFrames;
        }
        const LauncherState &skipState = launcher.state();
        std::printf(
            "[app] smoke: skip-launcher armed=%d dispatched=%d "
            "serviceFrames=%d actions=%d actionRom=%s romPath=%s romValid=%d "
            "settled=%d\n",
            launcher.skipArmedForSmoke() ? 1 : 0,
            launcher.skipDispatchedForSmoke() ? 1 : 0,
            skipServiceFrames, smokePlayActions,
            smokePlayActionRom.empty() ? "(none)" : smokePlayActionRom.c_str(),
            skipState.romPath.empty() ? "(none)" : skipState.romPath.c_str(),
            skipState.romInfo.valid ? 1 : 0,
            skipState.romValidationPending ? 0 : 1);
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
            const LauncherAction action = launcher.draw(host);
            observeSmokePlay(action);
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
                    showingCandidate ? state.romCandidatePath.c_str()
                                     : (state.romPath.empty()
                                            ? "(none)" : state.romPath.c_str()),
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
            "candidateVisible=%d cancelAvailable=%d persistenceWarning=%d\n",
            state.romPath.empty() ? "(none)" : state.romPath.c_str(),
            state.romInfo.valid,
            state.romCandidateVisible ? 1 : 0,
            (state.romInfo.valid && state.romCandidateVisible) ? 1 : 0,
            state.romPersistenceWarning[0] ? 1 : 0);
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
    if (smokePresentationPace) {
        const MdkrVideoConfig *desiredConfig = mdkr_video_config_desired();
        const char *limit = desiredConfig
            ? desiredConfig->values[MDKR_VIDEO_FRAME_LIMIT].text : "";
        const char *smoothing = desiredConfig
            ? desiredConfig->values[MDKR_VIDEO_MOTION_SMOOTHING].text : "";
        const char *resolved = desiredConfig
            ? mdkr_video_presentation_pace_name(
                  mdkr_video_presentation_pace(desiredConfig))
            : "custom";
        std::printf(
            "[app-ui-test] presentation-pace requested=%s actual=%s "
            "frameLimit=%s motionSmoothing=%s clicked=%d\n",
            smokePresentationPace, resolved,
            limit[0] ? limit : "(none)",
            smoothing[0] ? smoothing : "(none)",
            smokePaceClickQueued ? 1 : 0);
        if (!smokePaceClickQueued ||
            std::strcmp(resolved, smokePresentationPace) != 0) {
            std::fprintf(stderr,
                         "[app-ui-test] scripted presentation pace did not "
                         "reach both pacing keys\n");
            renderOk = false;
        }
    }
    if (smokeUiScaleDrag) {
        const float actual = AppTheme::uiScale();
        float persisted = 0.0f;
        const std::string persistedText = AppConfig::get("ui_scale", "");
        const bool persistedOk =
            AppUi_parseScale(persistedText.c_str(), &persisted) &&
            std::fabs(persisted - smokeUiScaleTarget) < 0.001f;
        const unsigned applications =
            AppTheme::uiScaleApplicationCount() - scaleApplicationsAtStart;
        const bool verdict = scaleRectCaptured && scaleDragQueued &&
            scaleStableWhileHeld && applications == 1 &&
            std::fabs(actual - smokeUiScaleTarget) < 0.001f && persistedOk;
        std::printf(
            "[app-ui-test] ui-scale drag requested=%.2f actual=%.2f "
            "applications=%u stableWhileHeld=%d persisted=%d\n",
            static_cast<double>(smokeUiScaleTarget),
            static_cast<double>(actual), applications,
            scaleStableWhileHeld ? 1 : 0, persistedOk ? 1 : 0);
        if (!verdict) {
            std::fprintf(stderr,
                         "[app-ui-test] UI-scale drag transaction failed\n");
            renderOk = false;
        }
    }
    if (smokeNavigation) {
        const int actualPanel = launcher.activePanelForSmoke();
        /*
         * The top navigation offers DESTINATIONS, so a panel target means "go
         * to the destination that owns this panel". Comparing panel indices
         * asserted something the interface no longer promises: Online Room is
         * reached from inside Play, and no width of window gives it a tab of
         * its own.
         *
         * Choose a target whose destination differs from the starting panel's,
         * or this proves nothing -- landing where you already were satisfies
         * any destination comparison. The registered gates target Settings from
         * Play for exactly that reason.
         */
        const AppUiDestination wanted =
            AppUi_destinationForPanel(static_cast<int>(smokeNavigationPanel));
        const AppUiDestination reached = AppUi_destinationForPanel(actualPanel);
        std::printf(
            "[app-ui-test] top navigation target=%ld actual=%d queued=%d\n",
            smokeNavigationPanel, actualPanel,
            smokeNavigationQueued ? 1 : 0);
        if (!smokeNavigationQueued || reached != wanted) {
            renderOk = false;
        }
    }
    if (std::getenv("MDKR_APP_TEST_RESTART_RECOVERY_FRAMES") != nullptr) {
        const LauncherState &state = launcher.state();
        std::printf(
            "[app-restart-test] recovery launcher rom=%s bootRecovery=%d\n",
            state.romPath.empty() ? "(none)" : state.romPath.c_str(),
            state.bootErrorVisible ? 1 : 0);
    }
    const bool presentOk = !requirePresent ||
                           host.presentedFrames() >= requiredPresents;
    /* The smoke's launcher window ends here, and host.shutdown() below reaches
     * SDL_Quit(). Release while SDL still owns the pads: ~Launcher runs after
     * main()'s scope ends, which is far too late to hand a handle back. The
     * seven earlier bails in this function return before a frame is drawn, so
     * they have no window to close. */
    AppLaunchHold_release();
    shutdownLauncherHost(host, launcher);
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

int runAutoplay(AppHost &host, Launcher &launcher, SessionRuntime &session,
                EngineSessionTransition *transition,
                bool *restartReplacement) {
    MdkrBootConfig config{};
    std::string restartRom;
    const bool restartHandoff = std::getenv("MDKR_APP_RESTART_GAME") != nullptr;
    const char *restartProbe = std::getenv("MDKR_APP_TEST_RESTART_APPLY");
    if (restartReplacement != nullptr) *restartReplacement = restartHandoff;
    if (restartHandoff) {
        if (!AppRestart_consumeGame(restartRom)) {
            std::fprintf(stderr,
                         "[app] invalid Restart & Apply handoff; returning safely\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        config.rom_path = restartRom.c_str();
        std::fprintf(stderr, "[app] Restart & Apply handoff accepted\n");
    } else {
        config.rom_path = std::getenv("MDKR_ROM");
    }
    if (restartHandoff && restartProbe != nullptr &&
        std::strcmp(restartProbe, "boot-failure") == 0) {
        /* Test only: fail after the replacement has consumed its real handoff.
         * The parent then has to restore a normal launcher, not autoplay again. */
        std::fprintf(stderr,
                     "[app-restart-test] forcing post-restart boot failure\n");
        shutdownLauncherHost(host, launcher);
        return 2;
    }
    config.video_mode = -1;
    if (!applyAutoplayVideoSetting()) {
        shutdownLauncherHost(host, launcher);
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
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        config.automation_ticks = static_cast<int>(parsed);
    }
    if (const char *frames = std::getenv("MDKR_APP_AUTOPLAY_FRAMES")) {
        char *end = nullptr;
        const long parsed = std::strtol(frames, &end, 10);
        if (end == frames || *end != '\0' || parsed < 1 || parsed > 1000000 ||
            config.automation_ticks > 0) {
            std::fprintf(stderr,
                         "[app] invalid/conflicting MDKR_APP_AUTOPLAY_FRAMES=%s "
                         "(expected 1..1000000 and no tick limit)\n",
                         frames);
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        config.automation_frames = static_cast<int>(parsed);
    }
    config.input_script = std::getenv("MDKR_APP_AUTOPLAY_INPUT_SCRIPT");

    /* Test-only process-isolation seam. It deliberately enters through the
     * launcher-owned session envelope rather than installing a roster in the
     * engine directly. Both variables are required so a stale CI environment
     * cannot silently select an online topology. The legacy V2 path remains
     * the rollback laboratory default; MDKR_APP_TEST_ONLINE_LAUNCH_V3 selects
     * the production descriptor/direct-load seam. */
    const bool haveTestLocalMask =
        std::getenv("MDKR_APP_TEST_ONLINE_LOCAL_MASK") != nullptr;
    const bool haveTestViewportMask =
        std::getenv("MDKR_APP_TEST_ONLINE_VIEWPORT_MASK") != nullptr;
    const bool testOnline = haveTestLocalMask || haveTestViewportMask;
    const bool testLaunchV3 =
        std::getenv("MDKR_APP_TEST_ONLINE_LAUNCH_V3") != nullptr;
    std::uint8_t testLocalMask = 0u;
    std::uint8_t testViewportMask = 0u;
    if (testOnline) {
        if (!haveTestLocalMask || !haveTestViewportMask ||
            !parseTestOnlineMask("MDKR_APP_TEST_ONLINE_LOCAL_MASK",
                                 &testLocalMask) ||
            !parseTestOnlineMask("MDKR_APP_TEST_ONLINE_VIEWPORT_MASK",
                                 &testViewportMask) ||
            testLocalMask == 0u ||
            (testViewportMask &
             static_cast<std::uint8_t>(~testLocalMask)) != 0u) {
            std::fprintf(stderr,
                         "[session-test] invalid online mask contract "
                         "(local must be 1..15; viewport must be its subset)\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        if (!session.beginOnline() ||
            !session.setConnectivity(MDKR_CONNECTIVITY_DIRECT) ||
            !session.setRoomPhase(MDKR_ROOM_LOADING)) {
            std::fprintf(stderr,
                         "[session-test] could not compose first online "
                         "launcher envelope\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        bool launchApplied = false;
        if (testLaunchV3) {
            MdkrSessionLaunchV3 launch{};
            launchApplied = makeTestOnlineLaunchV3(
                1u, testLocalMask, testViewportMask, &launch) &&
                session.applyLaunch(launch);
        } else {
            MdkrSessionLaunchV2 launch{};
            launchApplied = makeTestOnlineLaunch(
                1u, testLocalMask, testViewportMask, &launch) &&
                session.applyLaunch(launch);
        }
        if (!launchApplied) {
            std::fprintf(stderr,
                         "[session-test] could not freeze first online "
                         "launcher envelope\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
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
            shutdownLauncherHost(host, launcher);
            return 1;
        }
        host.beginFrame();
        launcher.draw(host);
        if (!host.endFrame()) {
            std::fprintf(stderr,
                         "[app] autoplay: host renderer failed during surface "
                         "warm-up\n");
            shutdownLauncherHost(host, launcher);
            return 1;
        }
        ++warmupAttempts;
        if (host.presentedFrames() == initialPresents) SDL_Delay(1);
    }
    if (host.presentedFrames() == initialPresents &&
        !host.lastSurfaceWasOccluded()) {
        std::fprintf(stderr,
                     "[app] autoplay: host surface did not present within 2000 ms\n");
        shutdownLauncherHost(host, launcher);
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
#if MDKR_ENABLE_ONLINE_BETA
    /* Live selection bridge proof: install the launcher->engine forward
     * feed and publish a deterministic scripted snapshot sequence with NO
     * adapter, then return. The native character/track screens
     * read this feed during menus; here it proves the publish path end to end
     * and emits [party-link-fake] witnesses for a driving harness. Ordinary
     * autoplay never sets this variable, so the seam stays inert. */
    if (std::getenv("MDKR_APP_TEST_PARTY_LINK_FAKE") != nullptr) {
        OnlineRoom_runTestPartyLinkFake();
        shutdownLauncherHost(host, launcher);
        return 0;
    }
    /* Scripted RESIDENT SOAK: prove >=2 engine races + RESULTS in ONE
     * engine process via the separated session loop, WITHOUT the live-loopback
     * harness (whose boot-once wall + post-exit results report is exactly what
     * makes multi-race impossible there; the resident LIVE lanes lift that limit).
     *
     * This installs a validated 2-slot roster + launch descriptor DIRECTLY (no
     * DTLS mesh, no match-input source), so mode_intro forks into the session and
     * the race runs as a plain autopilot race (network_input == false, because
     * the match-input runtime is NOT installed): both racers are AI-driven and
     * finish, objects.c captures the placements, and -- with MDKR_TEST_ONLINE_-
     * RESIDENT set -- the menu.c post-race hook re-enters the session's RESULTS
     * phase instead of exiting. The session shows RESULTS -> STANDINGS, then
     * re-boots the next race IN THIS SAME PROCESS. The autoplay tick budget ends
     * the run while the final standings holds. Ordinary autoplay never sets the
     * variable, so this stays inert. */
    if (const char *residentEnv = std::getenv("MDKR_TEST_ONLINE_RESIDENT");
        residentEnv != nullptr && std::strtoul(residentEnv, nullptr, 10) > 0ul) {
        /* The flag is the RACE COUNT -- arm only for a positive value, so all
         * three readers (here, online_session.c, online_results.c) agree that
         * "=0" is OFF and a half-armed harness cannot prove nothing. */
        /* Visual proof of the native online screens (e.g. the RESULTS "more races"
         * chooser) is a PLATFORM facility, armed here rather than by game code: the
         * interactive resident soak reaches those screens on a path that cannot be
         * given --dump-frames, so the launcher arms the engine frame-dump from the
         * shot env directory when it stands the soak up. Inert unless the env dir is
         * set, and never overrides an explicit --dump-frames. */
        if (g_dumpFramesDir == nullptr) {
            if (const char *shot =
                    std::getenv("MDKR_TEST_ONLINE_RESULTS_CHOOSER_SHOT");
                shot != nullptr && shot[0] != '\0') {
                g_dumpFramesDir = shot;
                std::fprintf(stderr,
                             "[online-resident] chooser: frame-dump armed -> %s\n",
                             shot);
            }
        }
        MdkrMatchManifestV1 manifest{};
        MdkrNetRoster roster{};
        MdkrMatchLaunchDescriptorV1 desc{};
        const std::uint8_t bothSlots[2] = {0u, 1u};
        unsigned bi;
        manifest.match_epoch = 1u;
        manifest.protocol_version = 1u;
        for (bi = 0u; bi < sizeof(manifest.build_id); ++bi) {
            manifest.build_id[bi] = static_cast<std::uint8_t>(bi + 1u);
        }
        for (bi = 0u; bi < sizeof(manifest.gameplay_digest); ++bi) {
            manifest.gameplay_digest[bi] = static_cast<std::uint8_t>(bi + 1u);
        }
        manifest.slot_owner[0] = UINT64_C(0x1001);
        manifest.slot_owner[1] = UINT64_C(0x1002);
        manifest.rng_seed = UINT64_C(0x0123456789ABCDEF);
        manifest.track_id = 5u; /* Ancient Lake (a real standard race track) */
        manifest.rom_revision = static_cast<std::uint8_t>(MDKR_ROM_US_11);
        manifest.cadence_hz = 30u;
        manifest.slot_count = 2u; /* >= 2 (manifest_validate floor) */
        manifest.rules = MDKR_MATCH_RULES_STANDARD_RACE;
        manifest.vehicle_mask = 0x7u; /* car/hovercraft/plane */
        manifest.input_delay = 2u;
        /* Both canonical slots are LOCAL here: the race then runs as the proven
         * offline 2-player split autopilot scenario (both AI-driven, both
         * finish). The RESULTS screen's local/remote split is driven separately
         * by the party_link snapshot the RESULTS test seam publishes. */
        if (!mdkr_net_roster_init(&roster, &manifest) ||
            !mdkr_net_roster_configure_local(&roster, bothSlots, 2u) ||
            !mdkr_net_roster_set_viewports(&roster, bothSlots, 2u)) {
            std::fprintf(stderr,
                         "[online-resident] roster build failed\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        desc.version = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
        desc.manifest = manifest;
        desc.selections[0].selection_revision = 1u;
        desc.selections[0].character_id = 1u; /* distinct valid characters */
        desc.selections[0].vehicle_id = 0u;   /* car (bit set in mask) */
        desc.selections[1].selection_revision = 1u;
        desc.selections[1].character_id = 2u;
        desc.selections[1].vehicle_id = 0u;
        desc.selections[2].character_id = MDKR_MATCH_NO_CHARACTER;
        desc.selections[2].vehicle_id = MDKR_MATCH_NO_VEHICLE;
        desc.selections[3].character_id = MDKR_MATCH_NO_CHARACTER;
        desc.selections[3].vehicle_id = MDKR_MATCH_NO_VEHICLE;
        if (!mdkr_match_launch_descriptor_validate(&desc)) {
            std::fprintf(stderr,
                         "[online-resident] launch descriptor invalid\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        mdkr_net_roster_runtime_clear();
        if (!mdkr_net_roster_runtime_install_launch(&desc, &roster)) {
            std::fprintf(stderr,
                         "[online-resident] roster/descriptor install refused\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        std::fprintf(stderr,
                     "[online-resident] soak: roster installed (track=%u "
                     "slots=%u); resident post-race re-entry armed\n",
                     static_cast<unsigned>(manifest.track_id),
                     static_cast<unsigned>(manifest.slot_count));
        platformSetHostWindow(host.window(), host.glContext());
        if (host.usingWebGpu()) {
            platformSetHostWebGpu(host.wgpuInstance(), host.wgpuAdapter(),
                                  host.wgpuDevice(), host.wgpuQueue(),
                                  host.wgpuSurface(), host.wgpuFormat());
            platformSetHostWebGpuRecovery(recoverAppHostWebGpu, &host);
        }
        /* Online epoch: the engine sees the NTSC source identity (rom_io.c)
         * so a PAL ROM races the 30 Hz manifest bit-identically to US;
         * cleared as soon as the bounded boot returns. */
        int residentResult;
        {
            OnlineNtscIdentityScope ntscIdentityScope(true);
            residentResult = mdkr64_engine_boot(&config);
        }
        platformSetHostWebGpuRecovery(nullptr, nullptr);
        platformSetHostWebGpu(nullptr, nullptr, nullptr, nullptr, nullptr, 0);
        platformSetHostWindow(nullptr, nullptr);
        mdkr_net_roster_runtime_clear();
        /* TEST: region re-entry proof -- the NTSC identity is EPOCH-scoped.
         * With this second variable set, boot ONE plain OFFLINE epoch in this
         * same process after the online session above: it must re-latch the
         * loaded ROM's authentic region (a European ROM prints its PAL 50 Hz
         * source clock again, with no override witness).  Ordinary soaks
         * never set the variable, so the seam stays inert. */
        if (residentResult == 0 &&
            std::getenv("MDKR_TEST_ONLINE_REGION_REENTRY") != nullptr) {
            std::fprintf(stderr,
                         "[online-resident] region re-entry: online epoch "
                         "done (override now %d); booting one OFFLINE epoch\n",
                         platform_source_ntsc_identity_override());
            /* The handoff is one-shot per engine session; re-arm it for the
             * second boot exactly like runEngineSession's teardown does. */
            if (!mdkr_video_config_engine_session_complete()) {
                std::fprintf(stderr,
                             "[online-resident] region re-entry: video-config "
                             "epoch did not close cleanly\n");
            }
            platformSetHostWindow(host.window(), host.glContext());
            if (host.usingWebGpu()) {
                platformSetHostWebGpu(host.wgpuInstance(), host.wgpuAdapter(),
                                      host.wgpuDevice(), host.wgpuQueue(),
                                      host.wgpuSurface(), host.wgpuFormat());
                platformSetHostWebGpuRecovery(recoverAppHostWebGpu, &host);
            }
            const int offlineResult = mdkr64_engine_boot(&config);
            platformSetHostWebGpuRecovery(nullptr, nullptr);
            platformSetHostWebGpu(nullptr, nullptr, nullptr, nullptr, nullptr,
                                  0);
            platformSetHostWindow(nullptr, nullptr);
            std::fprintf(stderr,
                         "[online-resident] region re-entry: offline epoch "
                         "result=%d\n",
                         offlineResult);
            if (offlineResult != 0) {
                shutdownLauncherHost(host, launcher);
                return offlineResult;
            }
        }
        shutdownLauncherHost(host, launcher);
        return residentResult;
    }
    /* Headless proof of the make-or-break wiring: stand up two REAL live adapters
     * over the in-process loopback mesh, drive them to a ready race transport,
     * then boot the VISIBLE engine on endpoint A's live transport while endpoint
     * B seals real input over the mesh. Reuses this run's config (MDKR_ROM +
     * MDKR_APP_AUTOPLAY_INPUT_SCRIPT navigate the engine to the agreed track;
     * MDKR_APP_AUTOPLAY_TICKS bounds the run). Ordinary autoplay never sets this
     * variable, so the loopback harness stays inert. */
    /* KEYSTONE PROOF: the LOBBY-START loopback lane. Stand up the two
     * loopback adapters but STOP at SELECTING (no descriptor), install party_link,
     * and boot the visible (host) engine DESCRIPTOR-LESS so its native CHARSELECT
     * -> TRACKSELECT own race 1: the host's scripted selection/ready/track/START
     * ride the real reverse feed into the adapter, BEGIN_LOADING builds the
     * descriptor live, and the engine's race-1 readiness gate boots exactly once
     * the descriptor + roster + match-input are ready (never a NULL deref). This
     * is the headless proof that NATIVE owns race 1. Ordinary autoplay never sets
     * this, so it stays inert; every other lane is byte-behavior-unchanged. */
    /* ROOM-READY TRIGGER probe (test seam). The full interactive loop is
     * not headless-runnable (it needs a live cloud adapter + a human), so this seam
     * drives the SAME loopback room to SELECTING and exercises the production
     * detection + consume-once publish DIRECTLY: OnlineRoom_pollRoomReadyTransition
     * must fire EXACTLY ONCE on first-SELECTING+2members+LOBBY for ANY mode
     * (route=lobby-start). The former TOURNAMENT-only gate is gone, so a
     * single-race room now takes the SAME native takeover as a tournament room
     * (both fire once + route to lobby-start); the earlier "single race defers to the
     * race-ready ImGui fallback" behaviour is gone. Ordinary autoplay never sets
     * this. */
    if (std::getenv("MDKR_APP_TEST_ONLINE_ROOM_READY_PROBE") != nullptr) {
        std::string probeErr;
        MdkrOnlineTestLoopbackRace *race =
            OnlineRoom_makeTestLobbyStartRoom(&probeErr);
        if (race == nullptr) {
            std::fprintf(stderr,
                         "[online-room-ready-probe] loopback room setup failed: %s\n",
                         probeErr.c_str());
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        /* Hold the VISIBLE endpoint as the PRODUCTION OwningLiveAdapter wrapper --
         * the exact wrapper class the Online Room panel builds -- and hand THAT to the
         * poll, so this probe is the only coverage of the production wrapper's
         * mdkrResolveLive resolve override end-to-end (a unit test can only reach a
         * stand-in wrapper); keep it holding the real OwningLiveAdapter, not a raw
         * LiveAdapter. The wrapper adopts the loopback visible inner; `race` still owns
         * the transports the inner borrows, so the wrapper MUST be destroyed BEFORE
         * `race`. */
        std::unique_ptr<IMdkrOnlineAdapter> visibleWrapper =
            OnlineRoom_wrapVisibleAsOwningAdapter(race);
        IMdkrOnlineAdapter *visible = visibleWrapper.get();
        IMdkrOnlineAdapter *peer = OnlineRoom_testLoopbackPeer(race);
        if (visible == nullptr || peer == nullptr) {
            std::fprintf(stderr,
                         "[online-room-ready-probe] visible wrapper/peer "
                         "unavailable\n");
            OnlineRoom_unwrapVisibleOwningAdapter(race, std::move(visibleWrapper));
            OnlineRoom_destroyTestLoopbackRace(race);
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        OnlineRoom_resetRoomReadyLatch();
        int fires = 0;
        bool everHeld = false;
        for (int i = 0; i < 240; ++i) {
            visible->service();
            peer->service();
            if (OnlineRoom_roomReadyConditionHolds(visible)) everHeld = true;
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        IMdkrOnlineAdapter *published = OnlineRoom_pollEngineRoomReady();
        /* The poll publishes the RESOLVED raw inner (OnlineRoom_resolveRawLiveAdapter),
         * so a correctly-resolving wrapper yields published == resolve(wrapper): a
         * STRONGER end-to-end proof of the wrapper's mdkrResolveLive hook than the
         * old raw-pointer identity (published == the wrapper itself would be wrong). */
        const bool routed =
            published != nullptr &&
            published == OnlineRoom_resolveRawLiveAdapter(visible);
        std::fprintf(stderr,
                     "[online-room-ready-probe] fires=%d conditionHeld=%d "
                     "published=%d route=%s\n",
                     fires, everHeld ? 1 : 0, routed ? 1 : 0,
                     routed ? "lobby-start" : "race-ready-fallback");
        /* Return the adopted inner to the loopback race's visible slot BEFORE
         * destroying it: OnlineRoom_destroyTestLoopbackRace runs the struct's
         * ordered teardown and (in tournament mode) its transport-level continuation,
         * both of which drive that adapter. This restores the struct's original
         * destruction path -- the wrapper existed only for the poll loop above. */
        OnlineRoom_unwrapVisibleOwningAdapter(race, std::move(visibleWrapper));
        OnlineRoom_destroyTestLoopbackRace(race);
        shutdownLauncherHost(host, launcher);
        /* EITHER mode fires exactly once + routes to lobby-start. Formerly a
         * single-race room (no tournament env) fired zero times and fell through to
         * the race-ready ImGui path; the tournament-only gate is now dropped, so the
         * single-race branch below asserts the SAME native takeover the tournament
         * branch always has. (modeEnv is kept only to document that the assertion is
         * now mode-independent.) */
        /* Bind the result to a named var so GCC/glibc's -Werror unused lane is
         * satisfied (a bare (void) cast on the call alone does not silence it);
         * the value is intentionally unread -- the assertion is now
         * mode-independent. Same idiom as tests/test_lan_party_launch.cpp. */
        const char *modeEnv = std::getenv("MDKR_APP_TEST_ONLINE_MODE");
        (void)modeEnv;
        return (fires == 1 && routed) ? 0 : 3;
    }
    if (std::getenv("MDKR_APP_TEST_ONLINE_ROOM_READY_REARM_PROBE") != nullptr) {
        /* Headless proof of the safe 2nd-tournament re-arm STATE
         * MACHINE. The full interactive 2-tournament loop needs a live cloud adapter +
         * a human, so this seam drives the loopback tournament room and exercises the
         * wiring's re-arm edges DIRECTLY. It proves, in ONE process:
         *   1. tournament #1 fires the room-ready takeover EXACTLY ONCE;
         *   2. a LEFT/ERROR return (no arm) does NOT re-fire even though the room-ready
         *      condition is STILL TRUE (SELECTING+2+LOBBY+tournament) -- no re-boot
         *      loop;
         *   3. the production FINISHED shape -- the tournament's final race parks the
         *      room in RESULTS, the host's FINISH wraps it back to a fresh-series
         *      SELECTING via the leader REMATCH, and only THEN does the session
         *      return FINISHED -- does NOT re-fire on the wrap alone (the latch is
         *      still set; a room transition without a FINISHED return never
         *      re-takes);
         *   4. the FINISHED return (arm) completes on the next observation and the
         *      takeover re-fires EXACTLY ONCE for session #2 (route=lobby-start) --
         *      the automatic FINISHED re-take -- with no further fires after the
         *      one consume;
         *   5. a later condition false->true cycle WITHOUT a FINISHED return does
         *      NOT re-fire (one re-take per FINISHED, not per room transition).
         * The room-ready condition is toggled by PARKING the room in RESULTS (a real
         * finished race: ready both seats -> leader START -> RACING -> leader
         * PUBLISH_RESULTS => phase RESULTS => condition false) and RETURNING it to
         * SELECTING via the leader's REMATCH (=> condition true) -- the exact
         * final-standings-park -> FINISH-wrap transition production rides. */
        std::string probeErr;
        MdkrOnlineTestLoopbackRace *race =
            OnlineRoom_makeTestLobbyStartRoom(&probeErr);
        if (race == nullptr) {
            std::fprintf(stderr,
                         "[online-room-ready-rearm-probe] loopback room setup "
                         "failed: %s\n",
                         probeErr.c_str());
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        IMdkrOnlineAdapter *visible = OnlineRoom_testLoopbackVisible(race);
        IMdkrOnlineAdapter *peer = OnlineRoom_testLoopbackPeer(race);
        auto pump = [&](int n) {
            for (int i = 0; i < n; ++i) {
                visible->service();
                peer->service();
            }
        };

        OnlineRoom_resetRoomReadyLatch();
        pump(30);
        int fires = 0;

        /* (1) tournament #1: exactly one fire, then the one-shot latch holds. */
        const bool cond1 = OnlineRoom_roomReadyConditionHolds(visible);
        if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        const int firesAfterT1 = fires;
        (void)OnlineRoom_pollEngineRoomReady(); /* the launcher would boot here */
        for (int i = 0; i < 60; ++i) {
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        const bool t1Once = cond1 && firesAfterT1 == 1 && fires == 1;

        /* (2) LEFT/ERROR return: DO NOT arm. With the condition still TRUE the observer
         * is a no-op and the trigger never re-fires -- proves no re-boot loop. */
        for (int i = 0; i < 60; ++i) {
            OnlineRoom_observeRoomReadyRearm(visible);
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        const bool leftNoRearm =
            fires == 1 && OnlineRoom_roomReadyConditionHolds(visible);

        /* (3) The production FINISHED shape happens BEFORE the return: the final
         * race parks the room in RESULTS (condition false), then the host's FINISH
         * wraps it back to a fresh-series SELECTING via the leader REMATCH
         * (condition true again) -- all while the session still owns the frames.
         * The wrap ALONE must NOT re-fire: the latch from tournament #1 is still
         * set and nothing is pending (the observer runs throughout as the panel
         * would, and must stay a no-op). */
        const bool wentFalse = OnlineRoom_testParkRoomInResults(visible, peer);
        for (int i = 0; i < 30; ++i) {
            OnlineRoom_observeRoomReadyRearm(visible); /* no-op: nothing pending */
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        const bool wentTrue = OnlineRoom_testReturnRoomToSelecting(visible, peer);
        for (int i = 0; i < 30; ++i) {
            OnlineRoom_observeRoomReadyRearm(visible); /* still a no-op */
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        const bool wrapAloneNoRefire =
            wentFalse && wentTrue && fires == 1 &&
            OnlineRoom_roomReadyConditionHolds(visible);
        /* DIRECT witness: latch set, nothing pending -- the takeover is NOT
         * engaged. It must flip once the FINISHED arm completes below. */
        const bool engagedBeforeArm = OnlineRoom_roomReadyTakeoverEngaged();

        /* (4) FINISHED return: arm. The next observation COMPLETES the re-arm
         * immediately ("re-arm complete" -- the wrap above already supplied the
         * out-and-back rising edge) and the following poll re-fires EXACTLY ONCE
         * for session #2 (route=lobby-start): the automatic FINISHED re-take.
         * Once the fire consumes the latch, further polls must not fire. */
        OnlineRoom_armRoomReadyRearm();
        OnlineRoom_observeRoomReadyRearm(visible);
        const bool engagedAfterComplete = OnlineRoom_roomReadyTakeoverEngaged();
        if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        const int firesAfterRetake = fires; /* expect 2 */
        IMdkrOnlineAdapter *published2 = OnlineRoom_pollEngineRoomReady();
        const bool routed2 = published2 == visible;
        for (int i = 0; i < 60; ++i) {
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        const bool finishedRetakeOnce =
            !engagedBeforeArm && engagedAfterComplete && firesAfterRetake == 2 &&
            fires == 2 && routed2;

        /* (5a) ONE re-take per FINISHED: a later condition false->true cycle
         * WITHOUT a FINISHED return (no arm) must NOT re-fire -- a room transition
         * alone never re-takes. */
        (void)OnlineRoom_testParkRoomInResults(visible, peer);
        for (int i = 0; i < 30; ++i) {
            OnlineRoom_observeRoomReadyRearm(visible);
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        (void)OnlineRoom_testReturnRoomToSelecting(visible, peer);
        pump(5);
        OnlineRoom_observeRoomReadyRearm(visible);
        if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        const bool noRetakeWithoutFinished = fires == 2;

        /* (5b) Coda: OnlineRoom_resetRoomReadyLatch must DROP a pending re-arm
         * (wiring: fresh adapter = clean slate), so a stale FINISHED cannot leak
         * into a successor room. Model it: arm (a stale FINISHED), reset (a fresh
         * adapter is built), then let the fresh room fire its own tournament #1
         * ONCE -- after which a full condition false->true cycle with the observer
         * running must NOT manufacture a spurious re-take. If reset had NOT
         * dropped the pending re-arm, the leaked arm would complete on the first
         * observation and the SELECTING return would fire a bogus 4th time. */
        OnlineRoom_armRoomReadyRearm();
        OnlineRoom_resetRoomReadyLatch(); /* must clear latch AND the pending re-arm */
        /* The room is already at SELECTING from step (5a)'s REMATCH, so the condition
         * holds again with no drive needed. */
        pump(5);
        if (OnlineRoom_pollRoomReadyTransition(visible)) fires++; /* fresh #1 fires once */
        (void)OnlineRoom_pollEngineRoomReady();                   /* launcher boots it */
        const int firesAfterFreshBoot = fires;                    /* expect 3 */
        (void)OnlineRoom_testParkRoomInResults(visible, peer);    /* condition false */
        for (int i = 0; i < 30; ++i) {
            OnlineRoom_observeRoomReadyRearm(visible); /* must be a NO-OP: rearm dropped */
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        (void)OnlineRoom_testReturnRoomToSelecting(visible, peer); /* condition true again */
        pump(5);
        OnlineRoom_observeRoomReadyRearm(visible);
        if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        const bool resetDropsPending =
            firesAfterFreshBoot == 3 && fires == 3; /* no spurious 4th fire */

        const bool ok = t1Once && leftNoRearm && wrapAloneNoRefire &&
                        finishedRetakeOnce && noRetakeWithoutFinished &&
                        resetDropsPending;
        std::fprintf(stderr,
                     "[online-room-ready-rearm-probe] totalFires=%d t1Once=%d "
                     "leftNoRearm=%d wrapAloneNoRefire=%d finishedRetakeOnce=%d "
                     "routed2=%d noRetakeWithoutFinished=%d resetDropsPending=%d "
                     "verdict=%s\n",
                     fires, t1Once ? 1 : 0, leftNoRearm ? 1 : 0,
                     wrapAloneNoRefire ? 1 : 0, finishedRetakeOnce ? 1 : 0,
                     routed2 ? 1 : 0, noRetakeWithoutFinished ? 1 : 0,
                     resetDropsPending ? 1 : 0, ok ? "PASS" : "FAIL");
        OnlineRoom_destroyTestLoopbackRace(race);
        shutdownLauncherHost(host, launcher);
        return ok ? 0 : 3;
    }
    if (std::getenv("MDKR_APP_TEST_ONLINE_LEFT_REENTRY_PROBE") != nullptr) {
        /* Headless proof of the LEFT/ERROR native RE-ENTRY control ("Return to
         * game"). After a native session returns LEFT or ERROR the room lands back at
         * SELECTING+2+LOBBY with the latch SET and nothing pending, so the takeover
         * deliberately never re-fires on its own (no re-boot loop) -- but with the
         * per-race ImGui fallback retired that is a dead end, so the SELECTING body
         * offers an explicit re-entry control. This seam drives the loopback room and
         * exercises the wiring's re-entry edges DIRECTLY, proving in ONE process:
         *   (a) the control is OFFERED -- a LEFT return records a re-entry reason while
         *       the takeover is NOT engaged and the room-ready condition still holds
         *       (the exact gate under which the SELECTING body draws the button);
         *   (b) WITHOUT a press the takeover does NOT re-fire within the frame budget,
         *       even with the per-frame re-arm observer running (LEFT/ERROR never arms);
         *   (c) the scripted press (OnlineRoom_requestRoomReadyReentry) re-fires the
         *       takeover EXACTLY ONCE -- the next poll re-takes native (route
         *       lobby-start) and the "[online-room-ready] latch set" line appears again.
         * The room is held at SELECTING throughout (no engine boot); the condition is
         * continuously TRUE, which is exactly the post-LEFT/ERROR state whose card would
         * be a dead end without this control. */
        std::string probeErr;
        MdkrOnlineTestLoopbackRace *race =
            OnlineRoom_makeTestLobbyStartRoom(&probeErr);
        if (race == nullptr) {
            std::fprintf(stderr,
                         "[online-left-reentry-probe] loopback room setup failed: "
                         "%s\n",
                         probeErr.c_str());
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        IMdkrOnlineAdapter *visible = OnlineRoom_testLoopbackVisible(race);
        IMdkrOnlineAdapter *peer = OnlineRoom_testLoopbackPeer(race);
        auto pump = [&](int n) {
            for (int i = 0; i < n; ++i) {
                visible->service();
                peer->service();
            }
        };

        OnlineRoom_resetRoomReadyLatch();
        pump(30);
        int fires = 0;

        /* (1) The native takeover fires once, then the one-shot latch holds. */
        if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        (void)OnlineRoom_pollEngineRoomReady(); /* the launcher would boot here */
        const int firesAfterBoot = fires;       /* expect 1 */

        /* (2) Simulate a LEFT native return: record the reason, do NOT arm. */
        OnlineRoom_noteSessionReturn(MDKR_PARTY_LINK_SESSION_END_LEFT);
        /* (a) The control is offered: a LEFT reason is recorded, the takeover is NOT
         * engaged (latch set, nothing pending), and the room-ready condition still
         * holds (room back at SELECTING+2+LOBBY) -- the panel's exact draw gate. */
        const bool controlOffered =
            OnlineRoom_roomReadyReentryReason() ==
                MDKR_PARTY_LINK_SESSION_END_LEFT &&
            !OnlineRoom_roomReadyTakeoverEngaged() &&
            OnlineRoom_roomReadyConditionHolds(visible);

        /* (b) No press: with the condition still TRUE the per-frame observer is a
         * no-op (LEFT never armed) and the trigger must NOT re-fire -- no re-boot. */
        for (int i = 0; i < 120; ++i) {
            OnlineRoom_observeRoomReadyRearm(visible);
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        const bool noRebootWithoutPress =
            fires == 1 && OnlineRoom_roomReadyConditionHolds(visible) &&
            !OnlineRoom_roomReadyTakeoverEngaged();

        /* (c) Scripted press: request re-entry. The latch clears immediately (a human
         * gesture is required per re-entry, so this cannot loop), the offer retires,
         * and the takeover becomes engaged again. */
        OnlineRoom_requestRoomReadyReentry();
        const bool engagedAfterPress = OnlineRoom_roomReadyTakeoverEngaged();
        const bool offerClearedAfterPress =
            OnlineRoom_roomReadyReentryReason() ==
            MDKR_PARTY_LINK_SESSION_END_NONE;
        pump(5);
        if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        const int firesAfterPress = fires; /* expect 2 */
        IMdkrOnlineAdapter *published = OnlineRoom_pollEngineRoomReady();
        const bool routed =
            published != nullptr &&
            published == OnlineRoom_resolveRawLiveAdapter(visible);
        /* No spurious further fires: the re-take is EXACTLY ONE. */
        for (int i = 0; i < 60; ++i) {
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        const bool reentryRefires = engagedAfterPress && offerClearedAfterPress &&
                                    firesAfterPress == 2 && fires == 2 && routed;

        const bool ok = firesAfterBoot == 1 && controlOffered &&
                        noRebootWithoutPress && reentryRefires;
        std::fprintf(stderr,
                     "[online-left-reentry-probe] totalFires=%d controlOffered=%d "
                     "noRebootWithoutPress=%d reentryRefires=%d routed=%d "
                     "verdict=%s\n",
                     fires, controlOffered ? 1 : 0, noRebootWithoutPress ? 1 : 0,
                     reentryRefires ? 1 : 0, routed ? 1 : 0, ok ? "PASS" : "FAIL");
        OnlineRoom_destroyTestLoopbackRace(race);
        shutdownLauncherHost(host, launcher);
        return ok ? 0 : 3;
    }
    if (std::getenv("MDKR_APP_TEST_ONLINE_ROOM_READY_REARM3_PROBE") != nullptr) {
        /* Multi-cycle sibling of the ROOM_READY_REARM_PROBE above. That probe proves
         * the re-arm across ONE FINISHED cycle (tournament #1 -> #2) plus a FRESH-
         * adapter reset coda. What it CANNOT show is that the re-arm is REPEATABLE on
         * the SAME adapter: a latent one-shot bug (e.g. a "re-armed once" static guard)
         * would pass #1->#2 yet silently drop tournament #3. The human plan calls the
         * 3rd tournament out explicitly ("a 3rd for good measure -- unexercised by any
         * lane"). This seam drives THREE consecutive tournaments through the wiring's
         * wrap -> FINISHED-arm -> immediate-completion re-take cycle, so the re-arm
         * must fire once PER FINISHED return (twice), for a total of exactly three
         * takeovers, and a LEFT return WEDGED BETWEEN #1 and #2 must NOT re-arm even
         * with the room-ready condition still TRUE (no re-boot loop across cycles).
         * Each cycle first replays the production shape -- PARK a real finished race
         * in RESULTS (=> condition false), RETURN to SELECTING via the leader's
         * REMATCH (the FINISH wrap => condition true) -- and asserts the wrap ALONE
         * re-fires nothing (latch still set, nothing pending); only the FINISHED
         * arm + observation re-takes, exactly once. */
        std::string probeErr;
        MdkrOnlineTestLoopbackRace *race =
            OnlineRoom_makeTestLobbyStartRoom(&probeErr);
        if (race == nullptr) {
            std::fprintf(stderr,
                         "[online-room-ready-rearm3-probe] loopback room setup "
                         "failed: %s\n",
                         probeErr.c_str());
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        IMdkrOnlineAdapter *visible = OnlineRoom_testLoopbackVisible(race);
        IMdkrOnlineAdapter *peer = OnlineRoom_testLoopbackPeer(race);
        auto pump = [&](int n) {
            for (int i = 0; i < n; ++i) {
                visible->service();
                peer->service();
            }
        };

        OnlineRoom_resetRoomReadyLatch();
        pump(30);
        int fires = 0;

        /* (1) Tournament #1: the loopback room starts at SELECTING+2+LOBBY+tournament,
         * so the condition holds and the takeover fires exactly once. */
        const bool cond1 = OnlineRoom_roomReadyConditionHolds(visible);
        if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        (void)OnlineRoom_pollEngineRoomReady(); /* the launcher would boot here */
        for (int i = 0; i < 30; ++i) {
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        const bool t1Once = cond1 && fires == 1;

        /* (2) A LEFT return between #1 and #2: DO NOT arm. The observer is a no-op with
         * nothing pending, so even with the condition STILL TRUE the takeover never
         * re-fires -- the no-re-boot-loop invariant, held across a multi-cycle run. */
        for (int i = 0; i < 60; ++i) {
            OnlineRoom_observeRoomReadyRearm(visible);
            pump(1);
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
        }
        const bool leftNoRearm =
            fires == 1 && OnlineRoom_roomReadyConditionHolds(visible);

        /* One FINISHED re-take cycle to the next tournament, in the production
         * order: park the finished final in RESULTS, wrap back to SELECTING via
         * the leader REMATCH (the FINISH wrap; the wrap ALONE must re-fire
         * nothing -- latch still set, nothing pending, observer a no-op), THEN the
         * FINISHED return arms and the next observation completes the re-arm
         * ("re-arm complete") so the following poll re-takes exactly once.
         * Returns {wrap-alone-held, this-cycle fired once + routed}. */
        auto rearmCycle = [&](int expectFires) -> std::pair<bool, bool> {
            const int before = fires;
            const bool wentFalse = OnlineRoom_testParkRoomInResults(visible, peer);
            for (int i = 0; i < 30; ++i) { /* parked: nothing pending, no fire */
                OnlineRoom_observeRoomReadyRearm(visible);
                pump(1);
                if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
            }
            const bool wentTrue = OnlineRoom_testReturnRoomToSelecting(visible, peer);
            for (int i = 0; i < 30; ++i) { /* wrapped back: latch still set, no fire */
                OnlineRoom_observeRoomReadyRearm(visible);
                pump(1);
                if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
            }
            const bool wrapHeld =
                fires == before && OnlineRoom_roomReadyConditionHolds(visible);
            OnlineRoom_armRoomReadyRearm(); /* FINISHED return arms (once per cycle) */
            OnlineRoom_observeRoomReadyRearm(visible); /* completes immediately */
            if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
            for (int i = 0; i < 30; ++i) { /* consumed: no further fires */
                pump(1);
                if (OnlineRoom_pollRoomReadyTransition(visible)) fires++;
            }
            const bool routed = OnlineRoom_pollEngineRoomReady() == visible;
            const bool once =
                wentFalse && wentTrue && fires == expectFires && routed;
            return {wrapHeld, once};
        };

        /* (3) FINISHED re-take #1 -> tournament #2 (total fires 2). */
        auto [wrapHeld2, t2Once] = rearmCycle(2);
        /* (4) FINISHED re-take #2 on the SAME adapter -> tournament #3 (total 3): the
         * repeatability the single-cycle probe cannot prove. */
        auto [wrapHeld3, t3Once] = rearmCycle(3);

        const bool ok = t1Once && leftNoRearm && wrapHeld2 && t2Once &&
                        wrapHeld3 && t3Once && fires == 3;
        std::fprintf(stderr,
                     "[online-room-ready-rearm3-probe] totalFires=%d t1Once=%d "
                     "leftNoRearm=%d wrapHeld2=%d t2Once=%d "
                     "wrapHeld3=%d t3Once=%d verdict=%s\n",
                     fires, t1Once ? 1 : 0, leftNoRearm ? 1 : 0,
                     wrapHeld2 ? 1 : 0, t2Once ? 1 : 0,
                     wrapHeld3 ? 1 : 0, t3Once ? 1 : 0,
                     ok ? "PASS" : "FAIL");
        OnlineRoom_destroyTestLoopbackRace(race);
        shutdownLauncherHost(host, launcher);
        return ok ? 0 : 3;
    }
    if (const char *lobbyStartEnv =
            std::getenv("MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START");
        lobbyStartEnv != nullptr &&
        std::strtoul(lobbyStartEnv, nullptr, 10) > 0ul) {
        std::string liveErr;
        MdkrOnlineTestLoopbackRace *race =
            OnlineRoom_makeTestLobbyStartRoom(&liveErr);
        if (race == nullptr) {
            std::fprintf(stderr,
                         "[online-lobby-start] loopback room setup failed: %s\n",
                         liveErr.c_str());
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        const int liveResult =
            runOnlineLobbyStartEngineSession(host, config, race);
        OnlineRoom_destroyTestLoopbackRace(race);
        shutdownLauncherHost(host, launcher);
        return liveResult;
    }
    /* Resolve the ROM's validated region up front so the in-process loopback
     * lanes below stage the manifest a real endpoint of THAT region would: a PAL
     * v80 payload authors the sim at 25 Hz, so its fixture compat must carry
     * revision 2 / cadence 25 for the engine's cadence admission to accept it.
     * The engine loads the ROM only when a loopback session boots --
     * after the fixture compat is frozen -- so this is the one point where the
     * region is known first. mdkr_validate_rom is the same pure contract the
     * launcher's ROM picker uses; a US ROM (or none) resolves to the historical
     * US identity, so the NTSC loopback is byte-behaviour unchanged. */
    if (config.rom_path != nullptr && config.rom_path[0] != '\0') {
        const RomInfo loopbackRom = mdkr_validate_rom(config.rom_path);
        uint8_t loopbackRev = 0u;
        if (loopbackRom.valid && loopbackRom.integrity_verified) {
            if (std::strcmp(loopbackRom.build, "pal.v80") == 0) {
                loopbackRev = 2u;
            } else if (std::strcmp(loopbackRom.build, "us.v80") == 0) {
                loopbackRev = 1u;
            }
        }
        OnlineRoom_setTestLoopbackRomRevision(loopbackRev);
    }
    /* KEYSTONE PROOF: the LIVE-loopback RESIDENT lane. Stand up the SAME
     * two-real-adapter loopback race the MDKR_APP_TEST_ONLINE_LIVE lane below
     * uses, but make the engine session RESIDENT: ONE mdkr64_engine_boot spans
     * >= 2 races, the native RESULTS screen fronting on the REAL reducer feed
     * between races (party_link bridge + launcher pump), the host advance driving
     * a REAL REMATCH through the reverse feed, and race N+1 re-cycled + booted in
     * the same process. Requires a tournament room (MDKR_APP_TEST_ONLINE_MODE=
     * tournament) so a cup schedules >= 2 rounds. Ordinary autoplay never sets
     * this, so it stays inert; the existing MDKR_APP_TEST_ONLINE_LIVE lane below
     * (no resident env) is byte-behavior-unchanged (boots once, exits, continues
     * at the transport level for the tournament gate). */
    if (const char *residentLiveEnv =
            std::getenv("MDKR_APP_TEST_ONLINE_LIVE_RESIDENT");
        residentLiveEnv != nullptr &&
        std::strtoul(residentLiveEnv, nullptr, 10) > 0ul) {
        std::string liveErr;
        MdkrOnlineTestLoopbackRace *race =
            OnlineRoom_makeTestLoopbackRace(&liveErr);
        if (race == nullptr) {
            std::fprintf(stderr,
                         "[online-resident-live] loopback race setup failed: %s\n",
                         liveErr.c_str());
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        const int liveResult = runOnlineLiveEngineSession(
            host, config, OnlineRoom_testLoopbackVisible(race),
            OnlineRoom_testLoopbackPeer(race), 0u, /*syntheticInput=*/true,
            /*endReasonOut=*/nullptr, /*resident=*/true);
        /* The resident coordinator OWNED every race's results poll + PUBLISH_-
         * RESULTS mid-residency, so there is NO post-exit report here (unlike the
         * non-resident lane below). */
        OnlineRoom_destroyTestLoopbackRace(race);
        shutdownLauncherHost(host, launcher);
        return liveResult;
    }
    if (std::getenv("MDKR_APP_TEST_ONLINE_LIVE") != nullptr) {
        std::string liveErr;
        MdkrOnlineTestLoopbackRace *race =
            OnlineRoom_makeTestLoopbackRace(&liveErr);
        if (race == nullptr) {
            std::fprintf(stderr,
                         "[online-live] loopback race setup failed: %s\n",
                         liveErr.c_str());
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        LiveRaceEndReason liveEndReason = LiveRaceEndReason::Completed;
        const int liveResult = runOnlineLiveEngineSession(
            host, config, OnlineRoom_testLoopbackVisible(race),
            OnlineRoom_testLoopbackPeer(race), 0u, /*syntheticInput=*/true,
            &liveEndReason);
        /* Same post-session seam as the interactive handoff: read WHY the
         * session ended and notify the peer of a deliberate mid-race leave
         * immediately (the survivor must not wait out the loss ladders). */
        notifyPeerOfDeliberateLeave(
            OnlineRoom_testLoopbackVisible(race),
            mdkr_party_link_take_session_end());
        /* In-process, the surviving endpoint's pump stopped with the engine;
         * service it so it drains what a real remote process would drain in
         * its own loop -- the lanes witness the typed reaction. The abort
         * crosses a real in-process SCTP channel asynchronously, so poll the
         * survivor's own latch (bounded) instead of assuming delivery is
         * synchronous with the send. */
        if (IMdkrOnlineAdapter *survivor = OnlineRoom_testLoopbackPeer(race)) {
            for (int drainPass = 0; drainPass < 400; ++drainPass) {
                survivor->service();
                if (mdkr_online_live_adapter_race_peer_lost(survivor)) break;
                SDL_Delay(1u);
            }
        }
        /* Same race-end seam as the interactive handoff: the results poll +
         * report must fire whenever the online engine session returns, and
         * this loopback proof is the fixture that witnesses it. */
        reportOnlineRaceResults(OnlineRoom_testLoopbackVisible(race),
                                liveEndReason);
        OnlineRoom_destroyTestLoopbackRace(race);
        shutdownLauncherHost(host, launcher);
        return liveResult;
    }
    /* The two-PROCESS proof: this process drives ONE production-shaped live
     * adapter -- the same OnlineRoom_makeGatedLiveAdapter factory the real
     * Online Room panel uses -- against the compiled-in MDKR_PARTY_ORIGIN,
     * to a ready race transport, then boots the VISIBLE engine on it with
     * peer=nullptr: the real remote process (a companion instance of this
     * SAME binary in the other role) supplies the peer's input over the real
     * mesh, exactly the `peer == nullptr` production path liveDrainMatchInput
     * already implements. MDKR_APP_TEST_ONLINE_LIVE above stays the loopback
     * proof; this is its live-cloud, cross-process sibling. Ordinary autoplay
     * never sets this variable, so the cloud harness stays inert. */
    if (std::getenv("MDKR_APP_TEST_ONLINE_LIVE_CLOUD") != nullptr) {
        const char *roleEnv = std::getenv("MDKR_APP_ONLINE_ROLE");
        const std::string role = roleEnv != nullptr ? roleEnv : "";
        if (role != "create" && role != "join") {
            std::fprintf(stderr,
                         "[online-live-cloud] invalid/missing "
                         "MDKR_APP_ONLINE_ROLE (expected create|join)\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        const char *joinCodeEnv = std::getenv("MDKR_APP_ONLINE_JOIN_CODE");
        const std::string joinCode = joinCodeEnv != nullptr ? joinCodeEnv : "";
        if (role == "join" && joinCode.empty()) {
            std::fprintf(stderr,
                         "[online-live-cloud] MDKR_APP_ONLINE_ROLE=join "
                         "requires MDKR_APP_ONLINE_JOIN_CODE\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        std::uint64_t timeoutMs = 60000u;
        if (const char *timeoutEnv =
                std::getenv("MDKR_APP_ONLINE_TIMEOUT_MS")) {
            char      *end    = nullptr;
            const long parsed = std::strtol(timeoutEnv, &end, 10);
            if (end == timeoutEnv || *end != '\0' || parsed < 1000 ||
                parsed > 600000) {
                std::fprintf(stderr,
                             "[online-live-cloud] invalid "
                             "MDKR_APP_ONLINE_TIMEOUT_MS=%s (expected "
                             "1000..600000)\n",
                             timeoutEnv);
                shutdownLauncherHost(host, launcher);
                return 2;
            }
            timeoutMs = static_cast<std::uint64_t>(parsed);
        }
        const bool isCreate = role == "create";
        /* Character 1/2, track 5 (Ancient Lake) and vehicle mask 0x07 exactly
         * match the combination tests/check_online_engine_boot.py already
         * proved the engine's online race admission accepts for
         * tests/input_scripts/race_2p_split.txt. */
        std::string cloudErr;
        MdkrOnlineTestCloudLiveSession *cloud = OnlineRoom_makeTestCloudLiveSession(
            isCreate ? MDKR_ONLINE_JOURNEY_CREATE : MDKR_ONLINE_JOURNEY_JOIN,
            joinCode, isCreate ? 1u : 2u, 5u, 7u, timeoutMs, &cloudErr);
        if (cloud == nullptr) {
            std::fprintf(stderr,
                         "[online-live-cloud] session setup failed: %s\n",
                         cloudErr.c_str());
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        /* Headless autoplay drains authored ticks unthrottled; a REAL peer
         * PROCESS (unlike the in-process loopback proof's synchronous
         * cross-pump) needs real wall-clock time between ticks for its
         * confirmations to cross the actual network before the drain
         * frontier moves on. Pace to the compiled compatibility fixture's
         * cadence (30Hz) by default; overridable for tuning. */
        unsigned paceHz = 30u;
        if (const char *paceEnv = std::getenv("MDKR_APP_ONLINE_PACE_HZ")) {
            char      *end    = nullptr;
            const long parsed = std::strtol(paceEnv, &end, 10);
            if (end == paceEnv || *end != '\0' || parsed < 0 || parsed > 1000) {
                std::fprintf(stderr,
                             "[online-live-cloud] invalid MDKR_APP_ONLINE_PACE_HZ="
                             "%s (expected 0..1000)\n",
                             paceEnv);
                OnlineRoom_destroyTestCloudLiveSession(cloud);
                shutdownLauncherHost(host, launcher);
                return 2;
            }
            paceHz = static_cast<unsigned>(parsed);
        }
        const int liveResult = runOnlineLiveEngineSession(
            host, config, OnlineRoom_testCloudLiveAdapter(cloud), nullptr,
            paceHz, /*syntheticInput=*/true);
        OnlineRoom_destroyTestCloudLiveSession(cloud);
        shutdownLauncherHost(host, launcher);
        return liveResult;
    }
#endif
    int roundTrips = 1;
    if (const char *roundText =
            std::getenv("MDKR_APP_TEST_SESSION_ROUNDTRIPS")) {
        char *end = nullptr;
        const long parsed = std::strtol(roundText, &end, 10);
        if (end == roundText || *end != '\0' || parsed < 1 || parsed > 3 ||
            restartProbe != nullptr) {
            std::fprintf(stderr,
                         "[session-test] invalid round-trip contract\n");
            shutdownLauncherHost(host, launcher);
            return 2;
        }
        roundTrips = static_cast<int>(parsed);
    }
    const std::uint64_t persistentSessionId = session.state().session_id;
    int result = 0;
    for (int round = 0; round < roundTrips; ++round) {
        if (testOnline && round > 0) {
            const std::uint32_t nextEpoch = session.state().match_epoch + 1u;
            bool launchApplied = false;
            if (testLaunchV3) {
                MdkrSessionLaunchV3 launch{};
                launchApplied = makeTestOnlineLaunchV3(
                    nextEpoch, testLocalMask, testViewportMask, &launch) &&
                    session.applyLaunch(launch);
            } else {
                MdkrSessionLaunchV2 launch{};
                launchApplied = makeTestOnlineLaunch(
                    nextEpoch, testLocalMask, testViewportMask, &launch) &&
                    session.applyLaunch(launch);
            }
            if (!launchApplied) {
                std::fprintf(stderr,
                             "[session-test] could not compose online "
                             "rematch envelope epoch=%u\n",
                             static_cast<unsigned>(nextEpoch));
                result = 2;
                break;
            }
        }
        result = runEngineSession(host, session, config, transition);
        if (result != 0 || (transition != nullptr &&
                            transition->request != OverlayExitRequest::None)) {
            break;
        }
        std::fprintf(stderr,
                     "[session-test] engine round=%d epoch=%u id=%llu ticks=%d "
                     "complete\n",
                     round + 1, session.state().match_epoch,
                     static_cast<unsigned long long>(session.state().session_id),
                     g_simTickCounter);
        if (roundTrips > 1 && config.automation_ticks > 0 &&
            g_simTickCounter != config.automation_ticks) {
            std::fprintf(stderr,
                         "[session-test] round=%d simulation witness mismatch "
                         "expected=%d actual=%d\n",
                         round + 1, config.automation_ticks,
                         g_simTickCounter);
            result = 2;
            break;
        }
        if (roundTrips > 1 && config.automation_frames > 0 &&
            g_frameCounter != config.automation_frames) {
            std::fprintf(stderr,
                         "[session-test] round=%d presentation witness mismatch "
                         "expected=%d actual=%d\n",
                         round + 1, config.automation_frames, g_frameCounter);
            result = 2;
            break;
        }
        if (round + 1 < roundTrips) {
            /* Prove the returned host can draw the launcher before it lends the
             * same window/device to the next match. */
            host.beginFrame();
            (void)launcher.draw(host);
            if (!host.endFrame()) {
                result = 1;
                break;
            }
        }
    }
    if (result == 0 && roundTrips > 1) {
        if (session.state().session_id != persistentSessionId ||
            session.state().match_epoch != static_cast<std::uint32_t>(roundTrips)) {
            std::fprintf(stderr,
                         "[session-test] persistent identity/epoch mismatch\n");
            result = 2;
        } else {
            std::fprintf(stderr,
                         "[session-test] persistent native lifecycle passed "
                         "rounds=%d id=%llu\n",
                         roundTrips,
                         static_cast<unsigned long long>(persistentSessionId));
        }
    }
    /* A deliberately narrow integration seam for the package restart gate.
     * It requests the exact post-engine transition the overlay produces, once:
     * the replacement process enters with MDKR_APP_RESTART_GAME and therefore
     * cannot request a second restart. This exercises main()'s actual staging,
     * AppRelaunch_replace(), executable-path, and one-shot handoff code without
     * creating a fake relaunch implementation or exposing a player-facing UI.
     */
    if (restartProbe != nullptr && !restartHandoff && result == 0 &&
        transition != nullptr &&
        std::strcmp(restartProbe, "return-to-launcher") == 0) {
        /* The overlay's other exit: Return to Launcher stages no ROM handoff
         * at all, it only asks main() to exec-replace this process with a
         * launcher. That path had no gate, so nothing proved the replacement
         * came back as a launcher rather than as a windowless engine run. */
        transition->request = OverlayExitRequest::ReturnToLauncher;
        std::fprintf(stderr, "[app-restart-test] requesting Return to Launcher\n");
    }
    if (restartProbe != nullptr &&
        (std::strcmp(restartProbe, "1") == 0 ||
         std::strcmp(restartProbe, "boot-failure") == 0 ||
         std::strcmp(restartProbe, "stage-failure") == 0)) {
        if (restartHandoff) {
            const MdkrVideoConfig *video = mdkr_video_config_current();
            std::fprintf(stderr,
                         "[app-restart-test] replacement boot rom=%s "
                         "frameLimit=%s\n",
                         config.rom_path ? config.rom_path : "(none)",
                         video->values[MDKR_VIDEO_FRAME_LIMIT].text);
        } else if (result == 0 && transition != nullptr &&
                   config.rom_path != nullptr && config.rom_path[0] != '\0') {
            transition->request = OverlayExitRequest::RestartGame;
            transition->romPath = config.rom_path;
            std::fprintf(stderr,
                         "[app-restart-test] requesting Restart & Apply "
                         "for active ROM\n");
        }
    }
    shutdownLauncherHost(host, launcher);
    return result;
}

void showPresentationFailure(AppHost &host) {
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
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             MDKR_BRAND_NAME " — Graphics Error",
                             message,
                             host.window());
}

void describeBootFailure(AppHost &host, int exitCode,
                         std::string *bootRecoveryMessage) {
    if (bootRecoveryMessage == nullptr) return;
    if (!host.webGpuRecoveryError().empty()) {
        *bootRecoveryMessage = host.webGpuRecoveryError();
        return;
    }
    char message[768];
    std::snprintf(
        message, sizeof(message),
        "Golden Balloon stopped safely while starting the game "
        "(error %d). Your ROM, saves, and settings were kept. "
        "Review Diagnostics for the exact failure, then try again.",
        exitCode);
    *bootRecoveryMessage = message;
}

/* Restart & Apply for a player. This is deliberately NOT runAutoplay: the
 * replacement of an interactive session must keep the interactive error
 * surfaces (a visible graphics dialog, a boot-recovery message routed back to
 * the launcher), must not consult any automation control, and must not turn a
 * compositor that is slow to schedule its first present into a failed restart.
 * The launcher frames below are the same warm-up the interactive Play path
 * gets for free by having drawn itself before the engine adopts the surface. */
int runRestartSession(AppHost &host, Launcher &launcher, SessionRuntime &session,
                      EngineSessionTransition *transition,
                      const std::string &romPath,
                      std::string *bootRecoveryMessage) {
    MdkrBootConfig config{};
    config.rom_path   = romPath.c_str();
    config.video_mode = -1;

    const std::uint64_t initialPresents = host.presentedFrames();
    const Uint64        warmupDeadline  = SDL_GetTicks64() + 2000u;
    while (host.presentedFrames() == initialPresents &&
           SDL_GetTicks64() < warmupDeadline) {
        if (host.pumpAndShouldQuit()) {
            shutdownLauncherHost(host, launcher);
            return 0;
        }
        host.beginFrame();
        launcher.draw(host);
        if (!host.endFrame()) {
            showPresentationFailure(host);
            shutdownLauncherHost(host, launcher);
            return 1;
        }
        if (host.presentedFrames() == initialPresents) SDL_Delay(1);
    }

    const int result = runEngineSession(host, session, config, transition);
    if (result != 0) describeBootFailure(host, result, bootRecoveryMessage);
    shutdownLauncherHost(host, launcher);
    return result;
}

int runInteractiveLauncher(AppHost &host, Launcher &launcher,
                           SessionRuntime &session,
                           EngineSessionTransition *transition,
                           std::string *bootRecoveryMessage) {
    bool running  = true;
    int  exitCode = 0;
    bool finishStudioSmokeAfterFrame = false;
    // Issue #54: show the "could not save" card the first time the player is
    // back at the launcher after a durable save write failed, for players who
    // cannot hear the in-game spoken notice.
    bool saveFailureCardShown = false;
    bool onlineClosing = false;
    Uint64 onlineClosingStarted = 0u;
    // The cancellable Workshop phase finishes first. Once online ownership is
    // retired, draw only closing progress and never return to normal dispatch.
    const auto readyToExit = [&]() {
        if (!onlineClosing) {
            if (!launcher.quitReady()) return false;
            launcher.finishCharacterWorkForExit();
            onlineClosing = true;
            onlineClosingStarted = SDL_GetTicks64();
            AppLaunchHold_release();
            Overlay_setPhonePartyHost(nullptr);
            launcher.beginNetworkShutdown();
        }
        const bool terminal = launcher.pollNetworkShutdown();
        if (terminal && launcher.networkShutdownFailed()) exitCode = 2;
        return terminal;
    };
    while (running) {
        launcher.serviceCharacterWork();
        if (readyToExit()) break;
        const bool drawableAvailable =
            host.drawableWidth() > 0 && host.drawableHeight() > 0;
        const AppUiIdleDecision idle = AppUi_idleDecision(
            drawableAvailable,
            host.lastSurfaceWasOccluded());
        if (idle.waitMilliseconds > 0) {
            // Occluded WebGPU surfaces are retried at a bounded 40 Hz. A truly
            // minimized zero-drawable window skips ImGui construction entirely
            // until restore, while close/quit remains event-driven and prompt.
            if (host.waitAndPump(static_cast<int>(idle.waitMilliseconds))) {
                launcher.requestQuit();
                if (readyToExit()) break;
            }
            if (!idle.buildFrame) continue;
        } else if (host.pumpAndShouldQuit()) {
            launcher.requestQuit();
            if (readyToExit()) break;
        }
        host.beginFrame();
        LauncherAction action{};
        if (onlineClosing) {
            launcher.drawOnlineClosing(SDL_GetTicks64() - onlineClosingStarted >= 10000u);
        } else {
            action = launcher.draw(host);
        }
        if (!host.endFrame()) {
            showPresentationFailure(host);
            exitCode = 1;
            running  = false;
            continue;
        }
        // Quit can be requested by a control inside this very frame. Honor it
        // before either online registry can dispatch a game, including while
        // a Workshop operation is still completing. The next iteration begins
        // retirement only when that operation has actually settled.
        if (onlineClosing || launcher.quitRequested()) continue;
        if (finishStudioSmokeAfterFrame) {
            std::fprintf(
                stderr,
                "[app-ui] exact-character-studio smoke completed "
                "published launcher return\n");
            running = false;
            continue;
        }
#if MDKR_ENABLE_ONLINE_BETA
        /* PRODUCTION ROOM-READY takeover (polled BEFORE the race-boot
         * handoff below). The panel (inside launcher.draw above) published its live
         * adapter the first frame an online room (ANY mode -- the takeover is
         * mode-agnostic) reached SELECTING with 2 members in LOBBY. Boot the
         * visible engine DESCRIPTOR-LESS (peer == nullptr) so the
         * NATIVE CHARSELECT -> TRACKSELECT own race 1 for the human, and the resident
         * coordinator re-cycles races 2..N single-endpoint in this one process. The
         * race-boot handoff below stays the UNCHANGED fallback for every non-takeover
         * path -- a descriptor-first race-boot, or a room that never armed room-ready
         * (including a post-LEFT/ERROR return whose latch stays set). Single race is
         * NOT a fallback trigger on its own: the takeover is mode-agnostic and claims
         * single-race lobby-start rooms too. On a watchdog
         * ERROR trip (nonzero result) we stay in the launcher loop -- the panel keeps
         * servicing the adapter and surfaces recovery, exactly like the race-boot
         * failure path. */
        if (IMdkrOnlineAdapter *roomReady = OnlineRoom_pollEngineRoomReady()) {
            MdkrBootConfig lobbyConfig{};
            const std::string lobbyRom = AppConfig::get("rom_path", "");
            lobbyConfig.rom_path = lobbyRom.c_str();
            lobbyConfig.video_mode = -1;
            /* DIAGNOSTIC [3/5] -- publish consumed by the launcher loop (the poll's
             * consume-once handoff fired non-null). */
            std::fprintf(stderr,
                         "[online-room-ready] publish consumed by launcher; "
                         "booting descriptor-less native session\n");
            std::fprintf(stderr,
                         "[online-live] engine ROOM-READY takeover accepted "
                         "(descriptor-less; native owns race 1)\n");
            MdkrPartyLinkSessionEndReason lobbyEndReason =
                MDKR_PARTY_LINK_SESSION_END_NONE;
            const int lobbyResult = runOnlineLobbyStartLiveSession(
                host, lobbyConfig, roomReady, &lobbyEndReason);
            /* DIAGNOSTIC [5/5] -- boot RESULT: exit code + session-end reason
             * (FINISHED/LEFT/ERROR) + whether a re-arm is now pending (a FINISHED
             * return arms it; the panel's per-frame observer completes it). Logged
             * for EVERY return so a real-hardware takeover no longer returns silently
             * (before, only a nonzero result got a single line). */
            std::fprintf(stderr,
                         "[online-room-ready] boot result=%d reason=%s "
                         "rearmPending=%d\n",
                         lobbyResult, onlineSessionEndLabel(lobbyEndReason),
                         OnlineRoom_roomReadyRearmPending() ? 1 : 0);
            if (lobbyResult != 0) {
                std::fprintf(stderr,
                             "[online-live] lobby-start session ended result=%d "
                             "(watchdog error / stuck wait); staying in the Online "
                             "Room (panel surfaces recovery)\n",
                             lobbyResult);
            }
            /* The session ran the whole tournament in-process (or bounded a stuck
             * wait); fall back into the launcher loop. The engine->launcher
             * FINISH/RETURN handshake fires -- runOnlineLobbyStartLiveSession
             * logged the [online-session-end] reason (FINISHED / LEFT / ERROR)
             * before returning. The panel still owns the adapter/room, so this
             * `continue` re-draws the Online Room with the human back in it. */
            continue;
        }
        /* Make-or-break handoff: an Online Room adapter (driven by the UX-owned
         * panel inside launcher.draw above) has reached the visual race-start
         * point and published itself. Boot the VISIBLE engine on its live
         * transport, then fall back into this loop. No UI change is required --
         * the trigger is adapter state, not a panel callback. */
        if (IMdkrOnlineAdapter *raceBoot = OnlineRoom_pollEngineRaceBoot()) {
            /* BACKOUT BELT: the native room-ready takeover did NOT claim this boot
             * (a descriptor-first race-boot, a room that never armed room-ready, or a
             * post-LEFT/ERROR return whose latch stays set). Single race is not a
             * trigger on its own -- the mode-agnostic takeover claims single-race
             * lobby-start rooms too. This per-race fallback is the intended safety
             * net, but the
             * native takeover is the production path -- WARN so any PRODUCTION use of
             * the fallback is visible in the logs. */
            std::fprintf(stderr,
                         "[online-room-ready] WARN race-boot fallback engaged "
                         "(native room-ready takeover did not claim this boot)\n");
            MdkrBootConfig onlineConfig{};
            const std::string onlineRom = AppConfig::get("rom_path", "");
            onlineConfig.rom_path = onlineRom.c_str();
            onlineConfig.video_mode = -1;
            std::fprintf(stderr,
                         "[online-live] engine race-boot handoff accepted\n");
            LiveRaceEndReason liveEndReason = LiveRaceEndReason::Completed;
            const int liveResult = runOnlineLiveEngineSession(
                host, onlineConfig, raceBoot, nullptr, 0u,
                /*syntheticInput=*/false, &liveEndReason);
            /* Race-end handoff back to the ROOM: the lobby (Online Room panel)
             * is still live and owns the RESULTS/standings view, so hand it the
             * finished race's placements and simply fall back into the launcher
             * UI loop. Nothing is torn down here. A peer-loss / barrier abort
             * suppresses the publish and the panel already fronts the
             * OPPONENT_LEFT / OPPONENT_NEVER_STARTED recovery card. */
            reportOnlineRaceResults(raceBoot, liveEndReason);
            /* This per-race race-boot fallback deliberately
             * does NOT arm the room-ready re-arm. It carries a single race and reports
             * a per-race LiveRaceEndReason, not the tournament-level party_link
             * session-end reason; a tournament run entirely on this fallback (only
             * reachable after a LEFT/ERROR native return) completes
             * via the reducer's final-standings landing + New Tournament REMATCH, which
             * emits no FINISHED session-end note to gate on. The latch stays set from
             * the original native takeover, so the next tournament in this room also
             * uses this fallback -- a benign degradation (never worse than BASE, never a
             * re-boot loop), not the native path. Arming here has no FINISHED signal to
             * hook, and arming would have to stay FINISHED-only to preserve the no-loop
             * property; a UI-event hook on the New-Tournament press is out of scope. */
            if (liveResult != 0) {
                /* A failed ONLINE boot must never quit the whole app -- that
                 * tore down the adapter/mesh and stranded the peer in a dead
                 * room. Stay in the launcher loop: the panel keeps servicing
                 * the adapter and surfaces the recovery itself (a PeerLost /
                 * mesh failure card immediately, otherwise the 30s view-timeout
                 * card -- "Race Did Not Load" -> Return to Lobby -- or the race
                 * chrome's Leave Race). Local/offline boots below keep their
                 * fatal describeBootFailure handling unchanged. */
                std::fprintf(stderr,
                             "[online-live] engine session failed result=%d; "
                             "staying in the Online Room (panel surfaces "
                             "recovery)\n",
                             liveResult);
            }
            continue;
        }
#endif
        if (action.type == LauncherActionType::Quit) {
            /* The reachable leak: skip armed, a remembered ROM that never
             * dispatched, and the player picks Quit. Nothing else on this path
             * has released the pads, and ~Launcher's backstop runs after
             * main()'s host.shutdown() -- i.e. after SDL_Quit(). Give them back
             * here, while SDL still owns them. */
            AppLaunchHold_release();
            running = false;
        } else if (action.type == LauncherActionType::Play) {
            // Blocks while the game renders into the launcher's host window.
            exitCode = runEngineSession(host, session, action.boot, transition);
            if (exitCode != 0) {
                describeBootFailure(host, exitCode, bootRecoveryMessage);
            } else if (!saveFailureCardShown &&
                       mdkr_user_paths_save_write_failed()) {
                saveFailureCardShown = true;
                launcher.setBootError(kSavePersistFailedNotice);
            }
            if (exitCode == 0 && transition != nullptr &&
                (transition->request == OverlayExitRequest::ReturnToLauncher ||
                 transition->request ==
                     OverlayExitRequest::ReturnToLauncherWithoutMeasurement)) {
                /* Return through the surviving host/runtime. The engine has
                 * released its adopted children; no exec, second app process,
                 * or lost Party state is needed. */
                if (transition->request ==
                        OverlayExitRequest::ReturnToLauncherWithoutMeasurement &&
                    action.boot.character_preview_result != nullptr) {
                    MdkrCharacterPreviewResult &result =
                        *action.boot.character_preview_result;
                    result.warmup_complete = 0;
                    result.fit_diagnostics_valid = 0;
                    result.camera_projection_valid = 0;
                    result.vehicle_surface_valid = 0;
                    result.fit_projection_valid = 0;
                }
                if (!session.returnHome()) {
                    std::fprintf(stderr,
                                 "[session] could not return engine result Home\n");
                    exitCode = 2;
                    running = false;
                } else {
                    transition->request = OverlayExitRequest::None;
                    running = true;
                    std::fprintf(stderr,
                                 "[session] returned to persistent launcher id=%llu\n",
                                 static_cast<unsigned long long>(
                                     session.state().session_id));
                    const char *studioSmokeToken = std::getenv(
                        "MDKR_APP_SMOKE_CHARACTER_STUDIO_TOKEN");
                    if (studioSmokeToken != nullptr &&
                        std::strcmp(studioSmokeToken,
                                    "mdkr64-character-studio-v1") == 0) {
                        finishStudioSmokeAfterFrame = true;
                    }
                }
            } else {
                running = false;
            }
        }
    }
    // Main's shared shutdown helper covers error exits as well as normal Quit,
    // completing Workshop work, all network owners and global RTC cleanup.
    /* Every way out of the loop above, including the ones that break early
     * (quitReady, a presentation failure, a session that could not return
     * Home). main() calls host.shutdown() after this returns, so this is the
     * last point at which giving the pads back is an ordinary close rather
     * than a call into freed SDL state. */
    AppLaunchHold_release();
    return exitCode;
}

} // namespace

int main(int argc, char **argv) {
    /* Arm the crash surface first, and deliberately ABOVE every dispatch below.
     * The automation branch hands control straight to the engine's own main()
     * body and never comes back, so a later install site would leave exactly
     * the invocation shape CI runs -- and the one this sprint's gate drives --
     * uncovered. Installing here costs three signal() calls and reads nothing:
     * every field the report names is collected at fault time. */
    CrashScreen_install();

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

    /* Test-only, and deliberately ahead of the automation dispatch below: the
     * invocation shape a relaunched process actually receives is the property
     * that decides whether Return to Launcher opens a launcher or starts a
     * windowless engine run. It has to be observable even when it is wrong. */
    if (std::getenv("MDKR_APP_TEST_RESTART_RECOVERY_FRAMES") != nullptr) {
        std::fprintf(stderr,
                     "[app-restart-test] invoked argc=%d ui=%d automation=%d\n",
                     argc, mdkr_argv_requests_ui(argc, argv),
                     mdkr_is_automation_invocation(argc, argv));
    }

    /* Resource/user-path initialization may still use the caller's launch
     * spelling. Relaunch is stricter on Windows: it is only enabled when the
     * kernel supplies the canonical image path, never as an argv[0] fallback. */
    std::string userPathExecutable = argv[0] ? argv[0] : "";
    std::string relaunchExecutable = userPathExecutable;
    char      *runningExecutable = nullptr;
    const bool resolvedRunning =
        mdkr_running_executable_path_utf8(&runningExecutable) == 0 &&
        runningExecutable != nullptr;
    if (resolvedRunning) {
        /* The kernel's own answer, so relaunch never depends on a PATH lookup
         * or on the directory a relative argv[0] was resolved against. */
        userPathExecutable = runningExecutable;
        relaunchExecutable = runningExecutable;
        std::free(runningExecutable);
    }
#if defined(_WIN32)
    if (!resolvedRunning) {
        std::fprintf(stderr,
                     "[app] could not resolve the running Windows executable; "
                     "Restart & Apply will remain unavailable\n");
        relaunchExecutable.clear();
    }
#endif
    /* Register a real bundle before any config/save/resource access, including
     * automation launched through Contents/MacOS. This does not change CWD:
     * relative ROM/script paths remain owned by their caller, while immutable
     * Resources and mutable per-user state resolve through separate policies. */
    if (mdkr_user_paths_init(userPathExecutable.c_str()) < 0) {
        const char *detail = mdkr_user_paths_last_error();
        const char *message = detail != nullptr && detail[0] != '\0' ? detail :
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

    // Windowless inventory consumed by the all-state Online Room render gate.
    // It is deliberately independent of providers and graphics initialization.
    if (std::getenv("MDKR_APP_DUMP_ONLINE_GALLERY")) {
        return OnlineRoom_dumpGalleryContract();
    }

    // Non-interactive tool-registry dump, on the same path and for the same
    // reason. Above the automation dispatch below, which would otherwise hand
    // the flag to the engine as an unrecognised argument and start a game.
    if (argvRequestsToolTableDump(argc, argv)) {
        DevTools_dumpTable();
        return 0;
    }

    /* This has to precede file-dialog SDL_Init, engine dispatch and AppHost:
     * on macOS SDL may create/activate NSApplication during initialization. */
    AppActivation_prepareProcess();

    // Automation/CLI invocations run the unchanged engine path. Deliberately
    // BEFORE the bundle chdir below: a script that passes a relative --rom path
    // must keep resolving it against the directory the script is running in.
    // The surface capability check above must precede this dispatch: bounded
    // engine automation also creates a native SDL window.
    if (mdkr_is_automation_invocation(argc, argv)) {
        return mdkr64_headless_main(argc, argv);
    }

    // Exercise the native picker through the same live-window activation state
    // as the launcher, then print its selection and ROM verdict.
    if (std::getenv("MDKR_APP_FILEDIALOG_SELFTEST")) {
        return runFileDialogSelfTest();
    }

    const AppUiSmokeInputMode smokeInputMode = AppUi_smokeInputMode();
    if (smokeInputMode == AppUiSmokeInputMode::Invalid) {
        std::fprintf(
            stderr,
            "[app-ui-test] rejected partial or stale synthetic-input contract\n");
        return 2;
    }

    /* Resolve durable settings before creating the one launcher/game window:
     * Window.Mode must participate in SDL_CreateWindow itself so a persisted
     * Windows fullscreen launch never flashes or starts as a decorated window. */
    mdkr_video_config_init(argc, argv);

    // Tee stdout/stderr into the in-app console + mdkr64.log BEFORE host.init, so
    // its fatal init diagnostics are captured even under the macOS .app bundle
    // and the Windows GUI subsystem, where there is no console to catch them.
    // Interactive path only — automation returned above and is never redirected.
    DiagLogScope diagnosticLog;

    /* Always record where saves go (issue #54). A support log that showed this
     * one line would have answered the ghost-saves report at a glance. */
    {
        char saveDirectory[4096];
        if (mdkr_user_save_directory(saveDirectory, sizeof(saveDirectory))) {
            std::fprintf(stderr, "[SAVE] directory %s (%s)\n", saveDirectory,
                         mdkr_user_paths_save_origin_label());
        }
    }

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

    Launcher launcher;
    Overlay_setPhonePartyHost(launcher.state().phoneParty);
    std::uint64_t sessionId =
        static_cast<std::uint64_t>(SDL_GetPerformanceCounter()) ^
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&host));
    if (sessionId == 0u) sessionId = 1u;
    SessionRuntime session(sessionId);

    std::string inheritedRecovery;
    if (AppRestart_getEnv("MDKR_APP_BOOT_RECOVERY", inheritedRecovery) &&
        !inheritedRecovery.empty()) {
        launcher.setBootError(inheritedRecovery.c_str());
        clearBootRecoveryEnvironment();
    }

    EngineSessionTransition transition;

    /* Issue #60. Made here, before any launcher path runs, so the headless
     * shell smoke and the real launcher take the identical decision through
     * the identical code -- a gate that armed the skip through a route a
     * player cannot take would prove nothing about the product.
     *
     * Autoplay is the one exception: it boots the engine itself and never
     * consults the launcher's Play transition, so arming it there would ask
     * for a boot nothing is waiting to receive. */
    if (std::getenv("MDKR_APP_AUTOPLAY") == nullptr) {
        (void)armSkipLauncher(launcher);
    }

    // Headless shell smoke (CI + design review): render a bounded launcher
    // sequence and optionally capture the last frame.
    if (const char *smoke = std::getenv("MDKR_APP_SMOKE_FRAMES")) {
        const int smokeResult = runShellSmoke(host, launcher, smokeInputMode, smoke);
        return launcher.networkShutdownFailed() && smokeResult == 0 ? 2 : smokeResult;
    }

    // Validation/CI: boot straight into the shell window, proving the
    // launcher -> engine handoff non-interactively.
    if (std::getenv("MDKR_APP_AUTOPLAY")) {
        bool restartReplacement = false;
        const int autoplayResult = runAutoplay(
            host, launcher, session, &transition, &restartReplacement);
        // Autoplay's terminal helper has joined its owners. Never exec or
        // convert failed cleanup into a successful automation result.
        if (launcher.networkShutdownFailed()) {
            return autoplayResult == 0 ? 2 : autoplayResult;
        }
        if (transition.request != OverlayExitRequest::None) {
            if (transition.request == OverlayExitRequest::RestartGame &&
                !AppRestart_stageGame(transition.romPath.c_str(),
                                      /*autoplaySession=*/true)) {
                return recoverRestartToLauncher(
                    relaunchExecutable,
                    "Restart & Apply could not preserve the active ROM. "
                    "The launcher was restored; your settings, saves, and "
                    "previous ROM selection were kept.");
            }
            if (transition.request == OverlayExitRequest::ReturnToLauncher) {
                /* An externally requested autoplay is not a one-shot restart
                 * handoff. Clear it explicitly or Return to Launcher would
                 * exec straight back into another autoplay session. */
                AppRestart_clear();
                /* Ignored unless the return-to-launcher gate armed it, and it
                 * is what lets that gate observe the real replacement drawing
                 * real launcher frames instead of waiting on a player. */
                prepareRestartRecoverySmokeForTest();
            }
            DiagLog_shutdown();
            return relaunchApplication(relaunchExecutable);
        }
        if (restartReplacement && autoplayResult != 0) {
            return recoverRestartToLauncher(
                relaunchExecutable,
                "Restart & Apply could not start the game. The launcher was "
                "restored; your ROM, saves, and settings were kept. Review "
                "Diagnostics for the exact failure, then try again.");
        }
        return autoplayResult;
    }

    std::string bootRecoveryMessage;
    int         exitCode = 0;
    std::string restartRom;
    if (AppRestart_pendingGame() && AppRestart_consumeGame(restartRom)) {
        std::fprintf(stderr, "[app] Restart & Apply handoff accepted\n");
        exitCode = runRestartSession(
            host, launcher, session, &transition, restartRom,
            &bootRecoveryMessage);
    } else {
        if (AppRestart_pendingGame()) {
            /* An inherited marker without a usable ROM is stale state, not a
             * request. The player gets the ordinary launcher rather than an
             * exit code no window is left to explain. */
            std::fprintf(stderr,
                         "[app] ignoring an incomplete Restart & Apply handoff; "
                         "opening the launcher\n");
            AppRestart_clear();
        }
        exitCode = runInteractiveLauncher(
            host, launcher, session, &transition, &bootRecoveryMessage);
    }
    shutdownLauncherHost(host, launcher);
    if (launcher.networkShutdownFailed()) return exitCode == 0 ? 2 : exitCode;
    if (transition.request != OverlayExitRequest::None ||
        !bootRecoveryMessage.empty()) {
        // End the tee before exec: fd 1/2 are restored, its pipe reader is
        // joined, and no private descriptor can leak into the replacement.
        DiagLog_shutdown();
        if (!bootRecoveryMessage.empty()) {
            setBootRecoveryEnvironment(bootRecoveryMessage);
        }
        if (transition.request == OverlayExitRequest::RestartGame &&
            !AppRestart_stageGame(transition.romPath.c_str())) {
            return recoverRestartToLauncher(
                relaunchExecutable,
                "Restart & Apply could not preserve the active ROM. The "
                "launcher was restored; your settings, saves, and previous "
                "ROM selection were kept.");
        }
        return relaunchApplication(relaunchExecutable);
    }
    return exitCode;
}
