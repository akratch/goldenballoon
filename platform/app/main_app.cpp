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
#include "platform_os.h"
#include "present_sched.h"
#include "ui_launcher.h"
#include "ui_online_room.h"
#include "ui_overlay.h"
#include "ui_settings.h"
#include "user_paths.h"

#include "video_config.h"   // mdkr_video_config_init / _schema (settings panel)

#include <SDL.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
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
    Overlay_setPauseAllowed(session.overlayMayPause());
    Overlay_install(host.window());
    const int result = mdkr64_engine_boot(&config);

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
        host.shutdown();
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
     * The scripted accessibility walk: hold Tab down through the launcher and
     * let the shared row helper announce whatever the keyboard lands on.
     *
     * It drives the SAME synthetic-keyboard route as the Frame limit script --
     * SDL event -> ImGui backend -> the production widget -- because a walk
     * that called the panel's draw functions directly would prove the rows can
     * speak, not that a player pressing Tab ever reaches them.
     */
    const bool smokeA11yWalk = AppUi_a11yWalkArmed();
    const char *smokeUiScale =
        std::getenv("MDKR_APP_SMOKE_UI_SCALE_DRAG");
    const char *smokeTouchScroll =
        std::getenv("MDKR_APP_SMOKE_TOUCH_SCROLL");
    const char *smokeTouchToken =
        std::getenv("MDKR_APP_SMOKE_TOUCH_TOKEN");
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
        host.shutdown();
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
        host.shutdown();
        return 2;
    }
    float smokeUiScaleTarget = 1.0f;
    const bool smokeUiScaleDrag = smokeUiScale && smokeUiScale[0];
    if (smokeUiScaleDrag &&
        (!AppUi_parseScale(smokeUiScale, &smokeUiScaleTarget) ||
         std::fabs(smokeUiScaleTarget - 2.0f) > 0.001f)) {
        std::fprintf(stderr,
                     "[app] smoke: UI-scale drag target must be 2.00\n");
        host.shutdown();
        return 2;
    }
    if ((smokeUiScaleDrag && smokeFrameLimit) ||
        (smokePresentationPace && (smokeUiScaleDrag || smokeFrameLimit)) ||
        (smokeA11yWalk && (smokeUiScaleDrag || smokeFrameLimit ||
                           smokePresentationPace || smokeTouch ||
                           smokeNavigation || smokeOnlineActionArmed)) ||
        (smokeTouch && (smokeUiScaleDrag || smokeFrameLimit ||
                        smokePresentationPace || smokeNavigation ||
                        smokeOnlineActionArmed)) ||
        (smokeOnlineActionArmed &&
         (smokeUiScaleDrag || smokeFrameLimit || smokePresentationPace ||
          smokeNavigation))) {
        std::fprintf(stderr,
                     "[app] smoke: pointer scripts cannot share one input run\n");
        host.shutdown();
        return 2;
    }
    if (smokePresentationPace &&
        std::strcmp(smokePresentationPace, "original") != 0 &&
        std::strcmp(smokePresentationPace, "smooth") != 0) {
        std::fprintf(stderr,
                     "[app] smoke: unsupported presentation pace %s "
                     "(original or smooth)\n", smokePresentationPace);
        host.shutdown();
        return 2;
    }
    if (smokePresentationPace && frames < 8) frames = 8;
    if (smokeUiScaleDrag && frames < 11) frames = 11;
    if (smokeTouch && frames < 10) frames = 10;
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
    bool onlineActionInputQueued = false;
    int smokePlayActions = 0;
    std::string smokePlayActionRom;
    auto observeSmokePlay = [&](const LauncherAction &action) {
        if (action.type != LauncherActionType::Play) return;
        ++smokePlayActions;
        smokePlayActionRom = action.boot.rom_path ? action.boot.rom_path : "";
    };
    if (smokeDropPlayMutate && !smokeDropPlay) {
        std::fprintf(stderr,
                     "[app] smoke: final Play mutation requires final Play check\n");
        host.shutdown();
        return 2;
    }
    if (smokeReplacementPlay && smokeReplacementPlay[0] &&
        (!smokeDrop || !smokeDrop[0])) {
        std::fprintf(stderr,
                     "[app] smoke: replacement Play requires an initial drop\n");
        host.shutdown();
        return 2;
    }
    if (smokeFrameLimit &&
        (std::strcmp(smokeFrameLimit, "240") != 0 ||
         smokeFrameLimitSteps < 0)) {
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
        const LauncherAction action = launcher.draw(host);
        observeSmokePlay(action);
        const bool ok = host.endFrame((i == frames - 1) ? shot : nullptr);
        renderOk      = renderOk && ok;
        if (i == frames - 1) captureOk = ok;
        /* A renderer failure is terminal. Starting another ImGui frame and
         * returning before ImGui::Render leaves dynamic texture state
         * half-transitioned and can make renderer shutdown release an
         * invalid atlas handle. Stop at the first failed presentation. */
        if (!ok) break;

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

        if (smokeA11yWalk) {
            /*
             * Two phases, not one interleaved stream. Tab is a linear walk of
             * the panel and is what carries the coverage claim, so it gets a
             * clean run at it; mixing arrow presses into that walk made the
             * sequence periodic and left the same nine rows unvisited on every
             * lap. The arrow keys are the other way a player moves, and a row
             * that answers only to Tab is still a row somebody cannot reach, so
             * the remainder of the run drives Down and Up over the same panel.
             * The boundary is printed because it is what lets the gate insist
             * the arrow phase spoke too, rather than counting the Tab phase's
             * utterances twice.
             */
            const int tabFrames = frames - frames / 4;
            if (i < tabFrames) {
                host.queueKeyPressForSmoke(SDLK_TAB);
            } else {
                if (i == tabFrames) {
                    std::printf("[app-a11y-walk] tab phase complete frame=%d\n", i);
                    std::fflush(stdout);
                }
                host.queueKeyPressForSmoke(
                    ((i - tabFrames) % 8) < 6 ? SDLK_DOWN : SDLK_UP);
            }
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
     * checked. Exercise the exact formerly-dead gold Play action: begin a real
     * second SDL drop, supersede only that candidate worker, then require one
     * final-check action for the original active path. */
    if (smokeReplacementPlay && smokeReplacementPlay[0] && renderOk) {
        const LauncherState &initial = launcher.state();
        const std::string activeRom = initial.romPath;
        const bool initialReady = !initial.romValidationPending &&
                                  initial.romInfo.valid && !activeRom.empty();
        bool replacementPending = false;
        bool superseded = false;
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
                const LauncherState &play = launcher.state();
                superseded = play.romValidationPending &&
                    play.romPlayValidationPending &&
                    play.romValidationPath == activeRom &&
                    !play.romCandidateVisible;
            }
        }
        const Uint64 deadline = SDL_GetTicks64() + 5000u;
        while (superseded && launcher.state().romValidationPending &&
               SDL_GetTicks64() < deadline && renderOk) {
            if (host.waitAndPump(1)) sawQuit = true;
            host.beginFrame();
            const LauncherAction action = launcher.draw(host);
            observeSmokePlay(action);
            renderOk = host.endFrame() && renderOk;
            ++serviceFrames;
        }
        const LauncherState &finalState = launcher.state();
        const bool settled = !finalState.romValidationPending;
        std::printf(
            "[app] smoke: replacement Play candidate=%s initialReady=%d "
            "replacementPending=%d superseded=%d serviceFrames=%d actions=%d "
            "actionRom=%s settled=%d active=%s candidateVisible=%d\n",
            smokeReplacementPlay, initialReady ? 1 : 0,
            replacementPending ? 1 : 0, superseded ? 1 : 0,
            serviceFrames, smokePlayActions,
            smokePlayActionRom.empty() ? "(none)" : smokePlayActionRom.c_str(),
            settled ? 1 : 0,
            finalState.romPath.empty() ? "(none)" : finalState.romPath.c_str(),
            finalState.romCandidateVisible ? 1 : 0);
        if (!initialReady || !replacementPending || !superseded || !settled ||
            serviceFrames < 1 || smokePlayActions != 1 ||
            smokePlayActionRom != activeRom ||
            finalState.romPath != activeRom ||
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
        std::printf(
            "[app-ui-test] top navigation target=%ld actual=%d queued=%d\n",
            smokeNavigationPanel, actualPanel,
            smokeNavigationQueued ? 1 : 0);
        if (!smokeNavigationQueued || actualPanel != smokeNavigationPanel) {
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
            host.shutdown();
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
        host.shutdown();
        return 2;
    }
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
    if (const char *frames = std::getenv("MDKR_APP_AUTOPLAY_FRAMES")) {
        char *end = nullptr;
        const long parsed = std::strtol(frames, &end, 10);
        if (end == frames || *end != '\0' || parsed < 1 || parsed > 1000000 ||
            config.automation_ticks > 0) {
            std::fprintf(stderr,
                         "[app] invalid/conflicting MDKR_APP_AUTOPLAY_FRAMES=%s "
                         "(expected 1..1000000 and no tick limit)\n",
                         frames);
            host.shutdown();
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
            host.shutdown();
            return 2;
        }
        if (!session.beginOnline() ||
            !session.setConnectivity(MDKR_CONNECTIVITY_DIRECT) ||
            !session.setRoomPhase(MDKR_ROOM_LOADING)) {
            std::fprintf(stderr,
                         "[session-test] could not compose first online "
                         "launcher envelope\n");
            host.shutdown();
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
            host.shutdown();
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
    int roundTrips = 1;
    if (const char *roundText =
            std::getenv("MDKR_APP_TEST_SESSION_ROUNDTRIPS")) {
        char *end = nullptr;
        const long parsed = std::strtol(roundText, &end, 10);
        if (end == roundText || *end != '\0' || parsed < 1 || parsed > 3 ||
            restartProbe != nullptr) {
            std::fprintf(stderr,
                         "[session-test] invalid round-trip contract\n");
            host.shutdown();
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
    host.shutdown();
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
            host.shutdown();
            return 0;
        }
        host.beginFrame();
        launcher.draw(host);
        if (!host.endFrame()) {
            showPresentationFailure(host);
            host.shutdown();
            return 1;
        }
        if (host.presentedFrames() == initialPresents) SDL_Delay(1);
    }

    const int result = runEngineSession(host, session, config, transition);
    if (result != 0) describeBootFailure(host, result, bootRecoveryMessage);
    host.shutdown();
    return result;
}

int runInteractiveLauncher(AppHost &host, Launcher &launcher,
                           SessionRuntime &session,
                           EngineSessionTransition *transition,
                           std::string *bootRecoveryMessage) {
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
            showPresentationFailure(host);
            exitCode = 1;
            running  = false;
            continue;
        }
        if (action.type == LauncherActionType::Quit) {
            running = false;
        } else if (action.type == LauncherActionType::Play) {
            // Blocks while the game renders into the launcher's host window.
            exitCode = runEngineSession(host, session, action.boot, transition);
            if (exitCode != 0) {
                describeBootFailure(host, exitCode, bootRecoveryMessage);
            }
            if (exitCode == 0 && transition != nullptr &&
                transition->request == OverlayExitRequest::ReturnToLauncher) {
                /* Return through the surviving host/runtime. The engine has
                 * released its adopted children; no exec, second app process,
                 * or lost Party state is needed. */
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
                }
            } else {
                running = false;
            }
        }
    }
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

    /* Fail closed before a file dialog, SDL video initialization, or Cocoa can
     * activate. MDKR64_HIDDEN is deliberately insufficient: it is a best-effort
     * visibility hint used by old harnesses. MDKR_APP_TESTS_ALLOWED=1 names
     * the requested test class, while MDKR_DEDICATED_TEST_DESKTOP=1 is the
     * independent caller attestation supported runners cannot manufacture.
     * Smoke/autoplay/file-dialog automation is caught even if a legacy script
     * forgot MDKR64_HIDDEN. Ordinary interactive launches are unchanged. */
    if (AppActivation_rejectUnauthorizedAutomationSurface()) {
        std::fprintf(
            stderr,
            "[app-test-safety] refused native surface: MDKR64_HIDDEN does not "
            "authorize app/window tests; both the class opt-in and independent "
            "dedicated-desktop attestation are required\n");
        return 2;
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

    // Headless shell smoke (CI + design review): render a bounded launcher
    // sequence and optionally capture the last frame.
    if (const char *smoke = std::getenv("MDKR_APP_SMOKE_FRAMES")) {
        return runShellSmoke(host, launcher, smokeInputMode, smoke);
    }

    // Validation/CI: boot straight into the shell window, proving the
    // launcher -> engine handoff non-interactively.
    if (std::getenv("MDKR_APP_AUTOPLAY")) {
        bool restartReplacement = false;
        const int autoplayResult = runAutoplay(
            host, launcher, session, &transition, &restartReplacement);
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
    host.shutdown();
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
