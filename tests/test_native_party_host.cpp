#include "party/native_party_host.h"
#include "party/native_remote_pad_ingress.h"
#include "party/party_protocol.h"

/* Assert-driven test: NDEBUG would compile every check away (see
 * test_native_remote_pad_ingress.cpp). */
#undef NDEBUG

#include <array>
#include <cassert>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public MdkrPartyTransport {
public:
    bool availableValue = true;
    bool commandResult = true;
    std::string reason;
    std::deque<MdkrPartyTransportEvent> events;
    std::vector<std::string> calls;

    bool available() const override { return availableValue; }
    const char *unavailableReason() const override { return reason.c_str(); }
    bool open(const std::string &origin) override {
        calls.push_back("open:" + origin); return commandResult;
    }
    bool approve(const std::string &id, unsigned seat) override {
        calls.push_back("approve:" + id + ":" + std::to_string(seat));
        return commandResult;
    }
    bool reject(const std::string &id) override {
        calls.push_back("reject:" + id); return commandResult;
    }
    bool remove(const std::string &id) override {
        calls.push_back("remove:" + id); return commandResult;
    }
    bool rotateInvite(unsigned generation) override {
        calls.push_back("rotate:" + std::to_string(generation));
        return commandResult;
    }
    bool revokeInvite() override {
        calls.push_back("revoke"); return commandResult;
    }
    bool closeRoom() override {
        calls.push_back("close"); return commandResult;
    }
    bool sendRumble(const std::string &id, uint16_t strength) override {
        calls.push_back("rumble:" + id + ":" + std::to_string(strength));
        return commandResult;
    }
    bool poll(MdkrPartyTransportEvent &event) override {
        if (events.empty()) return false;
        event = std::move(events.front());
        events.pop_front();
        return true;
    }
    void shutdown() override { calls.push_back("shutdown"); }
};

MdkrNativePartyController pending(std::string id) {
    MdkrNativePartyController value;
    value.id = std::move(id);
    value.name = "A friend's phone";
    value.publicKey = std::string(87u, 'C');
    value.pairingPhrase = "amber comet";
    /* The service allocates the first signaling generation at redemption. */
    value.connectionSequence = 1u;
    return value;
}

MdkrNativePartyController approved(
    std::string id, unsigned seat, uint32_t lease, uint32_t connection) {
    MdkrNativePartyController value = pending(std::move(id));
    value.phase = MdkrNativePartyControllerPhase::Leased;
    value.seat = seat;
    value.leaseGeneration = lease;
    value.connectionSequence = connection;
    return value;
}

MdkrPartyTransportEvent roomEvent(
    uint64_t transition, unsigned generation, uint64_t expiresInMs,
    std::vector<MdkrNativePartyController> controllers = {}) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::RoomState;
    event.room.transitionId = transition;
    event.room.inviteGeneration = generation;
    event.room.inviteExpiresInMs = expiresInMs;
    event.room.controllerUrl = "https://party.example/controller/#secret";
    event.room.fallbackCode = "123456";
    event.room.inviteActive = true;
    event.room.controllers = std::move(controllers);
    return event;
}

std::vector<uint8_t> padPacket(uint32_t connection, uint32_t sequence) {
    MdkrPartyPadPacket source{};
    source.flags = MDKR_PARTY_PAD_FLAG_PRESENT;
    source.connection_sequence = connection;
    source.sample_sequence = sequence;
    source.sender_time_ms = sequence;
    source.buttons = 0x8000u;
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    size_t length = 0u;
    assert(mdkr_party_pad_encode(
        &source, output.data(), output.size(), &length));
    return std::vector<uint8_t>(output.begin(), output.begin() + length);
}

void unavailableAndSecureOrigin() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    transport.availableValue = false;
    transport.reason = "This package has no native WebRTC adapter.";
    MdkrNativePartyHost host(transport);
    assert(!host.open("https://party.example"));
    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(host.view().message == transport.reason);

    transport.availableValue = true;
    assert(!host.open("http://party.example"));
    assert(host.view().phase == MdkrNativePartyPhase::Error);
}

