/*
 * Firewall-negative observation, native half.
 *
 * A firewall does not produce a tidy protocol error. It refuses the socket, or
 * lets the TCP connect through and then kills the WSS upgrade, or drops an
 * established connection with no close frame. This gate drives
 * MdkrNativePartyHost with a transport that fails each of those ways and
 * asserts the documented fail-neutral contract:
 *
 *   - no crash and no hang: every path is bounded, service() drains at most
 *     its per-call event bound and the host never retries on its own,
 *   - the seat and local play are unaffected: no pad binding is created by a
 *     refused open, and the other three ports are never touched,
 *   - the pad goes neutral rather than stale: after a drop, custody is
 *     released or input is refused, and no packet from the dead epoch reaches
 *     the engine,
 *   - the user-visible message is the documented one, verbatim.
 */
#include "party/native_party_host.h"
#include "party/native_remote_pad_ingress.h"
#include "party/party_protocol.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <array>
#include <cassert>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

/* The three shapes a blocked network actually produces at this seam. */
enum class Block {
    None,
    /* connect() refused: the transport cannot even start its handshake. */
    SocketRefused,
    /* TCP connected, then the WSS upgrade was dropped: open() succeeds and the
     * failure arrives asynchronously as a transport Error event. */
    UpgradeRefused,
};

class BlockedTransport final : public MdkrPartyTransport {
public:
    Block block = Block::None;
    bool availableValue = true;
    std::string reason;
    std::deque<MdkrPartyTransportEvent> events;
    std::vector<std::string> calls;

    bool available() const override { return availableValue; }
    const char *unavailableReason() const override { return reason.c_str(); }
    bool open(const std::string &origin) override {
        calls.push_back("open:" + origin);
        return block != Block::SocketRefused;
    }
    bool approve(const std::string &id, unsigned seat) override {
        calls.push_back("approve:" + id + ":" + std::to_string(seat));
        return block == Block::None;
    }
    bool reject(const std::string &id) override {
        calls.push_back("reject:" + id);
        return block == Block::None;
    }
    bool remove(const std::string &id) override {
        calls.push_back("remove:" + id);
        return block == Block::None;
    }
    bool rotateInvite(unsigned generation) override {
        calls.push_back("rotate:" + std::to_string(generation));
        return block == Block::None;
    }
    bool revokeInvite() override {
        calls.push_back("revoke");
        return block == Block::None;
    }
    bool closeRoom() override {
        calls.push_back("close");
        return block == Block::None;
    }
    bool sendRumble(const std::string &id, uint16_t strength) override {
        calls.push_back("rumble:" + id + ":" + std::to_string(strength));
        return block == Block::None;
    }
    bool poll(MdkrPartyTransportEvent &event) override {
        if (events.empty()) return false;
        event = std::move(events.front());
        events.pop_front();
        return true;
    }
    void shutdown() override { calls.push_back("shutdown"); }
};

MdkrNativePartyController approved(
    std::string id, unsigned seat, uint32_t lease, uint32_t connection) {
    MdkrNativePartyController value;
    value.id = std::move(id);
    value.name = "A friend's phone";
    value.publicKey = std::string(87u, 'C');
    /* SAS v2: room updates never carry a phrase; it arrives only as a
     * ControllerPhrase event after the WebRTC descriptions are set, and
     * nothing in this hostile-input battery depends on one existing. */
    value.phase = MdkrNativePartyControllerPhase::Leased;
    value.seat = seat;
    value.leaseGeneration = lease;
    value.connectionSequence = connection;
    return value;
}

MdkrPartyTransportEvent roomEvent(
    uint64_t transition, unsigned generation, uint64_t expires,
    std::vector<MdkrNativePartyController> controllers) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::RoomState;
    event.room.transitionId = transition;
    event.room.inviteGeneration = generation;
    event.room.inviteExpiresInMs = expires;
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

void queuePacket(
    BlockedTransport &transport, const std::string &id, uint32_t connection,
    uint32_t sequence) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::ControllerPacket;
    event.controllerId = id;
    event.packet = padPacket(connection, sequence);
    transport.events.push_back(std::move(event));
}

size_t drainSeat(unsigned port, uint64_t owner, uint32_t connection) {
    size_t delivered = 0u;
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> buffer{};
    while (mdkr_native_remote_pad_pop(
               port, owner, connection, buffer.data(), buffer.size()) > 0u) {
        delivered++;
    }
    return delivered;
}

/* Local play means the other three seats, and this one before it was leased,
 * were never touched by the network failure. */
