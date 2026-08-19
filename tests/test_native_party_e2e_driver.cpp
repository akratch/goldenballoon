/*
 * End-to-end Phone Party host driver.
 *
 * Owned by tests/check_party_native_e2e.py and deliberately NOT a ctest: it
 * opens the REAL libdatachannel transport against a live local Party Worker
 * (wrangler dev) whose lifetime, port and paired controller page only the
 * check script provides. It drives MdkrNativePartyHost exactly like the
 * launcher does — open, service in a loop, approve, drain the remote-pad
 * ingress — and narrates every observable transition as `[E2E] key=value`
 * lines on stdout so the check can assert ordering through the true stack.
 *
 *   --origin <url>     Party service origin (the check passes the loopback
 *                      wrangler origin; the host's token gate applies).
 *   --auto-approve     approve the first pending controller onto seat 1.
 *   --packets <n>      exit 0 once a controller has reached Connected and
 *                      <n> non-neutral pad packets crossed the ingress
 *                      (default 50).
 *   --timeout-ms <t>   exit 3 if that has not happened in time
 *                      (default 120000).
 *
 * Exit codes: 0 success, 1 usage, 2 host error, 3 timeout.
 */

#include "party/libdatachannel_party_transport.h"
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
        } else {
            std::fprintf(stderr,
                "usage: %s --origin <url> [--auto-approve] [--packets <n>] "
                "[--timeout-ms <t>]\n", argv[0]);
            return false;
        }
    }
    if (options.origin.empty() || options.packets == 0u ||
        options.timeoutMs == 0u) {
        std::fprintf(stderr, "--origin is required\n");
        return false;
    }
    return true;
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
        if (controller.phase != MdkrNativePartyControllerPhase::Pending ||
            controller.commandPending || controller.pairingPhrase.empty()) {
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

    std::unique_ptr<MdkrPartyTransport> transport =
        mdkr_create_native_party_transport();
    MdkrNativePartyHost host(*transport);
    std::printf("[E2E] origin=%s\n", options.origin.c_str());
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
        if (sawConnected && anyConnected(host.view()) &&
            nonNeutral >= options.packets) {
            std::printf("[E2E] result=ok nonneutral=%llu packets=%llu\n",
                static_cast<unsigned long long>(nonNeutral),
                static_cast<unsigned long long>(totalPackets));
            std::fflush(stdout);
            host.closeRoom();
            mdkr_native_remote_pad_reset_all();
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