void lifecycleAndCustody() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    assert(host.view().phase == MdkrNativePartyPhase::Opening);
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {pending("phone-a")}));
    host.service(1000u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().inviteVisible);
    assert(host.approve("phone-a", 2u));
    assert(!host.approve("phone-a", 3u));

    auto phone = approved("phone-a", 2u, 4u, 9u);
    transport.events.push_back(roomEvent(2u, 1u, 121000u, {phone}));
    host.service(1001u);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(1u, &owner, &connection));
    assert(owner == ((4u << 3u) | 2u) && connection == 9u);

    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    MdkrPartyTransportEvent packet;
    packet.type = MdkrPartyTransportEventType::ControllerPacket;
    packet.controllerId = "phone-a";
    packet.packet = padPacket(9u, 1u);
    transport.events.push_back(packet);
    host.service(1002u);
    assert(host.view().controllers[0].direct);
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    assert(mdkr_native_remote_pad_pop(
        1u, owner, connection, output.data(), output.size()) > 0u);

    assert(mdkr_native_remote_pad_request_rumble(1u, 1234u));
    host.service(1003u);
    assert(transport.calls.back() == "rumble:phone-a:1234");

    MdkrPartyTransportEvent disconnected;
    disconnected.type = MdkrPartyTransportEventType::ControllerDisconnected;
    disconnected.controllerId = "phone-a";
    transport.events.push_back(disconnected);
    host.service(1004u);
    assert(!host.view().controllers[0].direct);
    assert(mdkr_native_remote_pad_info(1u, &owner, &connection));

    assert(host.dismissInvite());
    auto revoked = roomEvent(3u, 1u, 0u, {phone});
    revoked.room.inviteActive = false;
    revoked.room.controllerUrl.clear();
    revoked.room.fallbackCode.clear();
    transport.events.push_back(std::move(revoked));
    host.service(1005u);
    assert(host.view().phase == MdkrNativePartyPhase::InviteRevoked);
    assert(!host.view().inviteVisible);
    assert(mdkr_native_remote_pad_info(1u, &owner, &connection));

    assert(host.closeRoom());
    assert(host.view().phase == MdkrNativePartyPhase::Closed);
    assert(!mdkr_native_remote_pad_info(1u, &owner, &connection));
}

void invalidAndStaleUpdatesFailClosed() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(2u, 2u, 5000u));
    host.service(1u);
    assert(host.view().transitionId == 2u);
    transport.events.push_back(roomEvent(1u, 1u, 5000u));
    host.service(2u);
    assert(host.view().transitionId == 2u);

    auto first = approved("one", 1u, 1u, 1u);
    auto second = approved("two", 1u, 2u, 2u);
    transport.events.push_back(roomEvent(3u, 2u, 5000u, {first, second}));
    host.service(3u);
    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(!host.view().inviteVisible);
}

void expiryPreservesApprovedSeat() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    /* Relative expiresInMs=9 latched against nowMs=1 (host's own clock)
     * lands the deadline at exactly 10, matching the original absolute
     * fixture value this test was written against. */
    transport.events.push_back(roomEvent(
        1u, 1u, 9u, {approved("phone", 4u, 9u, 12u)}));
    host.service(1u);
    host.service(10u);
    assert(host.view().phase == MdkrNativePartyPhase::InviteRevoked);
    assert(host.view().controllers.size() == 1u);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(3u, &owner, &connection));
    assert(host.rotateInvite());
    assert(transport.calls.back() == "rotate:1");
}

