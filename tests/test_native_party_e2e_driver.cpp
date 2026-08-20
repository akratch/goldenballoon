/*
 * End-to-end Phone Party host driver.
 *
 * Owned by two check scripts and deliberately NOT a ctest.
 *
 *   CLOUD arm (tests/check_party_native_e2e.py): opens the REAL libdatachannel
 *   transport against a live local Party Worker (wrangler dev) whose lifetime,
 *   port and paired controller page the check provides.
 *
 *   LAN arm (tests/check_party_lan_e2e.py, --lan): opens the REAL
 *   LanPartyTransport — the embedded MdkrLanPartyServer + in-process
 *   MdkrLanPartyRoom ARE the signaling backend, with NO wrangler and NO cloud
 *   in the loop. This proves the whole no-internet path: a real headless
 *   browser loads the controller page from the embedded server over plain
 *   http, redeems over /party-ws, and pairs by the pure-JS SAS fallback whose
 *   phrase must equal the native mbedtls phrase.
 *
 * Either way it drives MdkrNativePartyHost exactly like the launcher does —
 * open, service in a loop, approve, drain the remote-pad ingress — and narrates
 * every observable transition as `[E2E] key=value` lines on stdout so the check
 * can assert ordering through the true stack.
 *
 *   --origin <url>     Party service origin (cloud arm: the loopback wrangler
 *                      origin, host token gate applies). Ignored by the LAN
 *                      transport and optional under --lan.
 *   --auto-approve     approve the first pending controller onto seat 1.
 *   --packets <n>      exit 0 once a controller has reached Connected and
 *                      <n> non-neutral pad packets crossed the ingress
 *                      (default 50).
 *   --timeout-ms <t>   exit 3 if that has not happened in time
 *                      (default 120000).
 *   --lan              use the embedded LanPartyTransport instead of the cloud
 *                      libdatachannel transport (no wrangler in the loop).
 *   --lan-web-root <d> directory holding the packaged controller assets the
 *                      embedded server serves (dist/web); required under --lan.
 *   --lan-host <h>     LAN address the invite URL advertises (the host the
 *                      browser navigates to, e.g. 192.168.1.5 or 127.0.0.1);
 *                      required under --lan.
 *   --close-room-after-connected
 *                      after Connected + <packets> non-neutral packets, close
 *                      the ROOM (server kept alive so a phone re-redeem learns
 *                      host_closed) instead of exiting, then run a short grace
 *                      window and exit 0. The LAN stop scenario.
 *
 * Exit codes: 0 success, 1 usage, 2 host error, 3 timeout.
 */

#include "party/libdatachannel_party_transport.h"
#include "party/lan_party_launch.h"
#include "party/lan_party_transport.h"
#include "party/native_party_host.h"
#include "party/native_remote_pad_ingress.h"
#include "party/party_protocol.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