void assertNoSeatCustodyAnywhere() {
    for (unsigned port = 0u; port < MDKR_NATIVE_REMOTE_PAD_PORTS; ++port) {
        uint64_t owner = 0u;
        uint32_t connection = 0u;
        assert(!mdkr_native_remote_pad_info(port, &owner, &connection));
        MdkrNativeRemotePadIngressStats stats{};
        mdkr_native_remote_pad_stats(port, &stats);
        assert(stats.binds == 0u);
        assert(stats.packets == 0u);
        assert(stats.overflows == 0u);
    }
}

constexpr unsigned kSeat = 3u;
constexpr unsigned kPort = kSeat - 1u;
constexpr uint32_t kLease = 6u;
constexpr uint32_t kConnection = 11u;
constexpr uint64_t kOwner = (static_cast<uint64_t>(kLease) << 3u) | kSeat;

/*
 * A refused socket. open() returns false, the player is told the documented
 * sentence, nothing is left running, and no seat was ever taken from local
 * play. Retrying is the player's choice: the host must not spin on its own.
 */
void refusedSocketFailsNeutralAndBoundsRetries() {
    mdkr_native_remote_pad_reset_all();
    BlockedTransport transport;
    transport.block = Block::SocketRefused;
    MdkrNativePartyHost host(transport);

    for (int attempt = 0; attempt < 5; ++attempt) {
        assert(!host.open("https://party.example"));
        assert(host.view().phase == MdkrNativePartyPhase::Error);
        assert(host.view().message ==
               "Could not open Phone Party. Local controllers still work.");
        assert(!host.view().busy);
        assert(!host.view().inviteVisible);
        assert(host.view().controllerUrl.empty());
        assert(host.view().fallbackCode.empty());
        assert(host.view().controllers.empty());
        assertNoSeatCustodyAnywhere();

        /* The host never retries behind the player's back: servicing a failed
         * room issues no transport traffic at all. */
        const size_t callsBefore = transport.calls.size();
        for (int tick = 0; tick < 32; ++tick) {
            host.service(1000u + static_cast<uint64_t>(tick));
        }
        assert(transport.calls.size() == callsBefore);
    }

    /* Exactly one open attempt and one shutdown per player-initiated retry. */
    size_t opens = 0u;
    size_t shutdowns = 0u;
    for (const std::string &call : transport.calls) {
        if (call.rfind("open:", 0u) == 0u) opens++;
        if (call == "shutdown") shutdowns++;
    }
    assert(opens == 5u && shutdowns == 5u);
}

/*
 * A refused WSS upgrade. The TCP connect succeeded so open() returned true,
 * and the failure arrives as the transport's Error event. The room must fail
 * closed with the transport's own player-facing sentence.
 */
void refusedUpgradeReportsTheTransportSentence() {
    mdkr_native_remote_pad_reset_all();
    BlockedTransport transport;
    transport.block = Block::UpgradeRefused;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    assert(host.view().phase == MdkrNativePartyPhase::Opening);

    MdkrPartyTransportEvent failure;
    failure.type = MdkrPartyTransportEventType::Error;
    /* The libdatachannel transport's own non-recoverable sentence. */
    failure.message = "Phone controller room closed before it was ready.";
    transport.events.push_back(failure);
    host.service(1000u);

    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(host.view().message ==
           "Phone controller room closed before it was ready.");
    assert(!host.view().inviteVisible);
    assertNoSeatCustodyAnywhere();
    assert(transport.calls.back() == "shutdown");
}

/*
 * A transport that fails without saying anything useful — the common shape
 * when a middlebox simply drops the connection. The host must still never
 * print an empty or unprintable string at the player.
 */
void silentDropUsesTheDocumentedFallbackSentence() {
    mdkr_native_remote_pad_reset_all();
    BlockedTransport transport;
    transport.block = Block::UpgradeRefused;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));

    MdkrPartyTransportEvent failure;
    failure.type = MdkrPartyTransportEventType::Error;
    failure.message.clear();
    transport.events.push_back(failure);
    host.service(1000u);
    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(host.view().message ==
           "Phone controllers are unavailable. Local controllers still work.");

    /* A control character is not player-facing copy either. */
    mdkr_native_remote_pad_reset_all();
    BlockedTransport second;
    second.block = Block::UpgradeRefused;
    MdkrNativePartyHost other(second);
    assert(other.open("https://party.example"));
    MdkrPartyTransportEvent garbled;
    garbled.type = MdkrPartyTransportEventType::Error;
    garbled.message = std::string("connect\x01reset");
    second.events.push_back(garbled);
    other.service(1000u);
    assert(other.view().message ==
           "Phone controllers are unavailable. Local controllers still work.");
}