void commandRejectionAndRemovalStayRecoverable() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(1u, 1u, 5000u, {pending("phone")}));
    host.service(1u);
    assert(host.approve("phone", 1u));
    MdkrPartyTransportEvent rejected;
    rejected.type = MdkrPartyTransportEventType::CommandRejected;
    rejected.message = "That controller slot was just taken. Choose another.";
    transport.events.push_back(rejected);
    host.service(2u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(!host.view().controllers[0].commandPending);
    assert(host.view().inviteVisible);

    transport.events.push_back(roomEvent(
        2u, 1u, 5000u, {approved("phone", 1u, 2u, 1u)}));
    host.service(3u);
    assert(host.reject("phone"));
    assert(transport.calls.back() == "remove:phone");
}

/* C3 give-up review fix: a CommandRejected-shaped event that carries a
 * controller identity (the connect_timeout give-up path) must only clear
 * that one controller's commandPending. Before this fix, any CommandRejected
 * -- including this new autonomous 20-60 s timeout ladder, which can land at
 * any time, not just reactively right after a host-issued command -- cleared
 * every controller's commandPending, silently unblocking an unrelated
 * controller's genuinely in-flight command. An event with no controller
 * identity (a true host-command rejection) keeps the old room-wide
 * behavior. */
void giveUpClearsOnlyItsOwnControllersCommandPending() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 5000u, {pending("phone-a"), pending("phone-b")}));
    host.service(1u);
    assert(host.approve("phone-a", 1u));
    assert(host.approve("phone-b", 2u));
    assert(host.view().controllers[0].commandPending);
    assert(host.view().controllers[1].commandPending);

    /* A give-up event scoped to phone-b only, with no intervening RoomState
     * (which would otherwise reset every commandPending on its own). */
    MdkrPartyTransportEvent timedOut;
    timedOut.type = MdkrPartyTransportEventType::CommandRejected;
    timedOut.controllerId = "phone-b";
    timedOut.message = "This phone could not connect. Remove it and pair again.";
    transport.events.push_back(timedOut);
    host.service(2u);

    assert(host.view().controllers[0].commandPending);   // phone-a: untouched
    assert(!host.view().controllers[1].commandPending);  // phone-b: its own
    assert(host.view().message ==
        "This phone could not connect. Remove it and pair again.");
}

/* I3: the worker's typed command errors reach the player honestly, per
 * controller. host_command_result{ok:false,error:"<code>"} arrives as a
 * CommandRejected carrying the code verbatim in errorCode; the host maps
 * the known codes to their exact copy and -- when the event names a
 * controller -- clears only that controller's commandPending, leaving an
 * unrelated controller's genuinely in-flight command pending. This is the
 * give-up scoping generalized to every command rejection that names a
 * controller, not just connect_timeout. An unknown code keeps the existing
 * generic copy exactly as before. */
void typedCommandErrorSurfacesHonestCopyPerController() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 5000u, {pending("phone-a"), pending("phone-b")}));
    host.service(1u);
    assert(host.approve("phone-a", 1u));
    assert(host.approve("phone-b", 2u));
    assert(host.view().controllers[0].commandPending);
    assert(host.view().controllers[1].commandPending);

    /* The real transport shape: the generic prose is still present on the
     * event, but the typed code must win the copy decision. */
    MdkrPartyTransportEvent budget;
    budget.type = MdkrPartyTransportEventType::CommandRejected;
    budget.controllerId = "phone-b";
    budget.errorCode = "service_budget_safe";
    budget.message = "That controller action did not complete. Try again.";
    transport.events.push_back(budget);
    host.service(2u);

    /* The behavioral half: phone-a's own in-flight approve stays pending;
     * only phone-b's command was rejected. */
    assert(host.view().controllers[0].commandPending);
    assert(!host.view().controllers[1].commandPending);
    assert(host.view().message ==
        "The controller service has reached today's limit. "
        "Try again after midnight UTC.");

    MdkrPartyTransportEvent rotated;
    rotated.type = MdkrPartyTransportEventType::CommandRejected;
    rotated.errorCode = "invite_rotated";
    transport.events.push_back(rotated);
    host.service(3u);
    assert(host.view().message ==
        "That invite was replaced. Use the newest code.");

    MdkrPartyTransportEvent full;
    full.type = MdkrPartyTransportEventType::CommandRejected;
    full.controllerId = "phone-b";
    full.errorCode = "room_full";
    transport.events.push_back(full);
    host.service(4u);
    assert(host.view().message == "No free phone slot.");

    /* Unknown code: the existing copy, exactly as before this task. */
    MdkrPartyTransportEvent unknown;
    unknown.type = MdkrPartyTransportEventType::CommandRejected;
    unknown.errorCode = "invalid_state";
    transport.events.push_back(unknown);
    host.service(5u);
    assert(host.view().message ==
        "That controller action did not complete. Try again.");
}