namespace {

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char *hostPhaseName(MdkrNativePartyPhase phase) {
    switch (phase) {
        case MdkrNativePartyPhase::Closed: return "closed";
        case MdkrNativePartyPhase::Opening: return "opening";
        case MdkrNativePartyPhase::Open: return "open";
        case MdkrNativePartyPhase::InviteRevoked: return "invite_revoked";
        case MdkrNativePartyPhase::Recovering: return "recovering";
        case MdkrNativePartyPhase::Error: return "error";
        case MdkrNativePartyPhase::RoomEnded: return "room_ended";
    }
    return "unknown";
}

const char *controllerPhaseName(MdkrNativePartyControllerPhase phase) {
    switch (phase) {
        case MdkrNativePartyControllerPhase::Pending: return "pending";
        case MdkrNativePartyControllerPhase::Approved: return "approved";
        case MdkrNativePartyControllerPhase::Leased: return "leased";
        case MdkrNativePartyControllerPhase::Connected: return "connected";
    }
    return "unknown";
}

struct ControllerEcho {
    std::string phase;
    std::string phrase;
    unsigned seat = ~0u;
};

struct Options {
    std::string origin;
    bool autoApprove = false;
    uint64_t packets = 50u;
    uint64_t timeoutMs = 120000u;
    bool lan = false;
    std::string lanWebRoot;
    std::string lanHost;
    bool closeRoomAfterConnected = false;
};

bool parseOptions(int argc, char **argv, Options &options) {
    for (int index = 1; index < argc; index++) {
        const std::string argument = argv[index];
        const bool hasValue = index + 1 < argc;
        if (argument == "--origin" && hasValue) {
            options.origin = argv[++index];
        } else if (argument == "--auto-approve") {
            options.autoApprove = true;
        } else if (argument == "--packets" && hasValue) {
            options.packets = std::strtoull(argv[++index], nullptr, 10);
        } else if (argument == "--timeout-ms" && hasValue) {
            options.timeoutMs = std::strtoull(argv[++index], nullptr, 10);
        } else if (argument == "--lan") {
            options.lan = true;
        } else if (argument == "--lan-web-root" && hasValue) {
            options.lanWebRoot = argv[++index];
        } else if (argument == "--lan-host" && hasValue) {
            options.lanHost = argv[++index];
        } else if (argument == "--close-room-after-connected") {
            options.closeRoomAfterConnected = true;
        } else {
            std::fprintf(stderr,
                "usage: %s --origin <url> [--auto-approve] [--packets <n>] "
                "[--timeout-ms <t>] [--lan --lan-web-root <d> --lan-host <h>] "
                "[--close-room-after-connected]\n", argv[0]);
            return false;
        }
    }
    if (options.packets == 0u || options.timeoutMs == 0u) {
        std::fprintf(stderr, "--packets and --timeout-ms must be non-zero\n");
        return false;
    }
    if (options.lan) {
        /* The LAN transport is configured from these, not from --origin. */
        if (options.lanWebRoot.empty() || options.lanHost.empty()) {
            std::fprintf(stderr,
                "--lan requires --lan-web-root and --lan-host\n");
            return false;
        }
    } else if (options.origin.empty()) {
        std::fprintf(stderr, "--origin is required\n");
        return false;
    }
    return true;
}

/* Build the transport this driver runs the host over: the embedded
 * LanPartyTransport under --lan (the whole no-internet backend in-process), or
 * the cloud libdatachannel transport otherwise. Returns nullptr on a fatal
 * setup failure (a missing controller bundle), which main() reports as a host
 * error. */
std::unique_ptr<MdkrPartyTransport> makeTransport(const Options &options) {
    if (!options.lan) return mdkr_create_native_party_transport();
    MdkrLanPartyTransportConfig config;
    if (!mdkr_lan_party_build_manifest(options.lanWebRoot, config.manifest)) {
        std::fprintf(stderr,
            "[E2E] result=error message=controller assets missing under %s\n",
            options.lanWebRoot.c_str());
        return nullptr;
    }
    config.advertisedHost = options.lanHost;
    config.bindPort = 0u;  /* ephemeral: the server reports it into the URL. */
    return mdkr_create_lan_party_transport(std::move(config));
}

/* Every observable change becomes one stdout line the check script tails
 * live, so lines are flushed as they are produced. Phrases and messages may
 * contain spaces; they are always the final value on their line. */
void echoView(const MdkrNativePartyView &view, std::string &lastPhase,
              std::string &lastUrl, std::string &lastMessage,
              std::map<std::string, ControllerEcho> &echoes) {
    const std::string phase = hostPhaseName(view.phase);
    if (phase != lastPhase) {
        std::printf("[E2E] room phase=%s\n", phase.c_str());
        lastPhase = phase;
    }
    if (view.inviteVisible && !view.controllerUrl.empty() &&
        view.controllerUrl != lastUrl) {
        std::printf("[E2E] invite url=%s code=%s\n",
            view.controllerUrl.c_str(), view.fallbackCode.c_str());
        lastUrl = view.controllerUrl;
    }
    if (view.message != lastMessage) {
        std::printf("[E2E] message=%s\n", view.message.c_str());
        lastMessage = view.message;
    }
    for (const MdkrNativePartyController &controller : view.controllers) {
        ControllerEcho &echo = echoes[controller.id];
        const std::string controllerPhase =
            controllerPhaseName(controller.phase);
        if (echo.phase != controllerPhase || echo.seat != controller.seat) {
            std::printf("[E2E] controller=%s phase=%s seat=%u\n",
                controller.id.c_str(), controllerPhase.c_str(),
                controller.seat);
            echo.phase = controllerPhase;
            echo.seat = controller.seat;
        }
        if (!controller.pairingPhrase.empty() &&
            echo.phrase != controller.pairingPhrase) {
            std::printf("[E2E] controller=%s phrase=%s\n",
                controller.id.c_str(), controller.pairingPhrase.c_str());
            echo.phrase = controller.pairingPhrase;
        }
    }
    std::fflush(stdout);
}

void approveFirstPending(MdkrNativePartyHost &host) {
    for (const MdkrNativePartyController &controller :
         host.view().controllers) {
        /* SAS v2: a pending phone never has a phrase yet -- it arrives once
         * the approved phone's WebRTC descriptions are exchanged, and the
         * check script matches it against the page after Connected. */
        if (controller.phase != MdkrNativePartyControllerPhase::Pending ||
            controller.commandPending) {
            continue;
        }
        if (host.approve(controller.id, 1u)) {
            std::printf("[E2E] approve controller=%s seat=1\n",
                controller.id.c_str());
            std::fflush(stdout);
        }
        return;
    }
}

/* Engine-side of the crossing: drain every bound seat the way the SDL input
 * boundary does (tests/test_party_session_lifecycle.cpp drainSeat). The
 * owner is re-read per pass because the C1 self-heal rebinds a seat under a
 * fresh lease generation without any room transition. */
void pumpIngress(uint64_t &total, uint64_t &nonNeutral) {
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> buffer{};
    for (unsigned port = 0u; port < MDKR_NATIVE_REMOTE_PAD_PORTS; port++) {
        uint64_t owner = 0u;
        uint32_t connection = 0u;
        if (!mdkr_native_remote_pad_info(port, &owner, &connection)) continue;
        for (;;) {
            const size_t length = mdkr_native_remote_pad_pop(
                port, owner, connection, buffer.data(), buffer.size());
            if (length == 0u) break;
            MdkrPartyPadPacket decoded{};
            if (mdkr_party_pad_decode(buffer.data(), length, &decoded) !=
                MDKR_PARTY_DECODE_OK) {
                continue;
            }
            total++;
            if (decoded.buttons != 0u || decoded.stick_x != 0 ||
                decoded.stick_y != 0) {
                nonNeutral++;
            }
        }
    }
}

bool anyConnected(const MdkrNativePartyView &view) {
    for (const MdkrNativePartyController &controller : view.controllers) {
        if (controller.phase == MdkrNativePartyControllerPhase::Connected) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) return 1;

    std::unique_ptr<MdkrPartyTransport> transport = makeTransport(options);
    if (!transport) {
        std::fflush(stderr);
        return 2;
    }
    MdkrNativePartyHost host(*transport);
    std::printf("[E2E] origin=%s\n",
        options.lan ? ("lan/" + options.lanHost).c_str() : options.origin.c_str());
    std::fflush(stdout);
    if (!host.open(options.origin)) {
        std::printf("[E2E] result=error message=%s\n",
            host.view().message.c_str());
        std::fflush(stdout);
        return 2;
    }

    const uint64_t startedMs = nowMs();
    std::string lastPhase;
    std::string lastUrl;
    std::string lastMessage;
    std::map<std::string, ControllerEcho> echoes;
    uint64_t totalPackets = 0u;
    uint64_t nonNeutral = 0u;
    uint64_t echoedNonNeutral = 0u;
    bool sawConnected = false;
    /* --close-room-after-connected: the LAN stop scenario. Once the room is
     * closed the server stays up (transport->closeRoom() is room-close-only,
     * unlike host.closeRoom() which also tears the server down), so a phone's
     * re-redeem after the 4000 close learns host_closed. We then run a short
     * grace window before exiting 0 so the check can observe that on the page. */
    constexpr uint64_t kCloseGraceMs = 8000u;
    bool roomClosed = false;
    uint64_t roomClosedAtMs = 0u;

    while (nowMs() - startedMs < options.timeoutMs) {
        host.service(nowMs());
        echoView(host.view(), lastPhase, lastUrl, lastMessage, echoes);
        if (host.view().phase == MdkrNativePartyPhase::Error) {
            std::printf("[E2E] result=error message=%s\n",
                host.view().message.c_str());
            std::fflush(stdout);
            return 2;
        }
        if (options.autoApprove) approveFirstPending(host);
        pumpIngress(totalPackets, nonNeutral);
        if (nonNeutral >= echoedNonNeutral + 10u ||
            (nonNeutral >= options.packets &&
             nonNeutral != echoedNonNeutral)) {
            std::printf("[E2E] nonneutral=%llu packets=%llu\n",
                static_cast<unsigned long long>(nonNeutral),
                static_cast<unsigned long long>(totalPackets));
            std::fflush(stdout);
            echoedNonNeutral = nonNeutral;
        }
        sawConnected = sawConnected || anyConnected(host.view());
        /* Stop scenario: close the room once input has demonstrably flowed,
         * keeping the server alive so the page's re-redeem surfaces
         * host_closed, then fall through the grace window to a clean exit. */
        if (options.closeRoomAfterConnected) {
            if (!roomClosed && sawConnected && anyConnected(host.view()) &&
                nonNeutral >= options.packets) {
                (void)transport->closeRoom();
                roomClosed = true;
                roomClosedAtMs = nowMs();
                std::printf("[E2E] room_closed nonneutral=%llu packets=%llu\n",
                    static_cast<unsigned long long>(nonNeutral),
                    static_cast<unsigned long long>(totalPackets));
                std::fflush(stdout);
            }
            if (roomClosed && nowMs() - roomClosedAtMs >= kCloseGraceMs) {
                std::printf("[E2E] result=ok nonneutral=%llu packets=%llu\n",
                    static_cast<unsigned long long>(nonNeutral),
                    static_cast<unsigned long long>(totalPackets));
                std::fflush(stdout);
                host.closeRoom();
                mdkr_native_remote_pad_reset_all();
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (sawConnected && anyConnected(host.view()) &&
            nonNeutral >= options.packets) {
            std::printf("[E2E] result=ok nonneutral=%llu packets=%llu\n",
                static_cast<unsigned long long>(nonNeutral),
                static_cast<unsigned long long>(totalPackets));
            std::fflush(stdout);
            /* M4 real-transport guard: the goodbye's flush is capped at
             * 250 ms (kMdkrPartyCloseFlushDeadlineMs); the rest of this
             * call is libdatachannel's own socket/peer close calls. Quit
             * hanging here is exactly the defect the bound exists for, so
             * a full second is already an architecture failure, never a
             * slow network. */
            const uint64_t closeStartedMs = nowMs();
            host.closeRoom();
            const uint64_t closeTookMs = nowMs() - closeStartedMs;
            std::printf("[E2E] close_ms=%llu\n",
                static_cast<unsigned long long>(closeTookMs));
            std::fflush(stdout);
            mdkr_native_remote_pad_reset_all();
            if (closeTookMs > 1000u) {
                std::printf("[E2E] result=close_blocked ms=%llu\n",
                    static_cast<unsigned long long>(closeTookMs));
                std::fflush(stdout);
                return 4;
            }
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::printf("[E2E] result=timeout nonneutral=%llu packets=%llu\n",
        static_cast<unsigned long long>(nonNeutral),
        static_cast<unsigned long long>(totalPackets));
    std::fflush(stdout);
    host.closeRoom();
    mdkr_native_remote_pad_reset_all();
    return 3;
}