/*
 * The firewall closes an already-playing room. The approved phone keeps its
 * seat while the room socket retries (the documented limited-connection
 * meaning), but the moment the direct path is reported down the pad must go
 * neutral: no packet from the dead epoch may reach the engine.
 */
void midSessionDropGoesNeutralNotStale() {
    mdkr_native_remote_pad_reset_all();
    BlockedTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 600000u, {approved("phone-a", kSeat, kLease, kConnection)}));
    host.service(1000u);
    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    queuePacket(transport, "phone-a", kConnection, 1u);
    host.service(1001u);
    assert(host.view().controllers[0].direct);
    assert(drainSeat(kPort, kOwner, kConnection) == 1u);

    /* Room socket retrying: seat preserved, and the player is told. */
    transport.block = Block::UpgradeRefused;
    MdkrPartyTransportEvent recovering;
    recovering.type = MdkrPartyTransportEventType::Recovering;
    transport.events.push_back(recovering);
    host.service(1002u);
    assert(host.view().phase == MdkrNativePartyPhase::Recovering);
    assert(host.view().message == "Reconnecting the controller room…");
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(kPort, &owner, &connection));
    assert(owner == kOwner && connection == kConnection);

    /* Direct path down: neutral, and stale samples are refused, not queued. */
    MdkrPartyTransportEvent dropped;
    dropped.type = MdkrPartyTransportEventType::ControllerDisconnected;
    dropped.controllerId = "phone-a";
    transport.events.push_back(dropped);
    for (uint32_t sequence = 2u; sequence <= 6u; ++sequence) {
        queuePacket(transport, "phone-a", kConnection, sequence);
    }
    host.service(1003u);
    assert(!host.view().controllers[0].direct);
    assert(!host.view().controllers[0].haptics);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);
    assert(host.view().message == "Phone reconnecting; its controls are neutral.");
    assert(drainSeat(kPort, kOwner, kConnection) == 0u);
    assert(!mdkr_native_remote_pad_haptics_supported(kPort));

    /* The room never comes back: the seat is released and local play is told
     * it is still there. */
    MdkrPartyTransportEvent fatal;
    fatal.type = MdkrPartyTransportEventType::Error;
    transport.events.push_back(fatal);
    host.service(1004u);
    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(host.view().message ==
           "Phone controllers are unavailable. Local controllers still work.");
    assert(!mdkr_native_remote_pad_info(kPort, &owner, &connection));
    for (unsigned port = 0u; port < MDKR_NATIVE_REMOTE_PAD_PORTS; ++port) {
        if (port == kPort) continue;
        MdkrNativeRemotePadIngressStats idle{};
        mdkr_native_remote_pad_stats(port, &idle);
        assert(idle.binds == 0u && idle.packets == 0u);
    }
}

/*
 * A blocked network can also mean a flapping one that reports failure faster
 * than the launcher draws. One service() call must stay bounded rather than
 * draining an unbounded backlog inside a frame.
 */
void floodedFailuresStayBoundedPerServiceCall() {
    mdkr_native_remote_pad_reset_all();
    BlockedTransport transport;
    transport.block = Block::UpgradeRefused;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));

    constexpr size_t kFlood = 200u;
    for (size_t index = 0u; index < kFlood; ++index) {
        MdkrPartyTransportEvent recovering;
        recovering.type = MdkrPartyTransportEventType::Recovering;
        transport.events.push_back(recovering);
    }
    host.service(1000u);
    /* The documented per-call drain bound is 64 events. */
    assert(transport.events.size() == kFlood - 64u);
    assert(host.view().phase == MdkrNativePartyPhase::Recovering);
    assertNoSeatCustodyAnywhere();
}

}  // namespace

int main() {
    refusedSocketFailsNeutralAndBoundsRetries();
    refusedUpgradeReportsTheTransportSentence();
    silentDropUsesTheDocumentedFallbackSentence();
    midSessionDropGoesNeutralNotStale();
    floodedFailuresStayBoundedPerServiceCall();
    mdkr_native_remote_pad_reset_all();
    std::printf(
        "party_firewall_negative_native: PASS — refused socket, refused WSS "
        "upgrade, silent drop and mid-session loss all fail neutral with the "
        "documented player-facing sentences, no self-retry, a bounded 64-event "
        "drain, seats released rather than left stale and local play "
        "untouched\n");
    return 0;
}