/* I5 sweep: a room_state controller entry with no seat assigned yet (the
 * wire's "seat" key absent entirely, not merely null -- a phone that has
 * paired but has not been approved to a seat) must reach the host as an
 * ordinary no-seat pending controller and never crash the launcher.
 *
 * The actual undefined-behaviour seam this guards (nlohmann's const
 * operator[] on a JSON object missing the key, guarded in
 * libdatachannel_party_transport.cpp's parseRoom with a contains() check)
 * lives entirely in JSON parsing that this host-only test binary never
 * touches -- mdkr_native_party_host_test links native_party_host.cpp and
 * native_remote_pad_ingress.cpp, not the transport, exactly like every
 * other test in this file. This is the downstream contract this file CAN
 * prove: once the transport hands the host a controller with no seat
 * (seat=0, phase=Pending -- what the fixed parser produces for a missing
 * key), the host applies it cleanly, keeps servicing it without incident,
 * and can still approve it onto a seat later. */
void seatlessRoomEntryAppliedAsNoSeatPendingWithoutCrash() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {pending("phone-a")}));
    host.service(1000u);

    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().controllers.size() == 1u);
    assert(host.view().controllers[0].phase == MdkrNativePartyControllerPhase::Pending);
    assert(host.view().controllers[0].seat == 0u);

    /* Servicing again, as the launcher does every frame, must stay stable:
     * no seat means no owner derived, no ingress bind attempted, no crash. */
    host.service(1001u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().controllers[0].seat == 0u);

    /* Still a live, well-formed pending controller: approvable once a seat
     * is chosen, proving it was accepted rather than silently malformed. */
    assert(host.approve("phone-a", 3u));
}

/* I1 fix: the transport reports invite expiry as a RELATIVE duration
 * (inviteExpiresInMs, computed at parse time); the host must latch
 * nowMs + expiresInMs in its OWN service clock at the event-application
 * site (applyRoomState). Before this fix the host copied whatever number
 * the transport sent as if it were already an absolute instant in the
 * host's own clock -- correct only by coincidence when both clocks happen
 * to start near zero together. Real launches never satisfy that: the
 * transport's std::chrono::steady_clock runs since boot, the host's
 * SDL_GetTicks64 since SDL init. Simulate that gap with nowMs starting
 * near uptime scale (999,999,999 ms) while the invite is a fresh 2-minute
 * window. */
void expiryLatchesInHostsOwnClockDomain() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));

    constexpr uint64_t kHostNowMs = 999999999u;  // uptime far exceeds session runtime
    constexpr uint64_t kExpiresInMs = 120000u;   // the transport's relative report
    transport.events.push_back(roomEvent(1u, 1u, kExpiresInMs, {}));
    host.service(kHostNowMs);

    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().inviteVisible);
    /* The deadline must live in the HOST's own clock domain: nowMs plus the
     * relative duration, not the raw 120000 the transport reported (which,
     * compared directly against a nowMs of ~10^9, would have looked expired
     * for eleven straight days under the pre-fix cross-domain compare). */
    assert(host.view().inviteExpiresAtMs == kHostNowMs + kExpiresInMs);

    /* ui_phone_party.cpp:184-185's countdown formula, asserted on host state
     * rather than the drawn ImGui text (no headless render seam here):
     * ceil((expiry - now) / 1000) seconds must read as 2:00. */
    const uint64_t secondsLeft =
        (host.view().inviteExpiresAtMs - kHostNowMs + 999u) / 1000u;
    assert(secondsLeft == 120u);
    assert(secondsLeft / 60u == 2u && secondsLeft % 60u == 0u);

    /* Nowhere near the latched deadline yet: still visible. */
    host.service(kHostNowMs + 1u);
    assert(host.view().inviteVisible);

    /* Exactly at the latched deadline in the HOST's clock -- proving the
     * deadline really tracks nowMs's domain, not the raw 120000. */
    host.service(kHostNowMs + kExpiresInMs);
    assert(!host.view().inviteVisible);
    assert(host.view().phase == MdkrNativePartyPhase::InviteRevoked);
}

