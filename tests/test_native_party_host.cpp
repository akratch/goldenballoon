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
    uint64_t transition, unsigned generation, uint64_t expires,
    std::vector<MdkrNativePartyController> controllers = {}) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::RoomState;
    event.room.transitionId = transition;
    event.room.inviteGeneration = generation;
    event.room.inviteExpiresAtMs = expires;
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
    transport.events.push_back(roomEvent(
        1u, 1u, 10u, {approved("phone", 4u, 9u, 12u)}));
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

}  // namespace

int main() {
    unavailableAndSecureOrigin();
    lifecycleAndCustody();
    invalidAndStaleUpdatesFailClosed();
    expiryPreservesApprovedSeat();
    commandRejectionAndRemovalStayRecoverable();
    mdkr_native_remote_pad_reset_all();
    return 0;
}