/* Review fix: setError() releases every seat and shuts the transport down,
 * but before this fix left a controller's needsRebind flag standing. If a
 * push failure (queue overflow revokes ingress custody, same shape as the
 * stall-recovery coverage in test_party_session_lifecycle.cpp) and a
 * terminal Error land in the same drain cycle, service()'s top-of-loop heal
 * on the very next tick re-bound the already-released seat, flipped the
 * controller back to Connected and overwrote the terminal message -- all
 * inside a room the host had already declared Error. Prove the controller
 * stays put, the room stays in Error, and the error message is not
 * clobbered. */
void terminalErrorAfterPushFailureStaysErrorAndNeverRebinds() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(1u, 1u, 121000u, {pending("phone-a")}));
    host.service(1000u);
    assert(host.approve("phone-a", 1u));

    auto phone = approved("phone-a", 1u, 4u, 9u);
    transport.events.push_back(roomEvent(2u, 1u, 121000u, {phone}));
    host.service(1001u);

    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    host.service(1002u);
    assert(host.view().controllers[0].direct);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Connected);

    /* Flood the bounded ingress queue with live packets for this same
     * controller -- the same overflow shape
     * stall_with_live_packets_recovers_input() in
     * test_party_session_lifecycle.cpp drives -- so that, part-way through,
     * a push fails and the host marks the controller needsRebind. Queue a
     * terminal Error right behind it: both land in the ONE drain cycle
     * below. */
    const uint32_t flood = MDKR_NATIVE_REMOTE_PAD_QUEUE_CAPACITY + 8u;
    uint32_t sequence = 1u;
    for (uint32_t index = 0u; index < flood; ++index) {
        MdkrPartyTransportEvent packet;
        packet.type = MdkrPartyTransportEventType::ControllerPacket;
        packet.controllerId = "phone-a";
        packet.packet = padPacket(9u, ++sequence);
        transport.events.push_back(packet);
    }
    MdkrPartyTransportEvent fatal;
    fatal.type = MdkrPartyTransportEventType::Error;
    fatal.message =
        "Phone controllers are unavailable. Local controllers still work.";
    transport.events.push_back(fatal);
    host.service(2000u);

    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(host.view().message == fatal.message);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);

    /* The next tick, with no new events at all, is exactly where the bug
     * lived: the top-of-service heal loop must not resurrect the released
     * seat inside an Error room. */
    host.service(2100u);
    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(host.view().message == fatal.message);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);
    assert(!host.view().controllers[0].needsRebind);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(!mdkr_native_remote_pad_info(0u, &owner, &connection));
}

}  // namespace

int main() {
    unavailableAndSecureOrigin();
    lifecycleAndCustody();
    invalidAndStaleUpdatesFailClosed();
    expiryPreservesApprovedSeat();
    commandRejectionAndRemovalStayRecoverable();
    giveUpClearsOnlyItsOwnControllersCommandPending();
    typedCommandErrorSurfacesHonestCopyPerController();
    seatlessRoomEntryAppliedAsNoSeatPendingWithoutCrash();
    expiryLatchesInHostsOwnClockDomain();
    terminalErrorAfterPushFailureStaysErrorAndNeverRebinds();
    mdkr_native_remote_pad_reset_all();
    return 0;
}
